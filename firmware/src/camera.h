#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>
#include <stdint.h>

struct camera_stream_sink {
    int (*begin)(
        uint32_t fifo_length,
        void *context
    );

    int (*write)(
        const uint8_t *data,
        size_t length,
        void *context
    );

    /*
     * Optional event hook fired after autofocus + sensor capture are
     * complete and a valid JPEG is present in the Arducam FIFO, but
     * BEFORE BLE transport starts.
     *
     * This is the correct point for the wearable's "photo captured"
     * haptic: the user can stop holding the camera still, while JPEG
     * transmission continues independently.
     */
    void (*capture_complete)(
        void *context
    );

    int (*end)(
        uint32_t jpeg_length,
        void *context
    );

    void *context;
};

int camera_init(void);

int camera_capture_test(void);

/*
 * Legacy COM4 debug path retained for bench diagnostics.
 */
int camera_dump_jpeg_binary(void);

int camera_capture_and_dump(void);

/*
 * Capture one high-quality 5MP JPEG and stream it through a
 * transport-independent sink.
 *
 * The camera layer:
 *   autofocuses
 *   captures
 *   reads the Arducam FIFO
 *   strips FIFO padding after JPEG FF D9
 *
 * The transport layer decides what to do with the JPEG bytes.
 */
int camera_capture_and_stream(
    const struct camera_stream_sink *sink
);

#endif
