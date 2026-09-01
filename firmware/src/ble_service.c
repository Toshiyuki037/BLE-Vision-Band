#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <errno.h>
#include <stdbool.h>

#include "ble_service.h"


#define BLE_RESTART_DELAY_MS 250


static bool bluetooth_initialized = false;
static bool service_active = false;
static bool advertising_active = false;
static bool connected_state = false;

static struct bt_conn *active_connection = NULL;

static ble_connection_changed_cb_t connection_changed_callback = NULL;


/*
 * Advertising packet:
 *
 * - General Discoverable
 * - BR/EDR not supported
 * - Complete device name from CONFIG_BT_DEVICE_NAME
 */
static const struct bt_data advertising_data[] = {

    BT_DATA_BYTES(
        BT_DATA_FLAGS,
        (
            BT_LE_AD_GENERAL |
            BT_LE_AD_NO_BREDR
        )
    ),

    BT_DATA(
        BT_DATA_NAME_COMPLETE,
        CONFIG_BT_DEVICE_NAME,
        sizeof(CONFIG_BT_DEVICE_NAME) - 1
    ),
};


static int start_advertising_now(void);


/*
 * Bluetooth disconnect callbacks run inside Bluetooth host context.
 *
 * Restarting connectable advertising immediately from that callback
 * can race the stack's connection teardown. We therefore defer the
 * restart slightly to Zephyr's system workqueue.
 */
static void advertising_restart_work_handler(
    struct k_work *work
)
{
    ARG_UNUSED(work);


    if (!service_active) {

        return;
    }


    if (connected_state) {

        return;
    }


    printk(
        "Restarting BLE advertising after disconnect...\n"
    );


    (void)start_advertising_now();
}


K_WORK_DELAYABLE_DEFINE(
    advertising_restart_work,
    advertising_restart_work_handler
);


static int start_advertising_now(void)
{
    int ret;


    if (!bluetooth_initialized) {

        return -ENODEV;
    }


    if (!service_active) {

        return 0;
    }


    if (connected_state) {

        return 0;
    }


    if (advertising_active) {

        return 0;
    }


    ret =
        bt_le_adv_start(
            BT_LE_ADV_CONN_FAST_1,
            advertising_data,
            ARRAY_SIZE(advertising_data),
            NULL,
            0
        );

    if (ret < 0) {

        printk(
            "BLE advertising start failed: %d\n",
            ret
        );

        return ret;
    }


    advertising_active = true;


    printk(
        "BLE advertising as \"%s\"\n",
        CONFIG_BT_DEVICE_NAME
    );


    return 0;
}


static void schedule_advertising_restart(void)
{
    int ret;


    if (!service_active) {

        return;
    }


    ret =
        k_work_reschedule(
            &advertising_restart_work,
            K_MSEC(BLE_RESTART_DELAY_MS)
        );

    if (ret < 0) {

        printk(
            "BLE advertising restart scheduling failed: %d\n",
            ret
        );
    }
}


static void connected(
    struct bt_conn *conn,
    uint8_t err
)
{
    if (err) {

        printk(
            "BLE connection failed: 0x%02X\n",
            err
        );


        /*
         * A failed connection attempt can stop connectable
         * advertising. Defer the restart until the controller has
         * finished cleaning up the failed attempt.
         */
        advertising_active = false;

        schedule_advertising_restart();

        return;
    }


    /*
     * Legacy connectable advertising stops when a connection is
     * established.
     */
    advertising_active = false;
    connected_state = true;


    /*
     * There should be no pending advertising restart while connected.
     */
    (void)k_work_cancel_delayable(
        &advertising_restart_work
    );


    if (active_connection != NULL) {

        bt_conn_unref(
            active_connection
        );

        active_connection = NULL;
    }


    active_connection =
        bt_conn_ref(
            conn
        );


    printk(
        "BLE connected\n"
    );


    if (connection_changed_callback != NULL) {

        connection_changed_callback(
            true
        );
    }
}


static void disconnected(
    struct bt_conn *conn,
    uint8_t reason
)
{
    ARG_UNUSED(conn);


    connected_state = false;
    advertising_active = false;


    if (active_connection != NULL) {

        bt_conn_unref(
            active_connection
        );

        active_connection = NULL;
    }


    printk(
        "BLE disconnected: reason 0x%02X\n",
        reason
    );


    if (connection_changed_callback != NULL) {

        connection_changed_callback(
            false
        );
    }


    /*
     * If the Vision Band is still logically ON, become
     * discoverable/connectable again.
     *
     * Do not restart advertising directly inside the disconnect
     * callback; defer it a short time so the Bluetooth stack can
     * finish tearing down the old connection first.
     */
    schedule_advertising_restart();
}


BT_CONN_CB_DEFINE(connection_callbacks) = {

    .connected =
        connected,

    .disconnected =
        disconnected,
};


int ble_service_init(
    ble_connection_changed_cb_t connection_changed_cb
)
{
    int ret;


    connection_changed_callback =
        connection_changed_cb;


    printk(
        "Initializing Bluetooth...\n"
    );


    ret =
        bt_enable(
            NULL
        );

    if (ret < 0) {

        printk(
            "Bluetooth init failed: %d\n",
            ret
        );

        return ret;
    }


    bluetooth_initialized = true;


    printk(
        "Bluetooth initialized\n"
    );

    printk(
        "BLE standby: advertising disabled until system power ON\n"
    );


    return 0;
}


int ble_service_start(void)
{
    if (!bluetooth_initialized) {

        printk(
            "BLE start ignored: Bluetooth is not initialized\n"
        );

        return -ENODEV;
    }


    if (service_active) {

        /*
         * If logical power is already ON but advertising somehow
         * stopped while disconnected, restore it.
         */
        if (
            !connected_state &&
            !advertising_active
        ) {

            return start_advertising_now();
        }


        return 0;
    }


    service_active = true;


    printk(
        "BLE enabled for logical system ON\n"
    );


    return start_advertising_now();
}


int ble_service_stop(void)
{
    int ret = 0;


    if (!bluetooth_initialized) {

        return -ENODEV;
    }


    /*
     * Set this first so neither the disconnect callback nor a pending
     * work item can restart advertising after logical power OFF.
     */
    service_active = false;


    (void)k_work_cancel_delayable(
        &advertising_restart_work
    );


    if (advertising_active) {

        ret =
            bt_le_adv_stop();

        if (ret < 0) {

            printk(
                "BLE advertising stop failed: %d\n",
                ret
            );
        }
        else {

            advertising_active = false;

            printk(
                "BLE advertising stopped\n"
            );
        }
    }


    if (
        connected_state &&
        active_connection != NULL
    ) {

        int disconnect_ret;


        printk(
            "Disconnecting BLE central for system power OFF\n"
        );


        disconnect_ret =
            bt_conn_disconnect(
                active_connection,
                BT_HCI_ERR_REMOTE_USER_TERM_CONN
            );

        if (disconnect_ret < 0) {

            printk(
                "BLE disconnect request failed: %d\n",
                disconnect_ret
            );

            if (ret == 0) {

                ret =
                    disconnect_ret;
            }
        }
    }


    printk(
        "BLE disabled for logical system OFF\n"
    );


    return ret;
}


bool ble_service_is_connected(void)
{
    return connected_state;
}


bool ble_service_is_active(void)
{
    return service_active;
}
