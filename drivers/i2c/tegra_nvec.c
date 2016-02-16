/*
 * (C) Copyright 2013
 * Andrey Danin <andreydanin@mail.ru>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <fdtdec.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/arch/clock.h>
#include <asm/arch/funcmux.h>
#include <asm/arch-tegra/tegra_nvec.h>
#include <asm/arch-tegra/tegra_nvec_keyboard.h>

#ifndef CONFIG_TEGRA_NVEC
#error "You should enable CONFIG_TEGRA_NVEC"
#endif

DECLARE_GLOBAL_DATA_PTR;


/* Nvec perfroms io interval is beteween 20 and 500 ms,
no response in 600 ms means error */
enum {
	NVEC_TIMEOUT_MIN = 20,
	NVEC_TIMEOUT_MAX = 600,
};
enum {
	NVEC_WAIT_FOR_EC = 1,
	NVEC_DONT_WAIT_FOR_EC = 0,
	NVEC_ATTEMPTS_MAX = 10,
};

enum {
	nvec_io_error = -1,
	nvec_io_timeout,
	nvec_io_read_ok,
	nvec_io_write_ok,
	nvec_io_not_ready,
	nvec_io_retry,
};

enum {
	NVST_BEGIN = 0,
	NVST_CMD = 1,
	NVST_SUBCMD = 2,
	NVST_READ = 3,
	NVST_WRITE_SIZE = 4,
	NVST_WRITE = 5,
};

struct nvec_t {
	int gpio;
	int i2c_addr;
	int i2c_clk;
	void __iomem *base;
	int state;
	char rx_buf[34];
	int rx_pos;
	char *tx_buf;
	int tx_pos;
	int tx_size;
} nvec_data;

struct fdt_nvec_config {
	int gpio;
	int i2c_addr;
	int i2c_clk;
	fdt_addr_t base_addr;
	struct fdt_gpio_state request_gpio;
};


/* nvec commands */
char noop[] = { NVEC_CNTL, CNTL_NOOP };


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
 * See: nvec_push_key, nvec_pop_key, nvec_have_key
 *
 * @param nvec	nvec state struct
 */
void nvec_process_msg(struct nvec_t *nvec)
{
	const unsigned char *msg = (const unsigned char *)nvec->rx_buf;
	int event_type;

	if (!nvec_msg_is_event(msg))
		return;

	event_type = nvec_msg_event_type(msg);
	if (event_type == NVEC_KEYBOARD)
		nvec_process_keyboard_msg(msg);
}


static inline int is_read(unsigned long status)
{
	return (status & RNW) == 0;
}


static inline int is_ready(unsigned long status)
{
	return status & I2C_SL_IRQ;
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
int nvec_do_io(struct nvec_t *nvec, int wait_for_ec)
{
	unsigned int poll_start_ms = 0;
	unsigned long status;
	unsigned int received = 0;
	unsigned int to_send = 0;
	unsigned int timeout_ms = NVEC_TIMEOUT_MAX;
	int is_first_iteration = 1;

	poll_start_ms = get_timer(0);
	mdelay(NVEC_TIMEOUT_MIN);

	while (1) {
		status = readl(nvec->base + I2C_SL_STATUS);
		if (!is_ready(status)) {
			if (is_first_iteration && !wait_for_ec)
				return nvec_io_not_ready;

			if (get_timer(poll_start_ms) > timeout_ms)
				return nvec_io_timeout;

			is_first_iteration = 0;
			udelay(100);
			continue;
		}
		is_first_iteration = 0;

		if (is_read(status))
			received = readl(nvec->base + I2C_SL_RCVD);

		if (status == (I2C_SL_IRQ | RCVD)) {
			nvec->state = NVST_BEGIN;
			nvec->rx_pos = 0;
			nvec->tx_pos = 0;
		}

		switch (nvec->state) {
		case NVST_BEGIN:
			nvec->rx_pos = 0;
			nvec->tx_pos = 0;
			if (received != nvec->i2c_addr) {
				error("NVEC io: unknown addr 0x%x\n", received);
				return nvec_io_error;
			}
			nvec->state = NVST_CMD;
			break;

		case NVST_CMD:
			nvec->rx_buf[nvec->rx_pos++] = (char)received;
			nvec->state = NVST_SUBCMD;
			break;

		case NVST_SUBCMD:
			if (status == (I2C_SL_IRQ | RNW | RCVD)) {
				if (nvec->rx_buf[0] != 0x01) {
					error("NVEC io: wrong read\n");
					nvec->state = NVST_BEGIN;
					return nvec_io_error;
				}
				nvec->state = NVST_WRITE;
				if (nvec->tx_buf == 0) {
					debug("NVEC io: error, tx buffer is 0\n");
					nvec->tx_buf = noop;
					nvec->tx_size = 2;
					nvec->tx_pos = 0;
				}
				to_send = nvec->tx_size;
				writel(to_send, nvec->base + I2C_SL_RCVD);
				gpio_set_value(nvec_data.gpio, 1);
				nvec->state = NVST_WRITE;
			} else {
				nvec->state = NVST_READ;
				nvec->rx_buf[nvec->rx_pos] = (char)received;
				++nvec->rx_pos;
			}
			break;

		case NVST_READ:
			if (nvec->rx_pos >= 34) {
				error("NVEC io: read buffer is full\n");
				break;
			}
			nvec->rx_buf[nvec->rx_pos++] = (char)received;
			if (status & END_TRANS) {
				nvec_process_msg(nvec);
				nvec->rx_pos = 0;
				return nvec_io_read_ok;
			}
			break;

		case NVST_WRITE_SIZE:
			to_send = nvec->tx_size;
			writel(to_send, nvec->base + I2C_SL_RCVD);
			nvec->state = NVST_WRITE;
			break;

		case NVST_WRITE:
			if (nvec->tx_pos >= nvec->tx_size) {
				if (status & END_TRANS)
					return nvec_io_write_ok;

				error("NVEC io: no data to write\n");
				return nvec_io_error;
			}
			to_send = nvec->tx_buf[nvec->tx_pos++];
			writel(to_send, nvec->base + I2C_SL_RCVD);
			if (status & END_TRANS) {
				nvec->tx_pos = 0;
				nvec->tx_buf = 0;
				return nvec_io_write_ok;
			}

			break;

		default:
			error("NVEC io: unknown state\n");
			break;
		}
		if (status & END_TRANS)
			return nvec_io_retry;
	}
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
	int res = 0;
	int i = 0;

	nvec_data.tx_buf = buf;
	nvec_data.tx_size = size;

	while (i++ < NVEC_ATTEMPTS_MAX) {
		nvec_data.tx_pos = 0;

		/* request */
		gpio_set_value(nvec_data.gpio, 0);
		res = nvec_do_io(&nvec_data, NVEC_WAIT_FOR_EC);
		if (res != nvec_io_write_ok) {
			debug("warning: nvec failed to send request\n");
			continue;
		}

		/* response */
		res = nvec_do_io(&nvec_data, NVEC_WAIT_FOR_EC);
		if (res != nvec_io_read_ok) {
			debug("warning: nvec failed to read response\n");
			continue;
		}

		nvec_data.tx_buf = 0;
		nvec_data.tx_size = 0;
		nvec_data.tx_pos = 0;

		return 0;
	}

	error("nvec failed to perform request\n");
	return -1;
}


/**
 * Init i2c controller to operate in slave mode.
 *
 * @param nvec	nvec state struct
 */
static void nvec_init_i2c_slave(struct nvec_t *nvec)
{
	unsigned long val;

	val = I2C_CNFG_NEW_MASTER_SFM | I2C_CNFG_PACKET_MODE_EN |
	    (0x2 << I2C_CNFG_DEBOUNCE_CNT_SHIFT);
	writel(val, nvec->base + I2C_CNFG);

	/* i2c3 -> 67 */
	clock_start_periph_pll(67, CLOCK_ID_PERIPH,
			       nvec->i2c_clk * 8);

	reset_periph(67, 1);

	writel(I2C_SL_NEWSL, nvec->base + I2C_SL_CNFG);
	writel(0x1E, nvec->base + I2C_SL_DELAY_COUNT);

	writel(nvec->i2c_addr>>1, nvec->base + I2C_SL_ADDR1);
	writel(0, nvec->base + I2C_SL_ADDR2);

	funcmux_select(67, FUNCMUX_DEFAULT);
}


/**
 * Decode the nvec information from the fdt.
 *
 * @param blob		fdt blob
 * @param config	structure to store fdt config into
 * @return 0 if ok, -ve on error
 */
static int nvec_decode_config(const void *blob,
			      struct fdt_nvec_config *config)
{
	int node;

	node = fdtdec_next_compatible(blob, 0, COMPAT_NVIDIA_TEGRA20_NVEC);
	if (node < 0) {
		error("Cannot find NVEC node in fdt\n");
		return node;
	}

	config->base_addr = fdtdec_get_addr(blob, node, "reg");
	if (config->base_addr == FDT_ADDR_T_NONE) {
		error("No NVEC controller address\n");
		return -1;
	}

	if (fdtdec_decode_gpio(blob, node, "request-gpios",
			       &config->request_gpio)) {
		error("No NVEC request gpio\n");
		return -1;
	}

	config->i2c_addr = fdtdec_get_int(blob, node, "slave-addr", -1);
	config->i2c_clk = fdtdec_get_int(blob, node, "clock-frequency", -1);

	return 0;
}


int board_nvec_init(void)
{
	int res = 0;

	struct fdt_nvec_config cfg;
	if (nvec_decode_config(gd->fdt_blob, &cfg)) {
		debug("Can't parse NVEC node in device tree\n");
		return -1;
	}

	nvec_data.rx_pos = 0;
	nvec_data.tx_buf = 0;
	nvec_data.tx_pos = 0;
	nvec_data.tx_size = 0;
	nvec_data.state = NVST_BEGIN;

	nvec_data.gpio = cfg.request_gpio.gpio;
	nvec_data.i2c_addr = cfg.i2c_addr;
	nvec_data.i2c_clk = cfg.i2c_clk;
	nvec_data.base = (void __iomem *)cfg.base_addr;

	debug("NVEC initialization...\n");

	res = gpio_request(nvec_data.gpio, NULL);
	if (res != 0)
		error("NVEC: err, gpio_request\n");
	res = gpio_direction_output(nvec_data.gpio, 1);
	if (res != 0)
		error("NVEC: err, gpio_direction\n");
	res = gpio_set_value(nvec_data.gpio, 1);
	if (res != 0)
		error("NVEC: err, gpio_set_value\n");
	udelay(100);

	nvec_init_i2c_slave(&nvec_data);

	nvec_enable_kbd_events();

	return 1;
}


int nvec_read_events(void)
{
	int res;
	int cnt = 0;

	while (++cnt <= 8) {
		res = nvec_do_io(&nvec_data, NVEC_DONT_WAIT_FOR_EC);
		switch (res) {
		case nvec_io_not_ready:
			return 0;

		case nvec_io_read_ok:
		case nvec_io_retry:
			break;

		case nvec_io_error:
		case nvec_io_timeout:
			debug("NVEC events: io failed %d\n", res);
			return 0;

		case nvec_io_write_ok:
		default:
			debug("NVEC events: unexpected io result %d\n", res);
			return 0;
		}
	}

	return 0;
}
