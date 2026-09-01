#include "power_leds.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define LED1_NODE DT_ALIAS(power_led1)
#define LED2_NODE DT_ALIAS(power_led2)
#define LED3_NODE DT_ALIAS(power_led3)
#define LED4_NODE DT_ALIAS(power_led4)

#define LED_STEP_MS 375

static const struct gpio_dt_spec led1 =
    GPIO_DT_SPEC_GET(LED1_NODE, gpios);

static const struct gpio_dt_spec led2 =
    GPIO_DT_SPEC_GET(LED2_NODE, gpios);

static const struct gpio_dt_spec led3 =
    GPIO_DT_SPEC_GET(LED3_NODE, gpios);

static const struct gpio_dt_spec led4 =
    GPIO_DT_SPEC_GET(LED4_NODE, gpios);

int power_leds_init(void)
{
    if (!gpio_is_ready_dt(&led1) ||
        !gpio_is_ready_dt(&led2) ||
        !gpio_is_ready_dt(&led3) ||
        !gpio_is_ready_dt(&led4)) {
        return -1;
    }

    if (gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure_dt(&led3, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure_dt(&led4, GPIO_OUTPUT_INACTIVE) < 0) {
        return -1;
    }

    return 0;
}

void power_leds_set(int l1, int l2, int l3, int l4)
{
    gpio_pin_set_dt(&led1, l1);
    gpio_pin_set_dt(&led2, l2);
    gpio_pin_set_dt(&led3, l3);
    gpio_pin_set_dt(&led4, l4);
}

void power_leds_off(void)
{
    power_leds_set(0, 0, 0, 0);
}

void power_leds_all_on(void)
{
    power_leds_set(1, 1, 1, 1);
}

void power_leds_show_power_on_progress(int64_t held_ms)
{
    if (held_ms < LED_STEP_MS) {
        power_leds_set(1, 0, 0, 0);
    }
    else if (held_ms < (LED_STEP_MS * 2)) {
        power_leds_set(1, 1, 0, 0);
    }
    else if (held_ms < (LED_STEP_MS * 3)) {
        power_leds_set(1, 1, 1, 0);
    }
    else {
        power_leds_set(1, 1, 1, 1);
    }
}

void power_leds_show_power_off_progress(int64_t held_ms)
{
    if (held_ms < LED_STEP_MS) {
        power_leds_set(1, 1, 1, 1);
    }
    else if (held_ms < (LED_STEP_MS * 2)) {
        power_leds_set(1, 1, 1, 0);
    }
    else if (held_ms < (LED_STEP_MS * 3)) {
        power_leds_set(1, 1, 0, 0);
    }
    else {
        power_leds_set(1, 0, 0, 0);
    }
}