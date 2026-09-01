#include "camera.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/*
 * WBLE Vision Band - Arducam Mega 5MP
 * High-detail document/vision capture configuration.
 *
 * Current validation target:
 * - 2592x1944 JPEG
 * - high JPEG quality
 * - AE / AGC / AWB enabled
 * - autofocus command enabled
 * - AF status diagnostic via sensor register 0x3029
 * - capture continues if AF status bridge remains 0x00 so image quality
 *   can be evaluated independently from the status-read path
 */

#define CAMERA_SPI_NODE   DT_NODELABEL(spi4)
#define CONSOLE_UART_NODE DT_CHOSEN(zephyr_console)

static const struct device *camera_spi = DEVICE_DT_GET(CAMERA_SPI_NODE);
static const struct device *console_uart = DEVICE_DT_GET(CONSOLE_UART_NODE);

static const struct gpio_dt_spec camera_cs = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
    .pin = 5,
    .dt_flags = GPIO_ACTIVE_LOW
};

static const struct spi_config camera_spi_cfg = {
    .frequency = 1000000,
    .operation = SPI_OP_MODE_MASTER |
                 SPI_TRANSFER_MSB |
                 SPI_WORD_SET(8),
    .slave = 0
};

/* Arducam controller registers */
#define ARDUCHIP_TEST               0x00
#define ARDUCHIP_FIFO               0x04
#define CAM_REG_SENSOR_RESET        0x07
#define CAM_REG_DEBUG_DEVICE_ADDR   0x0A
#define CAM_REG_DEBUG_REG_HIGH      0x0B
#define CAM_REG_DEBUG_REG_LOW       0x0C
#define CAM_REG_FORMAT              0x20
#define CAM_REG_CAPTURE_RESOLUTION  0x21
#define CAM_REG_WHITE_BALANCE_MODE  0x26
#define CAM_REG_AUTO_FOCUS_CONTROL  0x29
#define CAM_REG_IMAGE_QUALITY       0x2A
#define CAM_REG_AE_GAIN_AWB_CONTROL 0x30
#define BURST_FIFO_READ             0x3C
#define CAM_REG_SENSOR_ID           0x40
#define CAM_REG_YEAR_ID             0x41
#define CAM_REG_MONTH_ID            0x42
#define CAM_REG_DAY_ID              0x43
#define ARDUCHIP_TRIG               0x44
#define FIFO_SIZE1                  0x45
#define FIFO_SIZE2                  0x46
#define FIFO_SIZE3                  0x47
#define SENSOR_DATA                 0x48
#define CAM_REG_FPGA_VERSION        0x49

/* Camera values */
#define CAM_SENSOR_RESET_ENABLE     0x40
#define CAM_I2C_READ_MODE           0x01
#define FIFO_CLEAR_ID_MASK          0x01
#define FIFO_START_MASK             0x02
#define CAP_DONE_MASK               0x04
#define CAM_IMAGE_PIX_FMT_JPEG      0x01
#define CAM_IMAGE_QUALITY_HIGH      0x00
#define CAM_WHITE_BALANCE_AUTO      0x00
#define CAM_AUTO_GAIN_ENABLE        0x80
#define CAM_AUTO_EXPOSURE_ENABLE    0x81
#define CAM_AUTO_WB_ENABLE          0x82

/* Full-resolution mode: legacy sensor 0x81 maps 2592x1944 to 0x09. */
#define CAM_IMAGE_MODE_FULL_5MP     0x0D
#define LEGACY_FULL_5MP_MODE        0x09

/* Autofocus */
/*
 * Autofocus control sequence used by Arducam's own full_featured example:
 *
 *   setAutoFocus(0x00);
 *   setAutoFocus(0x02);
 *
 * Their SDK's setAutoFocus() is a direct write to register 0x29 followed by
 * waitI2cIdle(), so we mirror that exact sequence instead of using 0x01.
 */
#define CAM_AUTOFOCUS_TRIGGER       0x00
#define CAM_AUTOFOCUS_RUN           0x02
#define CAM_AF_STATUS_REGISTER      0x3029
#define CAM_AF_FINISHED             0x10

/* Sensor IDs */
#define SENSOR_5MP_1                0x81
#define SENSOR_3MP_1                0x82
#define SENSOR_5MP_2                0x83
#define SENSOR_3MP_2                0x84
#define SENSOR_5MP                  0x85
#define SENSOR_3MP                  0x86
#define SENSOR_2MP                  0x87

/* Timing */
#define CAMERA_IDLE_TIMEOUT_MS      3000
#define CAMERA_CAPTURE_TIMEOUT_MS   8000
#define CAMERA_AF_TIMEOUT_MS        4000
#define CAMERA_CHUNK_SIZE           128

static uint8_t camera_sensor_id = 0;

static void camera_cs_low(void)
{
    gpio_pin_set_dt(&camera_cs, 1);
}

static void camera_cs_high(void)
{
    gpio_pin_set_dt(&camera_cs, 0);
}

static int camera_spi_transfer_byte(uint8_t tx, uint8_t *rx)
{
    struct spi_buf tx_buf = {.buf = &tx, .len = 1};
    struct spi_buf rx_buf = {.buf = rx, .len = 1};
    struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
    struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

    return spi_transceive(camera_spi, &camera_spi_cfg, &tx_set, &rx_set);
}

static int camera_spi_transfer_block(const uint8_t *tx, uint8_t *rx, size_t length)
{
    struct spi_buf tx_buf = {.buf = (void *)tx, .len = length};
    struct spi_buf rx_buf = {.buf = rx, .len = length};
    struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
    struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

    return spi_transceive(camera_spi, &camera_spi_cfg, &tx_set, &rx_set);
}

static void camera_uart_write_byte(uint8_t value)
{
    uart_poll_out(console_uart, value);
}

static void camera_uart_write_string(const char *text)
{
    while (*text != '\0') {
        camera_uart_write_byte((uint8_t)*text++);
    }
}

static int camera_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t rx;
    int ret;

    camera_cs_low();

    ret = camera_spi_transfer_byte(reg | 0x80, &rx);
    if (ret < 0) {
        camera_cs_high();
        return ret;
    }

    ret = camera_spi_transfer_byte(value, &rx);
    camera_cs_high();
    return ret;
}

static int camera_read_reg(uint8_t reg, uint8_t *value)
{
    uint8_t rx;
    int ret;

    camera_cs_low();

    ret = camera_spi_transfer_byte(reg & 0x7F, &rx);
    if (ret < 0) {
        camera_cs_high();
        return ret;
    }

    ret = camera_spi_transfer_byte(0x00, &rx);
    if (ret < 0) {
        camera_cs_high();
        return ret;
    }

    ret = camera_spi_transfer_byte(0x00, &rx);
    camera_cs_high();

    if (ret < 0) {
        return ret;
    }

    *value = rx;
    return 0;
}

static int camera_wait_idle(void)
{
    int64_t start = k_uptime_get();

    while (1) {
        uint8_t state;
        int ret = camera_read_reg(ARDUCHIP_TRIG, &state);

        if (ret < 0) {
            return ret;
        }

        if ((state & 0x03) == 0x02) {
            return 0;
        }

        if ((k_uptime_get() - start) >= CAMERA_IDLE_TIMEOUT_MS) {
            printk("Camera idle timeout, state=0x%02X\n", state);
            return -1;
        }

        k_msleep(1);
    }
}

static int camera_spi_self_test(void)
{
    uint8_t value;
    int ret;

    printk("Testing physical Arducam SPI link...\n");

    ret = camera_write_reg(ARDUCHIP_TEST, 0x55);
    if (ret < 0) {
        return ret;
    }

    k_msleep(1);

    ret = camera_read_reg(ARDUCHIP_TEST, &value);
    if (ret < 0) {
        return ret;
    }

    printk("Arducam TEST register returned: 0x%02X\n", value);

    if (value != 0x55) {
        printk("ARDUCAM SPI TEST FAILED\n");
        return -1;
    }

    printk("ARDUCAM SPI TEST PASSED\n");
    return 0;
}

/*
 * Read a register inside the image sensor through the Arducam debug bridge.
 * This mirrors the current SDK transaction ordering used for AF status.
 */
static int camera_read_sensor_reg(uint16_t sensor_reg, uint8_t *value)
{
    int ret;
    uint8_t register_high = (sensor_reg >> 8) & 0xFF;
    uint8_t register_low = sensor_reg & 0xFF;

    ret = camera_write_reg(CAM_REG_DEBUG_REG_HIGH, register_high);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    ret = camera_write_reg(CAM_REG_DEBUG_REG_LOW, register_low);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    ret = camera_write_reg(CAM_REG_SENSOR_RESET, CAM_I2C_READ_MODE);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    k_msleep(5);

    return camera_read_reg(SENSOR_DATA, value);
}

/*
 * Trigger autofocus and diagnose status.
 *
 * For this validation phase, status 0x00 does NOT abort capture. That lets us
 * determine whether the physical lens focused even if the sensor-register
 * status bridge still needs correction.
 */
static int camera_autofocus(void)
{
    int ret;
    int64_t start;
    uint8_t status = 0;
    uint8_t last_status = 0xFF;

    if (camera_sensor_id != SENSOR_5MP_1 &&
        camera_sensor_id != SENSOR_5MP_2) {
        printk("Autofocus skipped for sensor 0x%02X\n", camera_sensor_id);
        return 0;
    }

    printk("Starting Arducam autofocus sequence (0x00 -> 0x02)...\n");

    /*
     * Mirror Arducam's own full_featured example:
     *
     *     setAutoFocus(0);
     *     setAutoFocus(0x02);
     *
     * cameraSetAutoFocus() in the official SDK simply writes the supplied
     * value to CAM_REG_AUTO_FOCUS_CONTROL (0x29) and waits for I2C idle.
     */

    ret = camera_write_reg(
        CAM_REG_AUTO_FOCUS_CONTROL,
        CAM_AUTOFOCUS_TRIGGER
    );

    if (ret < 0) {
        printk("Autofocus trigger write failed: %d\n", ret);
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    ret = camera_write_reg(
        CAM_REG_AUTO_FOCUS_CONTROL,
        CAM_AUTOFOCUS_RUN
    );

    if (ret < 0) {
        printk("Autofocus run write failed: %d\n", ret);
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    /*
     * Give the AF engine enough time to start moving the VCM before reading
     * sensor status register 0x3029.
     */
    k_msleep(100);

    start = k_uptime_get();

    while ((k_uptime_get() - start) < CAMERA_AF_TIMEOUT_MS) {
        ret = camera_read_sensor_reg(
            CAM_AF_STATUS_REGISTER,
            &status
        );

        if (ret < 0) {
            printk("Autofocus status read failed: %d\n", ret);
            return ret;
        }

        /*
         * Avoid flooding COM4 with the same value dozens of times.
         * Print only when the status changes.
         */
        if (status != last_status) {
            printk("AF status changed: 0x%02X\n", status);
            last_status = status;
        }

        if (status == CAM_AF_FINISHED) {
            printk("Autofocus locked (0x10)\n");

            /*
             * Allow the voice-coil lens to mechanically settle before the
             * exposure is triggered.
             */
            k_msleep(75);

            return 0;
        }

        k_msleep(20);
    }

    printk(
        "Autofocus timeout: status never reached 0x10 "
        "(last=0x%02X)\n",
        status
    );

    /*
     * Keep this non-fatal for one more image-quality run. If the corrected
     * 0x00 -> 0x02 sequence still leaves status at 0x00, the next diagnosis
     * is the sensor debug-read/firmware behavior rather than guessing AF
     * command values again.
     */
    printk("Continuing capture so lens behavior can be verified visually\n");

    k_msleep(150);

    return 0;
}

static int camera_configure_auto_image_controls(void)
{
    int ret;

    printk("Enabling automatic exposure...\n");
    ret = camera_write_reg(CAM_REG_AE_GAIN_AWB_CONTROL,
                           CAM_AUTO_EXPOSURE_ENABLE);
    if (ret < 0) {
        return ret;
    }
    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    printk("Enabling automatic gain...\n");
    ret = camera_write_reg(CAM_REG_AE_GAIN_AWB_CONTROL,
                           CAM_AUTO_GAIN_ENABLE);
    if (ret < 0) {
        return ret;
    }
    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    printk("Enabling automatic white balance...\n");
    ret = camera_write_reg(CAM_REG_AE_GAIN_AWB_CONTROL,
                           CAM_AUTO_WB_ENABLE);
    if (ret < 0) {
        return ret;
    }
    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    ret = camera_write_reg(CAM_REG_WHITE_BALANCE_MODE,
                           CAM_WHITE_BALANCE_AUTO);
    if (ret < 0) {
        return ret;
    }
    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    k_msleep(300);

    printk("Automatic image controls ready\n");
    return 0;
}

static int camera_configure_document_capture(void)
{
    int ret;
    uint8_t resolution;

    printk("Setting JPEG format...\n");

    ret = camera_write_reg(CAM_REG_FORMAT, CAM_IMAGE_PIX_FMT_JPEG);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    printk("JPEG format accepted\n");

    printk("Setting HIGH JPEG quality...\n");

    ret = camera_write_reg(CAM_REG_IMAGE_QUALITY, CAM_IMAGE_QUALITY_HIGH);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    printk("HIGH JPEG quality accepted\n");

    if (camera_sensor_id < SENSOR_5MP) {
        resolution = LEGACY_FULL_5MP_MODE;
    } else {
        resolution = CAM_IMAGE_MODE_FULL_5MP;
    }

    printk("Setting resolution to 2592x1944 (mode 0x%02X)...\n",
           resolution);

    ret = camera_write_reg(CAM_REG_CAPTURE_RESOLUTION, resolution);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    printk("2592x1944 resolution accepted\n");

    k_msleep(200);
    return 0;
}

static int camera_get_fifo_length(uint32_t *length)
{
    uint8_t low;
    uint8_t mid;
    uint8_t high;
    int ret;

    ret = camera_read_reg(FIFO_SIZE1, &low);
    if (ret < 0) {
        return ret;
    }

    ret = camera_read_reg(FIFO_SIZE2, &mid);
    if (ret < 0) {
        return ret;
    }

    ret = camera_read_reg(FIFO_SIZE3, &high);
    if (ret < 0) {
        return ret;
    }

    *length = ((uint32_t)high << 16) |
              ((uint32_t)mid << 8) |
              low;

    *length &= 0x00FFFFFF;
    return 0;
}

static int camera_wait_capture_done(void)
{
    int64_t start = k_uptime_get();

    while (1) {
        uint8_t state;
        int ret = camera_read_reg(ARDUCHIP_TRIG, &state);

        if (ret < 0) {
            return ret;
        }

        if (state & CAP_DONE_MASK) {
            return 0;
        }

        if ((k_uptime_get() - start) >= CAMERA_CAPTURE_TIMEOUT_MS) {
            printk("Camera capture timeout, state=0x%02X\n", state);
            return -1;
        }

        k_msleep(5);
    }
}

static int camera_start_capture(void)
{
    int ret;

    printk("Clearing camera FIFO...\n");

    ret = camera_write_reg(ARDUCHIP_FIFO, FIFO_CLEAR_ID_MASK);
    if (ret < 0) {
        return ret;
    }

    k_msleep(20);

    printk("Starting capture...\n");

    ret = camera_write_reg(ARDUCHIP_FIFO, FIFO_START_MASK);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_capture_done();
    if (ret < 0) {
        return ret;
    }

    printk("Capture complete\n");
    return 0;
}

/*
 * Canonical Arducam FIFO reader.
 *
 * Each block:
 *   CS low
 *   send 0x3C
 *   first block only: one dummy byte
 *   read block
 *   CS high
 */
typedef int (*camera_fifo_callback_t)(
    const uint8_t *data,
    size_t length,
    void *context
);

static int camera_read_fifo(
    uint32_t length,
    camera_fifo_callback_t callback,
    void *context)
{
    uint8_t tx[CAMERA_CHUNK_SIZE];
    uint8_t rx[CAMERA_CHUNK_SIZE];
    uint32_t remaining = length;
    uint32_t total_read = 0;
    bool first_burst = true;

    memset(tx, 0x00, sizeof(tx));

    while (remaining > 0) {
        size_t chunk = remaining > CAMERA_CHUNK_SIZE
                           ? CAMERA_CHUNK_SIZE
                           : remaining;
        uint8_t dummy;
        int ret;

        camera_cs_low();

        ret = camera_spi_transfer_byte(BURST_FIFO_READ, &dummy);
        if (ret < 0) {
            camera_cs_high();
            return ret;
        }

        if (first_burst) {
            ret = camera_spi_transfer_byte(0x00, &dummy);
            if (ret < 0) {
                camera_cs_high();
                return ret;
            }

            first_burst = false;
        }

        ret = camera_spi_transfer_block(tx, rx, chunk);
        camera_cs_high();

        if (ret < 0) {
            printk("FIFO SPI read failed at byte %u\n", total_read);
            return ret;
        }

        ret = callback(rx, chunk, context);
        if (ret < 0) {
            return ret;
        }

        total_read += chunk;
        remaining -= chunk;
    }

    return 0;
}

struct jpeg_validation_state {
    uint32_t position;
    uint8_t first_byte;
    uint8_t second_byte;
    uint8_t previous_byte;
    bool eoi_found;
};

static int camera_validate_callback(
    const uint8_t *data,
    size_t length,
    void *context)
{
    struct jpeg_validation_state *state = context;

    for (size_t i = 0; i < length; i++) {
        uint8_t value = data[i];

        if (state->position == 0) {
            state->first_byte = value;
        } else if (state->position == 1) {
            state->second_byte = value;
        }

        if (state->position > 0 &&
            state->previous_byte == 0xFF &&
            value == 0xD9) {
            state->eoi_found = true;
        }

        state->previous_byte = value;
        state->position++;
    }

    return 0;
}

static int camera_validate_jpeg(uint32_t length)
{
    struct jpeg_validation_state state = {0};
    int ret;

    if (length < 4) {
        return -1;
    }

    printk("Reading JPEG FIFO...\n");

    ret = camera_read_fifo(length, camera_validate_callback, &state);
    if (ret < 0) {
        return ret;
    }

    printk("JPEG first bytes: %02X %02X\n",
           state.first_byte,
           state.second_byte);

    if (state.first_byte != 0xFF ||
        state.second_byte != 0xD8) {
        printk("JPEG START MARKER FAILED\n");
        return -1;
    }

    printk("JPEG START MARKER VALID\n");

    if (!state.eoi_found) {
        printk("JPEG END MARKER FAILED\n");
        return -1;
    }

    printk("JPEG END MARKER VALID\n");
    printk("JPEG MARKERS VALID\n");

    return 0;
}

int camera_init(void)
{
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint8_t fpga;
    int ret;

    if (!device_is_ready(camera_spi)) {
        printk("Camera SPI device not ready\n");
        return -1;
    }

    if (!device_is_ready(console_uart)) {
        printk("Console UART not ready\n");
        return -1;
    }

    if (!gpio_is_ready_dt(&camera_cs)) {
        printk("Camera CS GPIO not ready\n");
        return -1;
    }

    ret = gpio_pin_configure_dt(&camera_cs, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    camera_cs_high();
    k_msleep(100);

    printk("Camera SPI ready\n");
    printk("Camera SPI pins: SCK=P1.08 MOSI=P1.07 MISO=P1.06 CS=P1.05\n");

    ret = camera_spi_self_test();
    if (ret < 0) {
        printk("Camera communication failed\n");
        return ret;
    }

    printk("Camera communication verified\n");
    printk("Resetting camera...\n");

    ret = camera_write_reg(CAM_REG_SENSOR_RESET, CAM_SENSOR_RESET_ENABLE);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    k_msleep(100);
    printk("Camera reset complete\n");

    ret = camera_read_reg(CAM_REG_SENSOR_ID, &camera_sensor_id);
    if (ret < 0) {
        return ret;
    }

    printk("Camera sensor ID: 0x%02X\n", camera_sensor_id);

    ret = camera_read_reg(CAM_REG_YEAR_ID, &year);
    if (ret < 0) {
        return ret;
    }

    ret = camera_read_reg(CAM_REG_MONTH_ID, &month);
    if (ret < 0) {
        return ret;
    }

    ret = camera_read_reg(CAM_REG_DAY_ID, &day);
    if (ret < 0) {
        return ret;
    }

    ret = camera_read_reg(CAM_REG_FPGA_VERSION, &fpga);
    if (ret < 0) {
        return ret;
    }

    printk("Camera firmware date: %u/%u/%u\n", month, day, year);
    printk("Camera FPGA version: 0x%02X\n", fpga);

    switch (camera_sensor_id) {
    case SENSOR_5MP_1:
    case SENSOR_5MP_2:
        printk("Legacy 5MP autofocus Arducam detected\n");
        break;

    case SENSOR_5MP:
        printk("5MP Arducam detected\n");
        break;

    default:
        printk("Unexpected camera sensor ID: 0x%02X\n",
               camera_sensor_id);
        return -1;
    }

    ret = camera_write_reg(CAM_REG_DEBUG_DEVICE_ADDR, 0x78);
    if (ret < 0) {
        return ret;
    }

    ret = camera_wait_idle();
    if (ret < 0) {
        return ret;
    }

    ret = camera_configure_auto_image_controls();
    if (ret < 0) {
        printk("Automatic image-control configuration failed\n");
        return ret;
    }

    printk("Camera initialized for high-detail vision\n");
    return 0;
}

int camera_capture_test(void)
{
    uint32_t jpeg_length;
    int ret;

    printk("\n=== 5MP CAMERA QUALITY TEST ===\n");

    ret = camera_configure_document_capture();
    if (ret < 0) {
        return ret;
    }

    ret = camera_autofocus();
    if (ret < 0) {
        printk("Autofocus validation FAILED\n");
        return ret;
    }

    ret = camera_start_capture();
    if (ret < 0) {
        return ret;
    }

    ret = camera_get_fifo_length(&jpeg_length);
    if (ret < 0) {
        return ret;
    }

    printk("5MP JPEG length: %u bytes\n", jpeg_length);

    if (jpeg_length == 0 ||
        jpeg_length > 0x00FFFFFF) {
        printk("Invalid JPEG length\n");
        return -1;
    }

    printk("Validating JPEG markers...\n");

    ret = camera_validate_jpeg(jpeg_length);
    if (ret < 0) {
        printk("JPEG validation FAILED\n");
        return ret;
    }

    printk("5MP JPEG validation PASSED\n");
    printk("================================\n");

    return 0;
}

struct jpeg_export_state {
    uint32_t bytes_sent;
};

static int camera_uart_export_callback(
    const uint8_t *data,
    size_t length,
    void *context)
{
    struct jpeg_export_state *state = context;

    for (size_t i = 0; i < length; i++) {
        camera_uart_write_byte(data[i]);
    }

    state->bytes_sent += length;
    return 0;
}

int camera_dump_jpeg_binary(void)
{
    uint32_t jpeg_length;
    struct jpeg_export_state state = {0};
    char marker[64];
    int ret;

    ret = camera_get_fifo_length(&jpeg_length);
    if (ret < 0) {
        return ret;
    }

    if (jpeg_length == 0) {
        return -1;
    }

    k_msleep(100);

    snprintk(marker,
             sizeof(marker),
             "\nJPEG_BINARY_BEGIN:%u\n",
             jpeg_length);

    camera_uart_write_string(marker);

    /*
     * No printk() calls while binary JPEG bytes are on the UART.
     */
    ret = camera_read_fifo(
        jpeg_length,
        camera_uart_export_callback,
        &state
    );

    if (ret < 0) {
        return ret;
    }

    k_msleep(100);

    snprintk(marker,
             sizeof(marker),
             "\nJPEG_BINARY_END:%u\n",
             state.bytes_sent);

    camera_uart_write_string(marker);

    return 0;
}

int camera_capture_and_dump(void)
{
    uint32_t jpeg_length;
    int ret;

    printk("\n=== 5MP DOCUMENT CAPTURE EXPORT ===\n");

    ret = camera_configure_auto_image_controls();
    if (ret < 0) {
        printk("Image controls failed\n");
        return ret;
    }

    ret = camera_configure_document_capture();
    if (ret < 0) {
        printk("High-quality configuration failed\n");
        return ret;
    }

    ret = camera_autofocus();
    if (ret < 0) {
        printk("Autofocus failed\n");
        return ret;
    }

    printk("Autofocus phase complete - capturing 5MP frame\n");

    ret = camera_start_capture();
    if (ret < 0) {
        printk("5MP capture failed\n");
        return ret;
    }

    ret = camera_get_fifo_length(&jpeg_length);
    if (ret < 0) {
        return ret;
    }

    printk("Export JPEG length: %u bytes\n", jpeg_length);

    if (jpeg_length == 0 ||
        jpeg_length > 0x00FFFFFF) {
        printk("Export JPEG length invalid\n");
        return -1;
    }

    printk("Beginning raw 5MP JPEG transfer...\n");
    k_msleep(100);

    ret = camera_dump_jpeg_binary();
    if (ret < 0) {
        printk("JPEG transfer failed\n");
        return ret;
    }

    printk("5MP JPEG export complete\n");
    printk("==================================\n");

    return 0;
}
