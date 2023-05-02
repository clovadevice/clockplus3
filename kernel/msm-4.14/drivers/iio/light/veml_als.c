// SPDX-License-Identifier: GPL-2.0+
/*
 * VEML6030 Ambient Light Sensor
 *
 * Copyright (c) 2019, Rishi Gupta <gupt21@gmail.com>
 *
 * Datasheet: https://www.vishay.com/docs/84366/veml6030.pdf
 * Appnote-84367: https://www.vishay.com/docs/84367/designingveml6030.pdf
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
#define VEML3235_REG_ID         0x09

#define VEML_REG_ALS_CONF		0x00

#define VEML_BIT_ALS_GAIN     GENMASK(12, 11)

#define VEML6030_REG_ALS_CONF   0x00
#define VEML6030_REG_ALS_WH     0x01
#define VEML6030_REG_ALS_WL     0x02
#define VEML6030_REG_ALS_PSM    0x03
#define VEML6030_REG_ALS_DATA   0x04
#define VEML6030_REG_WH_DATA    0x05
#define VEML6030_REG_ALS_INT    0x06

#define VEML_REG_MAX			0x09

/* Bit masks for specific functionality */
#define VEML3235_ALS_IT       	GENMASK(6, 4)     /* ALS/W intergration time */
#define VEML3235_ALS_GAIN     	GENMASK(12, 11)   /* ALS Gain */
#define VEML3235_ALS_SD       	BIT(0)
#define VEML3235_ALS_SD0      	BIT(15)
#define VEML3235_ALS_DG			BIT(13)
#define VEML3235_CHIP_ID        0x35

#define VEML6030_ALS_IT       	GENMASK(9, 6)
#define VEML6030_PSM          	GENMASK(2, 1)
#define VEML6030_ALS_PERS     	GENMASK(5, 4)
#define VEML6030_ALS_GAIN     	GENMASK(12, 11)
#define VEML6030_PSM_EN       	BIT(0)
#define VEML6030_INT_TH_LOW   	BIT(15)
#define VEML6030_INT_TH_HIGH  	BIT(14)
#define VEML6030_ALS_INT_EN   	BIT(1)
#define VEML6030_ALS_SD       	BIT(0)


//#define USE_ALS_IRQ

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
struct veml_als_priv {
	struct i2c_client *client;
	struct regmap *regmap;
	int cur_resolution;
	int cur_gain;
	int cur_integration_time;
	unsigned char chip_id;
};


/* Integration time available in seconds */
static IIO_CONST_ATTR(in_illuminance_integration_time_available,
				"0.025 0.05 0.1 0.2 0.4 0.8");

static IIO_CONST_ATTR(in_illuminance_scale_available,
				"0.125 0.25 1.0 2.0 4.0");

static struct attribute *veml_als_attributes[] = {
	&iio_const_attr_in_illuminance_integration_time_available.dev_attr.attr,
	&iio_const_attr_in_illuminance_scale_available.dev_attr.attr,
	NULL
};

static const struct attribute_group veml_als_attr_group = {
	.attrs = veml_als_attributes,
};

/*
 * Persistence = 1/2/4/8 x integration time
 * Minimum time for which light readings must stay above configured
 * threshold to assert the interrupt.
 */
#ifdef USE_ALS_IRQ
static const char * const period_values[] = {
		"0.1 0.2 0.4 0.8",
		"0.2 0.4 0.8 1.6",
		"0.4 0.8 1.6 3.2",
		"0.8 1.6 3.2 6.4",
		"0.05 0.1 0.2 0.4",
		"0.025 0.050 0.1 0.2"
};

/*
 * Return list of valid period values in seconds corresponding to
 * the currently active integration time.
 */
static ssize_t in_illuminance_period_available_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret, reg, x;
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct veml6030_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, VEML6030_REG_ALS_CONF, &reg);
	if (ret) {
		dev_err(&data->client->dev,
				"can't read als conf register %d\n", ret);
		return ret;
	}

	ret = ((reg >> 6) & 0xF);
	switch (ret) {
	case 0:
	case 1:
	case 2:
	case 3:
		x = ret;
		break;
	case 8:
		x = 4;
		break;
	case 12:
		x = 5;
		break;
	default:
		return -EINVAL;
	}

	return snprintf(buf, PAGE_SIZE, "%s\n", period_values[x]);
}

static IIO_DEVICE_ATTR_RO(in_illuminance_period_available, 0);

static struct attribute *veml6030_event_attributes[] = {
	&iio_dev_attr_in_illuminance_period_available.dev_attr.attr,
	NULL
};

static const struct attribute_group veml6030_event_attr_group = {
	.attrs = veml6030_event_attributes,
};
#endif


static int veml_als_pwr_on(struct veml_als_priv *priv)
{
	int ret = 0;
	
	if(priv->chip_id == VEML3235_CHIP_ID) {
		dev_info(&priv->client->dev, "veml3235 power on\n", __func__);
		ret = regmap_update_bits(priv->regmap, VEML3235_REG_ALS_CONF,
                 VEML3235_ALS_SD, 0);
	    if(ret != 0)
    	    goto err;

		usleep_range(5000, 6000);

		ret = regmap_update_bits(priv->regmap, VEML3235_REG_ALS_CONF,
                 VEML3235_ALS_SD0, 0);

		if(ret != 0)
	        goto err;
	}
	else {
		dev_info(&priv->client->dev, "veml6030 power on\n", __func__);
		ret = regmap_update_bits(priv->regmap, VEML6030_REG_ALS_CONF,
                 VEML6030_ALS_SD, 0);
	}

err:
	return ret;
}


static int veml_als_shut_down(struct veml_als_priv *priv)
{
	int ret = 0;

	if(priv->chip_id == VEML3235_CHIP_ID) {
		dev_info(&priv->client->dev, "veml3235 shut down\n", __func__);
		ret = regmap_update_bits(priv->regmap, VEML_REG_ALS_CONF,
                 VEML3235_ALS_SD0, VEML3235_ALS_SD0);
		if(ret != 0)
	        goto err;

		usleep_range(5000, 6000);

		ret = regmap_update_bits(priv->regmap, VEML_REG_ALS_CONF,
                 VEML3235_ALS_SD, VEML3235_ALS_SD);
		if(ret != 0)
            goto err;
	}
	else {
		dev_info(&priv->client->dev, "veml6030 shut down\n", __func__);
		ret = regmap_update_bits(priv->regmap, VEML_REG_ALS_CONF,
                 VEML6030_ALS_SD, VEML6030_ALS_SD);
	}

err:
	return ret;
}


static void veml_als_shut_down_action(void *data)
{
	veml_als_shut_down(data);
}


static const struct iio_event_spec veml6030_event_spec[] = {
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
enum veml6030_chan {
	CH_ALS,
	CH_WHITE,
};

static const struct iio_chan_spec veml6030_channels[] = {
	{
		.type = IIO_LIGHT,
		.channel = CH_ALS,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PROCESSED) |
				BIT(IIO_CHAN_INFO_INT_TIME) |
				BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = veml6030_event_spec,
		.num_event_specs = ARRAY_SIZE(veml6030_event_spec),
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

static const struct regmap_config veml_als_regmap_config = {
	.name = "veml_als_regmap",
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = VEML_REG_MAX,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};


static int veml_als_get_intgrn_tm(struct iio_dev *indio_dev,
						int *val, int *val2)
{
	int ret;
	unsigned int value;
	struct veml_als_priv *priv = iio_priv(indio_dev);

	ret = regmap_read(priv->regmap, VEML_REG_ALS_CONF, &value);
	if (ret) {
		dev_err(&priv->client->dev,
				"can't read als conf register %d\n", ret);
		return ret;
	}

	if(priv->chip_id == VEML3235_CHIP_ID) {
		switch ((value >> 4) & 0x7) {
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
	} else {
		switch ((value >> 6) & 0xF) {
		case 0:
			*val2 = 100000;
			break;
		case 1:
			*val2 = 200000;
			break;
		case 2:
			*val2 = 400000;
			break;
		case 3:
			*val2 = 800000;
			break;
		case 8:
			*val2 = 50000;
			break;
		case 12:
			*val2 = 25000;
			break;
		default:
			return -EINVAL;
		}
		*val = 0;
	}

	return IIO_VAL_INT_PLUS_MICRO;
}


static int veml_als_set_intgrn_tm(struct veml_als_priv *priv,
						int val, int val2)
{
	int ret, new_int_time, int_idx;
	unsigned int mask;

	if (val)
		return -EINVAL;

	if(priv->chip_id == VEML3235_CHIP_ID) {
		mask = VEML3235_ALS_IT;
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
	} else {
		mask = VEML6030_ALS_IT;
		switch (val2) {
		case 25000:
			new_int_time = 0x300;
			int_idx = 5;
			break;
		case 50000:
			new_int_time = 0x200;
			int_idx = 4;
			break;
		case 100000:
			new_int_time = 0x00;
			int_idx = 3;
			break;
		case 200000:
			new_int_time = 0x40;
			int_idx = 2;
			break;
		case 400000:
			new_int_time = 0x80;
			int_idx = 1;
			break;
		case 800000:
			new_int_time = 0xC0;
			int_idx = 0;
			break;
		default:
			return -EINVAL;
		}
	}

	ret = regmap_update_bits(priv->regmap, VEML_REG_ALS_CONF,
					mask, new_int_time);
	if (ret) {
		dev_err(&priv->client->dev,
				"can't update als integration time %d\n", ret);
		return ret;
	}

	/*
	 * Cache current integration time and update resolution. For every
	 * increase in integration time to next level, resolution is halved
	 * and vice-versa.
	 */
	if (priv->cur_integration_time < int_idx)
		priv->cur_resolution <<= int_idx - priv->cur_integration_time;
	else if (priv->cur_integration_time > int_idx)
		priv->cur_resolution >>= priv->cur_integration_time - int_idx;

	priv->cur_integration_time = int_idx;

	return ret;
}

#ifdef USE_ALS_IRQ
static int veml6030_read_persistence(struct iio_dev *indio_dev,
						int *val, int *val2)
{
	int ret, reg, period, x, y;
	struct veml6030_data *data = iio_priv(indio_dev);

	ret = veml6030_get_intgrn_tm(indio_dev, &x, &y);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, VEML6030_REG_ALS_CONF, &reg);
	if (ret) {
		dev_err(&data->client->dev,
				"can't read als conf register %d\n", ret);
	}

	/* integration time multiplied by 1/2/4/8 */
	period = y * (1 << ((reg >> 4) & 0x03));

	*val = period / 1000000;
	*val2 = period % 1000000;

	return IIO_VAL_INT_PLUS_MICRO;
}

static int veml6030_write_persistence(struct iio_dev *indio_dev,
						int val, int val2)
{
	int ret, period, x, y;
	struct veml6030_data *data = iio_priv(indio_dev);

	ret = veml6030_get_intgrn_tm(indio_dev, &x, &y);
	if (ret < 0)
		return ret;

	if (!val) {
		period = val2 / y;
	} else {
		if ((val == 1) && (val2 == 600000))
			period = 1600000 / y;
		else if ((val == 3) && (val2 == 200000))
			period = 3200000 / y;
		else if ((val == 6) && (val2 == 400000))
			period = 6400000 / y;
		else
			period = -1;
	}

	if (period <= 0 || period > 8 || hweight8(period) != 1)
		return -EINVAL;

	ret = regmap_update_bits(data->regmap, VEML6030_REG_ALS_CONF,
				VEML6030_ALS_PERS, (ffs(period) - 1) << 4);
	if (ret)
		dev_err(&data->client->dev,
				"can't set persistence value %d\n", ret);

	return ret;
}
#endif


static int veml_als_set_gain(struct veml_als_priv *priv,
						int val, int val2)
{
	int ret, gain_idx;
	unsigned int new_gain = 0;

	if(priv->chip_id == VEML3235_CHIP_ID) {
		if(val == 1 && val2 == 0) {
     	   new_gain = 0x00;
     	   gain_idx = 2;
    	} else if(val == 2 && val2 == 0) {
        	new_gain = (0xff00 & (0x01 << 11));
        	gain_idx = 1;
    	} else if(val == 4 && val2 == 0) {
       		new_gain = (0xff00 & (0x03 << 11));
        	gain_idx = 0;
    	} else {
        	return -EINVAL;
    	}
	} else {
		if (val == 0 && val2 == 125000) {
        	new_gain = 0x1000; /* 0x02 << 11 */
	        gain_idx = 3;
    	} else if (val == 0 && val2 == 250000) {
        	new_gain = 0x1800;
	        gain_idx = 2;
    	} else if (val == 1 && val2 == 0) {
        	new_gain = 0x00;
        	gain_idx = 1;
	    } else if (val == 2 && val2 == 0) {
    	    new_gain = 0x800;
    	    gain_idx = 0;
    	} else {
    	    return -EINVAL;
    	}
	}

	ret = regmap_update_bits(priv->regmap, VEML_REG_ALS_CONF,
					VEML_BIT_ALS_GAIN, new_gain);
	if (ret) {
		dev_err(&priv->client->dev,
				"can't set als gain %d\n", ret);
		return ret;
	}

	/*
	 * Cache currently set gain & update resolution. For every
	 * increase in the gain to next level, resolution is halved
	 * and vice-versa.
	 */
	if (priv->cur_gain < gain_idx)
		priv->cur_resolution <<= gain_idx - priv->cur_gain;
	else if (priv->cur_gain > gain_idx)
		priv->cur_resolution >>= priv->cur_gain - gain_idx;

	priv->cur_gain = gain_idx;

	return ret;
}

static int veml_als_get_gain(struct iio_dev *indio_dev,
						int *val, int *val2)
{
	int ret;
	struct veml_als_priv *priv = iio_priv(indio_dev);
	unsigned int value;


	ret = regmap_read(priv->regmap, VEML_REG_ALS_CONF, &value);
	if (ret) {
		dev_err(&priv->client->dev,
				"can't read als conf register %d\n", ret);
		return ret;
	}

	if(priv->chip_id == VEML3235_CHIP_ID) {
		switch ((value >> 11) & 0x03) {
		case 0:
			*val = 1;      
	        *val2 = 0;  
			break;
		case 1:
			*val = 2;      
	        *val2 = 0;
			break;
		case 3:
			*val = 4;      
	        *val2 = 0;  
			break;
		default:
			return -EINVAL;
		}
	} else {
		switch ((value >> 11) & 0x03) {
		case 0:
			*val = 1;
			*val2 = 0;
			break;
		case 1:
			*val = 2;
			*val2 = 0;
			break;
		case 2:
			*val = 0;
			*val2 = 125000;
			break;
		case 3:
			*val = 0;
			*val2 = 250000;
			break;
		default:
			return -EINVAL;
		}
	}
	
	return IIO_VAL_INT_PLUS_MICRO;
}

#ifdef USE_ALS_IRQ
static int veml6030_read_thresh(struct iio_dev *indio_dev,
						int *val, int *val2, int dir)
{
	int ret, reg;
	struct veml6030_data *data = iio_priv(indio_dev);

	if (dir == IIO_EV_DIR_RISING)
		ret = regmap_read(data->regmap, VEML6030_REG_ALS_WH, &reg);
	else
		ret = regmap_read(data->regmap, VEML6030_REG_ALS_WL, &reg);
	if (ret) {
		dev_err(&data->client->dev,
				"can't read als threshold value %d\n", ret);
		return ret;
	}

	*val = reg & 0xffff;
	return IIO_VAL_INT;
}

static int veml6030_write_thresh(struct iio_dev *indio_dev,
						int val, int val2, int dir)
{
	int ret;
	struct veml6030_data *data = iio_priv(indio_dev);

	if (val > 0xFFFF || val < 0 || val2)
		return -EINVAL;

	if (dir == IIO_EV_DIR_RISING) {
		ret = regmap_write(data->regmap, VEML6030_REG_ALS_WH, val);
		if (ret)
			dev_err(&data->client->dev,
					"can't set high threshold %d\n", ret);
	} else {
		ret = regmap_write(data->regmap, VEML6030_REG_ALS_WL, val);
		if (ret)
			dev_err(&data->client->dev,
					"can't set low threshold %d\n", ret);
	}

	return ret;
}
#endif

/*
 * Provide both raw as well as light reading in lux.
 * light (in lux) = resolution * raw reading
 */
static int veml_als_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	int ret;
	struct veml_als_priv *priv = iio_priv(indio_dev);
	struct regmap *regmap = priv->regmap;
	struct device *dev = &priv->client->dev;

	unsigned int reg_als, reg_wh; 
	unsigned int value = 0;

	if(priv->chip_id == VEML3235_REG_ID) {
		reg_als = VEML3235_REG_ALS_DATA;
		reg_wh = VEML3235_REG_WH_DATA;
	} else {
		reg_als = VEML6030_REG_ALS_DATA;
		reg_wh = VEML6030_REG_WH_DATA;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		switch (chan->type) {
		case IIO_LIGHT:
			ret = regmap_read(regmap, reg_als, &value);
			if (ret < 0) {
				dev_err(dev, "can't read als data %d\n", ret);
				return ret;
			}
			if (mask == IIO_CHAN_INFO_PROCESSED) {
				*val = (value * priv->cur_resolution) / 10000;
				*val2 = (value * priv->cur_resolution) % 10000;
				return IIO_VAL_INT_PLUS_MICRO;
			}
			*val = value;
			return IIO_VAL_INT;
			
		case IIO_INTENSITY:
			ret = regmap_read(regmap, reg_wh, &value);
			if (ret < 0) {
				dev_err(dev, "can't read white data %d\n", ret);
				return ret;
			}
			if (mask == IIO_CHAN_INFO_PROCESSED) {
				*val = (value * priv->cur_resolution) / 10000;
				*val2 = (value * priv->cur_resolution) % 10000;
				return IIO_VAL_INT_PLUS_MICRO;
			}
			*val = value;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_INT_TIME:
		if (chan->type == IIO_LIGHT)
			return veml_als_get_intgrn_tm(indio_dev, val, val2);
		return -EINVAL;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_LIGHT)
			return veml_als_get_gain(indio_dev, val, val2);
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static int veml_als_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int val, int val2, long mask)
{
	struct veml_als_priv *priv = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		switch (chan->type) {
		case IIO_LIGHT:
			return veml_als_set_intgrn_tm(priv, val, val2);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_LIGHT:
			return veml_als_set_gain(priv, val, val2);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}



#ifdef USE_ALS_IRQ
static int veml6030_read_event_val(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan, enum iio_event_type type,
		enum iio_event_direction dir, enum iio_event_info info,
		int *val, int *val2)
{
	switch (info) {
	case IIO_EV_INFO_VALUE:
		switch (dir) {
		case IIO_EV_DIR_RISING:
		case IIO_EV_DIR_FALLING:
			return veml6030_read_thresh(indio_dev, val, val2, dir);
		default:
			return -EINVAL;
		}
		break;
	case IIO_EV_INFO_PERIOD:
		return veml6030_read_persistence(indio_dev, val, val2);
	default:
		return -EINVAL;
	}
}
#endif

#ifdef USE_ALS_IRQ
static int veml6030_write_event_val(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan, enum iio_event_type type,
		enum iio_event_direction dir, enum iio_event_info info,
		int val, int val2)
{
	switch (info) {
	case IIO_EV_INFO_VALUE:
		return veml6030_write_thresh(indio_dev, val, val2, dir);
	case IIO_EV_INFO_PERIOD:
		return veml6030_write_persistence(indio_dev, val, val2);
	default:
		return -EINVAL;
	}
}
#endif

#ifdef USE_ALS_IRQ
static int veml6030_read_interrupt_config(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan, enum iio_event_type type,
		enum iio_event_direction dir)
{
	int ret, reg;
	struct veml6030_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, VEML6030_REG_ALS_CONF, &reg);
	if (ret) {
		dev_err(&data->client->dev,
				"can't read als conf register %d\n", ret);
		return ret;
	}

	if (reg & VEML6030_ALS_INT_EN)
		return 1;
	else
		return 0;
}
#endif

/*
 * Sensor should not be measuring light when interrupt is configured.
 * Therefore correct sequence to configure interrupt functionality is:
 * shut down -> enable/disable interrupt -> power on
 *
 * state = 1 enables interrupt, state = 0 disables interrupt
 */
#ifdef USE_ALS_IRQ
static int veml6030_write_interrupt_config(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan, enum iio_event_type type,
		enum iio_event_direction dir, int state)
{
	int ret;
	struct veml6030_data *data = iio_priv(indio_dev);

	if (state < 0 || state > 1)
		return -EINVAL;

	ret = veml6030_als_shut_down(data);
	if (ret < 0) {
		dev_err(&data->client->dev,
			"can't disable als to configure interrupt %d\n", ret);
		return ret;
	}

	/* enable interrupt + power on */
	ret = regmap_update_bits(data->regmap, VEML6030_REG_ALS_CONF,
			VEML6030_ALS_INT_EN | VEML6030_ALS_SD, state << 1);
	if (ret)
		dev_err(&data->client->dev,
			"can't enable interrupt & poweron als %d\n", ret);

	return ret;
}
#endif

#ifdef USE_ALS_IRQ
static const struct iio_info veml6030_info = {
	.read_raw  = veml6030_read_raw,
	.write_raw = veml6030_write_raw,
	.read_event_value = veml6030_read_event_val,
	.write_event_value	= veml6030_write_event_val,
	.read_event_config = veml6030_read_interrupt_config,
	.write_event_config	= veml6030_write_interrupt_config,
	.attrs = &veml6030_attr_group,
	.event_attrs = &veml6030_event_attr_group,
};
#endif

#if 0
static const struct iio_info veml3235_info_no_irq = {
    .read_raw  = veml3235_read_raw,
    .write_raw = veml3235_write_raw,
    .attrs = &veml_als_attr_group,
};
#endif

static const struct iio_info veml_als_info_no_irq = {
	.read_raw  = veml_als_read_raw,
	.write_raw = veml_als_write_raw,
	.attrs = &veml_als_attr_group,
};


#ifdef USE_ALS_IRQ
static irqreturn_t veml6030_event_handler(int irq, void *private)
{
	int ret, reg, evtdir;
	struct iio_dev *indio_dev = private;
	struct veml6030_data *data = iio_priv(indio_dev);

	ret = regmap_read(data->regmap, VEML6030_REG_ALS_INT, &reg);
	if (ret) {
		dev_err(&data->client->dev,
				"can't read als interrupt register %d\n", ret);
		return IRQ_HANDLED;
	}

	/* Spurious interrupt handling */
	if (!(reg & (VEML6030_INT_TH_HIGH | VEML6030_INT_TH_LOW)))
		return IRQ_NONE;

	if (reg & VEML6030_INT_TH_HIGH)
		evtdir = IIO_EV_DIR_RISING;
	else
		evtdir = IIO_EV_DIR_FALLING;

	iio_push_event(indio_dev, IIO_UNMOD_EVENT_CODE(IIO_INTENSITY,
					0, IIO_EV_TYPE_THRESH, evtdir),
					iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}
#endif


static int _veml3235_hw_init(struct veml_als_priv *priv,
        struct i2c_client *client)
{
	int ret = 0;
	unsigned int value = 0;

	dev_info(&client->dev, "initialize veml3235\n", __func__);

	ret = veml_als_shut_down(priv);
    if (ret) {
        dev_err(&client->dev, "can't shutdown als %d\n", ret);
        goto err;
    }

	// default setting (intergration time: 800ms, gain: x1)
	ret = veml_als_set_gain(priv, 1, 0);
	if(ret < 0) {
		dev_err(&client->dev, "can't setup als gain %d\n", ret);
		goto err;
	}

	ret = veml_als_set_intgrn_tm(priv, 0, 800000);
	if(ret < 0) {
        dev_err(&client->dev, "can't setup als intergation time %d\n", ret);
        goto err;
    }

	// DG: 0(x1), 1(x2)
	ret = regmap_update_bits(priv->regmap, VEML_REG_ALS_CONF,
                 VEML3235_ALS_DG, /*VEML3235_ALS_DG*/0);
	if(ret < 0) {
        dev_err(&client->dev, "can't setup als dg %d\n", ret);
        goto err;
    }

	ret = veml_als_pwr_on(priv);
    if (ret) {
        dev_err(&client->dev, "can't poweron als %d\n", ret);
       	goto err;
    }

	/* Wait 4 ms to let processor & oscillator start correctly */
    usleep_range(4000, 4002);

	/* Cache currently active measurement parameters */
    priv->cur_gain = 1;
    priv->cur_resolution = 4608;
    priv->cur_integration_time = 0;

err:
	return ret;
}


/*
 * Set ALS gain to 1/8, integration time to 100 ms, PSM to mode 2,
 * persistence to 1 x integration time and the threshold
 * interrupt disabled by default. First shutdown the sensor,
 * update registers and then power on the sensor.
 */
static int _veml6030_hw_init(struct veml_als_priv *priv,
        struct i2c_client *client)
{
	int ret, val;

	dev_info(&client->dev, "initialize veml6030\n", __func__);

	ret = veml_als_shut_down(priv);
	if (ret) {
		dev_err(&client->dev, "can't shutdown als %d\n", ret);
		goto err;
	}

	ret = regmap_write(priv->regmap, VEML6030_REG_ALS_CONF, 0x1001);
	if (ret) {
		dev_err(&client->dev, "can't setup als configs %d\n", ret);
		goto err;
	}

	ret = regmap_update_bits(priv->regmap, VEML6030_REG_ALS_PSM,
				 VEML6030_PSM | VEML6030_PSM_EN, 0x03);
	if (ret) {
		dev_err(&client->dev, "can't setup default PSM %d\n", ret);
		goto err;
	}

	ret = regmap_write(priv->regmap, VEML6030_REG_ALS_WH, 0xFFFF);
	if (ret) {
		dev_err(&client->dev, "can't setup high threshold %d\n", ret);
		goto err;
	}

	ret = regmap_write(priv->regmap, VEML6030_REG_ALS_WL, 0x0000);
	if (ret) {
		dev_err(&client->dev, "can't setup low threshold %d\n", ret);
		goto err;
	}

	ret = veml_als_pwr_on(priv);
	if (ret) {
		dev_err(&client->dev, "can't poweron als %d\n", ret);
		goto err;
	}

	/* Wait 4 ms to let processor & oscillator start correctly */
	usleep_range(4000, 4002);

	/* Clear stale interrupt status bits if any during start */
	ret = regmap_read(priv->regmap, VEML6030_REG_ALS_INT, &val);
	if (ret < 0) {
		dev_err(&client->dev,
			"can't clear als interrupt status %d\n", ret);
		goto err;
	}

	/* Cache currently active measurement parameters */
	priv->cur_gain = 3;
	priv->cur_resolution = 4608;
	priv->cur_integration_time = 3;

err:
	return ret;
}

static int veml_als_hw_init(struct iio_dev *indio_dev)
{
    struct veml_als_priv *priv = iio_priv(indio_dev);
    struct i2c_client *client = priv->client;

    if(priv->chip_id == VEML3235_CHIP_ID)
        return _veml3235_hw_init(priv, client);
    else
        return _veml6030_hw_init(priv, client);
}


static int veml_als_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	int ret;
	unsigned int value;
	struct veml_als_priv *priv;
	struct iio_dev *indio_dev;
	struct regmap *regmap;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "i2c adapter doesn't support plain i2c\n");
		return -EOPNOTSUPP;
	}

	regmap = devm_regmap_init_i2c(client, &veml_als_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "can't setup regmap\n");
		return PTR_ERR(regmap);
	}

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	priv->client = client;
	priv->regmap = regmap;

	indio_dev->dev.parent = &client->dev;
	/*
	indio_dev->name = "veml-als";
	*/
	indio_dev->channels = veml6030_channels;
	indio_dev->num_channels = ARRAY_SIZE(veml6030_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	if(regmap_read(priv->regmap, VEML3235_REG_ID, &value)) {
        dev_err(&client->dev, "%s: could not read chip id\n", __func__);
		dev_err(&client->dev, "%s: default setting veml6030\n", __func__);
		priv->chip_id = 0;
		indio_dev->name = "veml6030";
	}
    else {
        priv->chip_id = (0xff & value);
        dev_err(&client->dev, "%s: chip id: 0x%02x\n", __func__, priv->chip_id);
        if(priv->chip_id == VEML3235_CHIP_ID) {
            dev_err(&client->dev, "%s: chip is veml3235\n", __func__);
			indio_dev->name = "veml3235";
		} else {
            dev_err(&client->dev, "%s: chip is veml6030\n", __func__);
			indio_dev->name = "veml6030";
		}
    }

#ifdef USE_ALS_IRQ
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
	indio_dev->info = &veml_als_info_no_irq;
#endif

	ret = veml_als_hw_init(indio_dev);
	if(ret < 0)
		return ret;
	ret = devm_add_action_or_reset(&client->dev,
                    veml_als_shut_down_action, priv);
    if (ret < 0)
    	return ret;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int __maybe_unused veml_als_runtime_suspend(struct device *dev)
{
	int ret;
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct veml_als_priv *priv = iio_priv(indio_dev);

	ret = veml_als_shut_down(priv);
	if (ret < 0)
		dev_err(&priv->client->dev, "can't suspend als %d\n", ret);

	return ret;
}

static int __maybe_unused veml_als_runtime_resume(struct device *dev)
{
	int ret;
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct veml_als_priv *priv = iio_priv(indio_dev);

	ret = veml_als_pwr_on(priv);
	if (ret < 0)
		dev_err(&priv->client->dev, "can't resume als %d\n", ret);

	return ret;
}

static const struct dev_pm_ops veml_als_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(veml_als_runtime_suspend,
				veml_als_runtime_resume, NULL)
};

static const struct of_device_id veml_als_of_match[] = {
	{ .compatible = "vishay,veml-als" },
	{ }
};
MODULE_DEVICE_TABLE(of, veml_als_of_match);

static const struct i2c_device_id veml_als_id[] = {
	{ "veml_als", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, veml_als_id);

static struct i2c_driver veml_als_driver = {
	.driver = {
		.name = "veml-als",
		.of_match_table = veml_als_of_match,
		.pm = &veml_als_pm_ops,
	},
	.probe = veml_als_probe,
	.id_table = veml_als_id,
};
module_i2c_driver(veml_als_driver);

MODULE_AUTHOR("Sim Wooseong<wsshim@markt.co.kr>");
MODULE_DESCRIPTION("VEML Ambient Light Sensor");
MODULE_LICENSE("GPL v2");
