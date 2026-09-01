#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ble_service.h"


#define BLE_RESTART_DELAY_MS 250

/*
 * Vision Band custom GATT UUIDs.
 *
 * Service:
 *   7f510000-1b15-4f0d-8f7b-4c8d4f3a1000
 *
 * TX characteristic (Vision Band -> central, NOTIFY):
 *   7f510001-1b15-4f0d-8f7b-4c8d4f3a1000
 *
 * RX characteristic (central -> Vision Band, WRITE):
 *   7f510002-1b15-4f0d-8f7b-4c8d4f3a1000
 */
#define BT_UUID_VISION_BAND_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x7f510000, 0x1b15, 0x4f0d, 0x8f7b, 0x4c8d4f3a1000)

#define BT_UUID_VISION_BAND_TX_VAL \
    BT_UUID_128_ENCODE(0x7f510001, 0x1b15, 0x4f0d, 0x8f7b, 0x4c8d4f3a1000)

#define BT_UUID_VISION_BAND_RX_VAL \
    BT_UUID_128_ENCODE(0x7f510002, 0x1b15, 0x4f0d, 0x8f7b, 0x4c8d4f3a1000)


static struct bt_uuid_128 vision_band_service_uuid =
    BT_UUID_INIT_128(
        BT_UUID_VISION_BAND_SERVICE_VAL
    );

static struct bt_uuid_128 vision_band_tx_uuid =
    BT_UUID_INIT_128(
        BT_UUID_VISION_BAND_TX_VAL
    );

static struct bt_uuid_128 vision_band_rx_uuid =
    BT_UUID_INIT_128(
        BT_UUID_VISION_BAND_RX_VAL
    );


static bool bluetooth_initialized = false;
static bool service_active = false;
static bool advertising_active = false;
static bool connected_state = false;
static bool tx_notifications_enabled = false;

static struct bt_conn *active_connection = NULL;

static ble_connection_changed_cb_t connection_changed_callback = NULL;


/*
 * Forward declaration because the GATT service references this callback.
 */
static ssize_t rx_write(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    const void *buf,
    uint16_t len,
    uint16_t offset,
    uint8_t flags
);

static void tx_ccc_changed(
    const struct bt_gatt_attr *attr,
    uint16_t value
);


/*
 * Attribute layout:
 *
 * [0] Primary service declaration
 * [1] TX characteristic declaration
 * [2] TX characteristic value       <-- notify from this attribute
 * [3] TX CCC descriptor
 * [4] RX characteristic declaration
 * [5] RX characteristic value
 */
BT_GATT_SERVICE_DEFINE(
    vision_band_service,

    BT_GATT_PRIMARY_SERVICE(
        &vision_band_service_uuid
    ),

    BT_GATT_CHARACTERISTIC(
        &vision_band_tx_uuid.uuid,
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL,
        NULL,
        NULL
    ),

    BT_GATT_CCC(
        tx_ccc_changed,
        BT_GATT_PERM_READ |
        BT_GATT_PERM_WRITE
    ),

    BT_GATT_CHARACTERISTIC(
        &vision_band_rx_uuid.uuid,
        BT_GATT_CHRC_WRITE |
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL,
        rx_write,
        NULL
    )
);


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


static int send_tx_message(
    const char *message
)
{
    int ret;
    size_t length;


    if (
        !connected_state ||
        active_connection == NULL
    ) {

        return -ENOTCONN;
    }


    if (!tx_notifications_enabled) {

        return -EACCES;
    }


    length =
        strlen(
            message
        );


    ret =
        bt_gatt_notify(
            active_connection,
            &vision_band_service.attrs[2],
            message,
            length
        );

    if (ret < 0) {

        printk(
            "BLE TX notify failed: %d\n",
            ret
        );

        return ret;
    }


    printk(
        "BLE TX: %s\n",
        message
    );


    return 0;
}


static void hello_work_handler(
    struct k_work *work
)
{
    ARG_UNUSED(work);


    if (
        connected_state &&
        tx_notifications_enabled
    ) {

        (void)send_tx_message(
            "HELLO_VISION_BAND"
        );
    }
}


K_WORK_DELAYABLE_DEFINE(
    hello_work,
    hello_work_handler
);


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


static void tx_ccc_changed(
    const struct bt_gatt_attr *attr,
    uint16_t value
)
{
    ARG_UNUSED(attr);


    tx_notifications_enabled =
        (value == BT_GATT_CCC_NOTIFY);


    if (tx_notifications_enabled) {

        printk(
            "BLE TX notifications enabled\n"
        );


        /*
         * Defer HELLO very slightly so the CCC write can finish before
         * the first application notification is sent.
         */
        (void)k_work_reschedule(
            &hello_work,
            K_MSEC(50)
        );
    }
    else {

        printk(
            "BLE TX notifications disabled\n"
        );


        (void)k_work_cancel_delayable(
            &hello_work
        );
    }
}


static ssize_t rx_write(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    const void *buf,
    uint16_t len,
    uint16_t offset,
    uint8_t flags
)
{
    const uint8_t *data =
        (const uint8_t *)buf;


    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);


    /*
     * Phase 6B uses complete, short command writes only.
     * Reject prepared/offset writes for now.
     */
    if (offset != 0) {

        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_OFFSET
        );
    }


    printk(
        "BLE RX (%u bytes): ",
        len
    );


    for (
        uint16_t i = 0;
        i < len;
        ++i
    ) {

        printk(
            "%c",
            (
                data[i] >= 32 &&
                data[i] <= 126
            )
                ? data[i]
                : '.'
        );
    }


    printk(
        "\n"
    );


    if (
        len == 4 &&
        memcmp(
            data,
            "PING",
            4
        ) == 0
    ) {

        printk(
            "BLE command PING received\n"
        );


        if (
            send_tx_message(
                "PONG"
            ) < 0
        ) {

            printk(
                "Could not send PONG notification\n"
            );
        }
    }
    else {

        printk(
            "BLE command not recognized\n"
        );
    }


    return len;
}


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


        advertising_active = false;

        schedule_advertising_restart();

        return;
    }


    advertising_active = false;
    connected_state = true;
    tx_notifications_enabled = false;


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
    tx_notifications_enabled = false;


    (void)k_work_cancel_delayable(
        &hello_work
    );


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
        "Vision Band GATT service registered\n"
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


    service_active = false;


    (void)k_work_cancel_delayable(
        &advertising_restart_work
    );

    (void)k_work_cancel_delayable(
        &hello_work
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
