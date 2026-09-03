#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Product-level callbacks.
 *
 * ble_service.c owns the BLE protocol only. Product behaviors such as
 * LEDs and haptics remain in main.c.
 */
typedef void (*ble_connection_changed_cb_t)(bool connected);

typedef void (*ble_command_received_cb_t)(
    const uint8_t *data,
    size_t length
);

int ble_service_init(
    ble_connection_changed_cb_t connection_changed_cb,
    ble_command_received_cb_t command_received_cb
);

int ble_service_start(void);

int ble_service_stop(void);

bool ble_service_is_connected(void);

bool ble_service_is_active(void);

/*
 * True only when the central is connected AND subscribed to the
 * dedicated image notification characteristic.
 */
bool ble_service_image_ready(void);

/*
 * Phase 6C image transport.
 *
 * Protocol on IMAGE_TX:
 *
 *   0x01 + uint32_le fifo_length
 *       image start
 *
 *   0x02 + uint32_le sequence + JPEG bytes
 *       image data
 *
 *   0x03 + uint32_le jpeg_length + uint32_le packet_count
 *       image end
 */
int ble_service_image_begin(
    uint32_t fifo_length
);

int ble_service_image_send(
    const uint8_t *data,
    size_t length
);

int ble_service_image_end(
    uint32_t jpeg_length
);

#endif
