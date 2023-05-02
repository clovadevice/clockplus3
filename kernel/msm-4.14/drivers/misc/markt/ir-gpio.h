#ifndef __IR_GPIO_H__
#define __IR_GPIO_H__

//// haksukim : Add L-IR RX 
#define IRC_PULSE_SIZE	1024
////

struct ir_gpio_tx_data_t {
	unsigned long carrier_low;
	unsigned long carrier_high;
	unsigned short pulse_count;
	unsigned short *pulse;
};

struct ir_gpio_rx_state_t {
	char is_rx_started;
	unsigned long rx_event_count;
};

//// haksukim : Add L-IR RX 
struct ir_gpio_rx_data_t {
	unsigned long carrier_time;
	unsigned long pulse_high_time[IRC_PULSE_SIZE];
	unsigned long pulse_low_time[IRC_PULSE_SIZE];
	unsigned short pulse_high_count;
	unsigned short pulse_low_count;
	unsigned char single_type;
};
////

#define IR_GPIO_TX					_IOWR('R', 1, void *)
#define IR_GPIO_RX_START			_IOWR('R', 2, void *)
#define IR_GPIO_RX_STOP				_IOWR('R', 3, void *)
#define IR_GPIO_RX_GET_STATE		_IOWR('R', 4, void *)
#define IR_GPIO_RX_ERASE			_IOWR('R', 5, void *)
#define IR_GPIO_RX_OUT				_IOWR('R', 6, void *)
#define IR_GPIO_RX_TEST				_IOWR('R', 7, void *)

#endif

