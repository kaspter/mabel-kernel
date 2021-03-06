// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip USB3.0 and PCIE COMBPHY with Innosilicon IP block driver
 *
 * Copyright (C) 2018 Fuzhou Rockchip Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <dt-bindings/phy/phy.h>

#define BIT_WRITEABLE_SHIFT		16

struct rockchip_combphy_priv;

enum rockchip_combphy_rst {
	OTG_RSTN	= 0,
	PHY_POR_RSTN	= 1,
	PHY_APB_RSTN	= 2,
	PHY_PIPE_RSTN	= 3,
	PHY_RESET_MAX	= 4,
};

struct combphy_reg {
	u32	offset;
	u32	bitend;
	u32	bitstart;
	u32	disable;
	u32	enable;
};

struct rockchip_combphy_grfcfg {
	struct combphy_reg	pipe_l1_sel;
	struct combphy_reg	pipe_l1_set;
	struct combphy_reg	pipe_l1pd_sel;
	struct combphy_reg	pipe_l1pd_p3;
	struct combphy_reg	pipe_l0pd_sel;
	struct combphy_reg	pipe_l0pd_p3;
	struct combphy_reg	pipe_clk_sel;
	struct combphy_reg	pipe_clk_set;
	struct combphy_reg	pipe_rate_sel;
	struct combphy_reg	pipe_rate_set;
	struct combphy_reg	pipe_mode_sel;
	struct combphy_reg	pipe_mode_set;
	struct combphy_reg	pipe_txrx_sel;
	struct combphy_reg	pipe_txrx_set;
	struct combphy_reg	pipe_width_sel;
	struct combphy_reg	pipe_width_set;
	struct combphy_reg	pipe_usb3_sel;
	struct combphy_reg	pipe_pll_lock;
	struct combphy_reg	pipe_status_l0;
};

struct rockchip_combphy_cfg {
	const struct rockchip_combphy_grfcfg grfcfg;
	int (*combphy_u3_cp_test)(struct rockchip_combphy_priv *priv);
	int (*combphy_cfg)(struct rockchip_combphy_priv *priv);
};

struct rockchip_combphy_priv {
	u8 phy_type;
	void __iomem *mmio;
	struct device *dev;
	struct clk *ref_clk;
	struct phy *phy;
	struct regmap *combphy_grf;
	struct reset_control *rsts[PHY_RESET_MAX];
	const struct rockchip_combphy_cfg *cfg;
};

static const char *get_reset_name(enum rockchip_combphy_rst rst)
{
	switch (rst) {
	case OTG_RSTN:
		return "otg-rst";
	case PHY_POR_RSTN:
		return "combphy-por";
	case PHY_APB_RSTN:
		return "combphy-apb";
	case PHY_PIPE_RSTN:
		return "combphy-pipe";
	default:
		return "invalid";
	}
}

static inline int param_write(struct regmap *base,
			      const struct combphy_reg *reg, bool en)
{
	u32 val, mask, tmp;

	tmp = en ? reg->enable : reg->disable;
	mask = GENMASK(reg->bitend, reg->bitstart);
	val = (tmp << reg->bitstart) | (mask << BIT_WRITEABLE_SHIFT);

	return regmap_write(base, reg->offset, val);
}

static u32 rockchip_combphy_pll_lock(struct rockchip_combphy_priv *priv)
{
	const struct rockchip_combphy_grfcfg *grfcfg;
	u32 mask, val;

	grfcfg = &priv->cfg->grfcfg;
	mask = GENMASK(grfcfg->pipe_pll_lock.bitend,
		       grfcfg->pipe_pll_lock.bitstart);

	regmap_read(priv->combphy_grf, grfcfg->pipe_pll_lock.offset, &val);
	val = (val & mask) >> grfcfg->pipe_pll_lock.bitstart;

	return val;
}

static u32 rockchip_combphy_is_ready(struct rockchip_combphy_priv *priv)
{
	const struct rockchip_combphy_grfcfg *grfcfg;
	u32 mask, val;

	grfcfg = &priv->cfg->grfcfg;
	mask = GENMASK(grfcfg->pipe_status_l0.bitend,
		       grfcfg->pipe_status_l0.bitstart);

	regmap_read(priv->combphy_grf, grfcfg->pipe_status_l0.offset, &val);
	val = (val & mask) >> grfcfg->pipe_status_l0.bitstart;

	return val;
}

static int phy_pcie_init(struct rockchip_combphy_priv *priv)
{
	const struct rockchip_combphy_grfcfg *grfcfg;
	u32 val;
	int ret = 0;

	grfcfg = &priv->cfg->grfcfg;

	reset_control_deassert(priv->rsts[PHY_POR_RSTN]);
	/* Wait PHY power on stable */
	udelay(5);
	reset_control_deassert(priv->rsts[PHY_APB_RSTN]);
	udelay(5);

	/* Start to configurate PHY registers for PCIE. */
	if (priv->cfg->combphy_cfg) {
		ret = priv->cfg->combphy_cfg(priv);
		if (ret)
			goto error;
	}

	/* Wait Tx PLL lock */
	usleep_range(300, 350);
	ret = readx_poll_timeout_atomic(rockchip_combphy_pll_lock, priv, val,
					val == grfcfg->pipe_pll_lock.enable,
					10, 1000);
	if (ret) {
		dev_err(priv->dev, "wait phy PLL lock timeout\n");
		goto error;
	}

	reset_control_deassert(priv->rsts[PHY_PIPE_RSTN]);

	/* Wait PIPE PHY status lane0 ready */
	ret = readx_poll_timeout_atomic(rockchip_combphy_is_ready, priv, val,
					val == grfcfg->pipe_status_l0.enable,
					10, 1000);
	if (ret)
		dev_err(priv->dev, "wait phy status lane0 ready timeout\n");

error:
	return ret;
}

static int phy_u3_init(struct rockchip_combphy_priv *priv)
{
	const struct rockchip_combphy_grfcfg *grfcfg;
	u32 val;
	int ret = 0;

	grfcfg = &priv->cfg->grfcfg;

	/* Reset the USB3 controller first. */
	reset_control_assert(priv->rsts[OTG_RSTN]);

	reset_control_deassert(priv->rsts[PHY_POR_RSTN]);
	/* Wait PHY power on stable. */
	udelay(5);

	reset_control_deassert(priv->rsts[PHY_APB_RSTN]);
	udelay(5);

	/*
	 * Start to configurate PHY registers for USB3.
	 * Note: set operation must be done before corresponding
	 * sel operation, otherwise, the PIPE PHY status lane0
	 * may be unable to get ready.
	 */

	/* Disable PHY lane1 which isn't needed for USB3 */
	param_write(priv->combphy_grf, &grfcfg->pipe_l1_set, true);
	param_write(priv->combphy_grf, &grfcfg->pipe_l1_sel, true);

	/* Set PHY Tx and Rx for USB3 */
	param_write(priv->combphy_grf, &grfcfg->pipe_txrx_set, true);
	param_write(priv->combphy_grf, &grfcfg->pipe_txrx_sel, true);

	/* Set PHY PIPE MAC pclk request */
	param_write(priv->combphy_grf, &grfcfg->pipe_clk_set, true);
	param_write(priv->combphy_grf, &grfcfg->pipe_clk_sel, true);

	/* Set PHY PIPE rate for USB3 */
	param_write(priv->combphy_grf, &grfcfg->pipe_rate_set, true);
	param_write(priv->combphy_grf, &grfcfg->pipe_rate_sel, true);

	/* Set PHY mode for USB3 */
	param_write(priv->combphy_grf, &grfcfg->pipe_mode_set, true);
	param_write(priv->combphy_grf, &grfcfg->pipe_mode_sel, true);

	/* Set PHY data bus width for USB3 */
	param_write(priv->combphy_grf, &grfcfg->pipe_width_set, true);
	param_write(priv->combphy_grf, &grfcfg->pipe_width_sel, true);

	/* Select PIPE for USB3 */
	param_write(priv->combphy_grf, &grfcfg->pipe_usb3_sel, true);

	if (priv->cfg->combphy_cfg) {
		ret = priv->cfg->combphy_cfg(priv);
		if (ret)
			goto error;
	}

	/* Wait Tx PLL lock */
	usleep_range(300, 350);
	ret = readx_poll_timeout_atomic(rockchip_combphy_pll_lock, priv, val,
					val == grfcfg->pipe_pll_lock.enable,
					10, 1000);
	if (ret) {
		dev_err(priv->dev, "wait phy PLL lock timeout\n");
		goto error;
	}

	reset_control_deassert(priv->rsts[PHY_PIPE_RSTN]);

	/* Wait PIPE PHY status lane0 ready */
	ret = readx_poll_timeout_atomic(rockchip_combphy_is_ready, priv, val,
					val == grfcfg->pipe_status_l0.enable,
					10, 1000);
	if (ret) {
		dev_err(priv->dev, "wait phy status lane0 ready timeout\n");
		goto error;
	}

	reset_control_deassert(priv->rsts[OTG_RSTN]);

error:
	return ret;
}

static int rockchip_combphy_set_phy_type(struct rockchip_combphy_priv *priv)
{
	int ret;

	switch (priv->phy_type) {
	case PHY_TYPE_PCIE:
		ret = phy_pcie_init(priv);
		break;
	case PHY_TYPE_USB3:
		ret = phy_u3_init(priv);
		break;
	default:
		dev_err(priv->dev, "incompatible PHY type\n");
		return -EINVAL;
	}

	return ret;
}

static int rockchip_combphy_init(struct phy *phy)
{
	struct rockchip_combphy_priv *priv = phy_get_drvdata(phy);
	int ret;

	ret = clk_prepare_enable(priv->ref_clk);
	if (ret) {
		dev_err(priv->dev, "failed to enable ref_clk\n");
		return ret;
	}

	ret = rockchip_combphy_set_phy_type(priv);
	if (ret) {
		dev_err(priv->dev, "failed to set phy type\n");
		return ret;
	}

	return 0;
}

static int rockchip_combphy_exit(struct phy *phy)
{
	struct rockchip_combphy_priv *priv = phy_get_drvdata(phy);

	reset_control_assert(priv->rsts[PHY_POR_RSTN]);
	reset_control_assert(priv->rsts[PHY_APB_RSTN]);
	reset_control_assert(priv->rsts[PHY_PIPE_RSTN]);

	clk_disable_unprepare(priv->ref_clk);

	return 0;
}

static int rockchip_combphy_u3_cp_test(struct phy *phy)
{
	struct rockchip_combphy_priv *priv = phy_get_drvdata(phy);
	int ret = 0;

	/*
	 * When do USB3 compliance test, we may connect the oscilloscope
	 * front panel Aux Out to the DUT SSRX+, the Aux Out of the
	 * oscilloscope outputs a negative pulse whose width is between
	 * 300- 400 ns which may trigger some DUTs to change the CP test
	 * pattern.
	 *
	 * The Inno USB3 PHY disable the function to detect the negative
	 * pulse in SSRX+ by default, so we need to enable the function
	 * to toggle the CP test pattern before do USB3 compliance test.
	 */
	if (priv->cfg->combphy_u3_cp_test)
		ret = priv->cfg->combphy_u3_cp_test(priv);

	return ret;
}

static const struct phy_ops rockchip_combphy_ops = {
	.init		= rockchip_combphy_init,
	.exit		= rockchip_combphy_exit,
	.cp_test	= rockchip_combphy_u3_cp_test,
	.owner		= THIS_MODULE,
};

static struct phy *rockchip_combphy_xlate(struct device *dev,
					  struct of_phandle_args *args)
{
	struct rockchip_combphy_priv *priv = dev_get_drvdata(dev);

	if (args->args_count < 1) {
		dev_err(dev, "invalid number of arguments\n");
		return ERR_PTR(-EINVAL);
	}

	if (priv->phy_type != PHY_NONE && priv->phy_type != args->args[0]) {
		dev_err(dev, "type select %d overwriting phy type %d\n",
			args->args[0], priv->phy_type);
		return ERR_PTR(-EINVAL);
	}

	priv->phy_type = args->args[0];

	if (priv->phy_type < PHY_TYPE_SATA || priv->phy_type > PHY_TYPE_USB3) {
		dev_err(dev, "invalid phy type select argument\n");
		return ERR_PTR(-EINVAL);
	}

	return priv->phy;
}

static int rockchip_combphy_parse_dt(struct device *dev,
				     struct rockchip_combphy_priv *priv)
{
	u32 i;

	priv->combphy_grf = syscon_regmap_lookup_by_phandle(dev->of_node,
							    "rockchip,combphygrf");
	if (IS_ERR(priv->combphy_grf)) {
		dev_err(dev, "failed to find combphy grf regmap\n");
		return PTR_ERR(priv->combphy_grf);
	}

	priv->ref_clk = devm_clk_get(dev, "refclk");
	if (IS_ERR(priv->ref_clk)) {
		dev_err(dev, "failed to find ref clock\n");
		return PTR_ERR(priv->ref_clk);
	}

	for (i = 0; i < PHY_RESET_MAX; i++) {
		priv->rsts[i] = devm_reset_control_get(dev, get_reset_name(i));
		if (IS_ERR(priv->rsts[i])) {
			dev_warn(dev, "no %s reset control specified\n",
				 get_reset_name(i));
			priv->rsts[i] = NULL;
		}
	}

	return 0;
}

static int rockchip_combphy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct rockchip_combphy_priv *priv;
	struct resource *res;
	const struct rockchip_combphy_cfg *phy_cfg;
	int ret;

	phy_cfg = of_device_get_match_data(dev);
	if (!phy_cfg) {
		dev_err(dev, "No OF match data provided\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->mmio = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->mmio)) {
		ret = PTR_ERR(priv->mmio);
		return ret;
	}

	ret = rockchip_combphy_parse_dt(dev, priv);
	if (ret) {
		dev_err(dev, "parse dt failed, ret(%d)\n", ret);
		return ret;
	}

	reset_control_assert(priv->rsts[PHY_POR_RSTN]);
	reset_control_assert(priv->rsts[PHY_APB_RSTN]);
	reset_control_assert(priv->rsts[PHY_PIPE_RSTN]);

	priv->phy_type = PHY_NONE;
	priv->dev = dev;
	priv->cfg = phy_cfg;
	priv->phy = devm_phy_create(dev, NULL, &rockchip_combphy_ops);
	if (IS_ERR(priv->phy)) {
		dev_err(dev, "failed to create combphy\n");
		return PTR_ERR(priv->phy);
	}

	dev_set_drvdata(dev, priv);
	phy_set_drvdata(priv->phy, priv);

	phy_provider = devm_of_phy_provider_register(dev,
						     rockchip_combphy_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
}

static int rk1808_combphy_u3_cp_test(struct rockchip_combphy_priv *priv)
{
	if (priv->phy_type != PHY_TYPE_USB3) {
		dev_err(priv->dev, "failed to set cp test for phy type %d\n",
			priv->phy_type);
		return -EINVAL;
	}

	writel(0x0c, priv->mmio + 0x4008);

	return 0;
}

static int rk1808_combphy_cfg(struct rockchip_combphy_priv *priv)
{
	unsigned long rate;
	u32 reg;
	bool ssc_en = false;

	rate = clk_get_rate(priv->ref_clk);

	/* Configure PHY reference clock frequency */
	switch (rate) {
	case 24000000:
		/*
		 * The default PHY refclk frequency
		 * configuration is 24MHz.
		 */
		break;
	case 25000000:
		writel(0x00, priv->mmio + 0x2118);
		writel(0x64, priv->mmio + 0x211c);
		writel(0x01, priv->mmio + 0x2020);
		writel(0x64, priv->mmio + 0x2028);
		writel(0x21, priv->mmio + 0x2030);
		break;
	case 50000000:
		writel(0x00, priv->mmio + 0x2118);
		writel(0x32, priv->mmio + 0x211c);
		writel(0x01, priv->mmio + 0x2020);
		writel(0x32, priv->mmio + 0x2028);
		writel(0x21, priv->mmio + 0x2030);
		break;
	default:
		dev_err(priv->dev, "Unsupported rate: %lu\n", rate);
		return -EINVAL;
	}

	if (priv->phy_type == PHY_TYPE_PCIE) {
		/* Adjust Lane 0 Rx interface timing */
		writel(0x20, priv->mmio + 0x20ac);

		/* Adjust Lane 1 Rx interface timing */
		writel(0x20, priv->mmio + 0x30ac);
	} else if (priv->phy_type == PHY_TYPE_USB3) {
		/* Adjust Lane 0 Rx interface timing */
		writel(0x20, priv->mmio + 0x20ac);

		/* Set and enable SSC */
		switch (rate) {
		case 24000000:
			/* Set SSC rate to 31.25KHz */
			reg = readl(priv->mmio + 0x2108);
			reg = (reg & ~0xf) | 0x1;
			writel(reg, priv->mmio + 0x2108);
			ssc_en = true;
			break;
		case 25000000:
			/* Set SSC rate to 32.55KHz */
			reg = readl(priv->mmio + 0x2108);
			reg = (reg & ~0xf) | 0x6;
			writel(reg, priv->mmio + 0x2108);
			ssc_en = true;
			break;
		default:
			dev_warn(priv->dev,
				 "failed to set SSC on rate: %lu\n", rate);
			break;
		}

		if (ssc_en) {
			/* Enable SSC */
			reg = readl(priv->mmio + 0x2120);
			reg &= ~BIT(4);
			writel(reg, priv->mmio + 0x2120);

			reg = readl(priv->mmio + 0x2000);
			reg &= ~0x6;
			writel(reg, priv->mmio + 0x2000);
		}

		/* Tuning Tx */

		/*
		 * Tuning Rx for RJTL:
		 * Decrease CDR Chump Bump current.
		 */
		reg = readl(priv->mmio + 0x20c8);
		reg = (reg & ~0x6) | BIT(1);
		writel(reg, priv->mmio + 0x20c8);
		reg = readl(priv->mmio + 0x2150);
		reg |= BIT(2);
		writel(reg, priv->mmio + 0x2150);
	} else {
		dev_err(priv->dev, "failed to cfg incompatible PHY type\n");
		return -EINVAL;
	}

	return 0;
}

static const struct rockchip_combphy_cfg rk1808_combphy_cfgs = {
	.grfcfg	= {
		.pipe_l1_sel	= { 0x0000, 15, 11, 0x00, 0x1f },
		.pipe_l1_set	= { 0x0008, 13, 8, 0x00, 0x13 },
		.pipe_l1pd_sel	= { 0x0000, 11, 11, 0x0, 0x1},
		.pipe_l1pd_p3	= { 0x0008, 9, 8, 0x0, 0x3 },
		.pipe_l0pd_sel	= { 0x0000, 6, 6, 0x0, 0x1 },
		.pipe_l0pd_p3	= { 0x0008, 1, 0, 0x0, 0x3 },
		.pipe_clk_sel	= { 0x0000, 3, 3, 0x0, 0x1 },
		.pipe_clk_set	= { 0x0004, 7, 6, 0x1, 0x0 },
		.pipe_rate_sel	= { 0x0000, 2, 2, 0x0, 0x1 },
		.pipe_rate_set	= { 0x0004, 5, 4, 0x0, 0x1 },
		.pipe_mode_sel	= { 0x0000, 1, 1, 0x0, 0x1 },
		.pipe_mode_set	= { 0x0004, 3, 2, 0x0, 0x1 },
		.pipe_txrx_sel	= { 0x0004, 15, 8, 0x14, 0x2f },
		.pipe_txrx_set	= { 0x0008, 15, 14, 0x0, 0x3 },
		.pipe_width_sel	= { 0x0000, 0, 0, 0x0, 0x1 },
		.pipe_width_set	= { 0x0004, 1, 0, 0x2, 0x0 },
		.pipe_usb3_sel	= { 0x000c, 0, 0, 0x0, 0x1 },
		.pipe_pll_lock	= { 0x0034, 14, 14, 0x0, 0x1 },
		.pipe_status_l0	= { 0x0034, 7, 7, 0x1, 0x0 },
	},
	.combphy_u3_cp_test	= rk1808_combphy_u3_cp_test,
	.combphy_cfg		= rk1808_combphy_cfg,
};

static const struct of_device_id rockchip_combphy_of_match[] = {
	{
		.compatible = "rockchip,rk1808-combphy",
		.data = &rk1808_combphy_cfgs,
	},
	{ },
};

MODULE_DEVICE_TABLE(of, rockchip_combphy_of_match);

static struct platform_driver rockchip_combphy_driver = {
	.probe	= rockchip_combphy_probe,
	.driver = {
		.name = "rockchip-combphy",
		.of_match_table = rockchip_combphy_of_match,
	},
};
module_platform_driver(rockchip_combphy_driver);

MODULE_AUTHOR("William Wu <william.wu@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip USB3.0 and PCIE COMBPHY driver");
MODULE_LICENSE("GPL v2");
