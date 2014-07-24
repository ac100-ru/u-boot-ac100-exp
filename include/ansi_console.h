/*
 * (C) Copyright 2012
 * Pali Rohár <pali.rohar@gmail.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/*
 * ANSI terminal
 */

#include <common.h>

struct ansi_console_t {
	void (*putc)(const char c);

	void (*previous_line)(int n);
	void (*new_line)(int n);

	void (*clear_line)(int line, int begin, int end);

	void (*clear)(void);
	void (*swap_colors)(void);

	/* Optional */
	void (*cursor_set)(void);
	void (*cursor_enable)(int state);

	int cols;
	int rows;
	int* console_col;
	int* console_row;

	char ansi_buf[10];
	int ansi_buf_size;
	int ansi_colors_need_revert;
	int ansi_cursor_hidden;
};

void ansi_putc(struct ansi_console_t* console, const char c);
