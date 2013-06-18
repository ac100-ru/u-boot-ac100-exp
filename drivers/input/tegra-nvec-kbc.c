/*
 *  (C) Copyright 2011,2013
 *  Andrey Danin <danindrey@mail.ru>
 *  NVIDIA Corporation <www.nvidia.com>
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <fdtdec.h>
#include <input.h>
#include <key_matrix.h>
#include <stdio_dev.h>
#include <tegra-kbc.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/funcmux.h>
#include <asm/arch-tegra/timer.h>
#include <asm/arch-tegra/nvec-events.h>
#include <asm/arch-tegra/nvec-keyboard.h>
#include <linux/input.h>

DECLARE_GLOBAL_DATA_PTR;

enum {
	KBC_MAX_GPIO		= 24,
	KBC_MAX_KPENT		= 8,	/* size of keypress entry queue */
};

#define KBC_FIFO_TH_CNT_SHIFT		14
#define KBC_DEBOUNCE_CNT_SHIFT		4
#define KBC_CONTROL_FIFO_CNT_INT_EN	(1 << 3)
#define KBC_CONTROL_KBC_EN		(1 << 0)
#define KBC_INT_FIFO_CNT_INT_STATUS	(1 << 2)
#define KBC_KPENT_VALID			(1 << 7)
#define KBC_ST_STATUS			(1 << 3)

enum {
	KBC_DEBOUNCE_COUNT	= 2,
	KBC_REPEAT_RATE_MS	= 30,
	KBC_REPEAT_DELAY_MS	= 240,
	KBC_CLOCK_KHZ		= 32,	/* Keyboard uses a 32KHz clock */
};

/* keyboard controller config and state */
static struct keyb {
	struct input_config input;	/* The input layer */
	struct key_matrix matrix;	/* The key matrix layer */

	struct kbc_tegra *kbc;		/* tegra keyboard controller */
	unsigned char inited;		/* 1 if keyboard has been inited */
	unsigned char first_scan;	/* 1 if this is our first key scan */
	unsigned char created;		/* 1 if driver has been created */

	/*
	 * After init we must wait a short time before polling the keyboard.
	 * This gives the tegra keyboard controller time to react after reset
	 * and lets us grab keys pressed during reset.
	 */
	unsigned int init_dly_ms;	/* Delay before we can read keyboard */
	unsigned int start_time_ms;	/* Time that we inited (in ms) */
	unsigned int last_poll_ms;	/* Time we should last polled */
	unsigned int next_repeat_ms;	/* Next time we repeat a key */
} config;


/**
 * Process all the keypress sequences in fifo and send key codes
 *
 * The fifo contains zero or more keypress sets. Each set
 * consists of from 1-8 keycodes, representing the keycodes which
 * were simultaneously pressed during that scan.
 *
 * This function works through each set and generates ASCII characters
 * for each. Not that one set may produce more than one ASCII characters -
 * for example holding down 'd' and 'f' at the same time will generate
 * two ASCII characters.
 *
 * Note: if fifo_cnt is 0, we will tell the input layer that no keys are
 * pressed.
 *
 * @param config	Keyboard config
 * @param fifo_cnt	Number of entries in the keyboard fifo
 */
#if 0
static void process_fifo(struct keyb *config, int fifo_cnt)
{
	int fifo[KBC_MAX_KPENT];
	int cnt = 0;

	/* Always call input_send_keycodes() at least once */
	do {
		if (fifo_cnt)
			cnt = tegra_kbc_find_keys(config, fifo, KBC_MAX_KPENT);

		input_send_keycodes(&config->input, fifo, cnt);
	} while (--fifo_cnt > 0);
}
#endif


/**
 * Check the keyboard controller and emit ASCII characters for any keys that
 * are pressed.
 *
 * @param config	Keyboard config
 */
static int check_for_keys(struct keyb *config)
{
	int res = 0;
	int fifo[KBC_MAX_KPENT];
	int cnt = 0;
	/*int fifo_cnt;*/

	/*
	if (!config->first_scan &&
			get_timer(config->last_poll_ms) < KBC_REPEAT_RATE_MS)
		return 0;

	config->last_poll_ms = get_timer(0);
	config->first_scan = 0;
	*/

	/*
	 * Once we get here we know the keyboard has been scanned. So if there
	 * scan waiting for us, we know that nothing is held down.
	 */
	/*fifo_cnt = (readl(&config->kbc->interrupt) >> 4) & 0xf;*/
	/*process_fifo(config, fifo_cnt);*/
	if (!nvec_have_keys())
		nvec_read_events();

	while (nvec_have_keys() && cnt < KBC_MAX_KPENT) {
		res = 1;
		fifo[cnt++] = nvec_pop_key();
		if (cnt == KBC_MAX_KPENT) {
			input_send_keycodes(&config->input, fifo, cnt);
			cnt = 0;
		}
	}

	if (cnt > 0) {
		input_send_keycodes(&config->input, fifo, cnt);
	}

	return res;
}


/**
 * Check the tegra keyboard, and send any keys that are pressed.
 *
 * This is called by input_tstc() and input_getc() when they need more
 * characters
 *
 * @param input		Input configuration
 * @return 1, to indicate that we have something to look at
 */
int tegra_nvec_kbc_check(struct input_config *input)
{
	return check_for_keys(&config);
}

/**
 * Test if keys are available to be read
 *
 * @return 0 if no keys available, 1 if keys are available
 */
static int kbd_tstc(void)
{
	/*printf("%s: \n", __func__);*/
	/* Just get input to do this for us */
	return input_tstc(&config.input);
}

/**
 * Read a key
 *
 * TODO: U-Boot wants 0 for no key, but Ctrl-@ is a valid key...
 *
 * @return ASCII key code, or 0 if no key, or -1 if error
 */
static int kbd_getc(void)
{
	/*printf("%s: \n", __func__);*/
	/* Just get input to do this for us */
	return input_getc(&config.input);
}


/**
 * Set up the tegra keyboard. This is called by the stdio device handler
 *
 * We want to do this init when the keyboard is actually used rather than
 * at start-up, since keyboard input may not currently be selected.
 *
 * Once the keyboard starts there will be a period during which we must
 * wait for the keyboard to init. We do this only when a key is first
 * read - see kbd_wait_for_fifo_init().
 *
 * @return 0 if ok, -ve on error
 */
static int init_nvec_keyboard(void)
{
	/*printf("%s: \n", __func__);*/

	/* check if already created */
	if (config.created)
		return 0;

#if 0
#ifdef CONFIG_OF_CONTROL
	int	node;

	node = fdtdec_next_compatible(gd->fdt_blob, 0,
					  COMPAT_NVIDIA_TEGRA20_KBC);
	if (node < 0) {
		printf("%s: cannot locate keyboard node\n", __func__);
		return node;
	}
	config.kbc = (struct kbc_tegra *)fdtdec_get_addr(gd->fdt_blob,
		       node, "reg");
	if ((fdt_addr_t)config.kbc == FDT_ADDR_T_NONE) {
		printf("%s: No keyboard register found\n", __func__);
		return -1;
	}
	input_set_delays(&config.input, KBC_REPEAT_DELAY_MS,
			KBC_REPEAT_RATE_MS);

	/* Decode the keyboard matrix information (16 rows, 8 columns) */
	if (key_matrix_init(&config.matrix, 16, 8, 1)) {
		printf("%s: Could not init key matrix\n", __func__);
		return -1;
	}
	if (key_matrix_decode_fdt(&config.matrix, gd->fdt_blob, node)) {
		printf("%s: Could not decode key matrix from fdt\n", __func__);
		return -1;
	}
	if (config.matrix.fn_keycode) {
		if (input_add_table(&config.input, KEY_FN, -1,
				    config.matrix.fn_keycode,
				    config.matrix.key_count)) {
			printf("%s: can't add table\n", __func__);
			return -1;
		}
	}
#else
#error "Tegra keyboard driver requires FDT definitions"
#endif

	/* Set up pin mux and enable the clock */
	funcmux_select(PERIPH_ID_KBC, FUNCMUX_DEFAULT);
	clock_enable(PERIPH_ID_KBC);
	config_kbc_gpio(config.kbc);

	tegra_kbc_open();
#endif // 0

	config.created = 1;
	config.first_scan = 1;

	printf("%s: NVEC keyboard ready\n", __func__);

	return 0;
}

int drv_keyboard_init(void)
{
	struct stdio_dev dev;
	char *stdinname = getenv("stdin");
	int error;

	printf("%s: \n", __func__);
	if (input_init(&config.input, 0)) {
		printf("%s: Cannot set up input\n", __func__);
		return -1;
	}
	config.input.read_keys = tegra_nvec_kbc_check;

	memset(&dev, '\0', sizeof(dev));
	strcpy(dev.name, "tegra-nvec-kbc");
	dev.flags = DEV_FLAGS_INPUT | DEV_FLAGS_SYSTEM;
	dev.getc = kbd_getc;
	dev.tstc = kbd_tstc;
	dev.start = init_nvec_keyboard;

	/* Register the device. init_tegra_keyboard() will be called soon */
	error = input_stdio_register(&dev);
	if (error) {
		printf("failed to register nvec keyboard, %d\n", error);
		return error;
	}
#ifdef CONFIG_CONSOLE_MUX
	error = iomux_doenv(stdin, stdinname);
	if (error) {
		printf("iomux_doenv failed, %d\n", error);
		return error;
	}
#endif
	return 0;
}
