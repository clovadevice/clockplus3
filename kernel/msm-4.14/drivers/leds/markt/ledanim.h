#ifndef __LEDANIM_H__
#define __LEDANIM_H__


#define LED_NAME_LEN	20

struct ledanim_t {
	char name[LED_NAME_LEN];
	short interval;
	unsigned long anim_count;
	unsigned char * anim;
	unsigned long repeat;
};


struct led_brightness_t {		
	uint8_t r;
	uint8_t g;
	uint8_t b;
};	


struct leddrv_func {
	int num_leds;
	int start_led;
	struct device *dev;
	int (*set_led_brightness)(int index, struct led_brightness_t *rgb_value);
	int (*update)(struct device *dev);
};

int register_leddrv_func(struct leddrv_func * func);
int register_leddrv_end_func(struct leddrv_func * func);

#endif
