#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include <stdbool.h>

#include "power_leds.h"
#include "rgb_led.h"
#include "camera.h"


#define BUTTON_NODE DT_ALIAS(sw0)

static const struct gpio_dt_spec button =
    GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);


#define POLL_MS               10
#define DEBOUNCE_MS           40
#define POWER_PROGRESS_MS     250
#define FULL_HOLD_MS          1500
#define RELEASE_DEBOUNCE_MS   120
#define POWER_ACTION_LOCKOUT_MS 400


static bool system_on = false;
static int64_t capture_lockout_until = 0;


static void wait_for_stable_button_release(void)
{
    int64_t released_at = 0;

    while (1) {

        if (!gpio_pin_get_dt(&button)) {

            if (released_at == 0) {
                released_at = k_uptime_get();
            }

            if ((k_uptime_get() - released_at) >= RELEASE_DEBOUNCE_MS) {
                return;
            }

        }
        else {
            released_at = 0;
        }

        k_msleep(POLL_MS);
    }
}


/*
 * ------------------------------------------------------------------
 * Capture one image only when the Vision Band is logically powered on.
 *
 * The camera itself is initialized once at boot. A short BUTTON1 press
 * while system_on == true performs:
 *
 *      autofocus -> one 5MP capture -> JPEG export over COM4
 *
 * No boot-time capture is performed.
 * ------------------------------------------------------------------
 */
static void handle_short_press(void)
{
    int ret;

    if (k_uptime_get() < capture_lockout_until) {

        printk(
            "Short press ignored: power transition lockout\n"
        );

        return;
    }

    if (!system_on) {

        printk(
            "Short press ignored: system is OFF\n"
        );

        power_leds_off();
        rgb_led_off();

        return;
    }


    /*
     * Keep the ON indicators asserted while the camera is working.
     */
    power_leds_all_on();
    rgb_led_white();


    printk(
        "BUTTON1 short press: starting camera capture\n"
    );


    ret = camera_capture_and_dump();

    if (ret < 0) {

        printk(
            "Camera capture/export failed: %d\n",
            ret
        );

        /*
         * The logical system remains ON even if a capture fails.
         */
        power_leds_all_on();
        rgb_led_white();

        return;
    }


    printk(
        "Camera capture/export complete\n"
    );


    /*
     * Restore normal powered-on indicators.
     */
    power_leds_all_on();
    rgb_led_white();
}


int main(void)
{
    int64_t press_start;
    int64_t held_ms;
    bool long_press_handled;


    /*
     * ==============================================================
     * Power LEDs
     * ==============================================================
     */

    if (power_leds_init() < 0) {

        printk(
            "Power LED init failed\n"
        );

        return 0;
    }


    /*
     * ==============================================================
     * RGB status LED
     * ==============================================================
     */

    if (rgb_led_init() < 0) {

        printk(
            "Status LED init failed\n"
        );

        return 0;
    }


    /*
     * ==============================================================
     * Camera initialization
     * ==============================================================
     *
     * Initialize the camera interface once at firmware startup.
     * Do NOT capture here.
     *
     * Actual image capture is triggered later by a short BUTTON1
     * press, and only while system_on == true.
     */

    if (camera_init() < 0) {

        printk(
            "Camera init failed\n"
        );

        return 0;
    }


    printk(
        "Camera interface initialized\n"
    );


    /*
     * ==============================================================
     * Button
     * ==============================================================
     */

    if (!gpio_is_ready_dt(&button)) {

        printk(
            "Button device not ready\n"
        );

        return 0;
    }


    if (
        gpio_pin_configure_dt(
            &button,
            GPIO_INPUT
        ) < 0
    ) {

        printk(
            "Button configuration failed\n"
        );

        return 0;
    }


    /*
     * Initial logical system state.
     */
    system_on = false;

    power_leds_off();
    rgb_led_off();


    printk(
        "Vision Band controller ready\n"
    );

    printk(
        "System state: OFF\n"
    );

    printk(
        "Long press BUTTON1: power ON/OFF\n"
    );

    printk(
        "Short press BUTTON1 while ON: capture image\n"
    );


    /*
     * ==============================================================
     * Main runtime
     * ==============================================================
     */

    while (1) {


        if (gpio_pin_get_dt(&button)) {

            press_start =
                k_uptime_get();

            long_press_handled =
                false;


            /*
             * Button remains pressed.
             */
            while (
                gpio_pin_get_dt(&button)
            ) {

                held_ms =
                    k_uptime_get()
                    -
                    press_start;


                /*
                 * Do not start the power animation immediately.
                 *
                 * This prevents a normal short camera press from
                 * visibly draining/filling the power LEDs.
                 */
                if (
                    held_ms >=
                    POWER_PROGRESS_MS
                ) {

                    /*
                     * OFF -> fill LEDs.
                     */
                    if (!system_on) {

                        power_leds_show_power_on_progress(
                            held_ms
                        );
                    }

                    /*
                     * ON -> drain LEDs.
                     */
                    else {

                        power_leds_show_power_off_progress(
                            held_ms
                        );
                    }
                }


                /*
                 * Long press reached: toggle logical power state.
                 */
                if (
                    held_ms >=
                    FULL_HOLD_MS
                ) {

                    long_press_handled =
                        true;


                    if (!system_on) {

                        system_on =
                            true;

                        power_leds_all_on();
                        rgb_led_white();


                        printk(
                            "System state: ON\n"
                        );

                        printk(
                            "Camera short-press capture enabled\n"
                        );
                    }
                    else {

                        system_on =
                            false;

                        power_leds_off();
                        rgb_led_off();


                        printk(
                            "System state: OFF\n"
                        );

                        printk(
                            "Camera short-press capture disabled\n"
                        );
                    }


                    /*
                     * Do not let the release/bounce from a power hold
                     * become a camera short press.
                     */
                    wait_for_stable_button_release();

                    capture_lockout_until =
                        k_uptime_get()
                        +
                        POWER_ACTION_LOCKOUT_MS;


                    break;
                }


                k_msleep(
                    POLL_MS
                );
            }


            /*
             * If the press did not become a long press, it is a
             * short-press action.
             */
            if (!long_press_handled) {

                held_ms =
                    k_uptime_get()
                    -
                    press_start;


                /*
                 * Ignore switch bounce / accidental ultra-short pulses.
                 */
                if (
                    held_ms >=
                    DEBOUNCE_MS
                ) {

                    handle_short_press();
                }
                else {

                    /*
                     * Restore whichever visual state was active.
                     */
                    if (system_on) {

                        power_leds_all_on();
                        rgb_led_white();
                    }
                    else {

                        power_leds_off();
                        rgb_led_off();
                    }
                }
            }


            /*
             * Small post-release debounce.
             */
            k_msleep(
                50
            );
        }


        k_msleep(
            POLL_MS
        );
    }


    return 0;
}
