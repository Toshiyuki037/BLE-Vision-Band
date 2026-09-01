#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include <stdbool.h>

typedef void (*ble_connection_changed_cb_t)(bool connected);

int ble_service_init(
    ble_connection_changed_cb_t connection_changed_cb
);

int ble_service_start(void);

int ble_service_stop(void);

bool ble_service_is_connected(void);

bool ble_service_is_active(void);

#endif
