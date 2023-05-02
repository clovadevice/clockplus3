/*
 * linux/drivers/leds-pwm.c
 *
 * simple PWM based LED control
 *
 * Copyright 2009 Luotao Fu @ Pengutronix (l.fu@pengutronix.de)
 *
 * based on leds-gpio.c by Raphael Assenat <raph@8d.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/fb.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/leds_pwm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/miscdevice.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#if defined(CONFIG_PRODUCT_IF_S1300N) || defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_MKT300) || defined(CONFIG_PRODUCT_IF_S1600N)
	#define USE_MARKT_LEDS_MISC	
#if 0 // wsshim - test always on
	#define USE_MARKT_LEDS_ANIMATION
#endif
#endif


#if defined(CONFIG_PRODUCT_IF_S1300N) || defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_S1600N)
	#define MAX_LEDS_DEV_COUNT		6
#else
	#define MAX_LEDS_DEV_COUNT		3
#endif


#ifdef USE_MARKT_LEDS_ANIMATION
	#include <linux/workqueue.h>
	#include "markt/ledanim.h"
#endif

//#define USE_PM_QOS
#if defined(USE_PM_QOS)
	#define PM_QOS_VALUE	(200)
	#include <linux/pm_qos.h>
#endif // USE_PM_QOS



struct led_pwm_data {
	struct led_classdev	cdev;
	struct pwm_device	*pwm;
	unsigned int		active_low;
	unsigned int		period;
	int			duty;
#ifdef USE_MARKT_LEDS_MISC
	int			index;
#endif
};

struct led_pwm_priv {
	int num_leds;
	#if 0 // wsshim - 2020-06-22
	struct led_pwm_data leds[0];
	#else
	struct led_pwm_data leds[MAX_LEDS_DEV_COUNT];	
	#endif
#ifdef USE_MARKT_LEDS_ANIMATION
	struct work_struct	led_update_work;
	#if defined(USE_PM_QOS)
		struct pm_qos_request pm_qos_request;
	#endif
#endif
};


#ifdef USE_MARKT_LEDS_MISC	
	enum {
		MARKT_LED_GROUP_MIN = 0,
		MARKT_LED_GROUP_1 = MARKT_LED_GROUP_MIN,
		MARKT_LED_GROUP_2,
		MARKT_LED_GROUP_MAX = MARKT_LED_GROUP_2,
	};
	
	enum {
		MARKT_LED_1_R = 0,
		MARKT_LED_1_G,
		MARKT_LED_1_B,
		MARKT_LED_2_R,
		MARKT_LED_2_G,
		MARKT_LED_2_B,
	};
		
	struct led_rgb {		
		uint8_t r;
		uint8_t g;
		uint8_t b;
	};	
	#define MISC_MODULE_NAME							"markt-leds-misc"
	#define IOCTL_LED_MAGIC_NUMBER                   	'l'
	#define IOCTL_SET_LED1_RGB				            _IOW(IOCTL_LED_MAGIC_NUMBER, 1, struct led_rgb)
	#define IOCTL_GET_LED1_RGB           				_IOR(IOCTL_LED_MAGIC_NUMBER, 2, struct led_rgb)
	#define IOCTL_SET_LED2_RGB				            _IOW(IOCTL_LED_MAGIC_NUMBER, 3, struct led_rgb)
	#define IOCTL_GET_LED2_RGB           				_IOR(IOCTL_LED_MAGIC_NUMBER, 4, struct led_rgb)	
	#define IOCTL_LED_MAXNR                          	4
	
	static struct led_pwm_priv *g_led_pwm_priv;
	static DEFINE_MUTEX(led_pwm_mutex);
#endif

#ifdef USE_MARKT_LEDS_ANIMATION
#define LED_COUNT MARKT_LED_GROUP_MAX + 1
#define LED_START MARKT_LED_GROUP_2

struct rgb_brightness
{
	int		led_group;
	struct	led_rgb rgbValue;
};

struct rgb_brightness g_rgb_brightness[LED_COUNT];

static int animation_set_led_brightness(int index, struct led_brightness_t *rgb_value);
static int animation_led_update(struct device *dev);

static struct leddrv_func leddrv_ops = {
	.num_leds = LED_COUNT,
	.start_led = LED_START,
	.set_led_brightness = animation_set_led_brightness,
	.update = animation_led_update,
};
#endif



static void __led_pwm_set(struct led_pwm_data *led_dat)
{
	int new_duty = led_dat->duty;

	pwm_config(led_dat->pwm, new_duty, led_dat->period);

	if (new_duty == 0)
		pwm_disable(led_dat->pwm);
	else
		pwm_enable(led_dat->pwm);
}

static int led_pwm_set(struct led_classdev *led_cdev,
		       enum led_brightness brightness)
{
	struct led_pwm_data *led_dat =
		container_of(led_cdev, struct led_pwm_data, cdev);
	unsigned int max = led_dat->cdev.max_brightness;
	unsigned long long duty =  led_dat->period;

	duty *= brightness;
	do_div(duty, max);

	if (led_dat->active_low)
		duty = led_dat->period - duty;

	led_dat->duty = duty;

	__led_pwm_set(led_dat);

	return 0;
}

static inline size_t sizeof_pwm_leds_priv(int num_leds)
{
	#if 0 // wsshim - 2020-06-22
	return sizeof(struct led_pwm_priv) +
		      (sizeof(struct led_pwm_data) * num_leds);
	#else
	return sizeof(struct led_pwm_priv);
	#endif
}

static void led_pwm_cleanup(struct led_pwm_priv *priv)
{
	while (priv->num_leds--)
		led_classdev_unregister(&priv->leds[priv->num_leds].cdev);
}

static int led_pwm_add(struct device *dev, struct led_pwm_priv *priv,
		       struct led_pwm *led, struct device_node *child)
{
	struct led_pwm_data *led_data = &priv->leds[priv->num_leds];
	struct pwm_args pargs;
	int ret;
	
#ifdef USE_MARKT_LEDS_MISC
	int idx;
#endif

	led_data->active_low = led->active_low;
	led_data->cdev.name = led->name;
	led_data->cdev.default_trigger = led->default_trigger;
	led_data->cdev.brightness = LED_OFF;
	led_data->cdev.max_brightness = led->max_brightness;
	led_data->cdev.flags = LED_CORE_SUSPENDRESUME;

	if (child)
		led_data->pwm = devm_of_pwm_get(dev, child, NULL);
	else
		led_data->pwm = devm_pwm_get(dev, led->name);
	if (IS_ERR(led_data->pwm)) {
		ret = PTR_ERR(led_data->pwm);
		dev_err(dev, "unable to request PWM for %s: %d\n",
			led->name, ret);
		return ret;
	}

	led_data->cdev.brightness_set_blocking = led_pwm_set;

	/*
	 * FIXME: pwm_apply_args() should be removed when switching to the
	 * atomic PWM API.
	 */
	pwm_apply_args(led_data->pwm);

	pwm_get_args(led_data->pwm, &pargs);

	led_data->period = pargs.period;
	if (!led_data->period && (led->pwm_period_ns > 0))
		led_data->period = led->pwm_period_ns;

	ret = led_classdev_register(dev, &led_data->cdev);
	if (ret == 0) {
		priv->num_leds++;
		led_pwm_set(&led_data->cdev, led_data->cdev.brightness);
	} else {
		dev_err(dev, "failed to register PWM led for %s: %d\n",
			led->name, ret);
	}
	
#ifdef USE_MARKT_LEDS_MISC
	ret = of_property_read_u32(child, "markt,led-index",
				     &idx);
	if(ret) {
		pr_err("%s: fail to get led index= %d\n", __func__, idx);
	} else
		led_data->index = idx;		
	
#endif

	return ret;
}

static int led_pwm_create_of(struct device *dev, struct led_pwm_priv *priv)
{
	struct device_node *child;
	struct led_pwm led;
	int ret = 0;

	memset(&led, 0, sizeof(led));

	for_each_child_of_node(dev->of_node, child) {
		led.name = of_get_property(child, "label", NULL) ? :
			   child->name;

		led.default_trigger = of_get_property(child,
						"linux,default-trigger", NULL);
		led.active_low = of_property_read_bool(child, "active-low");
		of_property_read_u32(child, "max_brightness",
				     &led.max_brightness);

		ret = led_pwm_add(dev, priv, &led, child);
		if (ret) {
			of_node_put(child);
			break;
		}
	}

	return ret;
}

#ifdef USE_MARKT_LEDS_MISC
static int set_markt_leds_pwm(int group, struct led_rgb *pData)
{
	struct led_pwm_data *led_data;
	int r_idx, g_idx, b_idx;
	
	if(pData == NULL) {
		pr_err("%s: rgb data is null\n", __func__);
		return -ENOMEM;
	}
	
	if(group < MARKT_LED_GROUP_MIN || group > MARKT_LED_GROUP_MAX) {
		pr_err("%s: LED Group Number is wroing\n", __func__);
		return -EINVAL;
	}
	
	
	#if defined(CONFIG_PRODUCT_IF_S1300N) || defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_S1600N)
	switch(group) {	
		case MARKT_LED_GROUP_2:		
			r_idx = 3; g_idx = 4; b_idx = 5;
			break;	
		case MARKT_LED_GROUP_1:
		default:
			r_idx = 0; g_idx = 1; b_idx = 2;
			break;
	}
	#else
	r_idx = 0; g_idx = 1; b_idx = 2;
	#endif
		
    
	if(g_led_pwm_priv->num_leds < MAX_LEDS_DEV_COUNT) {	
		pr_err("%s: need to leds count is %d\n", __func__, MAX_LEDS_DEV_COUNT );
		return -EINVAL;
	}
	
	
	/* LED - Red */		
	led_data = &g_led_pwm_priv->leds[r_idx];
	led_data->cdev.brightness = pData->r;
	led_pwm_set(&led_data->cdev, led_data->cdev.brightness);	
			
	/* LED - Green */	
	led_data = &g_led_pwm_priv->leds[g_idx];
	led_data->cdev.brightness = pData->g;
	led_pwm_set(&led_data->cdev, led_data->cdev.brightness);
		
	/* LED - Blue */
	led_data = &g_led_pwm_priv->leds[b_idx];
	led_data->cdev.brightness = pData->b;
	led_pwm_set(&led_data->cdev, led_data->cdev.brightness);
    

	pr_debug("%s: Total: %d R(%d:%d) G(%d:%d) B(%d:%d)\n", __func__, 
            g_led_pwm_priv->num_leds, 
            r_idx, pData->r, 
            g_idx, pData->g, 
            b_idx, pData->b);
	
	return 0;
}

static int get_markt_leds_pwm(int group, struct led_rgb *pData)
{
	struct led_pwm_data *led_data;
	int r_idx, g_idx, b_idx;
	struct led_rgb rgbValue;
	
	if(pData == NULL) {
		pr_err("%s: rgb data is null\n", __func__);
		return -ENOMEM;
	}
	
	if(group < MARKT_LED_GROUP_MIN || group > MARKT_LED_GROUP_MAX) {
		pr_err("%s: LED Group Number is wroing\n", __func__);
		return -EINVAL;
	}

	#if defined(CONFIG_PRODUCT_IF_S1300N) || defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_S1600N)
	switch(group) {	
		case MARKT_LED_GROUP_2:
			r_idx = 3; g_idx = 4; b_idx = 5;
			break;		
		case MARKT_LED_GROUP_1:
		default:
			r_idx = 0; g_idx = 1; b_idx = 2;
			break;
	}
	#else
	r_idx = 0; g_idx = 1; b_idx = 2;
	#endif
	
	memset(&rgbValue, 0, sizeof(struct led_rgb));
	
	/* LED - Red */	
	led_data = &g_led_pwm_priv->leds[r_idx];
	rgbValue.r = led_data->cdev.brightness;
	
	/* LED - Green */	
	led_data = &g_led_pwm_priv->leds[g_idx];
	rgbValue.g = led_data->cdev.brightness;
	
	
	/* LED - Blue */	
	led_data = &g_led_pwm_priv->leds[b_idx];
	rgbValue.b = led_data->cdev.brightness;		
	memcpy(pData, &rgbValue, sizeof(struct led_rgb));
	
	pr_debug("%s: R(%d) G(%d) B(%d)\n", __func__, pData->r, pData->g, pData->b);
	
	return 0;
}

static ssize_t led_misc_read(struct file *file, char *buf,
                        size_t count, loff_t *ppos)
{	
    return count;
}

static ssize_t led_misc_write(struct file *file, const char __user *buf,
                        size_t count, loff_t *ppos)
{	
    return count;
}


static int led_misc_open(struct inode *inode, struct file *file)
{	
#ifdef USE_MARKT_LEDS_ANIMATION	
	if(register_leddrv_end_func(&leddrv_ops) != 0) {
		pr_err("register_leddrv_end_func() failed\n"); 
	}
#endif
    return 0;
}

static int led_misc_close(struct inode *inodep, struct file *file)
{  
    return 0;
}

static long led_misc_do_ioctl(struct file *file, unsigned int cmd,
				 void __user *arg)
{
	struct led_rgb rgbValue;
	int ret;
		
	/*
     * extract the type and number bitfields, and don't decode
     * wrong cmds: return ENOTTY (inappropriate ioctl) before access_ok()
     */
	if (_IOC_TYPE(cmd) != IOCTL_LED_MAGIC_NUMBER) return -ENOTTY;
    if (_IOC_NR(cmd) > IOCTL_LED_MAXNR) return -ENOTTY;
	
	/*
     * the direction is a bitmask, and VERIFY_WRITE catches R/W
     * transfers. `Type' is user-oriented, while
     * access_ok is kernel-oriented, so the concept of "read" and
     * "write" is reversed
     */
    if (_IOC_DIR(cmd) & _IOC_READ)
        ret = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    else if (_IOC_DIR(cmd) & _IOC_WRITE)
        ret = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    if (ret) return -EFAULT;
	
	switch(cmd) {
	case IOCTL_SET_LED1_RGB:		
		ret = copy_from_user(&rgbValue, (struct led_rgb*)arg, sizeof(struct led_rgb));	
		if(ret) {
			pr_err("%s: fail to copy_from_user(cmd: %s)\n", __func__, "IOCTL_SET_LED1_RGB");
			return -EINVAL;
		}
		ret = set_markt_leds_pwm(MARKT_LED_GROUP_1, &rgbValue);		
		if(ret)
			return -EIO;		
		break;
	case IOCTL_GET_LED1_RGB:		
		ret = get_markt_leds_pwm(MARKT_LED_GROUP_1, &rgbValue);
		if(ret)
			return -EIO;		
		ret = copy_to_user((void __user *)arg, &rgbValue, sizeof(struct led_rgb));
		if(ret) {
			pr_err("%s: fail to copy_to_user(cmd: %s)\n", __func__, "IOCTL_GET_LED1_RGB");
			return -EINVAL;
		}
		break;
	case IOCTL_SET_LED2_RGB:		
		ret = copy_from_user(&rgbValue, (struct led_rgb*)arg, sizeof(struct led_rgb));	
		if(ret) {
			pr_err("%s: fail to copy_from_user(cmd: %s)\n", __func__, "IOCTL_SET_LED2_RGB");
			return -EINVAL;
		}
		ret = set_markt_leds_pwm(MARKT_LED_GROUP_2, &rgbValue);
		if(ret)
			return -EIO;		
		break;
	case IOCTL_GET_LED2_RGB:			
		ret = get_markt_leds_pwm(MARKT_LED_GROUP_2, &rgbValue);
		if(ret)
			return -EIO;		
		ret = copy_to_user((void __user *)arg, &rgbValue, sizeof(struct led_rgb));
		if(ret) {
			pr_err("%s: fail to copy_to_user(cmd: %s)\n", __func__, "IOCTL_GET_LED2_RGB");
			return -EINVAL;
		}
		break;
	}
	
	return 0;
}

static long led_misc_ioctl_handler(struct file *file, unsigned int cmd,
				 void __user *arg)
{
	int ret;

	ret = mutex_lock_interruptible(&led_pwm_mutex);
	if (ret) {
		pr_err("%s: fail to ioctl mutex lock\n", __func__);
		return ret;
	}
	
	ret = led_misc_do_ioctl(file, cmd, arg);
	
	mutex_unlock(&led_pwm_mutex);
	
	return ret;
}
	
static long led_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return led_misc_ioctl_handler(file, cmd, arg);
}
#endif

#if defined(USE_MARKT_LEDS_ANIMATION)
static int animation_set_led_brightness(int index, struct led_brightness_t *rgb_value)
{	
	g_rgb_brightness[index].led_group = index;
	g_rgb_brightness[index].rgbValue.r = rgb_value->r;
	g_rgb_brightness[index].rgbValue.g = rgb_value->g;
	g_rgb_brightness[index].rgbValue.b = rgb_value->b;
	
	/*
	pr_info("%s: index(%d) R(%d), G(%d), B(%d)\n", __func__, 
				index, rgb_value->r, rgb_value->g, rgb_value->b);
	*/
	
	return 0;
}

static int animation_led_update(struct device *dev)
{
	struct led_pwm_priv * priv = dev_get_drvdata(dev);

	if(priv)
		schedule_work(&priv->led_update_work);

	return 0;
}

static void animation_led_update_work(struct work_struct *work)
{
	int rc, i;
#if defined(USE_PM_QOS)
	struct led_pwm_priv *priv = container_of(work, struct led_pwm_priv, led_update_work);
#endif

#if defined(USE_PM_QOS)
	pm_qos_update_request(&priv->pm_qos_request, PM_QOS_VALUE);
#endif

	for (i = LED_START; i < LED_COUNT; i++) {
		rc = set_markt_leds_pwm(g_rgb_brightness[i].led_group, &g_rgb_brightness[i].rgbValue);
		if(rc != 0) {
#if defined(USE_PM_QOS)
			pm_qos_update_request(&priv->pm_qos_request, PM_QOS_DEFAULT_VALUE);
#endif
			pr_err("set_markt_leds_pwm() failed\n");
			return;
		}
	}

#if defined(USE_PM_QOS)
	pm_qos_update_request(&priv->pm_qos_request, PM_QOS_DEFAULT_VALUE);
#endif
}
#endif

#ifdef USE_MARKT_LEDS_MISC
static const struct file_operations led_misc_fops = {
    .owner = THIS_MODULE,
    .read = led_misc_read,
    .write = led_misc_write,
    .unlocked_ioctl = led_misc_ioctl,
    .open = led_misc_open,
    .release = led_misc_close,
};

struct miscdevice led_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = MISC_MODULE_NAME,
    .fops = &led_misc_fops,
};
#endif

static int led_pwm_probe(struct platform_device *pdev)
{
	struct led_pwm_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct led_pwm_priv *priv;
	int count, i;
	int ret = 0;

	if (pdata)
		count = pdata->num_leds;
	else
		count = of_get_child_count(pdev->dev.of_node);

	if (!count)
		return -EINVAL;
	
	pr_info("%s: leds count: %d\n", __func__, count);
	
	if(count != MAX_LEDS_DEV_COUNT) {
		pr_err("%s: leds need %d\n", __func__, MAX_LEDS_DEV_COUNT);
		return -EINVAL;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof_pwm_leds_priv(count),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (pdata) {
		for (i = 0; i < count; i++) {
			ret = led_pwm_add(&pdev->dev, priv, &pdata->leds[i],
					  NULL);
			if (ret)
				break;
		}
	} else {
		ret = led_pwm_create_of(&pdev->dev, priv);
	}

	if (ret) {
		led_pwm_cleanup(priv);
		return ret;
	}

	platform_set_drvdata(pdev, priv);

#ifdef USE_MARKT_LEDS_MISC
	g_led_pwm_priv = priv;
	ret = misc_register(&led_misc_device);
	if(ret) {
		pr_err("%s: fail to misc register...\n", __func__);
	}
#endif

#if 1
	{
		struct led_rgb rgbValue;
		rgbValue.r  = 255;
		rgbValue.g  = 245;
		rgbValue.b = 100;

		ret = set_markt_leds_pwm(MARKT_LED_GROUP_1, &rgbValue);
	}


	{
		struct led_rgb rgbValue;
		rgbValue.r  = 255;
		rgbValue.g  = 245;
		rgbValue.b = 100;

		ret = set_markt_leds_pwm(MARKT_LED_GROUP_2, &rgbValue);
	}

#endif


#if defined(USE_MARKT_LEDS_ANIMATION)

	INIT_WORK(&priv->led_update_work, animation_led_update_work);
	
	leddrv_ops.dev = &pdev->dev;
	if(register_leddrv_func(&leddrv_ops) != 0) {
		pr_err("%s: register_leddrv_func() failed\n", __func__); 
	}
#if defined(USE_PM_QOS)
	pm_qos_add_request(&priv->pm_qos_request, PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
#endif
#endif

	return 0;
}

static int led_pwm_remove(struct platform_device *pdev)
{
	struct led_pwm_priv *priv = platform_get_drvdata(pdev);

#ifdef USE_MARKT_LEDS_MISC
	misc_deregister(&led_misc_device);
#endif

#if defined(USE_PM_QOS)
	pm_qos_remove_request(&priv->pm_qos_request);
#endif

	led_pwm_cleanup(priv);

	return 0;
}

static const struct of_device_id of_pwm_leds_match[] = {
	{ .compatible = "pwm-leds", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_leds_match);

static struct platform_driver led_pwm_driver = {
	.probe		= led_pwm_probe,
	.remove		= led_pwm_remove,
	.driver		= {
		.name	= "leds_pwm",
		.of_match_table = of_pwm_leds_match,
	},
};

module_platform_driver(led_pwm_driver);

MODULE_AUTHOR("Luotao Fu <l.fu@pengutronix.de>");
MODULE_DESCRIPTION("generic PWM LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:leds-pwm");
