#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_service.h"


#define BLE_RESTART_DELAY_MS 250

#define BLE_IMAGE_PACKET_START 0x01
#define BLE_IMAGE_PACKET_DATA  0x02
#define BLE_IMAGE_PACKET_END   0x03

/*
 * Largest packet buffer used by the application.
 *
 * With ATT MTU 247:
 * notification value capacity = 244 bytes.
 */
#define BLE_IMAGE_PACKET_MAX 244
#define BLE_IMAGE_DATA_HEADER_SIZE 5

#define BLE_NOTIFY_RETRY_DELAY_MS 2
#define BLE_NOTIFY_RETRY_LIMIT 500


/*
 * Phase 6C throughput preferences.
 *
 * BLE connection interval units are 1.25 ms:
 *   12 = 15 ms
 *   24 = 30 ms
 *
 * Windows/iPhone remain free to negotiate the final values.
 */
static const struct bt_le_conn_param fast_connection_params =
    BT_LE_CONN_PARAM_INIT(
        12,
        24,
        0,
        400
    );

static const struct bt_conn_le_phy_param preferred_phy =
    BT_CONN_LE_PHY_PARAM_INIT(
        BT_GAP_LE_PHY_2M,
        BT_GAP_LE_PHY_2M
    );

static const struct bt_conn_le_data_len_param preferred_data_length =
    BT_CONN_LE_DATA_LEN_PARAM_INIT(
        BT_GAP_DATA_LEN_MAX,
        BT_GAP_DATA_TIME_MAX
    );


/*
 * Vision Band custom GATT UUIDs.
 *
 * Service:
 *   7f510000-1b15-4f0d-8f7b-4c8d4f3a1000
 *
 * Control TX (Vision Band -> central, NOTIFY):
 *   7f510001-1b15-4f0d-8f7b-4c8d4f3a1000
 *
 * Control RX (central -> Vision Band, WRITE):
 *   7f510002-1b15-4f0d-8f7b-4c8d4f3a1000
 *
 * Image TX (Vision Band -> central, NOTIFY):
 *   7f510003-1b15-4f0d-8f7b-4c8d4f3a1000
 */
#define BT_UUID_VISION_BAND_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x7f510000, 0x1b15, 0x4f0d, 0x8f7b, 0x4c8d4f3a1000)

#define BT_UUID_VISION_BAND_TX_VAL \
    BT_UUID_128_ENCODE(0x7f510001, 0x1b15, 0x4f0d, 0x8f7b, 0x4c8d4f3a1000)

#define BT_UUID_VISION_BAND_RX_VAL \
    BT_UUID_128_ENCODE(0x7f510002, 0x1b15, 0x4f0d, 0x8f7b, 0x4c8d4f3a1000)

#define BT_UUID_VISION_BAND_IMAGE_TX_VAL \
    BT_UUID_128_ENCODE(0x7f510003, 0x1b15, 0x4f0d, 0x8f7b, 0x4c8d4f3a1000)


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

static struct bt_uuid_128 vision_band_image_tx_uuid =
    BT_UUID_INIT_128(
        BT_UUID_VISION_BAND_IMAGE_TX_VAL
    );


static bool bluetooth_initialized = false;
static bool service_active = false;
static bool advertising_active = false;
static bool connected_state = false;
static bool tx_notifications_enabled = false;
static bool image_notifications_enabled = false;

static uint32_t image_packet_sequence = 0;

/*
 * Phase 6C.2 diagnostics.
 *
 * These values are populated by the Zephyr connection callbacks that
 * are already enabled in prj.conf.  We keep the last negotiated values
 * so one consolidated report can be printed immediately before an image
 * transfer.
 */
static uint16_t diag_conn_interval = 0;
static uint8_t diag_tx_phy = 0;
static uint8_t diag_rx_phy = 0;
static uint16_t diag_tx_data_len = 0;
static uint16_t diag_rx_data_len = 0;

static int64_t diag_image_started_ms = 0;
static uint32_t diag_image_payload_bytes = 0;
static uint32_t diag_notify_busy_retries = 0;

static struct bt_conn *active_connection = NULL;

static ble_connection_changed_cb_t connection_changed_callback = NULL;
static ble_command_received_cb_t command_received_callback = NULL;


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

static void image_tx_ccc_changed(
    const struct bt_gatt_attr *attr,
    uint16_t value
);


/*
 * Attribute layout:
 *
 * [0] Primary service
 *
 * [1] Control TX declaration
 * [2] Control TX value
 * [3] Control TX CCC
 *
 * [4] Control RX declaration
 * [5] Control RX value
 *
 * [6] Image TX declaration
 * [7] Image TX value
 * [8] Image TX CCC
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
    ),

    BT_GATT_CHARACTERISTIC(
        &vision_band_image_tx_uuid.uuid,
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL,
        NULL,
        NULL
    ),

    BT_GATT_CCC(
        image_tx_ccc_changed,
        BT_GATT_PERM_READ |
        BT_GATT_PERM_WRITE
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


static int notify_with_retry(
    const struct bt_gatt_attr *attr,
    const void *data,
    uint16_t length
)
{
    int ret;


    for (
        int attempt = 0;
        attempt < BLE_NOTIFY_RETRY_LIMIT;
        ++attempt
    ) {

        if (
            !connected_state ||
            active_connection == NULL
        ) {

            return -ENOTCONN;
        }


        ret =
            bt_gatt_notify(
                active_connection,
                attr,
                data,
                length
            );


        if (ret == 0) {

            return 0;
        }


        if (
            ret != -ENOMEM &&
            ret != -EAGAIN
        ) {

            return ret;
        }


        /*
         * -ENOMEM / -EAGAIN means the host/controller transmit path is
         * temporarily full. Counting these events lets us distinguish
         * radio/link throughput from camera/SPI throughput.
         */
        diag_notify_busy_retries++;


        k_msleep(
            BLE_NOTIFY_RETRY_DELAY_MS
        );
    }


    return -ETIMEDOUT;
}


static int send_control_message(
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
        notify_with_retry(
            &vision_band_service.attrs[2],
            message,
            (uint16_t)length
        );

    if (ret < 0) {

        printk(
            "BLE control TX failed: %d\n",
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

        (void)send_control_message(
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


    if (
        !service_active ||
        connected_state
    ) {

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
            "BLE control TX notifications enabled\n"
        );


        (void)k_work_reschedule(
            &hello_work,
            K_MSEC(50)
        );
    }
    else {

        printk(
            "BLE control TX notifications disabled\n"
        );


        (void)k_work_cancel_delayable(
            &hello_work
        );
    }
}


static void image_tx_ccc_changed(
    const struct bt_gatt_attr *attr,
    uint16_t value
)
{
    ARG_UNUSED(attr);


    image_notifications_enabled =
        (value == BT_GATT_CCC_NOTIFY);


    if (image_notifications_enabled) {

        printk(
            "BLE image TX notifications enabled\n"
        );
    }
    else {

        printk(
            "BLE image TX notifications disabled\n"
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
            send_control_message(
                "PONG"
            ) < 0
        ) {

            printk(
                "Could not send PONG notification\n"
            );
        }


        return len;
    }


    /*
     * Phase 7B:
     *
     * Forward product-level control commands to main.c instead of
     * coupling BLE transport code directly to haptics or other product
     * behavior. The callback must remain fast/non-blocking.
     */
    if (command_received_callback != NULL) {

        command_received_callback(
            data,
            (size_t)len
        );
    }
    else {

        printk(
            "BLE product command received but no callback is registered\n"
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


    if (
        !service_active ||
        connected_state ||
        advertising_active
    ) {

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


static void request_high_throughput_link(
    struct bt_conn *conn
)
{
    int ret;


    ret =
        bt_conn_le_param_update(
            conn,
            &fast_connection_params
        );

    if (ret < 0) {

        printk(
            "BLE connection parameter request failed: %d\n",
            ret
        );
    }
    else {

        printk(
            "BLE requested 15-30 ms connection interval\n"
        );
    }


    ret =
        bt_conn_le_phy_update(
            conn,
            &preferred_phy
        );

    if (ret < 0) {

        printk(
            "BLE 2M PHY request failed: %d\n",
            ret
        );
    }
    else {

        printk(
            "BLE requested 2M PHY\n"
        );
    }


    ret =
        bt_conn_le_data_len_update(
            conn,
            &preferred_data_length
        );

    if (ret < 0) {

        printk(
            "BLE data length request failed: %d\n",
            ret
        );
    }
    else {

        printk(
            "BLE requested maximum data length\n"
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
    image_notifications_enabled = false;

    diag_conn_interval = 0;
    diag_tx_phy = 0;
    diag_rx_phy = 0;
    diag_tx_data_len = 0;
    diag_rx_data_len = 0;


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


    request_high_throughput_link(
        conn
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
    image_notifications_enabled = false;


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


static void le_param_updated(
    struct bt_conn *conn,
    uint16_t interval,
    uint16_t latency,
    uint16_t timeout
)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(latency);
    ARG_UNUSED(timeout);


    diag_conn_interval =
        interval;


    printk(
        "BLE connection interval now: %u units (%u us)\n",
        interval,
        interval * 1250
    );
}


static void le_phy_updated(
    struct bt_conn *conn,
    struct bt_conn_le_phy_info *param
)
{
    ARG_UNUSED(conn);


    diag_tx_phy =
        param->tx_phy;

    diag_rx_phy =
        param->rx_phy;


    printk(
        "BLE PHY updated: TX=%u RX=%u\n",
        param->tx_phy,
        param->rx_phy
    );
}


static void le_data_len_updated(
    struct bt_conn *conn,
    struct bt_conn_le_data_len_info *info
)
{
    ARG_UNUSED(conn);


    diag_tx_data_len =
        info->tx_max_len;

    diag_rx_data_len =
        info->rx_max_len;


    printk(
        "BLE data length updated: TX=%u RX=%u\n",
        info->tx_max_len,
        info->rx_max_len
    );
}


BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_updated = le_param_updated,
    .le_phy_updated = le_phy_updated,
    .le_data_len_updated = le_data_len_updated,
};


int ble_service_init(
    ble_connection_changed_cb_t connection_changed_cb,
    ble_command_received_cb_t command_received_cb
)
{
    int ret;


    connection_changed_callback =
        connection_changed_cb;

    command_received_callback =
        command_received_cb;


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


bool ble_service_image_ready(void)
{
    return
        service_active &&
        connected_state &&
        image_notifications_enabled &&
        active_connection != NULL;
}


int ble_service_image_begin(
    uint32_t fifo_length
)
{
    uint8_t packet[5];

    uint16_t mtu;


    if (!ble_service_image_ready()) {

        return -EACCES;
    }


    mtu =
        bt_gatt_get_mtu(
            active_connection
        );


    packet[0] =
        BLE_IMAGE_PACKET_START;

    sys_put_le32(
        fifo_length,
        &packet[1]
    );


    image_packet_sequence =
        0;

    diag_image_payload_bytes =
        0;

    diag_notify_busy_retries =
        0;

    diag_image_started_ms =
        k_uptime_get();


    printk(
        "\n=== BLE LINK DIAGNOSTIC ===\n"
    );

    printk(
        "ATT MTU: %u\n",
        mtu
    );

    printk(
        "ATT notification value capacity: %u bytes\n",
        (mtu >= 3) ? (mtu - 3) : 0
    );

    printk(
        "Connection interval: %u units (%u us)\n",
        diag_conn_interval,
        diag_conn_interval * 1250
    );

    printk(
        "PHY: TX=%u RX=%u (2 means LE 2M)\n",
        diag_tx_phy,
        diag_rx_phy
    );

    printk(
        "Data length: TX=%u RX=%u bytes\n",
        diag_tx_data_len,
        diag_rx_data_len
    );

    printk(
        "===========================\n"
    );


    printk(
        "BLE image transfer start\n"
    );

    printk(
        "FIFO upper-bound: %u bytes\n",
        fifo_length
    );


    return notify_with_retry(
        &vision_band_service.attrs[7],
        packet,
        sizeof(packet)
    );
}


int ble_service_image_send(
    const uint8_t *data,
    size_t length
)
{
    uint8_t packet[BLE_IMAGE_PACKET_MAX];

    size_t offset =
        0;

    uint16_t mtu;
    size_t max_notification_value;
    size_t max_payload;


    if (
        data == NULL &&
        length > 0
    ) {

        return -EINVAL;
    }


    if (!ble_service_image_ready()) {

        return -ENOTCONN;
    }


    mtu =
        bt_gatt_get_mtu(
            active_connection
        );


    if (mtu <= 8) {

        return -EMSGSIZE;
    }


    max_notification_value =
        (size_t)mtu - 3;


    if (
        max_notification_value >
        BLE_IMAGE_PACKET_MAX
    ) {

        max_notification_value =
            BLE_IMAGE_PACKET_MAX;
    }


    if (
        max_notification_value <=
        BLE_IMAGE_DATA_HEADER_SIZE
    ) {

        return -EMSGSIZE;
    }


    max_payload =
        max_notification_value -
        BLE_IMAGE_DATA_HEADER_SIZE;


    while (offset < length) {

        size_t remaining =
            length - offset;

        size_t payload_length =
            remaining < max_payload
                ? remaining
                : max_payload;

        int ret;


        packet[0] =
            BLE_IMAGE_PACKET_DATA;


        sys_put_le32(
            image_packet_sequence,
            &packet[1]
        );


        memcpy(
            &packet[BLE_IMAGE_DATA_HEADER_SIZE],
            &data[offset],
            payload_length
        );


        ret =
            notify_with_retry(
                &vision_band_service.attrs[7],
                packet,
                (uint16_t)(
                    BLE_IMAGE_DATA_HEADER_SIZE +
                    payload_length
                )
            );

        if (ret < 0) {

            return ret;
        }


        image_packet_sequence++;

        diag_image_payload_bytes +=
            (uint32_t)payload_length;

        offset +=
            payload_length;


        if (
            image_packet_sequence % 1000U == 0U
        ) {

            int64_t elapsed_ms =
                k_uptime_get() -
                diag_image_started_ms;

            uint32_t kbps_x10 =
                0;


            if (elapsed_ms > 0) {

                /*
                 * kbps x 10 =
                 *   bytes * 8 bits/byte * 10 / milliseconds
                 *
                 * Because 1 kbps == 1 bit/ms.
                 */
                kbps_x10 =
                    (uint32_t)(
                        (
                            (uint64_t)diag_image_payload_bytes *
                            80ULL
                        ) /
                        (uint64_t)elapsed_ms
                    );
            }


            printk(
                "BLE TX progress: packets=%u bytes=%u elapsed=%lld ms rate=%u.%u kbps busy_retries=%u\n",
                image_packet_sequence,
                diag_image_payload_bytes,
                elapsed_ms,
                kbps_x10 / 10U,
                kbps_x10 % 10U,
                diag_notify_busy_retries
            );
        }


    }


    return 0;
}


int ble_service_image_end(
    uint32_t jpeg_length
)
{
    uint8_t packet[9];

    int ret;


    if (!ble_service_image_ready()) {

        return -ENOTCONN;
    }


    packet[0] =
        BLE_IMAGE_PACKET_END;


    sys_put_le32(
        jpeg_length,
        &packet[1]
    );


    sys_put_le32(
        image_packet_sequence,
        &packet[5]
    );


    ret =
        notify_with_retry(
            &vision_band_service.attrs[7],
            packet,
            sizeof(packet)
        );

    if (ret < 0) {

        return ret;
    }


    {
        int64_t elapsed_ms =
            k_uptime_get() -
            diag_image_started_ms;

        uint32_t kbps_x10 =
            0;


        if (elapsed_ms > 0) {

            kbps_x10 =
                (uint32_t)(
                    (
                        (uint64_t)jpeg_length *
                        80ULL
                    ) /
                    (uint64_t)elapsed_ms
                );
        }


        printk(
            "BLE image transfer complete: %u bytes, %u packets\n",
            jpeg_length,
            image_packet_sequence
        );

        printk(
            "BLE sender measured: %lld ms, %u.%u kbps\n",
            elapsed_ms,
            kbps_x10 / 10U,
            kbps_x10 % 10U
        );

        printk(
            "BLE host/controller busy retries: %u\n",
            diag_notify_busy_retries
        );
    }


    return 0;
}
