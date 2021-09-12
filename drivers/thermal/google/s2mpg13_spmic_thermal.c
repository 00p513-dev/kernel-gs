// SPDX-License-Identifier: GPL-2.0
/*
 * s2mpg13_spmic_thermal.c S2MPG13 Sub-PMIC thermistor driver
 *
 * Copyright (c) 2021, Google LLC. All rights reserved.
 *
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/mfd/samsung/s2mpg13.h>
#include <linux/mfd/samsung/s2mpg13-register.h>

#include "../thermal_core.h"

#define GTHERM_CHAN_NUM 8

struct s2mpg13_spmic_thermal_sensor {
	struct s2mpg13_spmic_thermal_chip *chip;
	struct thermal_zone_device *tzd;
	unsigned int adc_chan;
	bool thr_triggered;
	int emul_temperature;
	int irq;
};

struct s2mpg13_spmic_thermal_chip {
	struct device *dev;
	struct i2c_client *i2c;
	struct s2mpg13_dev *iodev;
	struct s2mpg13_spmic_thermal_sensor sensor[GTHERM_CHAN_NUM];
	u8 adc_chan_en;
};

/**
 * struct s2mpg13_spmic_thermal_map_pt - Map data representation for ADC
 * @volt: Represent the ADC voltage data.
 * @temp: Represent the temperature for given volt.
 */
struct adc_map_pt {
	int volt;
	int temp;
};

/*
 * voltage to temperature table organized descending in voltage,
 * ascending in temperature.
 */
static const struct adc_map_pt s2mpg13_adc_map[] = {
	{ 0xF8D, -26428 }, { 0xF6A, -21922 }, { 0xF29, -15958 },
	{ 0xEE4, -11060 }, { 0xE9D, -6890 },  { 0xE3F, -2264 },
	{ 0xDBF, 2961 },   { 0xD33, 7818 },   { 0xC97, 12525 },
	{ 0xBF5, 16945 },  { 0xB3A, 21623 },  { 0xA42, 27431 },
	{ 0x7F1, 40631 },  { 0x734, 44960 },  { 0x66B, 49757 },
	{ 0x5A3, 54854 },  { 0x4EE, 59898 },  { 0x446, 65076 },
	{ 0x43A, 65779 },  { 0x430, 65856 },  { 0x3C3, 69654 },
	{ 0x3BD, 69873 },  { 0x33B, 74910 },  { 0x2BB, 80691 },
	{ 0x259, 85844 },  { 0x206, 90915 },  { 0x1CE, 94873 },
	{ 0x191, 99720 },  { 0x160, 104216 }, { 0x12E, 109531 },
	{ 0xF9, 116445 },  { 0xD7, 121600 },  { 0x9F, 131839 },
};

/*
 * Convert the input voltage to a temperature using linear interpretation
 * from the lookup table.
 */
static int s2mpg13_map_volt_temp(int input)
{
	int low = 0;
	int high = ARRAY_SIZE(s2mpg13_adc_map) - 1;
	int mid = 0;

	if (s2mpg13_adc_map[low].volt <= input)
		return s2mpg13_adc_map[low].temp;
	else if (s2mpg13_adc_map[high].volt >= input)
		return s2mpg13_adc_map[high].temp;

	/* Binary search, value will be between index low and low - 1 */
	while (low <= high) {
		mid = (low + high) / 2;
		if (s2mpg13_adc_map[mid].volt < input)
			high = mid - 1;
		else if (s2mpg13_adc_map[mid].volt > input)
			low = mid + 1;
		else
			return s2mpg13_adc_map[mid].temp;
	}

	return s2mpg13_adc_map[low].temp +
	       mult_frac(s2mpg13_adc_map[low - 1].temp - s2mpg13_adc_map[low].temp,
			 input - s2mpg13_adc_map[low].volt,
			 s2mpg13_adc_map[low - 1].volt - s2mpg13_adc_map[low].volt);
}

/*
 * Convert the temperature to voltage to a temperature using linear interpretation
 * from the lookup table.
 */
static int s2mpg13_map_temp_volt(int input)
{
	int low = 0;
	int high = ARRAY_SIZE(s2mpg13_adc_map) - 1;
	int mid = 0;

	if (s2mpg13_adc_map[low].temp >= input)
		return s2mpg13_adc_map[low].volt;
	else if (s2mpg13_adc_map[high].temp <= input)
		return s2mpg13_adc_map[high].volt;

	/* Binary search, value will between index low and low - 1 */
	while (low <= high) {
		mid = (low + high) / 2;
		if (s2mpg13_adc_map[mid].temp < input)
			low = mid + 1;
		else if (s2mpg13_adc_map[mid].temp > input)
			high = mid - 1;
		else
			return s2mpg13_adc_map[mid].volt;
	}
	return s2mpg13_adc_map[low].volt +
	       mult_frac(s2mpg13_adc_map[low - 1].volt - s2mpg13_adc_map[low].volt,
			 input - s2mpg13_adc_map[low].temp,
			 s2mpg13_adc_map[low - 1].temp - s2mpg13_adc_map[low].temp);
}

/*
 * Get temperature for given tz.
 */
static int s2mpg13_spmic_thermal_get_temp(void *data, int *temp)
{
	struct s2mpg13_spmic_thermal_sensor *s = data;
	struct s2mpg13_spmic_thermal_chip *s2mpg13_spmic_thermal = s->chip;
	int emul_temp;
	int raw, ret = 0;
	u8 mask = 0x1;
	u8 data_buf[S2MPG13_METER_NTC_BUF];
	u8 reg = S2MPG13_METER_LPF_DATA_NTC0_1 +
		 S2MPG13_METER_NTC_BUF * s->adc_chan;

	emul_temp = s->emul_temperature;
	if (emul_temp) {
		*temp = emul_temp;
		return 0;
	}

	if (!(s2mpg13_spmic_thermal->adc_chan_en & (mask << s->adc_chan)))
		return -EIO;

	ret = s2mpg13_bulk_read(s2mpg13_spmic_thermal->i2c, reg,
				S2MPG13_METER_NTC_BUF, data_buf);
	raw = data_buf[0] + ((data_buf[1] & 0xf) << 8);
	*temp = s2mpg13_map_volt_temp(raw);

	return ret;
}

/*
 * Set monitor window for given tz.
 */
static int s2mpg13_spmic_thermal_set_trips(void *data, int low_temp,
					 int high_temp)
{
	struct s2mpg13_spmic_thermal_sensor *s = data;
	struct s2mpg13_spmic_thermal_chip *s2mpg13_spmic_thermal = s->chip;
	struct device *dev = s2mpg13_spmic_thermal->dev;
	int emul_temp, low_volt, ret = 0;
	u8 raw;

	/* Set threshold to extreme value when emul_temp set */
	emul_temp = s->emul_temperature;
	if (emul_temp) {
		high_temp = INT_MAX;
		low_temp = INT_MIN;
	}

	/*
	 * Ignore low_temp, and assuming trips are
	 * configured with passive for polling
	 */
	low_volt = s2mpg13_map_temp_volt(high_temp);

	raw = low_volt >> 4 & 0xFF;
	ret = s2mpg13_write_reg(s2mpg13_spmic_thermal->i2c,
				S2MPG13_METER_NTC_OT_WARN0 + s->adc_chan, raw);

	dev_dbg_ratelimited(dev,
			    "low_temp(mdegC):%d, high_temp(mdegC):%d adc:%d ret:%d\n",
			    low_temp, high_temp, raw, ret);

	return ret;
}

/*
 * Set emulation temperture for given tz.
 */
static int s2mpg13_spmic_thermal_set_emul_temp(void *data, int temp)
{
	struct s2mpg13_spmic_thermal_sensor *sensor = data;
	int ret;
	u8 value, mask = 0x1;

	if (sensor->chip->adc_chan_en & (mask << sensor->adc_chan)) {
		ret = s2mpg13_read_reg(sensor->chip->i2c, S2MPG13_METER_CTRL3, &value);
		if (ret)
			return ret;

		if (temp)
			value &= ~(mask << sensor->adc_chan);
		else
			value |= mask << sensor->adc_chan;

		ret = s2mpg13_write_reg(sensor->chip->i2c, S2MPG13_METER_CTRL3, value);
		if (ret)
			return ret;
	}
	sensor->emul_temperature = temp;
	return 0;
}

static void
s2mpg13_spmic_thermal_init(struct s2mpg13_spmic_thermal_chip *s2mpg13_spmic_thermal)
{
	int i;

	for (i = 0; i < GTHERM_CHAN_NUM; i++) {
		s2mpg13_spmic_thermal->sensor[i].chip = s2mpg13_spmic_thermal;
		s2mpg13_spmic_thermal->sensor[i].adc_chan = i;
	}
}

static struct thermal_zone_of_device_ops s2mpg13_spmic_thermal_ops = {
	.get_temp = s2mpg13_spmic_thermal_get_temp,
	.set_trips = s2mpg13_spmic_thermal_set_trips,
	.set_emul_temp = s2mpg13_spmic_thermal_set_emul_temp,
};

/*
 * Register thermal zones.
 */
static int
s2mpg13_spmic_thermal_register_tzd(struct s2mpg13_spmic_thermal_chip *s2mpg13_spmic_thermal)
{
	unsigned int i;
	struct thermal_zone_device *tzd;
	struct device *dev = s2mpg13_spmic_thermal->dev;
	u8 mask = 0x1;

	for (i = 0; i < GTHERM_CHAN_NUM; i++, mask <<= 1) {
		dev_info(dev, "Registering %d sensor\n", i);
		tzd = devm_thermal_zone_of_sensor_register(s2mpg13_spmic_thermal->dev, i,
							   &s2mpg13_spmic_thermal->sensor[i],
							   &s2mpg13_spmic_thermal_ops);

		if (IS_ERR(tzd)) {
			dev_err(dev,
				"Error registering thermal zone:%ld for channel:%d\n",
				PTR_ERR(tzd), i);
			continue;
		}
		s2mpg13_spmic_thermal->sensor[i].tzd = tzd;
		if (s2mpg13_spmic_thermal->adc_chan_en & mask)
			thermal_zone_device_enable(tzd);
		else
			thermal_zone_device_disable(tzd);
	}
	return 0;
}

static const struct of_device_id s2mpg13_spmic_thermal_match_table[] = {
	{ .compatible = "google,s2mpg13-spmic-thermal" },
	{}
};

static int s2mpg13_spmic_thermal_get_dt_data(struct platform_device *pdev,
					   struct s2mpg13_spmic_thermal_chip *s2mpg13_spmic_thermal)
{
	struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	const struct of_device_id *id;

	if (!node)
		return -EINVAL;

	id = of_match_node(s2mpg13_spmic_thermal_match_table, node);
	if (!id)
		return -EINVAL;

	if (of_property_read_u8(node, "adc_chan_en",
				&s2mpg13_spmic_thermal->adc_chan_en)) {
		dev_err(dev, "Cannot read adc_chan_en\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * Enable NTC thermistor engine.
 */
static int
s2mpg13_spmic_set_enable(struct s2mpg13_spmic_thermal_chip *s2mpg13_spmic_thermal,
		       bool on)
{
	int ret = 0;
	struct device *dev = s2mpg13_spmic_thermal->dev;

	if (on) {
		ret = s2mpg13_write_reg(s2mpg13_spmic_thermal->i2c,
					S2MPG13_METER_CTRL3,
					s2mpg13_spmic_thermal->adc_chan_en);

		if (ret) {
			dev_err(dev, "Cannot enable NTC engine\n");
		} else {
			dev_info(dev, "Enabled NTC channels: 0x%x\n",
				 s2mpg13_spmic_thermal->adc_chan_en);
		}
	} else {
		ret = s2mpg13_write_reg(s2mpg13_spmic_thermal->i2c,
					S2MPG13_METER_CTRL3, 0x00);
		if (ret)
			dev_err(dev, "Cannot disable NTC\n");
	}
	return ret;
}

static int s2mpg13_spmic_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s2mpg13_spmic_thermal_chip *chip;
	int ret = 0;
	struct s2mpg13_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct s2mpg13_platform_data *pdata;

	chip = devm_kzalloc(&pdev->dev, sizeof(struct s2mpg13_spmic_thermal_chip),
			    GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	if (!iodev) {
		dev_err(dev, "Failed to get parent s2mpg13_dev\n");
		return -EINVAL;
	}
	pdata = iodev->pdata;
	if (!pdata) {
		dev_err(dev, "Failed to get s2mpg13_platform_data\n");
		return -EINVAL;
	}

	chip->dev = dev;
	chip->i2c = iodev->meter;
	chip->iodev = iodev;
	ret = s2mpg13_spmic_thermal_get_dt_data(pdev, chip);
	if (ret) {
		dev_err(dev, "s2mpg13_spmic_thermal get dt data failed\n");
		return ret;
	}

	s2mpg13_spmic_thermal_init(chip);

	/* Set sampling rate */
	s2mpg13_update_reg(chip->i2c, S2MPG13_METER_CTRL1,
			   NTC_0P15625HZ << NTC_SAMP_RATE_SHIFT,
			   NTC_SAMP_RATE_MASK);

	ret = s2mpg13_spmic_set_enable(chip, true);
	if (ret) {
		dev_err(dev, "Failed to enable NTC engine\n");
		goto fail;
	}

	ret = s2mpg13_spmic_thermal_register_tzd(chip);
	if (ret) {
		dev_err(dev, "Failed to register with of thermal\n");
		goto disable_ntc;
	}

	platform_set_drvdata(pdev, chip);
	return ret;

disable_ntc:
	s2mpg13_spmic_set_enable(chip, false);
fail:
	return ret;
}

static int s2mpg13_spmic_thermal_remove(struct platform_device *pdev)
{
	struct s2mpg13_spmic_thermal_chip *chip = platform_get_drvdata(pdev);

	s2mpg13_spmic_set_enable(chip, false);

	return 0;
}

static struct platform_driver s2mpg13_spmic_thermal_driver = {
	.driver = {
		.name = "s2mpg13-spmic-thermal",
		.owner = THIS_MODULE,
	},
	.probe = s2mpg13_spmic_thermal_probe,
	.remove = s2mpg13_spmic_thermal_remove,
};
module_platform_driver(s2mpg13_spmic_thermal_driver);

MODULE_DESCRIPTION("Google LLC GS201 SPMIC Thermal Driver");
MODULE_AUTHOR("Sayanna Chandula <sayanna@google.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:google,s2mpg13_thermal");
