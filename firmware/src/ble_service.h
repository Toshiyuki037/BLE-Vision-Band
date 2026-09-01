#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include <stdbool.h>

typedef void (*ble_connection_changed_cb_t)(bool connected);

/*
 * Initialize the Bluetooth stack only.
 *
 * This does NOT start advertising. The Vision Band remains invisible
 * and cannot be connected to until ble_service_start() is called.
 */
int ble_service_init(
    ble_connection_changed_cb_t connection_changed_cb
);

/*
 * Make Vision Band discoverable/connectable.
 *
 * Intended to be called when the logical device powers ON.
 */
int ble_service_start(void);

/*
 * Make Vision Band non-discoverable/non-connectable and disconnect
 * an existing BLE central if one is connected.
 *
 * Intended to be called when the logical device powers OFF.
 */
int ble_service_stop(void);

bool ble_service_is_connected(void);

bool ble_service_is_active(void);

#endif
