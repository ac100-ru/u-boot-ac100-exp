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

typedef void (ansi_console_f)(void);
typedef void (ansi_console_char_f)(const char c);
typedef void (ansi_console_cursor_f)(int n);
typedef void (ansi_console_pos_f)(int x, int y);
typedef void (ansi_console_line_f)(int n, int start, int end);

struct ansi_console_t {
	ansi_console_char_f* putc;

	ansi_console_f*        cursor_set;
	ansi_console_cursor_f* cursor_enable;
	ansi_console_cursor_f* cursor_up;
	ansi_console_cursor_f* cursor_down;
	ansi_console_cursor_f* cursor_left;
	ansi_console_cursor_f* cursor_right;
	ansi_console_cursor_f* previous_line;
	ansi_console_cursor_f* new_line;

	ansi_console_pos_f*    set_position;

	ansi_console_line_f*   clear_line;

	ansi_console_f*        clear;
	ansi_console_f*        swap_colors;

	int* console_col;
	int* console_row;

	char ansi_buf[10];
	int ansi_buf_size;
	int ansi_colors_need_revert;
	int ansi_cursor_hidden;
};

void ansi_putc(struct ansi_console_t* console, const char c);
