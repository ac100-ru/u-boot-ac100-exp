#include <common.h>
#include <fdtdec.h>

#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/arch/clock.h>
#include <asm/arch/funcmux.h>
#include <asm/arch/pinmux.h>
#include <asm/arch/pmu.h>
#include <asm/arch/tegra.h>
#include <asm/arch-tegra/board.h>
#include <asm/arch-tegra/clk_rst.h>
#include <asm/arch-tegra/pmc.h>
#include <asm/arch-tegra/sys_proto.h>
#include <asm/arch-tegra/timer.h>
#include <asm/arch-tegra/uart.h>
#include <asm/arch-tegra/warmboot.h>
#include <i2c.h>
#include <asm/arch-tegra/nvec.h>
#include "nvec.h"
#include "nvec-keytable.h"

#define DEBUG

#ifndef CONFIG_TEGRA_NVEC
#error "You should enable CONFIG_TEGRA_NVEC"
#endif

DECLARE_GLOBAL_DATA_PTR;


int debug = 0;
struct dbg_t {
	unsigned long status;
	unsigned int data;
};

static struct dbg_t dbg[256];
static int dbg_i = -1;

static inline void dbg_save(unsigned long status, unsigned int data)
{
	++dbg_i;
	dbg[dbg_i].status = status;
	dbg[dbg_i].data = data;
}

static inline void dbg_print(void)
{
	if (dbg_i == -1)
		return;

#define AS_BOOL(x) ((int)((x) == 0 ? 0 : 1))
	int i = 0;
	for (i = 0; i <= dbg_i; ++i) {
		unsigned long status = dbg[i].status;
		printf("NVEC: status:0x%lx (RNW:%d, RCVD:%d, IRQ:%d, END:%d)",
				status,
				AS_BOOL(status & RNW), AS_BOOL(status & RCVD),
				AS_BOOL(status & I2C_SL_IRQ), AS_BOOL(status & END_TRANS));
		if ((status & I2C_SL_IRQ))
			printf(": %u", dbg[i].data);
		printf("\n");
	}
#undef AS_BOOL
	dbg_i = -1;
}


/* Nvec perfroms io interval is beteween 20 and 500 ms,
no response in 600 ms means error */
const unsigned int NVEC_TIMEOUT_MIN = 20;
const unsigned int NVEC_TIMEOUT_MAX = 600;
const int NVEC_WAIT_FOR_EC = 1;
const int NVEC_DONT_WAIT_FOR_EC = 0;

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

struct nvec_t
{
	int gpio;
	int i2c_addr;
	int i2c_clk;
	void __iomem *base;
	int state;
	char rx_buf[34];
	int rx_pos;
	char* tx_buf;
	int tx_pos;
	int tx_size;
} nvec_data;

struct fdt_nvec_config {
	int gpio;			/* config is valid */
	int i2c_addr;			/* width in pixels */
	int i2c_clk;			/* height in pixels */
	fdt_addr_t base_addr;
	struct fdt_gpio_state request_gpio;	/* GPIO for panel vdd */
};


enum nvec_msg_type {
	NVEC_KEYBOARD = 0,
	NVEC_SYS = 1,
	NVEC_BAT,
	NVEC_GPIO,
	NVEC_SLEEP,
	NVEC_KBD,
	NVEC_PS2,
	NVEC_CNTL,
	NVEC_OEM0 = 0x0d,
	NVEC_KB_EVT = 0x80,
	NVEC_PS2_EVT,
};

enum nvec_event_size {
	NVEC_2BYTES,
	NVEC_3BYTES,
	NVEC_VAR_SIZE,
};

enum kbd_subcmds {
	CNFG_WAKE = 3,
	CNFG_WAKE_KEY_REPORTING,
	SET_LEDS = 0xed,
	ENABLE_KBD = 0xf4,
	DISABLE_KBD,
};

enum cntl_subcmds {
	CNTL_RESET_EC = 0x00,
	CNTL_SELF_TEST = 0x01,
	CNTL_NOOP = 0x02,
	CNTL_GET_EC_SPEC_VER = 0x10,
	CNTL_GET_FIRMWARE_VERSION = 0x15,
};

enum nvec_sleep_subcmds {
	GLOBAL_EVENTS,
	AP_PWR_DOWN,
	AP_SUSPEND,
};

#define MOUSE_SEND_CMD 0x01
#define MOUSE_RESET 0xff

char enable_kbd[] = { NVEC_KBD, ENABLE_KBD };
char reset_kbd[] = { NVEC_PS2, MOUSE_SEND_CMD, MOUSE_RESET, 3 };
char clear_leds[] = { NVEC_KBD, SET_LEDS, 0 };
char cnfg_wake[] = { NVEC_KBD, CNFG_WAKE, 1, 1 };
char cnfg_wake_key_reporting[] = { NVEC_KBD, CNFG_WAKE_KEY_REPORTING, 1 };

char noop[] = { NVEC_CNTL, CNTL_NOOP };
char get_firmware_version[] = { NVEC_CNTL, CNTL_GET_FIRMWARE_VERSION };


struct key_t {
	int code;
	int state;
};

struct key_t keys[256];
int key_i = -1;

void nvec_push_key(int code, int state)
{
	++key_i;
	keys[key_i].code = code;
	keys[key_i].state = state;
}



int msg[256];
int msg_i = -1;

void msg_save(const char* buf)
{
	++msg_i;
	msg[msg_i] = *(int*)buf;
}

void msg_print(void)
{
	if (msg_i == -1)
		return;

	int i = 0;
	for (i = 0; i <= msg_i; ++i)
		printf("msg: 0x%04x\n", msg[i]);
	msg_i = -1;
}

static int nvec_is_event(struct nvec_t* nvec)
{
	return nvec->rx_buf[0] >> 7;
}

void nvec_process_msg(struct nvec_t* nvec)
{
	int code, state;
	unsigned char *msg;
	int event_type;
	int _size;

	msg_save(nvec->rx_buf);

	if (!nvec_is_event(nvec))
		return;

	event_type = nvec->rx_buf[0] & 0x0f;
	if (event_type != NVEC_KEYBOARD)
		return;

	msg = (unsigned char *)nvec->rx_buf;
	_size = (msg[0] & (3 << 5)) >> 5;

	if (_size == NVEC_VAR_SIZE)
		return;

	if (_size == NVEC_3BYTES)
		msg++;

	code = msg[1] & 0x7f;
	state = msg[1] & 0x80;

	nvec_push_key(code_tabs[_size][code], state);
}


void nvec_init_i2c_slave(struct nvec_t* nvec)
{
	unsigned long val;

	val = I2C_CNFG_NEW_MASTER_SFM | I2C_CNFG_PACKET_MODE_EN |
	    (0x2 << I2C_CNFG_DEBOUNCE_CNT_SHIFT);
	writel(val, nvec->base + I2C_CNFG);

	/* FIXME: get clock from DT */
	/* i2c3 -> 67 */
	clock_start_periph_pll(67, CLOCK_ID_PERIPH,
			80000 * 8);

	reset_periph(67, 1);

	writel(I2C_SL_NEWSL, nvec->base + I2C_SL_CNFG);
	writel(0x1E, nvec->base + I2C_SL_DELAY_COUNT);

	writel(nvec->i2c_addr>>1, nvec->base + I2C_SL_ADDR1);
	writel(0, nvec->base + I2C_SL_ADDR2);

	funcmux_select(67, FUNCMUX_DEFAULT);

	/*clk_disable_unprepare(nvec->i2c_clk);*/
}

static inline int is_read(unsigned long status)
{
	return (status & RNW) == 0;
}

static inline int is_ready(unsigned long status)
{
	return (status & I2C_SL_IRQ);
}


int nvec_do_io(struct nvec_t* nvec, int wait_for_ec)
{
	unsigned int poll_start_ms = 0;
	unsigned long status;
	unsigned int received = 0;
	unsigned int to_send = 0;
	unsigned int timeout_ms = NVEC_TIMEOUT_MAX;
	unsigned int old_state;
	int is_first_iteration = 1;

	poll_start_ms = get_timer(0);

#define AS_BOOL(x) ((int)((x) == 0 ? 0 : 1))
	while (1) {
		status = readl(nvec->base + I2C_SL_STATUS);
		if (!is_ready(status)) {
			if (is_first_iteration && !wait_for_ec)
				return nvec_io_not_ready;

			if (get_timer(poll_start_ms) > timeout_ms) {
				return nvec_io_timeout;
			}

			is_first_iteration = 0;
			udelay(100);
			continue;
		}
		is_first_iteration = 0;

		if (debug)
			printf("NVEC: status: 0x%lx\n", status);

		if (is_read(status)) {
			received = readl(nvec->base + I2C_SL_RCVD);
			if (status & RCVD) {
				//writel(0, nvec->base + I2C_SL_RCVD);
				//printf("%s: NVEC ack\n", __func__);
				//udelay(100);
			}
			dbg_save(status, received);
		}

		if (status == (I2C_SL_IRQ | RCVD)) {
			nvec->state = NVST_BEGIN;
			nvec->rx_pos = 0;
			nvec->tx_pos = 0;
		}

		/*
		if (status & RCVD) {
			if (status & END_TRANS)
				printf("%s: NVEC repeated start\n", __func__);
			else {
				printf("%s: NVEC new transaction\n", __func__);
			}
		}
		*/

		old_state = nvec->state;
		switch (nvec->state) {
			case NVST_BEGIN:
				nvec->rx_pos = 0;
				nvec->tx_pos = 0;
				if (received != nvec->i2c_addr) {
					printf("%s: NVEC unknown addr 0x%x\n", __func__, received);
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
						printf("%s: NVEC wrong read!\n", __func__);
						nvec->state = NVST_BEGIN;
						return nvec_io_error;
					}
					nvec->state = NVST_WRITE;
					if (nvec->tx_buf == 0) {
						printf("%s: NVEC erro, tx buffer is 0\n", __func__);
						nvec->tx_buf = noop;
						nvec->tx_size = 2;
						nvec->tx_pos = 0;
					}
					to_send = nvec->tx_size;
					writel(to_send, nvec->base + I2C_SL_RCVD);
					gpio_set_value(nvec_data.gpio, 1);
					nvec->state = NVST_WRITE;
					dbg_save(status, to_send);
				} else {
					nvec->state = NVST_READ;
					nvec->rx_buf[nvec->rx_pos] = (char)received;
					++nvec->rx_pos;
				}
				break;

			case NVST_READ:
				if (nvec->rx_pos >= 34) {
					printf("%s: NVEC read buffer is full.\n", __func__);
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
				//udelay(100);
				//printf("%s: NVEC sent size:0x%x\n", __func__, to_send);
				nvec->state = NVST_WRITE;
				break;

			case NVST_WRITE:
				if (nvec->tx_pos >= nvec->tx_size) {
					dbg_save(status, 0);
					if (status & END_TRANS)
						return nvec_io_write_ok;

					printf("%s: NVEC no data to write\n", __func__);
					return nvec_io_error;
				}
				to_send = nvec->tx_buf[nvec->tx_pos++];
				writel(to_send, nvec->base + I2C_SL_RCVD);
				dbg_save(status, to_send);
				if (status & END_TRANS) {
					nvec->tx_pos = 0;
					nvec->tx_buf = 0;
					return nvec_io_write_ok;
				}

				break;

			default:
				printf("%s: NVEC unknown state\n", __func__);
				break;
		}
		/*if (status & END_TRANS) {
			nvec->state = NVST_BEGIN;
			printf("%s: NVEC end of transaction\n", __func__);
			return;
		}*/
		if (status & END_TRANS) {
			/*printf("%s: NVEC: unknown operation ended (status:0x%x, state:%d, old state:%d)\n", __func__,
					status, nvec->state, old_state);*/
			return nvec_io_retry;
		}
	}
#undef AS_BOOL


	if (debug)
		printf("%s: NVEC:\n", __func__);
}

int nvec_do_request(char* buf, int size)
{
	int res = 0;
	/*int i = 0;*/

	nvec_data.tx_buf = buf;
	nvec_data.tx_size = size;
	nvec_data.tx_pos = 0;

	/*for (i = 0; i < size; ++i)
		printf("\\x%02x", buf[i]);
	printf("\n");*/

	gpio_set_value(nvec_data.gpio, 0);
	mdelay(NVEC_TIMEOUT_MIN);
	res = nvec_do_io(&nvec_data, NVEC_WAIT_FOR_EC);
	if (res != nvec_io_write_ok) {
		printf("nwec_write failed to send request\n");
		return -1;
	}
	mdelay(NVEC_TIMEOUT_MIN);
	res = nvec_do_io(&nvec_data, NVEC_WAIT_FOR_EC);
	if (res != nvec_io_read_ok) {
		printf("nwec_write failed to read response\n");
		return -1;
	}

	nvec_data.tx_buf = 0;
	nvec_data.tx_size = 0;
	nvec_data.tx_pos = 0;

	return 0;
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
		debug("%s: Cannot find NVEC node in fdt\n",
		      __func__);
		return node;
	}

	config->base_addr = fdtdec_get_addr(blob, node, "reg");
	if (config->base_addr == FDT_ADDR_T_NONE) {
		debug("%s: No NVEC controller address\n", __func__);
		return -1;
	}

	//fdt_decode_lcd;
	if (fdtdec_decode_gpio(blob, node, "request-gpios", &config->request_gpio)) {
		debug("%s: No NVEC request gpio\n", __func__);
		return -1;
	}

	config->i2c_addr = fdtdec_get_int(blob, node, "slave-addr", -1);
	config->i2c_clk = fdtdec_get_int(blob, node, "clock-frequency", -1);

	return 0;
}

static void nvec_configure_event(long mask, int state);
static void nvec_toggle_global_events(int state);

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

	nvec_data.gpio = cfg.request_gpio.gpio; // 170; /* PV2 */
	nvec_data.i2c_addr = cfg.i2c_addr; // 138; /* */
	nvec_data.i2c_clk = cfg.i2c_clk; // 80000; /* */
	nvec_data.base = (void __iomem *)cfg.base_addr; //0x7000c500;

	printf("NVEC initialization...\n");

	res = gpio_request(nvec_data.gpio, NULL);
	if (res != 0)
		printf("NVEC: err, gpio_request\n");
	res = gpio_direction_output(nvec_data.gpio, 1);
	if (res != 0)
		printf("NVEC: err, gpio_direction\n");
	res = gpio_set_value(nvec_data.gpio, 1);
	if (res != 0)
		printf("NVEC: err, gpio_set_value\n");
	udelay(100);

	/* init i2c and clocks */
	nvec_init_i2c_slave(&nvec_data);

	/* get firmware */
	/* enable keyboard */

	printf("NVEC is initialized\n");

	printf("%s: NVEC dummy io\n", __func__);
	res = nvec_do_io(&nvec_data, NVEC_DONT_WAIT_FOR_EC);
	printf("%s: NVEC dummy io res:%d\n", __func__, res);

	printf("%s: NVEC noop write\n", __func__);
	nvec_do_request(noop, 2);

	nvec_toggle_global_events(1);
	nvec_do_request(get_firmware_version, 2);

	/* LID */
	nvec_configure_event(0x02, 1);
	/* short POWER press */
	nvec_configure_event(0x80, 1);

	nvec_enable_kbd_events();

	dbg_print();

	while (1) {
		res = nvec_do_io(&nvec_data, NVEC_DONT_WAIT_FOR_EC);
		if (res != nvec_io_not_ready)
			printf("io result %d\n", res);
		/*dbg_print();
		msg_print();*/
		while (nvec_have_keys())
			printf("%d ", nvec_pop_key());
		dbg_i = -1;
		msg_i = -1;
		key_i = -1;
		udelay(100);
	}

	return 1;
}


void nvec_enable_kbd_events(void)
{
	int res;

	// TODO Remove mdelays ?

	/* Enable keyboard */
	if (nvec_do_request(enable_kbd, 2))
		printf("NVEC: failed to enable keyboard\n");
	mdelay(NVEC_TIMEOUT_MIN);

	/* FIXME Sometimes wake faild first time (maybe already fixed).
	 * Need to check
	 */
	if ((res = nvec_do_request(cnfg_wake, 4))) {
		printf("NVEC: wake reuqest were not configured (%d), retry\n", res);
		if (nvec_do_request(cnfg_wake, 4))
			printf("NVEC: wake reuqest were not configured (%d)\n", res);
	}
	mdelay(NVEC_TIMEOUT_MIN);

	/* keyboard needs reset via mouse command */
	if (nvec_do_request(reset_kbd, 4))
		printf("NVEC: failed to reset keyboard\n");
	mdelay(NVEC_TIMEOUT_MIN);

	if (nvec_do_request(cnfg_wake_key_reporting, 3))
		printf("NVEC: failed to configure waky key reporting\n");
	mdelay(NVEC_TIMEOUT_MIN);

	if (nvec_do_request(clear_leds, 3))
		printf("NVEC: failed to clear leds\n");
	mdelay(NVEC_TIMEOUT_MIN);

	/* Disable caps lock LED */
	/*nvec_do_request(clear_leds, sizeof(clear_leds));
	nvec_do_io(&nvec_data);*/

	printf("NVEC: keyboard initialization finished\n");
}


int nvec_read_events(void)
{
	int res;

	while (1) {
		dbg_i = -1;
		msg_i = -1;
		res = nvec_do_io(&nvec_data, NVEC_DONT_WAIT_FOR_EC);
		switch (res) {
			case nvec_io_not_ready:
				return 0;

			case nvec_io_read_ok:
			case nvec_io_retry:
				udelay(100);
				break;

			case nvec_io_error:
			case nvec_io_timeout:
				dbg_print();
				return 0;

			case nvec_io_write_ok:
			default:
				printf("!!! %s: unexpected io result %d\n", __func__, res);
				return 0;
		}
	}
	/*
	dbg_print();
	msg_print();
	*/

	return 0;
}


int nvec_have_keys(void)
{
	return key_i >= 0;
}


int nvec_pop_key(void)
{
	if (key_i == -1)
		return -1;

	int code = ((keys[key_i].state << 16) | keys[key_i].code);
	/*printf("key=%d, state=%d\n", code, keys[key_i].state);*/
	--key_i;

	return code;
}

static void nvec_configure_event(long mask, int state)
{
	char buf[7] = { NVEC_SYS, 1, state };

	buf[3] = (mask >> 16) & 0xff;
	buf[4] = (mask >> 24) & 0xff;
	buf[5] = (mask >> 0) & 0xff;
	buf[6] = (mask >> 8) & 0xff;

	if (nvec_do_request(buf, 7))
		printf("NVEC: failed to configure event (mask 0x%0lx, state %d)\n",
													mask, state);
};

static void nvec_toggle_global_events(int state)
{
	char global_events[] = { NVEC_SLEEP, GLOBAL_EVENTS, state };

	if (nvec_do_request(global_events, 3))
		printf("NVEC: failed to enable global events\n");
}
