#ifndef _ASM_ARCH_TEGRA_NVEC_H_
#define _ASM_ARCH_TEGRA_NVEC_H_

void nvec_enable_kbd_events(void);
int nvec_read_events(void);
int nvec_have_keys(void);
int nvec_pop_key(void);

#endif /* _ASM_ARCH_TEGRA_NVEC_H_ */
