//#define DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/init.h>

#include "ledanim.h"

#if defined(CONFIG_PRODUCT_IF_S1300N) || defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_S1600N)
// khs get mtd number 
#define USE_MTD_NUMBER
#endif

#if defined(USE_MTD_NUMBER)
#if defined(CONFIG_PRODUCT_IF_S1300N) || defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_S1600N)
#define MTD_SYSTEM			20
#define MTD_RECOVERYFS		14
#endif
#endif

#define LED_ANIMATION_LEDNUM_PER_FRAME	36

static struct timer_list	anim_timer;
static struct ledanim_t *	anim_current = NULL;
static uint32_t				anim_frame_count;
static uint32_t				anim_total_frames;
static uint32_t				anim_repeat_count;

static struct leddrv_func *	leddrv_func = NULL;


#include "anim_fade_in_out.c"
static struct ledanim_t ledanim_boot = {
	.name = "__boot__",
	.interval = 40,
	.anim_count = 1584,
	.anim = anim_fade_in_out,
	.repeat = 0,
};
#if defined(CONFIG_PRODUCT_IF_S1500N)
static struct ledanim_t ledanim_boot_rev1_0 = {
	.name = "__boot__",
	.interval = 40,
	.anim_count = 1584,
	.anim = anim_fade_in_out_rev1_0,
	.repeat = 0,
};
#endif
#if defined(USE_MTD_NUMBER)
static struct ledanim_t ledanim_boot_recovery = {
	.name = "__boot__",
	.interval = 70,
	.anim_count = 1584,
	.anim = anim_fade_in_out_orange,
	.repeat = 0,
};
#endif

static void ledanimation_timer_function(unsigned long data);

#if defined(USE_MTD_NUMBER)
// khs get mtd number.
static int _get_cmdline_param(const char * param, char * value, int len)
{
	char * start, *end;
	long int param_length;

	start = strstr(saved_command_line, param);
	if(start == NULL){
		printk(KERN_ERR "not found '%s' in saved_command_line\n", param);
		return -EINVAL;
	}

    start += strlen(param);
	end = strchr(start, ' ');

    if(end == NULL){
		return -EINVAL;		
	}

	param_length = (long int)end - (long int)start; 
	if(param_length > len)
		param_length = len;

	strncpy(value, start, param_length);
	return 0;
}

static int get_mtd_number(char * mtd_num, int len)
{
	return _get_cmdline_param("ubi.mtd=", mtd_num, len);
}

#if defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_S1600N)
int get_board_rev(unsigned char * rev)
{
	char buf[32] = {0,};
	unsigned long value;
	unsigned char major,minor;
	char * p;
	int rc;

	rc = _get_cmdline_param("androidboot.board_rev=", buf, sizeof(buf) - 1);
	if(rc != 0){
		return rc;
	}

	p = strchr(buf, '.');
	if(!p){
		printk(KERN_ERR "Invalid board revision:%s\n", buf);
		return -EINVAL;
	}

	*p = '\0';
	if (kstrtoul(buf, 0, &value)){
		printk(KERN_ERR "Invalid board revision:%s\n", buf);
		return -EINVAL;
	}
	major = (unsigned char)value;
	p++;
	if (kstrtoul(p, 0, &value)){
		printk(KERN_ERR "Invalid board revision:%s\n", buf);
		return -EINVAL;
	}
	minor = (unsigned char)value;
	*rev = (((major & 0xF) << 4) | (minor & 0xF));
	return 0;
}
#endif
#endif

static void _set_led_all_off(void)
{
	int i;
	struct led_brightness_t v_rgb;

	if(!leddrv_func)
		return;

	for(i = leddrv_func->start_led; i < leddrv_func->num_leds ; i++){
		v_rgb.r = 0;
		v_rgb.g = 0;
		v_rgb.b = 0;
		leddrv_func->set_led_brightness(i, &v_rgb);
	}
	leddrv_func->update(leddrv_func->dev);
}

static inline void _draw_led_frame(unsigned char * anim)
{
	int i;
	struct led_brightness_t v_rgb;

	if(!leddrv_func)
		return;

	for(i = leddrv_func->start_led; i < leddrv_func->num_leds ; i++){
		v_rgb.r = *(anim + (i * 3));
		v_rgb.g = *(anim + 1 + (i * 3));
		v_rgb.b = *(anim + 2 + (i * 3));
		leddrv_func->set_led_brightness(i, &v_rgb);
	}
	leddrv_func->update(leddrv_func->dev);
}

static inline void start_anim_current(struct ledanim_t * ledanim)
{
	if(!leddrv_func)
		return;

	anim_current = ledanim;
	anim_frame_count = 0;
	anim_total_frames = (uint32_t)(anim_current->anim_count / LED_ANIMATION_LEDNUM_PER_FRAME);
	anim_repeat_count = anim_current->repeat;
	ledanimation_timer_function(0);
}

static inline void delete_anim_current(void)
{
	if(anim_current){
		if(strcmp(anim_current->name, "__boot__") == 0){
			anim_current = NULL;
			return;
		}
		kfree(anim_current->anim);
		kfree(anim_current);
		anim_current = NULL;
	}
}



static void ledanimation_timer_function(unsigned long data)
{
	unsigned char * anim;
	if(anim_current == NULL){
		return;
	}
	if(leddrv_func == NULL){
		return;
	}


#ifdef DEBUG
	printk(KERN_ERR "led animation [%s][remain:%u][frame:%u/%u]\n",
			anim_current->name,
			anim_repeat_count,
			anim_frame_count + 1,
			anim_total_frames);
#endif

	// ctrl led on/off
	anim = anim_current->anim + (LED_ANIMATION_LEDNUM_PER_FRAME*anim_frame_count);
	_draw_led_frame(anim);

	anim_frame_count++;
	if(anim_frame_count >= anim_total_frames){
		anim_frame_count = 0;
		if(anim_repeat_count > 0 && --anim_repeat_count <= 0){
			delete_anim_current();
#ifdef DEBUG
			printk(KERN_ERR "end animation\n");
#endif			
			return;
		}		
	}
	mod_timer(&anim_timer, jiffies + msecs_to_jiffies(anim_current->interval));
}

static void _start_boot_animation(void)
{
#if defined(USE_MTD_NUMBER)
	char mtd_num[3] = {0,};
	unsigned long value;
	int rc = 0;
#if defined(CONFIG_PRODUCT_IF_S1500N)
	unsigned char rev;
#endif
#endif
	printk("start boot animation. \n");



#if defined(USE_MTD_NUMBER)
	rc = get_mtd_number(mtd_num, sizeof(mtd_num) - 1);
	if(rc == 0){
		if (kstrtoul(mtd_num, 0, &value)){
			pr_err("Invalid mtd_num :%s\n", mtd_num);
		}

		if (value == MTD_RECOVERYFS) {
			pr_err("boot animation LED -> recovery \n");
			start_anim_current(&ledanim_boot_recovery);
		} else {
			pr_err("boot animation LED -> system \n");
#if defined(CONFIG_PRODUCT_IF_S1500N)
			if(get_board_rev(&rev) == 0 && rev >= 0x10) {
				start_anim_current(&ledanim_boot_rev1_0);
			}
			else {
				start_anim_current(&ledanim_boot);
			}
#else
			start_anim_current(&ledanim_boot);
#endif
		}

	} else {
		pr_err("get_mtd_number() failed. default ledanim.\n");
		start_anim_current(&ledanim_boot);
	}
#else
	start_anim_current(&ledanim_boot);
#endif
}

int register_leddrv_func(struct leddrv_func * func)
{
	leddrv_func = func;

	init_timer(&anim_timer);
	anim_timer.function = ledanimation_timer_function;

	_start_boot_animation();

	return 0;
}
EXPORT_SYMBOL(register_leddrv_func);

int register_leddrv_end_func(struct leddrv_func * func)
{
	if(anim_current){
		delete_anim_current();
		del_timer(&anim_timer);
		_set_led_all_off();
	}

	return 0;
}
EXPORT_SYMBOL(register_leddrv_end_func);

