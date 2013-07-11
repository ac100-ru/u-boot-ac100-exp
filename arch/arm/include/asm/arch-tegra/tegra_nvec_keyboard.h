#ifndef _NVIDIA_NVEC_KEYBOARD_H_
#define _NVIDIA_NVEC_KEYBOARD_H_


#define NVEC_KEYS_QUEUE_SIZE		256

void nvec_enable_kbd_events(void);
void nvec_process_keyboard_msg(const unsigned char *msg);
int nvec_pop_key(void);
int nvec_have_keys(void);


#endif /* _NVIDIA_NVEC_KEYBOARD_H_ */
