#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include <stdbool.h>

/*
 * Called whenever the BLE connection state changes.
 *
 * connected == true  -> a central connected
 * connected == false -> the connection was lost
 */
typedef void (*ble_connection_changed_cb_t)(bool connected);

int ble_service_init(ble_connection_changed_cb_t connection_changed_cb);

bool ble_service_is_connected(void);

#endif
