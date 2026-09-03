#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "haptics.h"


/*
 * ------------------------------------------------------------------
 * DRV2605L hardware binding
 * ------------------------------------------------------------------
 *
 * Existing Vision Band overlay:
 *
 * &arduino_i2c {
 *     status = "okay";
 *
 *     drv2605l: drv2605l@5a {
 *         compatible = "ti,drv2605";
 *         reg = <0x5a>;
 *         actuator-mode = "ERM";
 *     };
 * };
 *
 * SDA = P1.02
 * SCL = P1.03
 */
#define DRV2605_NODE DT_NODELABEL(drv2605l)

#if !DT_NODE_EXISTS(DRV2605_NODE)
#error "Vision Band DRV2605L Devicetree node 'drv2605l' is missing"
#endif

static const struct i2c_dt_spec drv2605 =
    I2C_DT_SPEC_GET(DRV2605_NODE);


/*
 * ------------------------------------------------------------------
 * DRV2605L registers used by the already-validated Vision Band RTP path
 * ------------------------------------------------------------------
 */
#define DRV2605_REG_STATUS       0x00
#define DRV2605_REG_MODE         0x01
#define DRV2605_REG_RTP_INPUT    0x02

#define DRV2605_MODE_RTP         0x05

/*
 * Previously validated ERM drive level on the Vision Band prototype.
 */
#define HAPTIC_RTP_LEVEL         0x7F


/*
 * ------------------------------------------------------------------
 * Product haptic timings
 * ------------------------------------------------------------------
 */
#define HAPTIC_CONNECT_MS        100
#define HAPTIC_CAPTURE_MS        150
#define HAPTIC_SUCCESS_MS        150
#define HAPTIC_SUCCESS_GAP_MS    100
#define HAPTIC_RESULT_MS         250
#define HAPTIC_ERROR_MS          500


/*
 * ------------------------------------------------------------------
 * Dedicated asynchronous haptic worker
 * ------------------------------------------------------------------
 *
 * The queue is deliberately small. Haptic feedback is product-state
 * feedback, not a data stream. The worker owns all vibration sleeps so
 * the main camera/BLE path remains free to sustain the 6C.5 throughput.
 */
K_MSGQ_DEFINE(
    haptic_event_queue,
    sizeof(enum haptics_event),
    8,
    4
);

K_THREAD_STACK_DEFINE(
    haptic_worker_stack,
    1024
);

static struct k_thread haptic_worker_thread;

static bool haptics_ready = false;


/*
 * ------------------------------------------------------------------
 * Low-level RTP helpers
 * ------------------------------------------------------------------
 */

static int drv2605_set_rtp(
    uint8_t level
)
{
    return i2c_reg_write_byte_dt(
        &drv2605,
        DRV2605_REG_RTP_INPUT,
        level
    );
}


static int haptic_pulse(
    uint32_t duration_ms
)
{
    int ret;


    ret =
        drv2605_set_rtp(
            HAPTIC_RTP_LEVEL
        );

    if (ret < 0) {

        printk(
            "Haptics: RTP start failed: %d\n",
            ret
        );

        return ret;
    }


    k_msleep(
        duration_ms
    );


    ret =
        drv2605_set_rtp(
            0x00
        );

    if (ret < 0) {

        printk(
            "Haptics: RTP stop failed: %d\n",
            ret
        );

        return ret;
    }


    return 0;
}


static void play_event(
    enum haptics_event event
)
{
    switch (event) {


        case HAPTIC_EVENT_BLE_CONNECTED:

            (void)haptic_pulse(
                HAPTIC_CONNECT_MS
            );

            break;


        case HAPTIC_EVENT_PHOTO_CAPTURED:

            (void)haptic_pulse(
                HAPTIC_CAPTURE_MS
            );

            break;


        case HAPTIC_EVENT_IMAGE_SENT:

            (void)haptic_pulse(
                HAPTIC_SUCCESS_MS
            );

            k_msleep(
                HAPTIC_SUCCESS_GAP_MS
            );

            (void)haptic_pulse(
                HAPTIC_SUCCESS_MS
            );

            break;


        /*
         * Reserved for the next software/API phase:
         *
         * iPhone / PC processes the image, returns a result-ready control
         * command over BLE, and the Vision Band turns that command into
         * tactile confirmation here.
         */
        case HAPTIC_EVENT_RESULT_READY:

            (void)haptic_pulse(
                HAPTIC_RESULT_MS
            );

            break;


        case HAPTIC_EVENT_ERROR:

            (void)haptic_pulse(
                HAPTIC_ERROR_MS
            );

            break;


        default:

            printk(
                "Haptics: unknown event %d\n",
                event
            );

            break;
    }
}


static void haptic_worker(
    void *p1,
    void *p2,
    void *p3
)
{
    enum haptics_event event;


    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);


    while (1) {

        if (
            k_msgq_get(
                &haptic_event_queue,
                &event,
                K_FOREVER
            ) == 0
        ) {

            play_event(
                event
            );
        }
    }
}


/*
 * ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------
 */

int haptics_init(void)
{
    int ret;
    uint8_t status = 0;


    if (!device_is_ready(drv2605.bus)) {

        printk(
            "Haptics: I2C bus not ready\n"
        );

        return -ENODEV;
    }


    /*
     * Read STATUS once as an actual I2C communication check.
     */
    ret =
        i2c_reg_read_byte_dt(
            &drv2605,
            DRV2605_REG_STATUS,
            &status
        );

    if (ret < 0) {

        printk(
            "Haptics: DRV2605L not responding: %d\n",
            ret
        );

        return ret;
    }


    printk(
        "Haptics: DRV2605L STATUS = 0x%02X\n",
        status
    );


    /*
     * Real-Time Playback mode. This is the same direct mode that was
     * already validated on the Vision Band ERM motor.
     */
    ret =
        i2c_reg_write_byte_dt(
            &drv2605,
            DRV2605_REG_MODE,
            DRV2605_MODE_RTP
        );

    if (ret < 0) {

        printk(
            "Haptics: failed to enter RTP mode: %d\n",
            ret
        );

        return ret;
    }


    /*
     * Always begin with the motor stopped.
     */
    ret =
        drv2605_set_rtp(
            0x00
        );

    if (ret < 0) {

        printk(
            "Haptics: failed to clear RTP input: %d\n",
            ret
        );

        return ret;
    }


    k_thread_create(
        &haptic_worker_thread,
        haptic_worker_stack,
        K_THREAD_STACK_SIZEOF(haptic_worker_stack),
        haptic_worker,
        NULL,
        NULL,
        NULL,
        7,
        0,
        K_NO_WAIT
    );


    k_thread_name_set(
        &haptic_worker_thread,
        "vision_haptics"
    );


    haptics_ready =
        true;


    printk(
        "Haptics: asynchronous worker ready\n"
    );


    return 0;
}


int haptics_play(
    enum haptics_event event
)
{
    int ret;


    if (!haptics_ready) {

        return -ENODEV;
    }


    ret =
        k_msgq_put(
            &haptic_event_queue,
            &event,
            K_NO_WAIT
        );

    if (ret < 0) {

        printk(
            "Haptics: event queue full, dropped event %d\n",
            event
        );
    }


    return ret;
}
