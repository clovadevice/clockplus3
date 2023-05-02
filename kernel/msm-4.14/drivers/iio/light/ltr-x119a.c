/*
 * ltr501.c - Support for Lite-On LTR501 ambient light and proximity sensor
 *
 * Copyright 2014 Peter Meerwald <pmeerw@pmeerw.net>
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License.  See the file COPYING in the main
 * directory of this archive for more details.
 *
 * 7-bit I2C slave address 0x23
 *
 * TODO: IR LED characteristics
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/acpi.h>

#include <linux/iio/iio.h>
#include <linux/iio/events.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>

#define LTR_X119A_DRV_NAME			"ltr-x119a"

#define LTR_X119A_REG_ALS_CONFIG		0x7F
#define LTR_X119A_REG_ALS_CONTR			0x80
#define LTR_X119A_REG_ALS_INT_TIME		0x85
#define LTR_X119A_REG_PART_ID 			0x86
#define LTR_X119A_REG_ALS_STATUS		0x88
#define LTR_X119A_REG_IR_DATA_LSB		0x89
#define LTR_X119A_REG_IR_DATA_MSB		0x8A
#define LTR_X119A_REG_ALS_DATA_LSB		0x8B
#define LTR_X119A_REG_ALS_DATA_MSB		0x8C
#define LTR_X119A_REG_MAIN_CONTR		0xAD
#define LTR_X119A_REG_DARK_CONFIG		0xB9

#define LTR_X119A_REG_MAX				0xB9


#define LTR_X119A_BIT_ALS_DR_MASK		GENMASK(7, 6)
#define LTR_X119A_BIT_ALS_DR_16BIT		(0 << 6)
#define LTR_X119A_BIT_ALS_DR_15BIT      (1 << 6)
#define LTR_X119A_BIT_ALS_DR_14BIT      (2 << 6)
#define LTR_X119A_BIT_ALS_DR_13BIT      (3 << 6)

#define LTR_X119A_BIT_ALS_GAIN_MASK     GENMASK(4, 2)
#define LTR_X119A_BIT_ALS_GAIN_1X		(0 << 2)
#define LTR_X119A_BIT_ALS_GAIN_4X       (1 << 2)
#define LTR_X119A_BIT_ALS_GAIN_16X		(2 << 2)
#define LTR_X119A_BIT_ALS_GAIN_64X		(3 << 2)
#define LTR_X119A_BIT_ALS_GAIN_128X		(4 << 2)

#define LTR_X119A_BIT_ALS_MODE_MASK 	BIT(0)
#define LTR_X119A_BIT_ALS_ACTIVE		(1 << 0)
#define LTR_X119A_BIT_ALS_STAND_BY		(0 << 0)


#define LTR_X119A_BIT_ALS_INT_TM_MASK		GENMASK(3, 2)
#define LTR_X119A_BIT_ALS_MEAS_RATE_MASK	GENMASK(1, 0)


#define LTR_X119A_REGMAP_NAME "ltr_x119a_regmap"

/* ============================================ */

#define LTR501_DRV_NAME "ltr501"

#define LTR501_ALS_CONTR 0x80 /* ALS operation mode, SW reset */
#define LTR501_PS_CONTR 0x81 /* PS operation mode */
#define LTR501_PS_MEAS_RATE 0x84 /* measurement rate*/
#define LTR501_ALS_MEAS_RATE 0x85 /* ALS integ time, measurement rate*/
#define LTR501_PART_ID 0x86
#define LTR501_MANUFAC_ID 0x87
#define LTR501_ALS_DATA1 0x88 /* 16-bit, little endian */
#define LTR501_ALS_DATA0 0x8a /* 16-bit, little endian */
#define LTR501_ALS_PS_STATUS 0x8c
#define LTR501_PS_DATA 0x8d /* 16-bit, little endian */
#define LTR501_INTR 0x8f /* output mode, polarity, mode */
#define LTR501_PS_THRESH_UP 0x90 /* 11 bit, ps upper threshold */
#define LTR501_PS_THRESH_LOW 0x92 /* 11 bit, ps lower threshold */
#define LTR501_ALS_THRESH_UP 0x97 /* 16 bit, ALS upper threshold */
#define LTR501_ALS_THRESH_LOW 0x99 /* 16 bit, ALS lower threshold */
#define LTR501_INTR_PRST 0x9e /* ps thresh, als thresh */
#define LTR501_MAX_REG 0x9f

#define LTR501_ALS_CONTR_SW_RESET BIT(2)
#define LTR501_CONTR_PS_GAIN_MASK (BIT(3) | BIT(2))
#define LTR501_CONTR_PS_GAIN_SHIFT 2
#define LTR501_CONTR_ALS_GAIN_MASK BIT(3)
#define LTR501_CONTR_ACTIVE BIT(1)

#define LTR501_STATUS_ALS_INTR BIT(3)
#define LTR501_STATUS_ALS_RDY BIT(2)
#define LTR501_STATUS_PS_INTR BIT(1)
#define LTR501_STATUS_PS_RDY BIT(0)

#define LTR501_PS_DATA_MASK 0x7ff
#define LTR501_PS_THRESH_MASK 0x7ff
#define LTR501_ALS_THRESH_MASK 0xffff

#define LTR501_ALS_DEF_PERIOD 500000
#define LTR501_PS_DEF_PERIOD 100000

#define LTR501_REGMAP_NAME "ltr501_regmap"

#define LTR501_LUX_CONV(vis_coeff, vis_data, ir_coeff, ir_data) \
			((vis_coeff * vis_data) - (ir_coeff * ir_data))


struct ltr_x119a_priv {
    struct i2c_client *client;
    struct regmap *regmap;
};


static int ltr_x119a_als_enable(struct ltr_x119a_priv *priv, bool ena)
{
	int ret = -EINVAL;
	unsigned int value;

	dev_info(&priv->client->dev, "%s: mode: %s\n", __func__, ena?"active":"stand-by");

	ret = regmap_write(priv->regmap, LTR_X119A_REG_ALS_CONFIG, 0);	
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to write als config\n", __func__);
		goto err;
	}

	ret = regmap_write(priv->regmap, LTR_X119A_REG_MAIN_CONTR, 0x10);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to write main config\n", __func__);
		goto err;
	}

	if(ena)
		value = LTR_X119A_BIT_ALS_ACTIVE;
	else
		value = LTR_X119A_BIT_ALS_STAND_BY;

	ret = regmap_update_bits(priv->regmap, LTR_X119A_REG_ALS_CONTR,
			LTR_X119A_BIT_ALS_MODE_MASK, value);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to update als mode\n", __func__);
        goto err;
	} 

err:
	return ret;
}


static int ltr_x119a_als_set_dr(struct ltr_x119a_priv *priv, u8 value)
{
	int ret = -EINVAL;

	switch(value) {
	case LTR_X119A_BIT_ALS_DR_16BIT:
		dev_info(&priv->client->dev, "%s: ALS_DR 16bits intergration\n", __func__);
		break;
	case LTR_X119A_BIT_ALS_DR_15BIT:
		dev_info(&priv->client->dev, "%s: ALS_DR 15bits intergration\n", __func__);
		break;
	case LTR_X119A_BIT_ALS_DR_14BIT:
		dev_info(&priv->client->dev, "%s: ALS_DR 14bits intergration\n", __func__);
		break;
	case LTR_X119A_BIT_ALS_DR_13BIT:
		dev_info(&priv->client->dev, "%s: ALS_DR 13bits intergration\n", __func__);
		break;
	default:
		dev_info(&priv->client->dev, "%s: unknown dr intergration\n", __func__);
		goto err;
	}

	ret = regmap_update_bits(priv->regmap, LTR_X119A_REG_ALS_CONTR,
            LTR_X119A_BIT_ALS_DR_MASK, value);
	if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to set als dr\n", __func__);
        goto err;
    }

err:
	return ret;
}

static int ltr_x119a_als_get_gain(struct ltr_x119a_priv *priv,
                        int *val, int *val2)
{
	int ret = -EINVAL;
	unsigned int value = 0;

	ret = regmap_read(priv->regmap, LTR_X119A_REG_ALS_STATUS, &value);
    if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to read als status\n", __func__);
        goto err;
    }
	
	switch((value >> 3) & 0x07) {
	case 0:
		*val = 1; *val2 = 0;
		break;
	case 1:
		*val = 4; *val2 = 0;
		break;
	case 2:
		*val = 16; *val2 = 0;
		break;
	case 3:
		*val = 64; *val2 = 0;
		break;
	case 4:
		*val = 128; *val2 = 0;
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	dev_info(&priv->client->dev, "%s: value: %02x / gain: %d.%d\n", __func__, value, *val, *val2);

	ret = IIO_VAL_INT_PLUS_MICRO;

err:
	return ret;
}
	

static int ltr_x119a_als_set_gain(struct ltr_x119a_priv *priv,
                        int val, int val2)
{
	int ret = -EINVAL;
	unsigned int new_gain = 0;

	dev_info(&priv->client->dev, "%s: set gain: %d.%d\n", __func__, val, val2);

	if (val == 1 && val2 == 0) {
		new_gain = LTR_X119A_BIT_ALS_GAIN_1X;
	} else if (val == 4 && val2 == 0) {
		new_gain = LTR_X119A_BIT_ALS_GAIN_4X;
	} else if (val == 16 && val2 == 0) {
		new_gain = LTR_X119A_BIT_ALS_GAIN_16X;
	} else if (val == 64 && val2 == 0) {
		new_gain = LTR_X119A_BIT_ALS_GAIN_64X;
	} else if (val == 128 && val2 == 0) {
		new_gain = LTR_X119A_BIT_ALS_GAIN_128X;
	} else {
		ret = -EINVAL;
		goto err;
	}
	
	ret = regmap_update_bits(priv->regmap, LTR_X119A_REG_ALS_CONTR,
            LTR_X119A_BIT_ALS_GAIN_MASK, new_gain);
    if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to set als dr\n", __func__);
        goto err;
    }

err:
	return ret;
}

static int ltr_x119a_als_get_meas_rate(struct ltr_x119a_priv *priv,
                        int *val, int *val2)
{
	int ret = -EINVAL;
   	unsigned int value = 0;

    ret = regmap_read(priv->regmap, LTR_X119A_REG_ALS_INT_TIME, &value);
    if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to read als int time\n", __func__);
        goto err;
    }
    
    switch((value) & 0x03) {
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
    default:
        return -EINVAL;
    }
    *val = 0;

    dev_info(&priv->client->dev, "%s: meas rate: %d.%d (reg: %02x)\n", __func__, *val, *val2, value);

    ret = IIO_VAL_INT_PLUS_MICRO;
err:
    return ret;
}


static int ltr_x119a_als_set_meas_rate(struct ltr_x119a_priv *priv,
                        int val, int val2)
{
	int ret = -EINVAL;
	unsigned int value = 0;
    u8 meas_rate = 0, intrn_tm = 0;
    
    dev_info(&priv->client->dev, "%s: meas rate: %d.%d\n", __func__, val, val2);
    
    if (val)
        goto err;
    
    ret = regmap_read(priv->regmap, LTR_X119A_REG_ALS_INT_TIME, &value);
    if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to read als int time\n", __func__);
        goto err;
    }
    intrn_tm = value & (0x03 << 2);
    
    switch (val2) {
    case 100000:
        meas_rate = 0;
        break;
    case 200000: 
        meas_rate = 1;
        break;
    case 400000: 
        meas_rate = 2;
        break;
    case 800000: 
        meas_rate = 3;
        break;
    default:
		ret = -EINVAL;
        goto err;;
    }
    
    value = 0xA0 | intrn_tm | meas_rate;
    ret = regmap_write(priv->regmap, LTR_X119A_REG_ALS_INT_TIME, value);
    if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to write als int time\n", __func__);
        goto err;
    }
    
    ret = IIO_VAL_INT_PLUS_MICRO;
err:
    return ret;
}


static int ltr_x119a_als_get_intgrn_tm(struct ltr_x119a_priv *priv,
                        int *val, int *val2)
{
	int ret = -EINVAL;
	unsigned int value = 0;
	ret = regmap_read(priv->regmap, LTR_X119A_REG_ALS_INT_TIME, &value);
	if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to read als int time\n", __func__);
        goto err;
    }

	switch((value >> 2) & 0x03) {
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
	default:
		return -EINVAL;
	}
	*val = 0;

	dev_info(&priv->client->dev, "%s: intgn time: %d.%d (reg: %02x)\n", __func__, *val, *val2, value);

	ret = IIO_VAL_INT_PLUS_MICRO;
err:
	return ret;
}


static int ltr_x119a_als_set_intgrn_tm(struct ltr_x119a_priv *priv,
                        int val, int val2)
{
	int ret = -EINVAL;
	unsigned int value = 0;
	u8 meas_rate = 0, intrn_tm = 0;

	dev_info(&priv->client->dev, "%s: intgn time: %d.%d\n", __func__, val, val2);

	if (val)
        goto err;
    
	ret = regmap_read(priv->regmap, LTR_X119A_REG_ALS_INT_TIME, &value);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to read als int time\n", __func__);
		goto err;
	}
	meas_rate = value & 0x03;

	switch (val2) {
	case 50000:
		intrn_tm = 0; 
		break;
	case 100000:
		intrn_tm = 1 << 2;
		break;
	case 200000:
		intrn_tm = 2 << 2;
		break;
	case 400000:
		intrn_tm = 3 << 2;
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	value = 0xA0 | intrn_tm | meas_rate;
	ret = regmap_write(priv->regmap, LTR_X119A_REG_ALS_INT_TIME, value);
	if(ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to write als int time\n", __func__);
        goto err;
    }
	
err:
	return ret;
}


static int ltr_x119a_read_als_data(struct ltr_x119a_priv *priv, u16 *value)
{
	int ret = -EINVAL;
	unsigned int buf[2] = {0, };
	u16 temp;

	ret = regmap_read(priv->regmap, LTR_X119A_REG_ALS_DATA_LSB, &buf[0]);
	if (ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to read als data(lsb)\n", __func__);
		goto err;
	}

	ret = regmap_read(priv->regmap, LTR_X119A_REG_ALS_DATA_MSB, &buf[1]);
    if (ret < 0) {
        dev_err(&priv->client->dev, "%s: fail to read als data(msb)\n", __func__);
        goto err;
    }

	*value = (buf[1] << 8) | buf[0];

	ret = regmap_bulk_read(priv->regmap, LTR_X119A_REG_ALS_DATA_LSB, &temp, 2);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to read als data(bulk)\n", __func__);
		goto err;
	}

	//dev_info(&priv->client->dev, "%s: als data = %04x(%02x %02x), %04x\n", __func__, *value, buf[1], buf[0], temp);

err:
	return ret;
}

/* Channel number */
enum ltr_x119a_chan {
    CH_ALS,
    CH_WHITE,
};

static const struct iio_chan_spec ltr_x119a_channels[] = {
	{
        .type = IIO_LIGHT,
        .channel = CH_ALS,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
                BIT(IIO_CHAN_INFO_PROCESSED) |
                BIT(IIO_CHAN_INFO_INT_TIME) |
                BIT(IIO_CHAN_INFO_SCALE),
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


static int ltr_x119a_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct ltr_x119a_priv *priv = iio_priv(indio_dev);
	int ret = 0;
	u16 value;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		//dev_info(&priv->client->dev, "%s: mask - IIO_CHAN_INFO_RAW\n", __func__);
		switch(chan->type) {
		case IIO_LIGHT:
			ret = ltr_x119a_read_als_data(priv, &value);
			if(ret < 0) {
				dev_err(&priv->client->dev, "%s: can't read als data\n", __func__);
				return ret;
			}
			if(mask == IIO_CHAN_INFO_PROCESSED) {
				*val = 0;
				*val2 = 0;
				return IIO_VAL_INT_PLUS_MICRO;
			}
			*val = value;
			return IIO_VAL_INT;	
		case IIO_INTENSITY:
			*val = 0;
			*val2 = 0;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_INT_TIME:
		//dev_info(&priv->client->dev, "%s: mask - IIO_CHAN_INFO_INT_TIME\n", __func__);
		if (chan->type == IIO_LIGHT)
			return ltr_x119a_als_get_intgrn_tm(priv, val, val2);
		return -EINVAL;
	case IIO_CHAN_INFO_SCALE:
		//dev_info(&priv->client->dev, "%s: mask - IIO_CHAN_INFO_SCALE\n", __func__);
		if (chan->type == IIO_LIGHT)
			return ltr_x119a_als_get_gain(priv, val, val2);
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static int ltr_x119a_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct ltr_x119a_priv *priv = iio_priv(indio_dev);
	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		dev_info(&priv->client->dev, "%s: mask - IIO_CHAN_INFO_INT_TIME\n", __func__);
		if(chan->type == IIO_LIGHT)
			return ltr_x119a_als_set_intgrn_tm(priv, val, val2);
		return -EINVAL;
	case IIO_CHAN_INFO_SCALE:
		dev_info(&priv->client->dev, "%s: mask - IIO_CHAN_INFO_SCALE\n", __func__);
		if(chan->type == IIO_LIGHT)
			return ltr_x119a_als_set_gain(priv, val, val2);
		return -EINVAL;
	default:
		return -EINVAL;
	}
}


/* Integration time available in seconds */
static IIO_CONST_ATTR(in_illuminance_integration_time_available,
                "0.05 0.1 0.2 0.4");
static IIO_CONST_ATTR(in_illuminance_measure_rate_available,
                "0.1 0.2 0.4 0.8");
static IIO_CONST_ATTR(in_illuminance_scale_available,
                "1 4 16 128");

static struct attribute *ltr_x119a_attributes[] = {
    &iio_const_attr_in_illuminance_integration_time_available.dev_attr.attr,
	&iio_const_attr_in_illuminance_measure_rate_available.dev_attr.attr,
    &iio_const_attr_in_illuminance_scale_available.dev_attr.attr,
    NULL
};

static const struct attribute_group ltr_x119a_attribute_group = {
	.attrs = ltr_x119a_attributes,
};

static const struct iio_info ltr_x119a_info_no_irq = {
	.read_raw = ltr_x119a_read_raw,
	.write_raw = ltr_x119a_write_raw,
	.attrs = &ltr_x119a_attribute_group,
	.driver_module = THIS_MODULE,
};


static int ltr_x119a_init(struct ltr_x119a_priv *priv)
{
	int ret = -EINVAL;

	/* set default als dr(16bits) */
	ret = ltr_x119a_als_set_dr(priv, LTR_X119A_BIT_ALS_DR_16BIT);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to set als dr\n", __func__);
		goto err;
	}
	
	/* set default als measure rate(400ms) */
	ret = ltr_x119a_als_set_meas_rate(priv, 0, 400000);	
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to set measure rate\n", __func__);
        goto err;
    }

	/* set default als integration time(100ms) */
	ret = ltr_x119a_als_set_intgrn_tm(priv, 0, 100000);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to set als integration time\n", __func__);
        goto err;
    }

	/* set default als gain(1x) */
	ret = ltr_x119a_als_set_gain(priv, 1, 0);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to set gain\n", __func__);
        goto err;
    }

	/* eanble als */
	ret = ltr_x119a_als_enable(priv, true);
	if(ret < 0) {
		dev_err(&priv->client->dev, "%s: fail to enable als\n", __func__);
        goto err;
    }

err:
	return ret;
}

static bool ltr_x119a_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LTR_X119A_REG_ALS_CONFIG:
	case LTR_X119A_REG_ALS_CONTR:
	case LTR_X119A_REG_ALS_INT_TIME:
	case LTR_X119A_REG_PART_ID:
	case LTR_X119A_REG_ALS_STATUS:
	case LTR_X119A_REG_IR_DATA_LSB:
	case LTR_X119A_REG_IR_DATA_MSB:
	case LTR_X119A_REG_ALS_DATA_LSB:
	case LTR_X119A_REG_ALS_DATA_MSB:
	case LTR_X119A_REG_MAIN_CONTR:
	case LTR_X119A_REG_DARK_CONFIG:
		return true;
	default:
		return false;
	}
}


static struct regmap_config ltr_x119a_regmap_config = {
	.name =  LTR_X119A_REGMAP_NAME,
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LTR_X119A_REG_MAX,
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = ltr_x119a_is_volatile_reg,
};


static int ltr_x119a_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct ltr_x119a_priv *priv;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int ret, partid, chip_idx = 0;
	const char *name = NULL;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(&client->dev, "i2c adapter doesn't support plain i2c\n");
        return -EOPNOTSUPP;
    }

	regmap = devm_regmap_init_i2c(client, &ltr_x119a_regmap_config);
    if (IS_ERR(regmap)) {
        dev_err(&client->dev, "Regmap initialization failed.\n");
        return PTR_ERR(regmap);
    }


	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	priv->client = client;
	priv->regmap = regmap;
	

	ret = regmap_read(priv->regmap, LTR_X119A_REG_PART_ID, &partid);
	if (ret < 0) {
		dev_err(&client->dev, "%s: could not read part id\n", __func__);
		return ret;
	}

	dev_err(&client->dev, "%s: part id: 0x%02x\n", __func__, partid);

	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &ltr_x119a_info_no_irq;
	indio_dev->channels = ltr_x119a_channels;
	indio_dev->num_channels = ARRAY_SIZE(ltr_x119a_channels);
	indio_dev->name = "ltr-x119a";
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = ltr_x119a_init(priv);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(&client->dev, indio_dev);;
}


#ifdef CONFIG_PM_SLEEP
static int ltr_x119a_suspend(struct device *dev)
{
	struct ltr_x119a_priv *priv = iio_priv(i2c_get_clientdata(
					    to_i2c_client(dev)));
	return ltr_x119a_als_enable(priv, false);
}

static int ltr_x119a_resume(struct device *dev)
{
	struct ltr_x119a_priv *priv = iio_priv(i2c_get_clientdata(
					    to_i2c_client(dev)));
	return ltr_x119a_als_enable(priv, true);
}
#endif

static SIMPLE_DEV_PM_OPS(ltr_x119a_pm_ops, ltr_x119a_suspend, ltr_x119a_resume);


static const struct of_device_id ltr_x119a_of_match[] = {
    { .compatible = "lite-on,ltr-x119a" },
    { }
};
MODULE_DEVICE_TABLE(of, ltr_x119a_of_match);


static const struct i2c_device_id ltr_x119a_id[] = {
	{ "ltr-x119a", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ltr_x119a_id);

static struct i2c_driver ltr_x119a_driver = {
	.driver = {
		.name   = LTR_X119A_DRV_NAME,
		.of_match_table = ltr_x119a_of_match,
		.pm	= &ltr_x119a_pm_ops,
	},
	.probe  = ltr_x119a_probe,
	.id_table = ltr_x119a_id,
};

module_i2c_driver(ltr_x119a_driver);

MODULE_AUTHOR("Sim Wooseong");
MODULE_DESCRIPTION("Lite-On LTR-X119A ambient light and proximity sensor driver");
MODULE_LICENSE("GPL");
