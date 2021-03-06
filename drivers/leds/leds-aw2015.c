/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>

/* register address */
#define REG_RESET			0x00
#define REG_GLOBAL_CONTROL		0x01
#define REG_LED_IMAX			0x03
#define REG_LED_CONFIG_BASE		0x04
#define REG_LED_ENABLE		0x07
#define REG_LED_CONTROL		0x08
#define REG_LED_START		0x09
#define REG_LED_BRIGHTNESS_BASE	0x10
#define REG_LED_PWM_DUTY_BASE	0x1C

#define REG_TIMESET1_BASE		0x30
#define REG_TIMESET2_BASE		0x31
#define REG_TIMESET3_BASE		0x32

/* register bits */
#define AW2015_CHIPID			0x31
#define AW_LED_MOUDLE_ENABLE_MASK	0x01
#define AW_CHARGER_DISABLE_MASK		0x02

#define AW_LED_BREATHE_MODE_MASK	0x01
#define AW_LED_RESET_MASK		0x55

#define AW_LED_RESET_DELAY		8

#define MAX_RISE_TIME_MS		7
#define MAX_HOLD_TIME_MS		5
#define MAX_FALL_TIME_MS		7
#define MAX_OFF_TIME_MS			5

/* The definition of each time described as shown in figure.
 *        /-----------\
 *       /      |      \
 *      /|      |      |\
 *     / |      |      | \-----------
 *       |hold_time_ms |      |
 *       |             |      |
 * rise_time_ms  fall_time_ms |
 *                       off_time_ms
 */

struct aw2015_platform_data {
	int max_current;
	int rise_time_ms;
	int hold_time_ms;
	int fall_time_ms;
	int off_time_ms;
	int pwm_duty;
	struct aw2015_led *led;
};


struct aw2015_led {
	struct i2c_client *client;
	struct led_classdev cdev;
	struct aw2015_platform_data *pdata;
	struct work_struct brightness_work;
	struct mutex lock;
	struct regulator *vdd;
	struct regulator *vcc;
	int num_leds;
	int id;
	int pwm_duty;
	bool poweron;
	atomic_t unset;
};

static int aw2015_write(struct aw2015_led *led, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(led->client, reg, val);
}

static int aw2015_read(struct aw2015_led *led, u8 reg, u8 *val)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(led->client, reg);
	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static void aw2015_brightness_work_set(struct aw2015_led *led)
{
	u8 val, id;
	u8 leds_brightness[3] = {0, 0, 0}; /* red, green, blue */

	/* enable regulators if they are disabled */

	leds_brightness[2] = (led->cdev.brightness) & 0xFF;
	leds_brightness[1] = (led->cdev.brightness >> 8) & 0xFF;
	leds_brightness[0] = (led->cdev.brightness >> 16) & 0xFF;

	if ((led->cdev.brightness & 0xFFFFFF) == 0) {
		aw2015_read(led, REG_LED_ENABLE, &val);
		aw2015_write(led, REG_LED_ENABLE, 0);
	} else {
		aw2015_write(led, REG_GLOBAL_CONTROL,
			AW_LED_MOUDLE_ENABLE_MASK | AW_CHARGER_DISABLE_MASK);
		aw2015_write(led, REG_LED_IMAX, led->pdata->max_current);
		aw2015_write(led, REG_LED_PWM_DUTY_BASE,
			led->pdata->pwm_duty);
		aw2015_write(led, REG_LED_CONTROL, 8);
		aw2015_write(led, REG_LED_CONFIG_BASE, 0);
		for (id = 0; id < 3; id++) {
			aw2015_write(led, REG_LED_CONFIG_BASE + id, 0);
			aw2015_write(led, REG_LED_BRIGHTNESS_BASE + id,
				leds_brightness[id]);
		}
		aw2015_write(led, REG_LED_ENABLE, 7);
	}

	aw2015_read(led, REG_LED_ENABLE, &val);
	/*
	 * If value in REG_LED_ENABLE is 0, it means the RGB leds are
	 * all off. So we need to power it off.
	 */
}

static void aw2015_brightness_work(struct work_struct *work)
{
	struct aw2015_led *led = container_of(work, struct aw2015_led,
					brightness_work);
	mutex_lock(&led->pdata->led->lock);
	if (atomic_read(&led->unset)) {
		aw2015_brightness_work_set(led);
		atomic_set(&led->unset,0);
	}
	mutex_unlock(&led->pdata->led->lock);
}

static void aw2015_led_blink_set(struct aw2015_led *led, unsigned long blinking)
{
	u8 val, id;
	u8 leds_brightness[3] = {0, 0, 0}; /* red, green, blue */

	/* enable regulators if they are disabled */

	led->cdev.brightness = blinking;
	leds_brightness[2] = (led->cdev.brightness) & 0xFF;
	leds_brightness[1] = (led->cdev.brightness >> 8) & 0xFF;
	leds_brightness[0] = (led->cdev.brightness >> 16) & 0xFF;

	if (blinking > 0) {
		aw2015_write(led, REG_GLOBAL_CONTROL,
			AW_LED_MOUDLE_ENABLE_MASK | AW_CHARGER_DISABLE_MASK);
		aw2015_write(led, REG_TIMESET1_BASE,
			led->pdata->rise_time_ms << 4 |
			led->pdata->hold_time_ms);
		aw2015_write(led, REG_TIMESET2_BASE ,
			led->pdata->fall_time_ms << 4 |
			led->pdata->off_time_ms);
		aw2015_write(led, REG_LED_IMAX, led->pdata->max_current);
		aw2015_write(led, REG_LED_PWM_DUTY_BASE,
			led->pdata->pwm_duty);
		aw2015_write(led, REG_LED_CONTROL, 8);

		for (id = 0; id < 3; id++) {
			aw2015_write(led, REG_LED_CONFIG_BASE + id,
				AW_LED_BREATHE_MODE_MASK);
			aw2015_write(led, REG_LED_BRIGHTNESS_BASE + id,
				leds_brightness[id]);
			aw2015_write(led, REG_TIMESET3_BASE + id * 5, 0);
		}
		aw2015_write(led, REG_LED_ENABLE, 7);
		aw2015_write(led, REG_LED_START, 1);
	} else {
		led->cdev.brightness =  0;
		aw2015_write(led, REG_LED_ENABLE, 0);
		aw2015_write(led, REG_LED_START, 0x70);
	}

	aw2015_read(led, REG_LED_ENABLE, &val);
	/*
	 * If value in REG_LED_ENABLE is 0, it means the RGB leds are
	 * all off. So we need to power it off.
	 */
}

static void aw2015_set_brightness(struct led_classdev *cdev,
			     enum led_brightness brightness)
{
	struct aw2015_led *led = container_of(cdev, struct aw2015_led, cdev);

	led->cdev.brightness = brightness;

	atomic_set(&led->unset,1);

	schedule_work(&led->brightness_work);
}

static ssize_t aw2015_store_blink(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t len)
{
	unsigned long blinking;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw2015_led *led =
			container_of(led_cdev, struct aw2015_led, cdev);
	ssize_t ret = -EINVAL;

	ret = kstrtoul(buf, 10, &blinking);
	if (ret)
		return ret;
	mutex_lock(&led->pdata->led->lock);
	atomic_set(&led->unset,0);
	aw2015_led_blink_set(led, blinking);
	mutex_unlock(&led->pdata->led->lock);

	return len;
}

static ssize_t aw2015_led_time_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw2015_led *led =
			container_of(led_cdev, struct aw2015_led, cdev);

	return snprintf(buf, PAGE_SIZE, "%d %d %d %d\n",
			led->pdata->rise_time_ms, led->pdata->hold_time_ms,
			led->pdata->fall_time_ms, led->pdata->off_time_ms);
}

static ssize_t aw2015_led_time_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw2015_led *led =
			container_of(led_cdev, struct aw2015_led, cdev);
	int rc, rise_time_ms, hold_time_ms, fall_time_ms, off_time_ms;

	rc = sscanf(buf, "%d %d %d %d",
			&rise_time_ms, &hold_time_ms,
			&fall_time_ms, &off_time_ms);

	mutex_lock(&led->pdata->led->lock);
	led->pdata->rise_time_ms = (rise_time_ms > MAX_RISE_TIME_MS) ?
				MAX_RISE_TIME_MS : rise_time_ms;
	led->pdata->hold_time_ms = (hold_time_ms > MAX_HOLD_TIME_MS) ?
				MAX_HOLD_TIME_MS : hold_time_ms;
	led->pdata->fall_time_ms = (fall_time_ms > MAX_FALL_TIME_MS) ?
				MAX_FALL_TIME_MS : fall_time_ms;
	led->pdata->off_time_ms = (off_time_ms > MAX_OFF_TIME_MS) ?
				MAX_OFF_TIME_MS : off_time_ms;
	aw2015_led_blink_set(led, 1);
	mutex_unlock(&led->pdata->led->lock);
	return len;
}

static ssize_t aw2015_led_reg_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct aw2015_led *led = container_of(led_cdev,
			struct aw2015_led, cdev);
	unsigned char reg_val;
	ssize_t len = 0;
	u8 i;

	for (i = 0; i < 0x3E; i++) {
		aw2015_read(led, i, &reg_val);
		len += snprintf(buf+len, PAGE_SIZE-len, "reg%2X = 0x%2X, ",
			i, reg_val);
	}

	return len;
}

static DEVICE_ATTR(blink, 0664, NULL, aw2015_store_blink);
static DEVICE_ATTR(led_time, 0664, aw2015_led_time_show, aw2015_led_time_store);
static DEVICE_ATTR(reg, 0664, aw2015_led_reg_show, NULL);

static struct attribute *aw2015_led_attributes[] = {
	&dev_attr_blink.attr,
	&dev_attr_led_time.attr,
	&dev_attr_reg.attr,
	NULL,
};

static struct attribute_group aw2015_led_attr_group = {
	.attrs = aw2015_led_attributes
};

static int aw2015_check_chipid(struct aw2015_led *led)
{
	int ret;
	u8 val = 0;
	struct i2c_client *client = led->client;

	aw2015_write(led, REG_RESET, AW_LED_RESET_MASK);
	usleep_range(AW_LED_RESET_DELAY, AW_LED_RESET_DELAY);
	aw2015_read(led, REG_RESET,  &val);

	if (val != AW2015_CHIPID) {
		dev_err(&client->dev, "aw2015 chipid failed (%02X)\n", val);
		ret = -1;
	} else {
		aw2015_write(led, REG_LED_ENABLE, 0);
		aw2015_write(led, REG_LED_BRIGHTNESS_BASE, 0);
		dev_info(&client->dev, "Found aw2015 %02X sensor\n", val);
		ret =  0;
	}

	return ret;
}

static int aw2015_led_err_handle(struct aw2015_led *led_array,
				int parsed_leds)
{
	int i;
	/*
	 * If probe fails, cannot free resource of all LEDs, only free
	 * resources of LEDs which have allocated these resource really.
	 */
	for (i = 0; i < parsed_leds; i++) {
		sysfs_remove_group(&led_array[i].cdev.dev->kobj,
				&aw2015_led_attr_group);
		led_classdev_unregister(&led_array[i].cdev);
		cancel_work_sync(&led_array[i].brightness_work);
		devm_kfree(&led_array->client->dev, led_array[i].pdata);
		led_array[i].pdata = NULL;
	}
	return i;
}

static int aw2015_led_parse_child_node(struct aw2015_led *led_array,
				struct device_node *node)
{
	struct aw2015_led *led;
	struct device_node *temp;
	struct aw2015_platform_data *pdata;
	int rc = 0, parsed_leds = 0;

	for_each_child_of_node(node, temp) {
		led = &led_array[parsed_leds];
		led->client = led_array->client;

		pdata = devm_kzalloc(&led->client->dev,
				sizeof(struct aw2015_platform_data),
				GFP_KERNEL);
		if (!pdata) {
			dev_err(&led->client->dev,
				"Failed to allocate memory\n");
			goto free_err;
		}
		pdata->led = led_array;
		led->pdata = pdata;

		rc = of_property_read_string(temp, "aw2015,name",
			&led->cdev.name);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading led name, rc = %d\n", rc);
			goto free_pdata;
		}

		rc = of_property_read_u32(temp, "aw2015,id",
			&led->id);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading id, rc = %d\n", rc);
			goto free_pdata;
		}

		rc = of_property_read_u32(temp, "aw2015,max-brightness",
			&led->cdev.max_brightness);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading max-brightness, rc = %d\n",
				rc);
			goto free_pdata;
		}

		rc = of_property_read_u32(temp, "aw2015,max-current",
			&led->pdata->max_current);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading max-current, rc = %d\n", rc);
			goto free_pdata;
		}

		rc = of_property_read_u32(temp, "aw2015,rise-time-ms",
			&led->pdata->rise_time_ms);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading rise-time-ms, rc = %d\n", rc);
			goto free_pdata;
		}

		rc = of_property_read_u32(temp, "aw2015,hold-time-ms",
			&led->pdata->hold_time_ms);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading hold-time-ms, rc = %d\n", rc);
			goto free_pdata;
		}

		rc = of_property_read_u32(temp, "aw2015,fall-time-ms",
			&led->pdata->fall_time_ms);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading fall-time-ms, rc = %d\n", rc);
			goto free_pdata;
		}

		rc = of_property_read_u32(temp, "aw2015,off-time-ms",
			&led->pdata->off_time_ms);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading off-time-ms, rc = %d\n", rc);
			goto free_pdata;
		}

		INIT_WORK(&led->brightness_work, aw2015_brightness_work);

		led->cdev.brightness_set = aw2015_set_brightness;

		rc = led_classdev_register(&led->client->dev, &led->cdev);
		if (rc) {
			dev_err(&led->client->dev,
				"unable to register led %d,rc=%d\n",
				led->id, rc);
			goto free_pdata;
		}

		atomic_set(&led->unset, 0);

		rc = of_property_read_u32(temp, "aw2015,pwm-duty",
			&led->pdata->pwm_duty);
		if (rc < 0) {
			dev_err(&led->client->dev,
				"Failure reading pwm-duty, rc = %d\n", rc);
			led->pdata->pwm_duty = 255;
		}

		rc = sysfs_create_group(&led->cdev.dev->kobj,
				&aw2015_led_attr_group);
		if (rc) {
			dev_err(&led->client->dev, "led sysfs rc: %d\n", rc);
			goto free_class;
		}

		led->cdev.brightness = LED_OFF;

		parsed_leds++;
	}

	return 0;

free_class:
	aw2015_led_err_handle(led_array, parsed_leds);
	led_classdev_unregister(&led_array[parsed_leds].cdev);
	cancel_work_sync(&led_array[parsed_leds].brightness_work);
	devm_kfree(&led->client->dev, led_array[parsed_leds].pdata);
	led_array[parsed_leds].pdata = NULL;
	return rc;

free_pdata:
	aw2015_led_err_handle(led_array, parsed_leds);
	devm_kfree(&led->client->dev, led_array[parsed_leds].pdata);
	return rc;

free_err:
	aw2015_led_err_handle(led_array, parsed_leds);
	return rc;
}

static int aw2015_led_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct aw2015_led *led_array;
	struct device_node *node;
	int ret, num_leds = 0;

	node = client->dev.of_node;
	if (node == NULL)
		return -EINVAL;

	num_leds = of_get_child_count(node);

	if (!num_leds)
		return -EINVAL;

	led_array = devm_kzalloc(&client->dev,
			(sizeof(struct aw2015_led) * num_leds), GFP_KERNEL);
	if (!led_array)
		return -ENOMEM;

	led_array->client = client;
	led_array->num_leds = num_leds;

	mutex_init(&led_array->lock);

	ret = aw2015_check_chipid(led_array);
	if (ret < 0) {
		dev_err(&client->dev, "Check chip id error\n");
		goto free_led_arry;
	}

	ret = aw2015_led_parse_child_node(led_array, node);
	if (ret) {
		dev_err(&client->dev, "parsed node error\n");
		goto free_led_arry;
	}

	i2c_set_clientdata(client, led_array);

	return 0;

free_led_arry:
	mutex_destroy(&led_array->lock);
	devm_kfree(&client->dev, led_array);
	led_array = NULL;
	return ret;
}

static int aw2015_led_remove(struct i2c_client *client)
{
	struct aw2015_led *led_array = i2c_get_clientdata(client);
	int i, parsed_leds = led_array->num_leds;

	for (i = 0; i < parsed_leds; i++) {
		sysfs_remove_group(&led_array[i].cdev.dev->kobj,
				&aw2015_led_attr_group);
		led_classdev_unregister(&led_array[i].cdev);
		cancel_work_sync(&led_array[i].brightness_work);
		devm_kfree(&client->dev, led_array[i].pdata);
		led_array[i].pdata = NULL;
	}
	mutex_destroy(&led_array->lock);
	devm_kfree(&client->dev, led_array);
	led_array = NULL;
	return 0;
}

static const struct i2c_device_id aw2015_led_id[] = {
	{"aw2015_led_rgb", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, aw2015_led_id);

static struct of_device_id aw2015_match_table[] = {
	{ .compatible = "awinic,aw2015",},
	{ },
};

static struct i2c_driver aw2015_led_driver = {
	.probe = aw2015_led_probe,
	.remove = aw2015_led_remove,
	.driver = {
		.name = "aw2015_led_rgb",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(aw2015_match_table),
	},
	.id_table = aw2015_led_id,
};

static int __init aw2015_led_init(void)
{
	return i2c_add_driver(&aw2015_led_driver);
}
module_init(aw2015_led_init);

static void __exit aw2015_led_exit(void)
{
	i2c_del_driver(&aw2015_led_driver);
}
module_exit(aw2015_led_exit);

MODULE_DESCRIPTION("AWINIC aw2015 LED driver");
MODULE_LICENSE("GPL v2");
