#include <linux/module.h>
#include <linux/ctype.h>
#include <asm/setup.h>

static u8 cmdline_wifi_mac[6] = {0,};

static int __parse_mac_addr(u8* mac, char *str)
{
	int count, i, val;

	for (count = 0; count < 6 && *str; count++, str += 3) {
		if (!isxdigit(str[0]) || !isxdigit(str[1]))
			return 0;
		if (str[2] != ((count < 5) ? ':' : '\0'))
			return 0;

		for (i = 0, val = 0; i < 2; i++) {
			val = val << 4;
			val |= isdigit(str[i]) ?
				str[i] - '0' : toupper(str[i]) - 'A' + 10;
		}
		mac[count] = val;
	}
	return 1;
}

static int __init get_wifi_mac_addr_from_cmdline(char *str)
{
	return __parse_mac_addr(cmdline_wifi_mac, str);
}
__setup("androidboot.wifimacaddr=", get_wifi_mac_addr_from_cmdline);


int get_factory_info_wifi_mac(unsigned char * mac)
{
	if (!memcmp(cmdline_wifi_mac, "\x00\x00\x00\x00\x00\x00", 6)){
		return 1;
	}
	memcpy(mac, cmdline_wifi_mac, 6);
	return 0;
}
EXPORT_SYMBOL(get_factory_info_wifi_mac);
