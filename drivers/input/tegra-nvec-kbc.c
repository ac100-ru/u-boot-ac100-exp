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
#include <input.h>
#include <circbuf.h>
#include <asm/arch-tegra/tegra_nvec.h>
#include <asm/arch-tegra/tegra_nvec_keyboard.h>

enum {
	KBC_MAX_KPENT = 8,
};

/* keyboard config/state */
static struct keyb {
	int registered;
	struct input_config input;	/* The input layer */
} config;

/* keyboard events buffer */
static circbuf_t key_buf = { 0, 0, NULL, NULL, NULL, NULL };

/* nvec commands */
static char enable_kbd[] = { NVEC_KBD, ENABLE_KBD };
static char reset_kbd[] = { NVEC_PS2, MOUSE_SEND_CMD, MOUSE_RESET, 3 };
static char clear_leds[] = { NVEC_KBD, SET_LEDS, 0 };


void nvec_push_key(unsigned short code, unsigned short state)
{
	int code_state;

	assert(key_buf.totalsize > 0);

	if (key_buf.size == key_buf.totalsize)
		return;

	code_state = ((state << 16) | code);
	buf_push(&key_buf, (const char *)&code_state, sizeof(code_state));
}


int nvec_have_keys(void)
{
	return key_buf.size > 0;
}


int nvec_pop_key(void)
{
	int code_state;
	int len = buf_pop(&key_buf, (char *)&code_state, sizeof(code_state));

	if (len < sizeof(code_state))
		return -1;

	return code_state;
}


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

	if (cnt > 0)
		input_send_keycodes(input, fifo, cnt);

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


int tegra_nvec_process_keyboard_msg(const unsigned char *msg)
{
	int code, state;
	int event_type;
	int _size;

	event_type = nvec_msg_event_type(msg);
	if (event_type != NVEC_KEYBOARD)
		return -1;

	_size = (msg[0] & (3 << 5)) >> 5;

	if (_size == NVEC_VAR_SIZE)
		return -1;

	if (_size == NVEC_3BYTES)
		msg++;

	code = msg[1] & 0x7f;
	state = msg[1] & 0x80;

	nvec_push_key(code_tabs[_size][code], state);

	return 0;
}


int tegra_nvec_enable_kbd_events(void)
{
	buf_init(&key_buf, NVEC_KEYS_QUEUE_SIZE * sizeof(int));

	if (nvec_do_request(reset_kbd, 4))
		error("NVEC: failed to reset keyboard\n");
	if (nvec_do_request(clear_leds, 3))
		error("NVEC: failed to clear leds\n");
	if (nvec_do_request(enable_kbd, 2))
		error("NVEC: failed to enable keyboard\n");

	debug("NVEC: keyboard initialization finished\n");

	return 0;
}


static int tegra_nvec_kbc_start(void)
{
	if (config.registered)
		return 0;

	struct nvec_periph nvec_keyboard;
	memset(&nvec_keyboard, 0, sizeof(nvec_keyboard));
	nvec_keyboard.start = tegra_nvec_enable_kbd_events;
	nvec_keyboard.process_msg = tegra_nvec_process_keyboard_msg;

	if (nvec_register_periph(NVEC_KEYBOARD, &nvec_keyboard)) {
		error("NVEC: failed to register keyboard perephirial device");
		return -1;
	}

	config.registered = 1;

	return 0;
}


int drv_keyboard_init(void)
{
	struct stdio_dev dev;
	char *stdinname = getenv("stdin");
	int error;

	config.registered = 0;

	if (input_init(&config.input, 0)) {
		printf("nvec kbc: cannot set up input\n");
		return -1;
	}
	config.input.read_keys = tegra_nvec_kbc_check;

	memset(&dev, '\0', sizeof(dev));
	strcpy(dev.name, "tegra-nvec-kbc");
	dev.flags = DEV_FLAGS_INPUT | DEV_FLAGS_SYSTEM;
	dev.getc = kbd_getc;
	dev.tstc = kbd_tstc;
	dev.start = tegra_nvec_kbc_start;

	/* Register the device. tegra_nvec_kbc_start() will be called soon */
	error = input_stdio_register(&dev);
	if (error) {
		printf("nvec kbc: failed to register stdio device, %d\n",
		       error);
		return error;
	}
#ifdef CONFIG_CONSOLE_MUX
	error = iomux_doenv(stdin, stdinname);
	if (error) {
		printf("nvec kbc: iomux_doenv failed, %d\n", error);
		return error;
	}
#endif
	return 0;
}
