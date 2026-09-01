#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "rgb_led.h"

int main(void)
{
	if (rgb_led_init() < 0) {
		printk("RGB LED init failed\n");
		return 0;
	}

	printk("RGB LED test starting\n");

	while (1) {
		printk("RED\n");
		rgb_led_red();
		k_msleep(1000);

		printk("GREEN\n");
		rgb_led_green();
		k_msleep(1000);

		printk("BLUE\n");
		rgb_led_blue();
		k_msleep(1000);

		printk("WHITE\n");
		rgb_led_white();
		k_msleep(1000);

		printk("OFF\n");
		rgb_led_off();
		k_msleep(1000);
	}

	return 0;
}