#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "haptics.h"

#define BUTTON1_NODE DT_ALIAS(sw0)
#define BUTTON2_NODE DT_ALIAS(sw1)
#define BUTTON3_NODE DT_ALIAS(sw2)

static const struct gpio_dt_spec button1 =
	GPIO_DT_SPEC_GET(BUTTON1_NODE, gpios);

static const struct gpio_dt_spec button2 =
	GPIO_DT_SPEC_GET(BUTTON2_NODE, gpios);

static const struct gpio_dt_spec button3 =
	GPIO_DT_SPEC_GET(BUTTON3_NODE, gpios);

static void wait_for_release(const struct gpio_dt_spec *button)
{
	while (gpio_pin_get_dt(button) > 0) {
		k_msleep(10);
	}
}

int main(void)
{
	if (haptics_init() < 0) {
		printk("Haptics init failed\n");
		return 0;
	}

	if (!gpio_is_ready_dt(&button1) ||
	    !gpio_is_ready_dt(&button2) ||
	    !gpio_is_ready_dt(&button3)) {
		printk("Button GPIO not ready\n");
		return 0;
	}

	gpio_pin_configure_dt(&button1, GPIO_INPUT);
	gpio_pin_configure_dt(&button2, GPIO_INPUT);
	gpio_pin_configure_dt(&button3, GPIO_INPUT);

	printk("Haptic test ready\n");
	printk("BUTTON1 = capture\n");
	printk("BUTTON2 = TX complete\n");
	printk("BUTTON3 = result received\n");

	while (1) {

		if (gpio_pin_get_dt(&button1) > 0) {
			printk("Capture feedback\n");
			haptics_capture_ok();
			wait_for_release(&button1);
		}

		if (gpio_pin_get_dt(&button2) > 0) {
			printk("TX feedback\n");
			haptics_tx_ok();
			wait_for_release(&button2);
		}

		if (gpio_pin_get_dt(&button3) > 0) {
			printk("Result feedback\n");
			haptics_result_received();
			wait_for_release(&button3);
		}

		k_msleep(10);
	}

	return 0;
}