
#ifndef _TM1652_IOCTL_H_
#define _TM1652_IOCTL_H_

struct tm1652_auto_addr_mode {
    uint8_t data[5];
    uint8_t ctrl;
    uint8_t reserved;
};

struct tm1652_fixed_addr_mode {
    uint8_t cmd;
    uint8_t data;
};

#define IOCTL_TM1652_MAGIC_NUMBER                   't'
#define IOCTL_TM1652_SEND_AUTO_ADDR_MODE            _IOW(IOCTL_TM1652_MAGIC_NUMBER, 1, struct tm1652_auto_addr_mode)
#define IOCTL_TM1652_SEND_FIXED_ADDR_MODE           _IOW(IOCTL_TM1652_MAGIC_NUMBER, 2, struct tm1652_fixed_addr_mode)
#define IOCTL_TM1652_MAXNR                          2

#endif /* _TM1652_IOCTL_H_  */
