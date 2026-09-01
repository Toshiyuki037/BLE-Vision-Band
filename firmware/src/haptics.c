#include "haptics.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

#define DRV_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(ti_drv2605)

static const struct i2c_dt_spec drv =
	I2C_DT_SPEC_GET(DRV_NODE);

static int drv_write(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&drv, reg, value);
}

static int haptic_on(void)
{
	int ret;

	ret = drv_write(0x01, 0x05);

	if (ret < 0) {
		return ret;
	}

	return drv_write(0x02, 0x7F);
}

static int haptic_off(void)
{
	return drv_write(0x02, 0x00);
}

static int haptic_buzz(uint32_t duration_ms)
{
	int ret;

	ret = haptic_on();

	if (ret < 0) {
		return ret;
	}

	k_msleep(duration_ms);

	return haptic_off();
}

int haptics_init(void)
{
	if (!device_is_ready(drv.bus)) {
		return -1;
	}

	return 0;
}

int haptics_capture_ok(void)
{
	return haptic_buzz(150);
}

int haptics_tx_ok(void)
{
	return haptic_buzz(500);
}

int haptics_result_received(void)
{
	int ret;

	ret = haptic_buzz(150);

	if (ret < 0) {
		return ret;
	}

	k_msleep(150);

	return haptic_buzz(150);
}