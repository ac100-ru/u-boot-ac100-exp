#include <asm/arch-tegra/nvec-keyboard.h>
#include "nvec-keytable.h"
#include "nvec.h"


struct key_t {
	int code;
	int state;
};

struct key_t keys[NVEC_KEYS_QUEUE_SIZE];
int key_i = -1;


void nvec_process_keyboard_msg(const unsigned char* msg)
{
	int code, state;
	int event_type;
	int _size;

	event_type = nvec_msg_event_type(msg);
	if (event_type != NVEC_KEYBOARD)
		return;

	_size = (msg[0] & (3 << 5)) >> 5;

	if (_size == NVEC_VAR_SIZE)
		return;

	if (_size == NVEC_3BYTES)
		msg++;

	code = msg[1] & 0x7f;
	state = msg[1] & 0x80;

	nvec_push_key(code_tabs[_size][code], state);
}


void nvec_push_key(int code, int state)
{
	if (key_i + 1 >= NVEC_KEYS_QUEUE_SIZE)
		return;

	++key_i;
	keys[key_i].code = code;
	keys[key_i].state = state;
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
	--key_i;

	return code;
}

