/*
 *  (C) Copyright 2014
 *  Andrey Danin <danindrey@mail.ru>
 *  (C) Copyright 2011
 *  NVIDIA Corporation <www.nvidia.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <input.h>
#include <asm/arch-tegra/tegra_nvec.h>
#include <asm/arch-tegra/tegra_nvec_keyboard.h>


enum {
	KBC_MAX_KPENT = 8,
	KBC_REPEAT_RATE_MS	= 30,
	KBC_REPEAT_DELAY_MS	= 240,
};

/* keyboard config/state */
static struct keyb {
	int registered;
	struct input_config input;	/* The input layer */
	int pressed_keys[KBC_MAX_KPENT];
	int pressed_keys_cnt;
} config;


/* 0 - pressed, other - released */
char keys[256];


/* nvec commands */
static char enable_kbd[] = { NVEC_KBD, ENABLE_KBD };
static char reset_kbd[] = { NVEC_PS2, MOUSE_SEND_CMD, MOUSE_RESET, 3 };
static char clear_leds[] = { NVEC_KBD, SET_LEDS, 0 };


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
	int i;

	nvec_read_events();

	config.pressed_keys_cnt = 0;
	/* TODO Optimization required */
	for (i = 0; i < sizeof(keys); ++i) {
		if (keys[i] == 0 && config.pressed_keys_cnt < KBC_MAX_KPENT) {
			config.pressed_keys[config.pressed_keys_cnt] = i;
			++config.pressed_keys_cnt;
		}
	}

	input_send_keycodes(input,
			    config.pressed_keys,
			    config.pressed_keys_cnt);

	return config.pressed_keys_cnt;
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
	int key;


	event_type = nvec_msg_event_type(msg);
	if (event_type != NVEC_KEYBOARD)
		return -1;

	_size = (msg[0] & (3 << 5)) >> 5;

	if (_size == NVEC_VAR_SIZE) {
		debug("Skip unsupported msg (size: %d)\n", _size);
		return -1;
	}

	if (_size == NVEC_3BYTES)
		msg++;

	code = msg[1] & 0x7f;
	state = msg[1] & 0x80;

	key = code_tabs[_size][code];
	keys[key] = state;

	return 0;
}


int tegra_nvec_enable_kbd_events(void)
{
	if (nvec_do_request(reset_kbd, 4)) {
		error("NVEC: failed to reset keyboard\n");
		return -1;
	}
	if (nvec_do_request(clear_leds, 3)) {
		error("NVEC: failed to clear leds\n");
		return -1;
	}
	if (nvec_do_request(enable_kbd, 2)) {
		error("NVEC: failed to enable keyboard\n");
		return -1;
	}

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
	int error;
#ifdef CONFIG_CONSOLE_MUX
	char *stdinname = getenv("stdin");
#endif

	memset(keys, 1, sizeof(keys));

	config.registered = 0;

	if (input_init(&config.input, 0)) {
		printf("nvec kbc: cannot set up input\n");
		return -1;
	}
	input_set_delays(&config.input, KBC_REPEAT_DELAY_MS,
			 KBC_REPEAT_RATE_MS);
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
