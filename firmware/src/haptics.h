#ifndef HAPTICS_H
#define HAPTICS_H

/*
 * Vision Band semantic haptic events.
 *
 * Callers describe product events instead of motor timing. This keeps
 * main.c / BLE / camera code independent from DRV2605L register details.
 */
enum haptics_event {
    HAPTIC_EVENT_BLE_CONNECTED = 0,
    HAPTIC_EVENT_PHOTO_CAPTURED,
    HAPTIC_EVENT_IMAGE_SENT,
    HAPTIC_EVENT_RESULT_READY,
    HAPTIC_EVENT_ERROR
};

/*
 * Initializes the DRV2605L RTP path and starts the dedicated haptic worker.
 */
int haptics_init(void);

/*
 * Non-blocking event submission.
 *
 * Returns immediately after queueing the event. Motor timing and k_msleep()
 * calls happen only on the dedicated haptic worker thread, so high-throughput
 * BLE image transfer is never intentionally stalled by vibration timing.
 */
int haptics_play(enum haptics_event event);

#endif
