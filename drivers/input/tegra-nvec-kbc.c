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

enum {
	KBC_MAX_KPENT = 8,
};

/* keyboard config/state */
static struct keyb {
	struct input_config input;	/* The input layer */
	int created;			/* 1 is driver has been created */
} config;


/**
 * Check the tegra nvec keyboard, and send any keys that are pressed.
 *
 * This is called by input_tstc() and input_getc() when they need more
 * characters
 *
 * @param input		Input configuration
 * @return 1, to indicate that we have something to look at
 */
int tegra_nvec_kbc_check(struct input_config *input)
{
	if (!config.created)
		return 0;

	int res = 0;
	int fifo[KBC_MAX_KPENT];
	int cnt = 0;

	if (!nvec_have_keys())
		nvec_read_events();

	while (nvec_have_keys() && cnt < KBC_MAX_KPENT) {
		res = 1;
		fifo[cnt++] = nvec_pop_key();
		if (cnt == KBC_MAX_KPENT) {
			input_send_keycodes(input, fifo, cnt);
			cnt = 0;
		}
	}

	if (cnt > 0) {
		input_send_keycodes(input, fifo, cnt);
	}

	return res;
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
 * Set up the keyboard. This is called by the stdio device handler
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
static int tegra_nvec_kbd_init(void)
{
	if (config.created)
		return 0;

	config.created = 1;

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
	config.created = 0;

	memset(&dev, '\0', sizeof(dev));
	strcpy(dev.name, "tegra-nvec-kbc");
	dev.flags = DEV_FLAGS_INPUT | DEV_FLAGS_SYSTEM;
	dev.getc = kbd_getc;
	dev.tstc = kbd_tstc;
	dev.start = tegra_nvec_kbd_init;

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
