#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "power_leds.h"
#include "rgb_led.h"

#define BUTTON_NODE DT_ALIAS(sw0)

static const struct gpio_dt_spec button =
    GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

#define POLL_MS       10
#define FULL_HOLD_MS  1500

static bool system_on = false;

int main(void)
{
    int64_t press_start;
    int64_t held_ms;

    /*
     * Initialize LEDs 1-4.
     */
    if (power_leds_init() < 0) {
        printk("Power LED init failed\n");
        return 0;
    }

    /*
     * Initialize LED5 RGB status LED.
     */
    if (rgb_led_init() < 0) {
        printk("Status LED init failed\n");
        return 0;
    }

    /*
     * Initialize Button 1.
     */
    if (!gpio_is_ready_dt(&button)) {
        printk("Button device not ready\n");
        return 0;
    }

    if (gpio_pin_configure_dt(&button, GPIO_INPUT) < 0) {
        printk("Button configuration failed\n");
        return 0;
    }

    /*
     * Device begins in OFF state.
     */
    power_leds_off();
    rgb_led_off();

    printk("Vision Band power controller ready\n");
    printk("System state: OFF\n");

    while (1) {

        /*
         * Button 1 pressed.
         */
        if (gpio_pin_get_dt(&button)) {

            press_start = k_uptime_get();

            if (!system_on) {
                printk("Power-on hold started\n");
            }
            else {
                printk("Power-off hold started\n");
            }

            /*
             * Continue updating the LED bar
             * while Button 1 remains held.
             */
            while (gpio_pin_get_dt(&button)) {

                held_ms = k_uptime_get() - press_start;

                /*
                 * Device currently OFF:
                 * fill LEDs from left to right.
                 */
                if (!system_on) {

                    power_leds_show_power_on_progress(held_ms);

                }

                /*
                 * Device currently ON:
                 * drain LEDs from right to left.
                 */
                else {

                    power_leds_show_power_off_progress(held_ms);

                }

                /*
                 * 1.5-second hold completed.
                 */
                if (held_ms >= FULL_HOLD_MS) {

                    /*
                     * OFF -> ON
                     */
                    if (!system_on) {

                        /*
                         * Lock power bar fully ON.
                         */
                        power_leds_all_on();

                        /*
                         * LED5 WHITE means:
                         *
                         * Device powered
                         * but BLE not connected.
                         */
                        rgb_led_white();

                        system_on = true;

                        printk("System state: ON\n");
                        printk("Status LED: WHITE\n");
                    }

                    /*
                     * ON -> OFF
                     */
                    else {

                        /*
                         * All five LEDs off.
                         */
                        power_leds_off();
                        rgb_led_off();

                        system_on = false;

                        printk("System state: OFF\n");
                        printk("Status LED: OFF\n");
                    }

                    /*
                     * Wait until Button 1 is released.
                     *
                     * Without this, continuing to hold
                     * the button could immediately begin
                     * the opposite power transition.
                     */
                    while (gpio_pin_get_dt(&button)) {
                        k_msleep(POLL_MS);
                    }

                    break;
                }

                k_msleep(POLL_MS);
            }

            /*
             * Check how long the button was held.
             *
             * A release before 1.5 seconds means
             * the power operation was cancelled.
             */
            held_ms = k_uptime_get() - press_start;

            if (held_ms < FULL_HOLD_MS) {

                /*
                 * Device was already ON.
                 *
                 * Restore all power LEDs and
                 * leave LED5 white.
                 */
                if (system_on) {

                    power_leds_all_on();
                    rgb_led_white();

                    printk("Power-off cancelled\n");
                }

                /*
                 * Device was OFF.
                 *
                 * Return everything to OFF.
                 */
                else {

                    power_leds_off();
                    rgb_led_off();

                    printk("Power-on cancelled\n");
                }
            }

            /*
             * Simple debounce delay.
             */
            k_msleep(50);
        }

        k_msleep(POLL_MS);
    }

    return 0;
}