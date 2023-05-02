// SPDX-License-Identifier: GPL-2.0+
/*
 * VEML3235 Ambient Light Sensor
 *
 * Copyright (c) 2022, Wooseong Sim <wsshim@markt.co.kr>
 *
 * Datasheet: https://www.vishay.com/docs/80131/veml3235.pdf
 * Appnote: https://www.vishay.com/docs/80222/designingveml3235.pdf
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>

/* Device registers */
#define VEML3235_REG_ALS_CONF   0x00
#define VEML3235_REG_WH_DATA    0x04
#define VEML3235_REG_ALS_DATA   0x05
#define VEML3235_REG_ID			0x09
#define VEML3235_REG_MAX		VEML3235_REG_ID

/* Bit masks for specific functionality */
#define VEML3235_ALS_IT       GENMASK(6, 4)		/* ALS/W intergration time */
#define VEML3235_ALS_GAIN     GENMASK(12, 11)	/* ALS Gain */

#define VEML3235_ALS_SD       BIT(0)
#define VEML3235_ALS_SD0      BIT(15)

/*
 * The resolution depends on both gain and integration time. The
 * cur_resolution stores one of the resolution mentioned in the
 * table during startup and gets updated whenever integration time
 * or gain is changed.
 *
 * Table 'resolution and maximum detection range' in appnote 84367
 * is visualized as a 2D array. The cur_gain stores index of gain
 * in this table (0-3) while the cur_integration_time holds index
 * of integration time (0-5).
 */
struct veml3235_data {
	struct i2c_client *client;
	struct regmap *regmap;
	int cur_resolution;
	int cur_gain;
	int cur_integration_time;
};

/* Integration time available in seconds
   - 50ms, 100ms, 200ms, 400ms, 800ms
*/
static IIO_CONST_ATTR(in_illuminance_integration_time_available,
                "0.05 0.1 0.2 0.4 0.8");

/*
 * Gain available
 * - x1, x2, x4
 */
static IIO_CONST_ATTR(in_illuminance_scale_available,
                "1 2 4");

static struct attribute *veml3235_attributes[] = {
	&iio_const_attr_in_illuminance_integration_time_available.dev_attr.attr,
	&iio_const_attr_in_illuminance_scale_available.dev_attr.attr,
	NULL
};

static const struct attribute_group veml3235_attr_group = {
	.attrs = veml3235_attributes,
};


static int veml3235_als_pwr_on(struct veml3235_data *data)
{
	int ret = 0;
	ret = regmap_update_bits(data->regmap, VEML3235_REG_ALS_CONF,
                 VEML3235_ALS_SD, 0);
	if(ret != 0)
		goto err;

	usleep_range(5000, 6000);

	ret = regmap_update_bits(data->regmap, VEML3235_REG_ALS_CONF,
                 VEML3235_ALS_SD0, 0);
	if(ret != 0)
        goto err;

err:
	return ret;
}

static int veml3235_als_shut_down(struct veml3235_data *data)
{
	int ret = 0;

    ret = regmap_update_bits(data->regmap, VEML3235_REG_ALS_CONF,
                 VEML3235_ALS_SD0, 1);
    if(ret != 0)
        goto err;

    usleep_range(5000, 6000);

    ret = regmap_update_bits(data->regmap, VEML3235_REG_ALS_CONF,
                 VEML3235_ALS_SD, 1);
    if(ret != 0)
        goto err;

err:
    return ret;
}

static void veml3235_als_shut_down_action(void *data)
{
	veml3235_als_shut_down(data);
}

static const struct iio_event_spec veml3235_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_PERIOD) |
		BIT(IIO_EV_INFO_ENABLE),
	},
};

/* Channel number */
enum veml3235_chan {
	CH_ALS,
	CH_WHITE,
};

static const struct iio_chan_spec veml3235_channels[] = {
	{
		.type = IIO_LIGHT,
		.channel = CH_ALS,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PROCESSED) |
				BIT(IIO_CHAN_INFO_INT_TIME) |
				BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = veml3235_event_spec,
		.num_event_specs = ARRAY_SIZE(veml3235_event_spec),
	},
	{
		.type = IIO_INTENSITY,
		.channel = CH_WHITE,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_BOTH,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static const struct regmap_config veml3235_regmap_config = {
	.name = "veml3235_regmap",
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = VEML3235_REG_MAX,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int veml3235_get_intgrn_tm(struct iio_dev *indio_dev,
						int *val, int *val2)
{
	int ret, reg;
	struct veml3235_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, VEML3235_REG_ALS_CONF, &reg);
	if (ret) {
		dev_err(&data->client->dev,
				"can't read als conf register %d\n", ret);
		return ret;
	}

	pr_info("%s: reg: 0x%x / bit: 0x%02x\n", __func__, reg, ((reg >> 4) & 0x07) );

	switch ((reg >> 4) & 0x7) {
    case 0:
        *val2 = 50000;
        break;
    case 1:
        *val2 = 100000;
        break;
    case 2:
        *val2 = 200000;
        break;
    case 3:
        *val2 = 400000;
        break;
	case 4:
        *val2 = 800000;
        break;
    default:
        return -EINVAL;
    }
	*val = 0;
	return IIO_VAL_INT_PLUS_MICRO;
}

static int veml3235_set_intgrn_tm(struct iio_dev *indio_dev,
						int val, int val2)
{
	int ret, new_int_time, int_idx;
	struct veml3235_data *data = iio_priv(indio_dev);

	if (val)
		return -EINVAL;

	switch (val2) {
    case 50000:
        new_int_time = 0x00;
        int_idx = 4;
        break;
    case 100000:
        new_int_time = (0x01 << 4) & 0xff;
        int_idx = 3;
        break;
    case 200000:
        new_int_time = (0x02 << 4) & 0xff;
		int_idx = 2;
        break;
    case 400000:
        new_int_time = (0x03 << 4) & 0xff;
        int_idx = 1;
        break;
    case 800000:
		new_int_time = (0x04 << 4) & 0xff;
        int_idx = 0;
        break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(data->regmap, VEML3235_REG_ALS_CONF,
					VEML3235_ALS_IT, new_int_time);
	if (ret) {
		dev_err(&data->client->dev,
				"can't update als integration time %d\n", ret);
		return ret;
	}

	/*
	 * Cache current integration time and update resolution. For every
	 * increase in integration time to next level, resolution is halved
	 * and vice-versa.
	 */
	if (data->cur_integration_time < int_idx)
		data->cur_resolution <<= int_idx - data->cur_integration_time;
	else if (data->cur_integration_time > int_idx)
		data->cur_resolution >>= data->cur_integration_time - int_idx;

	data->cur_integration_time = int_idx;

	return ret;
}

static int veml3235_set_als_gain(struct iio_dev *indio_dev,
						int val, int val2)
{
	int ret, gain_idx;
	struct veml3235_data *data = iio_priv(indio_dev);
	unsigned int new_gain = 0;

	pr_info("%s: val: %d, val2: %d\n", __func__, val, val2);
	if(val == 1 && val2 == 0) {
		new_gain = 0x00;
		gain_idx = 2;
	} else if(val == 2 && val2 == 0) {
		new_gain = (0xff & (0x01 << 3));
		gain_idx = 1;
	} else if(val == 4 && val2 == 0) {
		new_gain = (0xff & (0x03 << 3));
		gain_idx = 0;
	} else {
		return -EINVAL;
	}

	ret = regmap_update_bits(data->regmap, VEML3235_REG_ALS_CONF,
					VEML3235_ALS_GAIN, new_gain);
	if (ret) {
		dev_err(&data->client->dev,
				"can't set als gain %d\n", ret);
		return ret;
	}

	/*
	 * Cache currently set gain & update resolution. For every
	 * increase in the gain to next level, resolution is halved
	 * and vice-versa.
	 */
	if (data->cur_gain < gain_idx)
		data->cur_resolution <<= gain_idx - data->cur_gain;
	else if (data->cur_gain > gain_idx)
		data->cur_resolution >>= data->cur_gain - gain_idx;

	data->cur_gain = gain_idx;

	return ret;
}

static int veml3235_get_als_gain(struct iio_dev *indio_dev,
						int *val, int *val2)
{
	int ret, reg;
	struct veml3235_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, VEML3235_REG_ALS_CONF, &reg);
	if (ret) {
		dev_err(&data->client->dev,
				"can't read als conf register %d\n", ret);
		return ret;
	}

	pr_info("%s: reg: 0x%x / bit: 0x%02x\n", __func__, reg, ((reg >> 11) & 0x03) );

	switch ((reg >> 11) & 0x03) {
	case 0:
        *val = 4;
        *val2 = 0;
        break;
    case 1:
        *val = 2;
        *val2 = 0;
        break;
    case 2:
        *val = 1;
        *val2 = 0;
        break;
	default:
		return -EINVAL;
	}

	return IIO_VAL_INT_PLUS_MICRO;
}

/*
 * Provide both raw as well as light reading in lux.
 * light (in lux) = resolution * raw reading
 */
static int veml3235_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	int ret, reg;
	struct veml3235_data *data = iio_priv(indio_dev);
	struct regmap *regmap = data->regmap;
	struct device *dev = &data->client->dev;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		pr_info("%s: mask : IIO_CHAN_INFO_RAW\n", __func__);
		switch (chan->type) {
		case IIO_LIGHT:
			ret = regmap_read(regmap, VEML3235_REG_ALS_DATA, &reg);
			if (ret < 0) {
				pr_info("can't read als data %d\n", ret);
				return ret;
			}
			pr_info("%s: type: IIO_LIGHT - raw: 0x%x\n", __func__, reg);
			if (mask == IIO_CHAN_INFO_PROCESSED) {
				*val = (reg * data->cur_resolution) / 10000;
				*val2 = (reg * data->cur_resolution) % 10000;
				return IIO_VAL_INT_PLUS_MICRO;
			}
			*val = reg;
			return IIO_VAL_INT;
		case IIO_INTENSITY:
			ret = regmap_read(regmap, VEML3235_REG_WH_DATA, &reg);
			if (ret < 0) {
				dev_err(dev, "can't read white data %d\n", ret);
				return ret;
			}
			pr_info("%s: type: IIO_INTENSITY - raw: 0x%x\n", __func__, reg);
			if (mask == IIO_CHAN_INFO_PROCESSED) {
				*val = (reg * data->cur_resolution) / 10000;
				*val2 = (reg * data->cur_resolution) % 10000;
				return IIO_VAL_INT_PLUS_MICRO;
			}
			*val = reg;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_INT_TIME:
		pr_info("%s: mask : IIO_CHAN_INFO_INT_TIME\n", __func__);
		if (chan->type == IIO_LIGHT) {
			dev_err(dev, "%s: type: IIO_LIGHT\n", __func__); 
			return veml3235_get_intgrn_tm(indio_dev, val, val2);
		}
		return -EINVAL;
	case IIO_CHAN_INFO_SCALE:
		pr_info("%s: mask : IIO_CHAN_INFO_SCALE\n", __func__);
		if (chan->type == IIO_LIGHT) {
			dev_err(dev, "%s: type: IIO_LIGHT\n", __func__); 
			return veml3235_get_als_gain(indio_dev, val, val2);
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static int veml3235_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int val, int val2, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		pr_info("%s: mask : IIO_CHAN_INFO_INT_TIME\n", __func__);
		switch (chan->type) {
		case IIO_LIGHT: {
			pr_info("%s: type: IIO_LIGHT\n", __func__); 
			return veml3235_set_intgrn_tm(indio_dev, val, val2);
		}
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		pr_info("%s: mask : IIO_CHAN_INFO_SCALE\n", __func__);
		switch (chan->type) {
		case IIO_LIGHT: {
			pr_info("%s: type: IIO_LIGHT\n", __func__); 
			return veml3235_set_als_gain(indio_dev, val, val2);
		}
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

	
static const struct iio_info veml3235_info_no_irq = {
	.read_raw  = veml3235_read_raw,
	.write_raw = veml3235_write_raw,
	.attrs = &veml3235_attr_group,
};


static int veml3235_hw_init(struct iio_dev *indio_dev)
{
	int ret;
	struct veml3235_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;

	ret = veml3235_als_shut_down(data);
	if (ret) {
		dev_err(&client->dev, "can't shutdown als %d\n", ret);
		return ret;
	}
	
	// default setting (intergration time: 800ms, gain: x2)
	ret = regmap_write(data->regmap, VEML3235_REG_ALS_CONF, 0x8441); 
	if (ret) {
		dev_err(&client->dev, "can't setup als configs %d\n", ret);
		return ret;
	}

	ret = veml3235_als_pwr_on(data);
	if (ret) {
		dev_err(&client->dev, "can't poweron als %d\n", ret);
		return ret;
	}

	/* Wait 4 ms to let processor & oscillator start correctly */
	usleep_range(4000, 4002);

	/* Cache currently active measurement parameters */
	data->cur_gain = 1;
	data->cur_resolution = 4608;
	data->cur_integration_time = 0;

	return ret;
}

static int veml3235_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	int ret;
	unsigned int value = 0;
	struct veml3235_data *data;
	struct iio_dev *indio_dev;
	struct regmap *regmap;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "i2c adapter doesn't support plain i2c\n");
		return -EOPNOTSUPP;
	}

	regmap = devm_regmap_init_i2c(client, &veml3235_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "can't setup regmap\n");
		return PTR_ERR(regmap);
	}

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	data->regmap = regmap;

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = "veml3235";
	indio_dev->channels = veml3235_channels;
	indio_dev->num_channels = ARRAY_SIZE(veml3235_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	if(!regmap_read(data->regmap, VEML3235_REG_ID, &value)) {
		dev_err(&client->dev, "%s: chip id: 0x%04x\n", __func__, value);
	} else {
		dev_err(&client->dev, "%s: could not read chip id\n", __func__);
	}
	

#if 0
	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, veml6030_event_handler,
						IRQF_TRIGGER_LOW | IRQF_ONESHOT,
						"veml6030", indio_dev);
		if (ret < 0) {
			dev_err(&client->dev,
					"irq %d request failed\n", client->irq);
			return ret;
		}
		indio_dev->info = &veml6030_info;
	} else {
		indio_dev->info = &veml6030_info_no_irq;
	}
#else
	indio_dev->info = &veml3235_info_no_irq;
#endif

	ret = veml3235_hw_init(indio_dev);
	if (ret < 0)
		return ret;

	ret = devm_add_action_or_reset(&client->dev,
					veml3235_als_shut_down_action, data);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int __maybe_unused veml3235_runtime_suspend(struct device *dev)
{
	int ret;
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct veml3235_data *data = iio_priv(indio_dev);

	ret = veml3235_als_shut_down(data);
	if (ret < 0)
		dev_err(&data->client->dev, "can't suspend als %d\n", ret);

	return ret;
}

static int __maybe_unused veml3235_runtime_resume(struct device *dev)
{
	int ret;
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct veml3235_data *data = iio_priv(indio_dev);

	ret = veml3235_als_pwr_on(data);
	if (ret < 0)
		dev_err(&data->client->dev, "can't resume als %d\n", ret);

	return ret;
}

static const struct dev_pm_ops veml3235_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(veml3235_runtime_suspend,
				veml3235_runtime_resume, NULL)
};

static const struct of_device_id veml3235_of_match[] = {
	{ .compatible = "vishay,veml3235" },
	{ }
};
MODULE_DEVICE_TABLE(of, veml3235_of_match);

static const struct i2c_device_id veml3235_id[] = {
	{ "veml3235", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, veml3235_id);

static struct i2c_driver veml3235_driver = {
	.driver = {
		.name = "veml3235",
		.of_match_table = veml3235_of_match,
		.pm = &veml3235_pm_ops,
	},
	.probe = veml3235_probe,
	.id_table = veml3235_id,
};
module_i2c_driver(veml3235_driver);

MODULE_AUTHOR("Wooseong sim <wsshim@markt.co.kr>");
MODULE_DESCRIPTION("VEML3235 Ambient Light Sensor");
MODULE_LICENSE("GPL v2");
