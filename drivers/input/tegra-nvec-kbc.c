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
	/* check if already created */
	if (config.created)
		return 0;

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
