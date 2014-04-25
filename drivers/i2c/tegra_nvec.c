/*
 * (C) Copyright 2014
 * Andrey Danin <danindrey@mail.ru>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <fdtdec.h>
#include <i2c.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/arch/clock.h>
#include <asm/arch/funcmux.h>
#include <asm/arch-tegra/tegra_nvec.h>

#ifndef CONFIG_SYS_I2C_TEGRA_NVEC
#error "You should enable CONFIG_SYS_I2C_TEGRA_NVEC"
#endif

DECLARE_GLOBAL_DATA_PTR;

struct nvec_periph devices[NVEC_LAST_MSG];


struct nvec_t {
	int			i2c_bus;
	struct fdt_gpio_state	gpio;
} nvec_data;


/* nvec commands */
char noop[] = { NVEC_CNTL, CNTL_NOOP };

void nvec_signal_request(void)
{
	gpio_set_value(nvec_data.gpio.gpio, 0);
}


void nvec_clear_request(void)
{
	gpio_set_value(nvec_data.gpio.gpio, 1);
}


int nvec_msg_is_event(const unsigned char *msg)
{
	return msg[0] >> 7;
}


int nvec_msg_event_type(const unsigned char *msg)
{
	return msg[0] & 0x0f;
}


/**
 * Process incoming io message.
 * If message is keyboard event then key code will
 * be added to keys buffer.
 *
 * @param nvec	nvec state struct
 */
void nvec_process_msg(struct nvec_t *nvec, struct i2c_transaction *trans)
{
	(void) nvec;

	const unsigned char *msg = (const unsigned char *)&trans->rx_buf[1];
	int event_type;

	if (!nvec_msg_is_event(msg))
		return;

	event_type = nvec_msg_event_type(msg);

	if (event_type < NVEC_KEYBOARD || event_type >= NVEC_LAST_MSG)
		return;

	if (devices[event_type].process_msg)
		devices[event_type].process_msg(msg);
}


/**
 * Perform complete io operation (read or write).
 * NOTE: function will wait NVEC_TIMEOUT_MIN (20ms)
 * before status check to avoid nvec hang.
 *
 * @param nvec		nvec state struct
 * @param wait_for_ec	if 1(NVEC_WAIT_FOR_EC) operation
 *			timeout is NVEC_TIMEOUT_MAX (600ms),
 *			otherwise function will return if io
 *			is not ready.
 *
 * @return nvec_io_* code
 */
int nvec_do_io(struct nvec_t *nvec)
{
	static unsigned int prev_timer;
	unsigned int poll_start_ms = get_timer(prev_timer);
	struct i2c_transaction trans;
	int res;

	if (poll_start_ms > 30000) {
		if (prev_timer != 0)
			nvec_do_request(noop, sizeof(noop));
		prev_timer = get_timer(0);
	}

	memset(&trans, 0, sizeof(trans));
	trans.start_timeout = 1;
	trans.timeout = 6000;

	res = i2c_slave_io(&trans);
	if (res == 0) {
		nvec_process_msg(nvec, &trans);
		return 0;
	}

	if (res != -1)
		debug("Error: i2c slave io failed with code %d\n", res);

	return -1;
}

/**
 * Send request and read response. If write or read failed
 * operation will be repeated NVEC_ATTEMPTS_MAX times.
 *
 * @param buf	request data
 * @param size	request data size
 * @return 0 if ok, -1 on error
 */
int nvec_do_request(char *buf, int size)
{
	int res;
	struct i2c_transaction trans;
	int i;

	nvec_signal_request();

	/* Request */
	for (i = 0; i < 10; ++i) {
		memset(&trans, 0, sizeof(trans));
		trans.start_timeout = 600;
		trans.timeout = 6000;

		trans.tx_buf[0] = (char)size;
		memcpy(&trans.tx_buf[1], buf, size);
		trans.tx_size = size + 1;

		res = i2c_slave_io(&trans);
		if (res == 0) {
			if (trans.tx_pos == trans.tx_size)
				break;

			debug("Request was not sent completely");
		} else if (res != -1) {
			debug("Unknown error while slave io");
		}
	}
	nvec_clear_request();
	if (res != 0) {
		error("nvec failed to perform request\n");
		return -1;
	}

	/* Response */
	for (i = 0; i < 10; ++i) {
		memset(&trans, 0, sizeof(trans));
		trans.start_timeout = 600;
		trans.timeout = 6000;

		res = i2c_slave_io(&trans);
		if (res == 0)
			break;
	}
	if (res != 0) {
		error("nvec failed to read response\n");
		return -1;
	}

	/* TODO Parse response */

	return 0;
}


/**
 * Decode the nvec information from the fdt.
 *
 * @param blob		fdt blob
 * @param nvec		nvec device sturct
 * @return 0 if ok, -ve on error
 */
static int nvec_decode_config(const void *blob,
			      struct nvec_t *nvec)
{
	int node, parent;
	int i2c_bus;

	node = fdtdec_next_compatible(blob, 0, COMPAT_NVIDIA_TEGRA20_NVEC);
	if (node < 0) {
		error("Cannot find NVEC node in fdt\n");
		return node;
	}

	parent = fdt_parent_offset(blob, node);
	if (parent < 0) {
		debug("%s: Cannot find node parent\n", __func__);
		return -1;
	}

	i2c_bus = i2c_get_bus_num_fdt(parent);
	if (i2c_bus < 0)
		return -1;
	nvec->i2c_bus = i2c_bus;

	if (fdtdec_decode_gpio(blob, node, "request-gpios",
			       &nvec->gpio)) {
		error("No NVEC request gpio\n");
		return -1;
	}

	debug("NVEC: i2c:%d, gpio:%s(%u)\n",
	      nvec->i2c_bus, nvec->gpio.name, nvec->gpio.gpio);
	return 0;
}


int board_nvec_init(void)
{
	int res = 0;
	int i;

	if (nvec_decode_config(gd->fdt_blob, &nvec_data)) {
		error("Can't parse NVEC node in device tree\n");
		return -1;
	}

	debug("NVEC initialization...\n");

	res = gpio_request(nvec_data.gpio.gpio, NULL);
	if (res != 0)
		error("NVEC: err, gpio_request\n");
	res = gpio_direction_output(nvec_data.gpio.gpio, 1);
	if (res != 0)
		error("NVEC: err, gpio_direction\n");
	res = gpio_set_value(nvec_data.gpio.gpio, 1);
	if (res != 0)
		error("NVEC: err, gpio_set_value\n");
	udelay(100);

	i2c_set_bus_num(nvec_data.i2c_bus);

	for (i = NVEC_KEYBOARD; i < NVEC_LAST_MSG; ++i)
		if (devices[i].start) {
			debug("Starting device %d(0x%x)\n", i, i);
			devices[i].start();
		}

	return 1;
}


int nvec_read_events(void)
{
	int res;
	int cnt = 0;

	while (++cnt <= 8) {
		res = nvec_do_io(&nvec_data);
		if (res)
			break;

		/* TODO Process nvec communication errors */
	}

	return cnt;
}


int nvec_register_periph(int msg_type, struct nvec_periph *periph)
{
	if (devices[msg_type].start || devices[msg_type].process_msg) {
		error("Device for msg %d already registered\n", msg_type);
		return -1;
	}

	devices[msg_type] = *periph;
	return 0;
}
