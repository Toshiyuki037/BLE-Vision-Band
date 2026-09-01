#include "rgb_led.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define RED_NODE   DT_ALIAS(vision_red)
#define GREEN_NODE DT_ALIAS(vision_green)
#define BLUE_NODE  DT_ALIAS(vision_blue)

static const struct gpio_dt_spec red =
	GPIO_DT_SPEC_GET(RED_NODE, gpios);

static const struct gpio_dt_spec green =
	GPIO_DT_SPEC_GET(GREEN_NODE, gpios);

static const struct gpio_dt_spec blue =
	GPIO_DT_SPEC_GET(BLUE_NODE, gpios);

static void set_rgb(int r, int g, int b)
{
	gpio_pin_set_dt(&red, r);
	gpio_pin_set_dt(&green, g);
	gpio_pin_set_dt(&blue, b);
}

int rgb_led_init(void)
{
	if (!gpio_is_ready_dt(&red) ||
	    !gpio_is_ready_dt(&green) ||
	    !gpio_is_ready_dt(&blue)) {
		return -1;
	}

	if (gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE) < 0 ||
	    gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE) < 0 ||
	    gpio_pin_configure_dt(&blue, GPIO_OUTPUT_INACTIVE) < 0) {
		return -1;
	}

	return 0;
}

void rgb_led_off(void)
{
	set_rgb(0, 0, 0);
}

void rgb_led_red(void)
{
	set_rgb(1, 0, 0);
}

void rgb_led_green(void)
{
	set_rgb(0, 1, 0);
}

void rgb_led_blue(void)
{
	set_rgb(0, 0, 1);
}

void rgb_led_white(void)
{
	set_rgb(1, 1, 1);
}