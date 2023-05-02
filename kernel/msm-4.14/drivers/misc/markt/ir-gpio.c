
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/of_gpio.h>
#include <linux/sysfs.h>
#include <linux/export.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/kmod.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include "ir-gpio.h"

#define DRIVER_NAME	"ir-gpio"
#define DEVICE_NAME	"ir-gpio"

#define DEBUG_DATA			1
#define DEBUG_DETAIL		0
#define DEBUG_RX_PULSE_TEST	0

struct rx_event_t {	
	s64 duration;
	char value;
};

struct rx_event_buffer_t {
	int count;
//// haksukim : Add L-IR RX 
	unsigned long pulse_total_time;
	struct ir_gpio_rx_data_t rx_data;
////

	struct rx_event_t rx_events[];
};

struct rx_context_t {
	struct task_struct  *rx_thread;
	volatile bool		is_thread_run;
	ktime_t				last_evt_time;
	int					state;

	struct rx_event_buffer_t * evb;
	char *__buffer;
};

struct ir_gpio_t {
	struct gpio_desc	*tx_gpio;
	struct gpio_desc	*rx_gpio;
	struct gpio_desc	*dcdc_en_gpio;
	struct gpio_desc	*rx_en_gpio;
	struct miscdevice	miscdev;

	struct rx_context_t rx_ctx;
	spinlock_t lock;
};



#define IR_RX_EVENT_BUFFER_MAX	(1024*64)
#define IR_RX_TOTAL_TIMEOUT_MSEC	12000	// 12sec
#define IR_RX_PULSE_TIMEOUT_MSEC	125		// 125 msec
//// haksukim : Add L-IR RX 
//#define IR_RX_CARRIER_LENGTH_LIMIT	100000		// 100000 nsec
#define IR_RX_CARRIER_LENGTH_LIMIT	200000			// 200000 nsec
#define IR_RX_CARRIER_AVERAGE_TIME	10				// 10 count
#define IR_RX_SINGLE_TYPE			500000000		// 500ms
////

static int init_rx_event_buffer(struct rx_context_t  * rx_ctx)
{
	int size, i;
	size = sizeof(struct rx_event_buffer_t) + (sizeof(struct rx_event_t) *  IR_RX_EVENT_BUFFER_MAX);
	if(!rx_ctx->__buffer){
		rx_ctx->__buffer = (char *)kmalloc(size, GFP_KERNEL);
		if(!rx_ctx->__buffer){
			pr_err("%s: kmalloc() failed\n", __func__);		
			return -ENOMEM;
		}
	}
	memset(rx_ctx->__buffer, 0, size);
	rx_ctx->evb = (struct rx_event_buffer_t *)rx_ctx->__buffer;
	rx_ctx->evb->count = 0;

//// haksukim : Add L-IR RX 
	rx_ctx->evb->pulse_total_time = 0;
	rx_ctx->evb->rx_data.carrier_time = 0;
	rx_ctx->evb->rx_data.pulse_high_count = 0;
	rx_ctx->evb->rx_data.pulse_low_count = 0;
	rx_ctx->evb->rx_data.single_type = 0;

	for(i = 0 ; i < IRC_PULSE_SIZE; i++){
		rx_ctx->evb->rx_data.pulse_high_time[i] = 0;
		rx_ctx->evb->rx_data.pulse_low_time[i] = 0;
	}
////
	return 0;
}

static void destory_rx_event_buffer(struct rx_context_t  * rx_ctx)
{
	if(rx_ctx->__buffer){
		kfree(rx_ctx->__buffer);
		rx_ctx->__buffer = NULL;		
	}
	rx_ctx->evb = NULL;
}

static inline void rx_event_buffer_put(struct rx_context_t  * rx_ctx, char value)
{
	if(rx_ctx->evb && rx_ctx->evb->count < IR_RX_EVENT_BUFFER_MAX){
		struct rx_event_buffer_t * rx_evb = rx_ctx->evb;
		ktime_t	now = ktime_get();		 
		rx_evb->rx_events[rx_evb->count].duration = ktime_to_ns(ktime_sub(now, rx_ctx->last_evt_time));
		rx_evb->rx_events[rx_evb->count].value = value;
		rx_evb->count++;
		rx_ctx->last_evt_time = now;
	}
}

static inline struct rx_event_t * rx_event_buffer_get(struct rx_context_t  * rx_ctx, int index)
{
	if(rx_ctx->evb && index < IR_RX_EVENT_BUFFER_MAX){
		return &rx_ctx->evb->rx_events[index];
	}
	return NULL;
}

//// haksukim : Add L-IR RX 
static inline void rx_data_put(struct rx_context_t  * rx_ctx, struct rx_event_t  * rx_etx)
{
	struct rx_event_buffer_t * rx_evb = rx_ctx->evb;

	if(rx_etx->duration < IR_RX_CARRIER_LENGTH_LIMIT){
		rx_evb->rx_data.pulse_high_time[rx_evb->rx_data.pulse_high_count] += rx_etx->duration;
	} else if(rx_etx->value == 0) {
		rx_evb->rx_data.pulse_high_count++;
		rx_evb->rx_data.pulse_low_time[rx_evb->rx_data.pulse_low_count] = rx_etx->duration;
		rx_evb->rx_data.pulse_low_count++;
	}

	rx_evb->pulse_total_time += rx_etx->duration;
}

static inline void rx_conveert_ns_to_us(struct rx_context_t  * rx_ctx)
{
	struct rx_event_buffer_t * rx_evb = rx_ctx->evb;

	for(int i = 0 ; i <= rx_evb->rx_data.pulse_high_count; i++){

		rx_evb->rx_data.pulse_high_time[i] = rx_evb->rx_data.pulse_high_time[i] / 1000;
		rx_evb->rx_data.pulse_low_time[i] = rx_evb->rx_data.pulse_low_time[i] / 1000;
	}
}

static inline void rx_get_carrier_time(struct rx_context_t  * rx_ctx)
{
	struct rx_event_buffer_t * rx_evb = rx_ctx->evb;
	int i, count = 0, duration_time = 0;

	for(i = 11; i <= rx_evb->count; i++){
		if((rx_evb->rx_events[i].duration < IR_RX_CARRIER_LENGTH_LIMIT) && (rx_evb->rx_events[i+1].duration < IR_RX_CARRIER_LENGTH_LIMIT)){
			duration_time += rx_evb->rx_events[i].duration;
			i++;
			duration_time += rx_evb->rx_events[i].duration;
			count++;	
		}

		if(count >= IR_RX_CARRIER_AVERAGE_TIME){
			break;
		}
	}

	rx_evb->rx_data.carrier_time = duration_time / IR_RX_CARRIER_AVERAGE_TIME;
}

static inline void rx_get_single_type(struct rx_context_t  * rx_ctx)
{
	struct rx_event_buffer_t * rx_evb = rx_ctx->evb;

	if(rx_evb->pulse_total_time <= IR_RX_SINGLE_TYPE){
		rx_evb->rx_data.single_type = 1;
	}
	else {
		rx_evb->rx_data.single_type = 0;
	}
}

static long rx_pulse_capture_data(struct ir_gpio_t * ir_gpio)
{
	unsigned long flags;
	int i;
	struct rx_event_t * rx;
#if DEBUG_RX_PULSE_TEST
	ktime_t edge;
	s64 delta;
#endif

	if(!ir_gpio) return 0;

	spin_lock_irqsave(&ir_gpio->lock, flags);

	rx = rx_event_buffer_get(&ir_gpio->rx_ctx, 0);

	for(i = 1 ; i < ir_gpio->rx_ctx.evb->count; i++){
		rx = rx_event_buffer_get(&ir_gpio->rx_ctx, i);
		if(!rx) break;
#if DEBUG_DETAIL
		pr_info("buffer rx[%d]->duration=%lld, ->value=%d\n", i, (long long)rx->duration, (int)rx->value);
#endif
		rx_data_put(&ir_gpio->rx_ctx, rx);
	}
	ir_gpio->rx_ctx.evb->rx_data.pulse_high_count++;

	rx_conveert_ns_to_us(&ir_gpio->rx_ctx);

#if DEBUG_DATA
	pr_info("ir_gpio->rx_ctx.evb->rx_data.pulse_high_count = %d\n", ir_gpio->rx_ctx.evb->rx_data.pulse_high_count);
	pr_info("ir_gpio->rx_ctx.evb->rx_data.pulse_low_count = %d\n", ir_gpio->rx_ctx.evb->rx_data.pulse_low_count);

	for(i = 0 ; i <= ir_gpio->rx_ctx.evb->rx_data.pulse_high_count; i++){
		pr_info("ir_gpio->rx_ctx.evb->rx_data.pulse_high_time[%d] = %d\n", i, ir_gpio->rx_ctx.evb->rx_data.pulse_high_time[i]);
		pr_info("ir_gpio->rx_ctx.evb->rx_data.pulse_low_time[%d] = %d\n", i, ir_gpio->rx_ctx.evb->rx_data.pulse_low_time[i]);
	}
#endif

	rx_get_carrier_time(&ir_gpio->rx_ctx);
#if DEBUG_DATA
	pr_info("ir_gpio->rx_ctx.evb->rx_data.carrier_time = %d\n", ir_gpio->rx_ctx.evb->rx_data.carrier_time);
	pr_info("ir_gpio->rx_ctx.evb->pulse_total_time = %d\n", ir_gpio->rx_ctx.evb->pulse_total_time);
#endif

	rx_get_single_type(&ir_gpio->rx_ctx);
#if DEBUG_DATA
	pr_info("ir_gpio->rx_ctx.evb->rx_data.single_type = %d\n", ir_gpio->rx_ctx.evb->rx_data.single_type);
#endif
	spin_unlock_irqrestore(&ir_gpio->lock, flags);

#if DEBUG_RX_PULSE_TEST
	spin_lock_irqsave(&ir_gpio->lock, flags);
	edge = ktime_get();
	for(i = 0; i <= ir_gpio->rx_ctx.evb->rx_data.pulse_high_count; i++){
	
		//
		// pulse
		//
		ktime_t last = ktime_add_ns(edge, ir_gpio->rx_ctx.evb->rx_data.pulse_high_time[i]);
		while (ktime_before(ktime_get(), last)) {
			gpiod_set_value(ir_gpio->tx_gpio, 1);
			edge = ktime_add_ns(edge, 13250);
			delta = ktime_to_ns(ktime_sub(edge, ktime_get()));
			if (delta > 0)
				ndelay(delta);
			gpiod_set_value(ir_gpio->tx_gpio, 0);
			edge = ktime_add_ns(edge, 13250);
			delta = ktime_to_ns(ktime_sub(edge, ktime_get()));
			if (delta > 0)
				ndelay(delta);
		}

		edge = ktime_add_ns(edge, ir_gpio->rx_ctx.evb->rx_data.pulse_low_time[i]);
		while (ktime_before(ktime_get(), edge)) {
			delta = ktime_to_ns(ktime_sub(edge,  ktime_get()));
			if (delta > 10000) {
				ndelay(10000);
			} else if(delta > 0) {
				ndelay(delta);
			}
		}
	}

	gpiod_set_value(ir_gpio->tx_gpio, 0);
	spin_unlock_irqrestore(&ir_gpio->lock, flags);
#endif

	return 0;
}
////

static const struct of_device_id ir_gpio_of_match[] = {
	{ .compatible = "markt,ir-gpio", },
	{ },
};
MODULE_DEVICE_TABLE(of, ir_gpio_of_match);


static int ir_recv_thread(void *data)
{
	struct ir_gpio_t * ir_gpio = (struct ir_gpio_t *)data;	
	struct rx_context_t  * rx_ctx = &ir_gpio->rx_ctx;
	int value, old_value;
	bool is_timeout = false;

	if(!rx_ctx->evb) {
		pr_err("%s: rx event buffer not allocated\n", __func__);
		return -1;
	}
	pr_info("%s: start ir_recv thread.\n", __func__);

	old_value = 1;//gpiod_get_value(ir_gpio->rx_gpio);
	rx_ctx->last_evt_time = ktime_get();
	rx_ctx->evb->count = 0;
	while (rx_ctx->is_thread_run) {
		value = gpiod_get_value(ir_gpio->rx_gpio);
		if(value != old_value){
			rx_event_buffer_put(rx_ctx, (value ? 1 : 0));
			old_value = value;
			rx_ctx->evb->rx_events[0].duration = 0;
			break;
		}
		if(ktime_to_ms(ktime_sub(ktime_get(), rx_ctx->last_evt_time)) > IR_RX_TOTAL_TIMEOUT_MSEC){
			is_timeout = true;
			break;
		}
	}

	if(is_timeout){
		pr_info("%s: stop ir_recv thread. -- timeout\n", __func__);
		return 0;
	}

	while (rx_ctx->is_thread_run) {
		value = gpiod_get_value(ir_gpio->rx_gpio);
		if(value != old_value){
			rx_event_buffer_put(rx_ctx, (value ? 1 : 0));
			old_value = value;
		}
#if 0
		if(ktime_to_ms(ktime_sub(ktime_get(), rx_ctx->last_evt_time)) > IR_RX_PULSE_TIMEOUT_MSEC){
			is_timeout = true;
			break;
		}
#endif
	}
	pr_info("%s: stop ir_recv thread.\n", __func__);
	return 0;
}


static long __gpio_pulse(struct ir_gpio_t * ir_gpio, struct ir_gpio_tx_data_t * tx_data)
{
	int i;
	ktime_t edge;
	s64 delta;
	unsigned long flags;
	unsigned long * txbuf = NULL;
	unsigned long carrier_length;

	txbuf = (unsigned long *)kmalloc(tx_data->pulse_count * sizeof(unsigned long), GFP_KERNEL);
	if(!txbuf){
		pr_err("%s: kmalloc() failed\n", __func__);		
		return -EFAULT;
	}

	carrier_length = tx_data->carrier_low + tx_data->carrier_high;
	for(i = 0 ; i < tx_data->pulse_count; i++){
		txbuf[i] = tx_data->pulse[i] * carrier_length;
#if DEBUG_DETAIL
		pr_info("%s: pulse[%d] = %lu \t txbuf[%d] = %lu\n", __func__, i, tx_data->pulse[i], i, txbuf[i]);
#endif
	}


	spin_lock_irqsave(&ir_gpio->lock, flags);
	edge = ktime_get();
	for(i = 0; i < tx_data->pulse_count; i++){
		if(i % 2) {
			//
			// space
			//
			edge = ktime_add_ns(edge, txbuf[i]);
			while (ktime_before(ktime_get(), edge)) {
				delta = ktime_to_ns(ktime_sub(edge,  ktime_get()));
				if (delta > 10000) {
					ndelay(10000);
				} else if(delta > 0) {
					ndelay(delta);
				}
			}
		} else {
			//
			// pulse
			//
			ktime_t last = ktime_add_ns(edge, txbuf[i]);
			while (ktime_before(ktime_get(), last)) {
				gpiod_set_value(ir_gpio->tx_gpio, 1);
				edge = ktime_add_ns(edge, tx_data->carrier_high);
				delta = ktime_to_ns(ktime_sub(edge, ktime_get()));
				if (delta > 0)
					ndelay(delta);
				gpiod_set_value(ir_gpio->tx_gpio, 0);
				edge = ktime_add_ns(edge, tx_data->carrier_low);
				delta = ktime_to_ns(ktime_sub(edge, ktime_get()));
				if (delta > 0)
					ndelay(delta);
			}
		}
	}
	gpiod_set_value(ir_gpio->tx_gpio, 0);
	spin_unlock_irqrestore(&ir_gpio->lock, flags);	

	kfree(txbuf);
	txbuf = NULL;

	return 0;
}

static long __gpio_pulse_event_buffer(struct ir_gpio_t * ir_gpio)
{
	unsigned long flags;
	ktime_t edge;
	s64 delta;
	int i,cnt=0;
	struct rx_context_t * rx_ctx;
	struct rx_event_t * rx;

	if(!ir_gpio){
		return 0;
	}

	rx_ctx = &ir_gpio->rx_ctx;

	pr_info("start test tx\n");
	spin_lock_irqsave(&ir_gpio->lock, flags);	
	rx = rx_event_buffer_get(&ir_gpio->rx_ctx, 0);
	edge = ktime_get();
	gpiod_set_value(ir_gpio->tx_gpio, !rx->value);
	for(i = 1 ; i < rx_ctx->evb->count; i++){
		rx = rx_event_buffer_get(&ir_gpio->rx_ctx, i);
		if(!rx) break;
		
		cnt++;
		edge = ktime_add_ns(edge, rx->duration);
		while (ktime_before(ktime_get(), edge)) {
			delta = ktime_to_ns(ktime_sub(edge,  ktime_get()));
			if (delta > 10000) {
				ndelay(10000);
			} else if(delta > 0) {
				ndelay(delta);
			}
		}
		gpiod_set_value(ir_gpio->tx_gpio, !rx->value);
	}
	gpiod_set_value(ir_gpio->tx_gpio, 0);
	spin_unlock_irqrestore(&ir_gpio->lock, flags);
	pr_info("end test tx: %d\n", cnt);
	return 0;
}


static long ir_gpio_rx_start(struct ir_gpio_t * ir_gpio)
{
	unsigned long flags;
	if(!ir_gpio) return 0;


	spin_lock_irqsave(&ir_gpio->lock, flags);
	if(ir_gpio->rx_ctx.is_thread_run){
		pr_err("%s: rx already started.\n", __func__);
		spin_unlock_irqrestore(&ir_gpio->lock, flags);
		return -EBUSY;
	}
	init_rx_event_buffer(&ir_gpio->rx_ctx);
	gpiod_set_value(ir_gpio->rx_en_gpio, 1);
	spin_unlock_irqrestore(&ir_gpio->lock, flags);

	ir_gpio->rx_ctx.is_thread_run = true;
	ir_gpio->rx_ctx.rx_thread = kthread_run(ir_recv_thread, ir_gpio, "ir_gpio_rx");
	if (IS_ERR(ir_gpio->rx_ctx.rx_thread)){
		pr_err("%s: kthread_run() failed.\n", __func__);		
		return PTR_ERR(ir_gpio->rx_ctx.rx_thread);
	}
	
	return 0;
}

static long ir_gpio_rx_stop(struct ir_gpio_t * ir_gpio, unsigned long param)
{
	unsigned long flags;
//// haksukim : Add L-IR RX 
	int size;
////

	if(!ir_gpio) return 0;

	spin_lock_irqsave(&ir_gpio->lock, flags);
	ir_gpio->rx_ctx.is_thread_run = false;
	gpiod_set_value(ir_gpio->rx_en_gpio, 0);
	spin_unlock_irqrestore(&ir_gpio->lock, flags);

	pr_info("recv count: %d\n", ir_gpio->rx_ctx.evb->count);


//// haksukim : Add L-IR RX 
	rx_pulse_capture_data(ir_gpio);

	size = sizeof(struct ir_gpio_rx_data_t);
	if (copy_to_user((void __user *)param, &ir_gpio->rx_ctx.evb->rx_data, size)){
		return -EFAULT;
	}
////

	return 0;
}

static long ir_gpio_rx_get_state(struct ir_gpio_t * ir_gpio, unsigned long param)
{
	unsigned long flags;
	struct ir_gpio_rx_state_t state;

	spin_lock_irqsave(&ir_gpio->lock, flags);
	state.is_rx_started = ir_gpio->rx_ctx.is_thread_run ? 1 : 0;
	state.rx_event_count = ir_gpio->rx_ctx.evb->count;
	spin_unlock_irqrestore(&ir_gpio->lock, flags);

	if (copy_to_user((void __user *)param, &state, sizeof(state))){
		return -EFAULT;
	}
	pr_info("%s: rx running? %s. received event count: %d\n", 
			__func__, 
			(state.is_rx_started ? "yes": "no"),
			state.rx_event_count);

	return 0;
}

static long ir_gpio_rx_erase(struct ir_gpio_t * ir_gpio)
{
	unsigned long flags;

	spin_lock_irqsave(&ir_gpio->lock, flags);
	if(ir_gpio->rx_ctx.is_thread_run){
		pr_err("%s: rx started.\n", __func__);
		spin_unlock_irqrestore(&ir_gpio->lock, flags);
		return -EBUSY;
	}
	init_rx_event_buffer(&ir_gpio->rx_ctx);
	spin_unlock_irqrestore(&ir_gpio->lock, flags);

	pr_info("erase OK. received event count: %d\n", ir_gpio->rx_ctx.evb->count);
	return 0;
}

static long ir_gpio_rx_out(struct ir_gpio_t * ir_gpio, unsigned long param)
{
	unsigned long flags;
//// haksukim : Add L-IR RX
	struct ir_gpio_tx_data_t rx_data_tmp, rx_data;	
	int size;
	int rc;
////

	spin_lock_irqsave(&ir_gpio->lock, flags);
	if(ir_gpio->rx_ctx.is_thread_run){
		pr_err("%s: rx started.\n", __func__);
		spin_unlock_irqrestore(&ir_gpio->lock, flags);
		return -EBUSY;
	}
	if(ir_gpio->rx_ctx.evb->count < 128){
		pr_err("%s: invalid received event count -- %d\n", __func__, ir_gpio->rx_ctx.evb->count);
		spin_unlock_irqrestore(&ir_gpio->lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&ir_gpio->lock, flags);

//// haksukim : Add L-IR RX
	//__gpio_pulse_event_buffer(ir_gpio);  //deleate
	memset(&rx_data, 0, sizeof(rx_data));	

	if (copy_from_user(&rx_data_tmp, (int __user *)param, sizeof(struct ir_gpio_tx_data_t))){
		pr_err("%s: copy_from_user() failed\n", __func__);
		return -EFAULT;
	}
	memcpy(&rx_data, &rx_data_tmp, sizeof(struct ir_gpio_tx_data_t));

	pr_info("%s: carrier_low:%u\n", __func__, (unsigned int)rx_data.carrier_low);
	pr_info("%s: carrier_high:%u\n", __func__, (unsigned int)rx_data.carrier_high);
	pr_info("%s: pulse_count:%u\n", __func__, (unsigned int)rx_data.pulse_count);

	if(rx_data.pulse_count == 0 || (rx_data.pulse_count % 2) !=  0){
		pr_err("%s: copy_from_user() failed2\n", __func__);
		return -EFAULT;		
	}

	rx_data.pulse = (unsigned short *)kmalloc(rx_data.pulse_count * sizeof(unsigned short), GFP_KERNEL);
	if(!rx_data.pulse){
		pr_err("%s: kmalloc() failed\n", __func__);
		return -EFAULT;
	}
	if (copy_from_user(rx_data.pulse, (int __user *)rx_data_tmp.pulse, rx_data.pulse_count * sizeof(unsigned short))){
		pr_err("%s: copy_from_user() failed2\n", __func__);
		kfree(rx_data.pulse);
		return -EFAULT;
	}

	if(rx_data.carrier_low == 0 && rx_data.carrier_high == 0 && rx_data.pulse_count == 4 && rx_data.pulse[0] == 99 && rx_data.pulse[1] == 99 && rx_data.pulse[2] == 99 && rx_data.pulse[3] == 99) {
		pr_err("%s: __gpio_pulse_event_buffer\n", __func__);
		rc = __gpio_pulse_event_buffer(ir_gpio);
	} else {
		pr_err("%s: __gpio_pulse\n", __func__);
		rc = __gpio_pulse(ir_gpio, &rx_data);
	}

	if(rx_data.pulse){
		kfree(rx_data.pulse);
		rx_data.pulse = NULL;
	}

	return rc;
////
}
static long ir_gpio_rx_test(struct ir_gpio_t * ir_gpio, unsigned long param)
{
	unsigned long flags;
	int size;

	spin_lock_irqsave(&ir_gpio->lock, flags);
	if(ir_gpio->rx_ctx.is_thread_run){
		pr_err("%s: rx started.\n", __func__);
		spin_unlock_irqrestore(&ir_gpio->lock, flags);
		return -EBUSY;
	}
	if(ir_gpio->rx_ctx.evb->count < 128){
		pr_err("%s: invalid received event count -- %d\n", __func__, ir_gpio->rx_ctx.evb->count);
		spin_unlock_irqrestore(&ir_gpio->lock, flags);
		return -EINVAL;
	}
	
#if 0
	for(i = 0 ; i < ir_gpio->rx_event_buffer.count; i++){
		rx = rx_event_buffer_get(&ir_gpio->rx_event_buffer, i);
		if(!rx) break;
		pr_info("[%d][%lld] %d\n",i, (long long)rx->duration, (int)rx->value);

	}
#endif
	spin_unlock_irqrestore(&ir_gpio->lock, flags);

	__gpio_pulse_event_buffer(ir_gpio);
#if 1
	size = sizeof(struct rx_event_buffer_t) + (sizeof(struct rx_event_t) *  ir_gpio->rx_ctx.evb->count);
	if (copy_to_user((void __user *)param, ir_gpio->rx_ctx.evb, size)){
		return -EFAULT;
	}
#endif

	return 0;
}


static long ir_gpio_tx(struct ir_gpio_t * ir_gpio, unsigned long param)
{
	int rc;
	struct ir_gpio_tx_data_t tx_data_tmp, tx_data;	

	memset(&tx_data, 0, sizeof(tx_data));	

	if (copy_from_user(&tx_data_tmp, (int __user *)param, sizeof(struct ir_gpio_tx_data_t))){
		pr_err("%s: copy_from_user() failed\n", __func__);
		return -EFAULT;
	}
	memcpy(&tx_data, &tx_data_tmp, sizeof(struct ir_gpio_tx_data_t));

	pr_info("%s: carrier_low:%u\n", __func__, (unsigned int)tx_data.carrier_low);
	pr_info("%s: carrier_high:%u\n", __func__, (unsigned int)tx_data.carrier_high);
	pr_info("%s: pulse_count:%u\n", __func__, (unsigned int)tx_data.pulse_count);

	if(tx_data.pulse_count == 0 || (tx_data.pulse_count % 2) !=  0){
		pr_err("%s: copy_from_user() failed2\n", __func__);
		return -EFAULT;		
	}

	tx_data.pulse = (unsigned short *)kmalloc(tx_data.pulse_count * sizeof(unsigned short), GFP_KERNEL);
	if(!tx_data.pulse){
		pr_err("%s: kmalloc() failed\n", __func__);
		return -EFAULT;
	}
	if (copy_from_user(tx_data.pulse, (int __user *)tx_data_tmp.pulse, tx_data.pulse_count * sizeof(unsigned short))){
		pr_err("%s: copy_from_user() failed2\n", __func__);
		kfree(tx_data.pulse);
		return -EFAULT;
	}

	rc = __gpio_pulse(ir_gpio, &tx_data);

	if(tx_data.pulse){
		kfree(tx_data.pulse);
		tx_data.pulse = NULL;
	}


	return rc;
}

static const char * ir_gpio_ioctl_cmd_to_str(unsigned int cmd)
{
	switch (cmd) {
	case IR_GPIO_TX:
		return "IR_GPIO_TX";
	case IR_GPIO_RX_START:
		return "IR_GPIO_RX_START";
	case IR_GPIO_RX_STOP:
		return "IR_GPIO_RX_STOP";
	case IR_GPIO_RX_GET_STATE:
		return "IR_GPIO_RX_GET_STATE";
	case IR_GPIO_RX_ERASE:
		return "IR_GPIO_RX_ERASE";
	case IR_GPIO_RX_OUT:
		return "IR_GPIO_RX_OUT";
	case IR_GPIO_RX_TEST:
		return "IR_GPIO_RX_TEST";
	}
	return "undefined ioctl command";
}

static long ir_gpio_ioctl(struct file *file, unsigned int cmd, unsigned long param)
{
	struct ir_gpio_t * ir_gpio = container_of(file->private_data, struct ir_gpio_t, miscdev);
	long err = -EBADRQC;
	pr_info("ir_gpio_ioctl: %s\n", ir_gpio_ioctl_cmd_to_str(cmd));

	switch (cmd) {
	case IR_GPIO_TX:
		err = ir_gpio_tx(ir_gpio, param);
		break;
	case IR_GPIO_RX_START:
		err = ir_gpio_rx_start(ir_gpio);
		break;
	case IR_GPIO_RX_STOP:
		err = ir_gpio_rx_stop(ir_gpio, param);
		break;
	case IR_GPIO_RX_GET_STATE:
		err = ir_gpio_rx_get_state(ir_gpio, param);
		break;
	case IR_GPIO_RX_ERASE:
		err = ir_gpio_rx_erase(ir_gpio);
		break;
	case IR_GPIO_RX_OUT:
		err = ir_gpio_rx_out(ir_gpio, param);
		break;
	case IR_GPIO_RX_TEST:
		err = ir_gpio_rx_test(ir_gpio, param);
		break;
	}
	return err;
}

static const struct file_operations ir_gpio_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= ir_gpio_ioctl,
};

static int ir_gpio_probe(struct platform_device *pdev)
{
	struct ir_gpio_t *ir_gpio;
	int rc;

	ir_gpio = devm_kzalloc(&pdev->dev, sizeof(*ir_gpio), GFP_KERNEL);
	if (!ir_gpio)
		return -ENOMEM;
	
	//
	// get 'tx-gpios'
	//
	ir_gpio->tx_gpio = devm_gpiod_get(&pdev->dev, "tx", GPIOD_OUT_LOW);
	if (IS_ERR(ir_gpio->tx_gpio)) {
		if (PTR_ERR(ir_gpio->tx_gpio) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to get 'tx' gpio (%ld)\n",
				PTR_ERR(ir_gpio->tx_gpio));
		return PTR_ERR(ir_gpio->tx_gpio);
	}
	gpiod_set_value(ir_gpio->tx_gpio, 0);
	dev_info(&pdev->dev, "tx gpio set to 0\n");

	//
	// get 'dcdc-en-gpios'
	//
	ir_gpio->dcdc_en_gpio = devm_gpiod_get(&pdev->dev, "dcdc-en", GPIOD_OUT_LOW);
	if (IS_ERR(ir_gpio->dcdc_en_gpio)) {
		if (PTR_ERR(ir_gpio->dcdc_en_gpio) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to get 'dcdc-en' gpio (%ld)\n",
				PTR_ERR(ir_gpio->dcdc_en_gpio));
		return PTR_ERR(ir_gpio->dcdc_en_gpio);
	}
	gpiod_set_value(ir_gpio->dcdc_en_gpio, 1);
	dev_info(&pdev->dev, "dcdc-en gpio set to 1\n");


	//
	// setup 'rx-gpios'
	//
	ir_gpio->rx_gpio = devm_gpiod_get(&pdev->dev, "rx", GPIOD_IN);
	if (IS_ERR(ir_gpio->rx_gpio)) {
		if (PTR_ERR(ir_gpio->rx_gpio) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to get 'rx' gpio (%ld)\n",
				PTR_ERR(ir_gpio->rx_gpio));
		return PTR_ERR(ir_gpio->rx_gpio);
	}
	gpiod_direction_input(ir_gpio->rx_gpio);
	dev_info(&pdev->dev, "rx gpio set direction input\n");


	//
	// setup 'rx-en-gpios'
	//
	ir_gpio->rx_en_gpio = devm_gpiod_get(&pdev->dev, "rx-en", GPIOD_OUT_LOW);
	if (IS_ERR(ir_gpio->rx_en_gpio)) {
		if (PTR_ERR(ir_gpio->rx_en_gpio) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Failed to get 'dcdc-en' gpio (%ld)\n",
				PTR_ERR(ir_gpio->rx_en_gpio));
		return PTR_ERR(ir_gpio->rx_en_gpio);
	}
	gpiod_set_value(ir_gpio->rx_en_gpio, 0);
	dev_info(&pdev->dev, "rx-en gpio set to 0\n");

	//
	// lock init
	//
	spin_lock_init(&ir_gpio->lock);

	//
	// init rx buffer
	//
	init_rx_event_buffer(&ir_gpio->rx_ctx);


	//
	// misc device init
	//
	ir_gpio->miscdev.minor = MISC_DYNAMIC_MINOR;
	ir_gpio->miscdev.name = DEVICE_NAME;
	ir_gpio->miscdev.fops = &ir_gpio_fops;
	ir_gpio->miscdev.parent = &pdev->dev;
	rc = misc_register(&ir_gpio->miscdev);

	dev_set_drvdata(&pdev->dev, ir_gpio);

	if (rc){
		dev_err(&pdev->dev, "Unable to register device\n");
		return rc;
	}

	gpiod_set_value(ir_gpio->rx_en_gpio, 1);
	dev_info(&pdev->dev, "rx-en gpio set to 1\n");
	
	dev_info(&pdev->dev, "ir-gpio load ok\n");
	return 0;
}

static int ir_gpio_remove(struct platform_device *pdev)
{
	struct ir_gpio_t *ir_gpio = dev_get_drvdata(&pdev->dev);

	destory_rx_event_buffer(&ir_gpio->rx_ctx);
	misc_deregister(&ir_gpio->miscdev);

	dev_info(&pdev->dev, "ir-gpio remove ok\n");

	return 0;
}

static struct platform_driver ir_gpio_driver = {
	.probe	= ir_gpio_probe,
	.remove = ir_gpio_remove,
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table = of_match_ptr(ir_gpio_of_match),
	},
};
module_platform_driver(ir_gpio_driver);

MODULE_DESCRIPTION("ir gpio driver");
MODULE_AUTHOR("hjkoh");
MODULE_LICENSE("GPL");

