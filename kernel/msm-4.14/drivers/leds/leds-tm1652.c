#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/serdev.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/fcntl.h>
#include <linux/miscdevice.h>
#include <linux/string.h>
#include <linux/reboot.h>
#include <linux/tm1652_ioctl.h>

#define TM1652_DRV_VERSION		"0.1.0"
#define TM1652_DRV_NAME			"tm1652"
#define MISC_MODULE_NAME		"tm1652-misc"


#define TM1652_DEFAULT_BAUDRATE				19200
#define TM1652_PACKET_AUTO_ADDR_LEN			6
#define TM1652_PACKET_CTRL_CMD_LEN			2
#define TM1652_UART_TX_TIMEOUT				100


#define TM1652_ADDR_GR1						0x00
#define TM1652_ADDR_GR2						0x04
#define TM1652_ADDR_GR3						0x02
#define TM1652_ADDR_GR4						0x06
#define TM1652_ADDR_GR5						0x01
#define TM1652_ADDR_GR6						0x05


#define TM1652_PACKET_ADDR_CMD(x)			((x << 5) | 0x08)
#define TM1652_PACKET_CTRL_CMD				0x18

//#define USE_TEST_DISPLAY

#if defined(CONFIG_PRODUCT_IF_S1300N) || defined(CONFIG_PRODUCT_IF_S1500N) || defined(CONFIG_PRODUCT_IF_S1600N)
    #define USE_MTD_NUMBER
#endif

#ifdef USE_MTD_NUMBER
    #define MTD_SYSTEM          20
    #define MTD_RECOVERYFS      14
#endif


static int tm1652_send_auto_addr_packet(struct tm1652_auto_addr_mode* pData);
static int tm1652_send_fixed_addr_packet(struct tm1652_fixed_addr_mode* pData);


struct tm1652_priv {
	struct device *dev;
	struct serdev_device *serdev;
	uint32_t baudrate;	
	uint32_t tx_state;
	struct work_struct tx_work;
	spinlock_t lock;
};

static struct tm1652_priv* g_tm1652;
static DEFINE_MUTEX(tm1652_misc_mutex);

static int tm1652_set_all_on_off(bool on)
{
	struct tm1652_auto_addr_mode packet;
	uint8_t value = 0;
	int i;	
	memset(&packet, 0, sizeof(struct tm1652_auto_addr_mode));
	
	if(on)
		value = 0x7f;
	else
		value = 0;
	
	for(i=0; i< 5; i++)
		packet.data[i] = value;	
	packet.ctrl = 0xfe;
	
	tm1652_send_auto_addr_packet(&packet);

    return 0;
}


#ifdef USE_MTD_NUMBER
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
#endif


static int tm1652_get_display_data(struct tm1652_auto_addr_mode *out)
{
    int ret = -1;
#ifdef USE_MTD_NUMBER
    char mtd_num[3] = {0,};
    unsigned long mtd_value;
#endif

    if(out == NULL)
        goto err;

#ifdef USE_MTD_NUMBER
    ret = get_mtd_number(mtd_num, sizeof(mtd_num) - 1);
    if(ret != 0) {
        pr_err("%s: fail to get mtd number\n", __func__);
        goto default_display_data;
    }

    if(kstrtoul(mtd_num, 0, &mtd_value)) {
        pr_err("%s: invalid mtd number :%s\n", __func__, mtd_num);
        goto default_display_data;
    }
        
    if(mtd_value != MTD_RECOVERYFS)
        goto default_display_data;

    pr_info("%s: set recovery display data\n", __func__); 
    out->data[0] = 0x00;
    out->data[1] = 0x00;
    out->data[2] = 0x00;
    out->data[3] = 0x00;
    out->data[4] = 0x00;
    out->ctrl = 0xfe;
    goto exit;

default_display_data:
#endif
    pr_info("%s: set defailt display data\n", __func__);
	#if 0 // wsshim - test always on
    out->data[0] = 0x40;
    out->data[1] = 0x40;
    out->data[2] = 0x40;
    out->data[3] = 0x40;
	// haksukim Turn on "GR5/SG8".
	////out->data[4] = 0x00;
	out->data[4] = 0x80;
	#else
	out->data[0] = 0x7f;
    out->data[1] = 0x7f;
    out->data[2] = 0x7f;
    out->data[3] = 0x7f;
    out->data[4] = 0x7f;
	#endif
    out->ctrl = 0xfe;
#ifdef USE_MTD_NUMBER
exit:
#endif
    ret = 0;
err:
    return ret;   
}

static int tm1652_set_boot_display(void)
{
    struct tm1652_auto_addr_mode packet;
    memset(&packet, 0, sizeof(struct tm1652_auto_addr_mode));

    if(!tm1652_get_display_data(&packet))
        tm1652_send_auto_addr_packet(&packet);
    else
        pr_err("%s: fail to get boot display data\n", __func__);
    return 0; 
}

static int tm1652_send_auto_addr_packet(struct tm1652_auto_addr_mode* pData)
{
	uint8_t disp_data[TM1652_PACKET_AUTO_ADDR_LEN];
	uint8_t ctrl_data[TM1652_PACKET_CTRL_CMD_LEN];
	int written;
	int i;
	
	disp_data[0] = TM1652_PACKET_ADDR_CMD(TM1652_ADDR_GR1);
	for(i=1; i<TM1652_PACKET_AUTO_ADDR_LEN; i++)
		disp_data[i] = pData->data[i-1];
		
	// haksukim Turn on "GR5/SG8".
	disp_data[TM1652_PACKET_AUTO_ADDR_LEN-1] |= 0x80;

	
	ctrl_data[0] = TM1652_PACKET_CTRL_CMD;
	ctrl_data[1] = pData->ctrl;
	
	serdev_device_write_wakeup(g_tm1652->serdev);
	written = serdev_device_write_buf(g_tm1652->serdev, disp_data, ARRAY_SIZE(disp_data));	
	serdev_device_wait_until_sent(g_tm1652->serdev, TM1652_UART_TX_TIMEOUT);
	mdelay(5);
	
	serdev_device_write_wakeup(g_tm1652->serdev);	
	written = serdev_device_write_buf(g_tm1652->serdev, ctrl_data, ARRAY_SIZE(ctrl_data));
	serdev_device_wait_until_sent(g_tm1652->serdev, TM1652_UART_TX_TIMEOUT);	
	mdelay(20);
	
	pr_debug("%s: Data(%02x %02x %02x %02x %02x %02x) / Ctrl(%02x)", __func__,
					pData->data[0], pData->data[1], pData->data[2],
					pData->data[3], pData->data[4], pData->data[5],
					pData->ctrl);	
	return 0;
}


static int tm1652_send_fixed_addr_packet(struct tm1652_fixed_addr_mode* pData)
{
	int written;
	uint8_t packet[2];
	
	packet[0] = pData->cmd;
	packet[1] = pData->data;

	// haksukim Turn on "GR5/SG8".
	if (packet[0] == 0x28)
		packet[1] = packet[1] | 0x80;

	
	serdev_device_write_wakeup(g_tm1652->serdev);
	written = serdev_device_write_buf(g_tm1652->serdev, packet, ARRAY_SIZE(packet));
	serdev_device_wait_until_sent(g_tm1652->serdev, TM1652_UART_TX_TIMEOUT);
	mdelay(5);
	
	pr_debug("%s: Data(%02x %02x) / Written(%d)\n", __func__,
					packet[0], packet[1], written);	
	
	return 0;
}

static long tm1652_misc_do_ioctl(struct file *file, unsigned int cmd,
				 void __user *arg)
{
	struct tm1652_auto_addr_mode recvAutoData;
	struct tm1652_fixed_addr_mode recvFixedData;
	int ret;
	
	/*
     * extract the type and number bitfields, and don't decode
     * wrong cmds: return ENOTTY (inappropriate ioctl) before access_ok()
     */
	if (_IOC_TYPE(cmd) != IOCTL_TM1652_MAGIC_NUMBER) return -ENOTTY;
    if (_IOC_NR(cmd) > IOCTL_TM1652_MAXNR) return -ENOTTY;
	
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
	case IOCTL_TM1652_SEND_AUTO_ADDR_MODE:
		ret = copy_from_user(&recvAutoData, (struct tm1652_auto_addr_mode*)arg, sizeof(struct tm1652_auto_addr_mode));		
		if(ret) {
			pr_err("%s: fail to copy_from_user(cmd: %s)\n", __func__, "IOCTL_TM1652_SEND_AUTO_ADDR_MODE");
			return -EINVAL;
		}
		tm1652_send_auto_addr_packet(&recvAutoData);		
		break;
	case IOCTL_TM1652_SEND_FIXED_ADDR_MODE:
		ret = copy_from_user(&recvFixedData, (struct tm1652_fixed_addr_mode*)arg, sizeof(struct tm1652_fixed_addr_mode));		
		if(ret) {
			pr_err("%s: fail to copy_from_user(cmd: %s)\n", __func__, "IOCTL_TM1652_SEND_FIXED_ADDR_MODE");
			return -EINVAL;
		}
		tm1652_send_fixed_addr_packet(&recvFixedData);
		break;
	}
	
	return 0;
}

static long tm1652_misc_ioctl_handler(struct file *file, unsigned int cmd,
				 void __user *arg)
{
	int ret;

	ret = mutex_lock_interruptible(&tm1652_misc_mutex);
	if (ret) {
		pr_err("%s: fail to ioctl mutex lock\n", __func__);
		return ret;
	}
	
	ret = tm1652_misc_do_ioctl(file, cmd, arg);
	
	mutex_unlock(&tm1652_misc_mutex);
	
	return ret;
}

static long tm1652_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return tm1652_misc_ioctl_handler(file, cmd, arg);
}


static ssize_t tm1652_misc_read(struct file *file, char *buf,
                        size_t count, loff_t *ppos)
{
    return count;

}

static ssize_t tm1652_misc_write(struct file *file, const char __user *buf,
                        size_t count, loff_t *ppos)
{
    return count;
}


static int tm1652_misc_open(struct inode *inode, struct file *file)
{
    //pr_info("%s: \n", __func__);
    return 0;
}

static int tm1652_misc_close(struct inode *inodep, struct file *file)
{
    //pr_info("%s: \n", __func__);
    return 0;
}


static int tm1652_uart_receive(struct serdev_device *serdev, 
							const unsigned char *data, size_t count)
{
	return 0;
}


static void tm1652_uart_wakeup(struct serdev_device *serdev)
{
	//struct tm1652_priv* priv = serdev_device_get_drvdata(serdev);	
	//pr_info("%s:\n", __func__);
}


static struct serdev_device_ops tm1652_serdev_ops = {
	.receive_buf = tm1652_uart_receive,
    .write_wakeup = tm1652_uart_wakeup,
};


static int tm1652_parse_dt(struct tm1652_priv* priv, struct device_node *np)
{
	uint32_t baudrate;
	int ret = 0;
	
	pr_info("%s: +\n", __func__);
	
	ret = of_property_read_u32(np, "baudrate", &baudrate);
	if(ret) {
		pr_err("%s: Fail to get baudrate and set default baudrate\n", __func__);
		priv->baudrate = TM1652_DEFAULT_BAUDRATE;
	}
	priv->baudrate = baudrate;	
	
	pr_info("%s: -\n", __func__);
	
	return 0;
}


static const struct file_operations tm1652_misc_fops = {
    .owner = THIS_MODULE,
    .read = tm1652_misc_read,
    .write = tm1652_misc_write,
    .unlocked_ioctl = tm1652_misc_ioctl,
    .open = tm1652_misc_open,
    .release = tm1652_misc_close,
};


struct miscdevice tm1652_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = MISC_MODULE_NAME,
    .fops = &tm1652_misc_fops,
};

/*
 * tm1652_halt()
 *
 * called by the reboot notifier chain at shutdown and stops   
 *
 */
static int tm1652_halt(struct notifier_block *, unsigned long, void *);

static struct notifier_block tm1652_notifier = {
    .notifier_call = tm1652_halt,
};
static int notifier_disabled = 0;

static int tm1652_halt(struct notifier_block *nb, unsigned long event, void *buf)
{
	struct tm1652_auto_addr_mode packet;
	
	if (notifier_disabled)
        return NOTIFY_OK;
	
	notifier_disabled = 1;
	
	switch (event) {
	case SYS_RESTART:
		pr_info("%s: SYSTEM RESTART\n", __func__);
		break;
	case SYS_HALT:
		pr_info("%s: SYSTEM HALT\n", __func__);
		break;
	case SYS_POWER_OFF:
		pr_info("%s: SYSTEM POWER OFF\n", __func__);
		break;
	default:
		pr_info("%s: Other Event\n", __func__);
		return NOTIFY_DONE;
	}
	
	tm1652_set_all_on_off(false); // All Off
	
	return NOTIFY_OK;
}



static int tm1652_serdev_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct tm1652_priv *tm1652 = NULL;
	int ret = 0;
	uint32_t baudrate;
	
	pr_info("%s: +\n", __func__);
	
	tm1652 = devm_kzalloc(dev, sizeof(struct tm1652_priv), GFP_KERNEL);
	if(!tm1652) {
		pr_err("%s: fail to allocate device memory", __func__);
		return -ENOMEM;
	}
	
	tm1652->serdev = serdev;
	tm1652->dev = &serdev->dev;	
	tm1652_parse_dt(tm1652, serdev->dev.of_node);
	
	serdev_device_set_drvdata(serdev, tm1652);
	serdev_device_set_client_ops(serdev, &tm1652_serdev_ops);
	
	g_tm1652 = tm1652;
	
	ret = serdev_device_open(serdev) ;
	if(ret) {
		pr_err("%s: Unable to open device\n", __func__);
		goto free;
	}
	
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_ODD);
	if(ret) {
		pr_err("%s: fail to set parity\n", __func__);
	}
	pr_info("%s: success set parity\n", __func__);
	
	baudrate = serdev_device_set_baudrate(serdev, tm1652->baudrate);
	pr_info("%s: set baudrate = %d\n", __func__, baudrate);
	
	serdev_device_set_flow_control(serdev, false);
	
	ret = misc_register(&tm1652_misc_device);
	if(ret) {
		pr_err("%s: fail to misc register...\n", __func__);
	}
	
	serdev_device_write_flush(serdev);

	//tm1652_set_all_on_off(true); // All On
    tm1652_set_boot_display();

	register_reboot_notifier(&tm1652_notifier);
	
	pr_info("%s: -\n", __func__);
	
	return 0;
	
free:
	return ret;
}

static void tm1652_serdev_remove(struct serdev_device *serdev)
{
	pr_info("%s: +\n", __func__);
	
	tm1652_set_all_on_off(false); // All Off	
	unregister_reboot_notifier(&tm1652_notifier);	
	misc_deregister(&tm1652_misc_device);	
	serdev_device_close(serdev);
	pr_info("%s: -\n", __func__);
}


static const struct of_device_id tm1652_serdev_of_match[] = {
    {
     .compatible = "markt,tm1652",
    },
    {}
};
MODULE_DEVICE_TABLE(of, tm1652_serdev_of_match);

static struct serdev_device_driver tm1652_serdev_driver = {
    .probe = tm1652_serdev_probe,
    .remove = tm1652_serdev_remove,
    .driver = {
        .name = TM1652_DRV_NAME,
        .of_match_table = of_match_ptr(tm1652_serdev_of_match),
    },
};

module_serdev_device_driver(tm1652_serdev_driver);


MODULE_DESCRIPTION("markT TM1652 UART Driver");
MODULE_AUTHOR("wsshim <wsshim@markt.co.kr>");
MODULE_LICENSE("GPL");
MODULE_VERSION(TM1652_DRV_VERSION);
