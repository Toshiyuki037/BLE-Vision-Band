#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <stdbool.h>

#include "ble_service.h"


static bool connected_state = false;
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

        return;
    }


    connected_state = true;


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


    printk(
        "BLE disconnected: reason 0x%02X\n",
        reason
    );


    if (connection_changed_callback != NULL) {

        connection_changed_callback(
            false
        );
    }
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


    printk(
        "Bluetooth initialized\n"
    );


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


    printk(
        "BLE advertising as \"%s\"\n",
        CONFIG_BT_DEVICE_NAME
    );


    return 0;
}


bool ble_service_is_connected(void)
{
    return connected_state;
}
