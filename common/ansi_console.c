#include <ansi_console.h>


static void console_cursor_fix(struct ansi_console_t* console)
{
	if (*console->console_row < 0)
		*console->console_row = 0;
	if (*console->console_row >= console->rows)
		*console->console_row = console->rows - 1;
	if (*console->console_col < 0)
		*console->console_col = 0;
	if (*console->console_col >= console->cols)
		*console->console_col = console->cols - 1;
}

static void console_cursor_set_position(struct ansi_console_t* console,
		int col, int row)
{
	if (*console->console_row != -1)
		*console->console_row = row;
	if (*console->console_col != -1)
		*console->console_col = col;
	console_cursor_fix(console);
}

static inline void console_cursor_up(struct ansi_console_t* console, int n)
{
	*console->console_row -= n;
	if (console->console_row < 0)
		console->console_row = 0;
}

static inline void console_cursor_down(struct ansi_console_t* console, int n)
{
	*console->console_row += n;
	if (*console->console_row >= console->rows)
		*console->console_row = console->rows - 1;
}

static inline void console_cursor_left(struct ansi_console_t* console, int n)
{
	*console->console_col -= n;
	if (*console->console_col < 0)
		*console->console_col = 0;
}

static inline void console_cursor_right(struct ansi_console_t* console, int n)
{
	*console->console_col += n;
	if (*console->console_col >= console->cols)
		*console->console_col = console->cols - 1;
}

static inline void console_previous_line(struct ansi_console_t* console, int n)
{
	*console->console_col = 0;
	*console->console_row -= n;

	/* Check if we need to scroll the terminal */
	if (*console->console_row < 0) {
		if (console->scroll)
			console->scroll(1 - *console->console_row);
	}
	else if (console->sync)
		console->sync();
}

void ansi_putc(struct ansi_console_t* console, const char c)
{
	int i;

	if (c == 27) {
		for (i = 0; i < console->ansi_buf_size; ++i)
			console->putc(console->ansi_buf[i]);
		console->ansi_buf[0] = 27;
		console->ansi_buf_size = 1;
		return;
	}

	if (console->ansi_buf_size > 0) {
		/*
		 * 0 - ESC
		 * 1 - [
		 * 2 - num1
		 * 3 - ..
		 * 4 - ;
		 * 5 - num2
		 * 6 - ..
		 * - cchar
		 */
		int next = 0;

		int flush = 0;
		int fail = 0;

		int num1 = 0;
		int num2 = 0;
		int cchar = 0;

		console->ansi_buf[console->ansi_buf_size++] = c;

		if (console->ansi_buf_size >= sizeof(console->ansi_buf))
			fail = 1;

		for (i = 0; i < console->ansi_buf_size; ++i) {
			if (fail)
				break;

			switch (next) {
			case 0:
				if (console->ansi_buf[i] == 27)
					next = 1;
				else
					fail = 1;
				break;

			case 1:
				if (console->ansi_buf[i] == '[')
					next = 2;
				else
					fail = 1;
				break;

			case 2:
				if (console->ansi_buf[i] >= '0' && console->ansi_buf[i] <= '9') {
					num1 = console->ansi_buf[i]-'0';
					next = 3;
				} else if (console->ansi_buf[i] != '?') {
					--i;
					num1 = 1;
					next = 4;
				}
				break;

			case 3:
				if (console->ansi_buf[i] >= '0' && console->ansi_buf[i] <= '9') {
					num1 *= 10;
					num1 += console->ansi_buf[i]-'0';
				} else {
					--i;
					next = 4;
				}
				break;

			case 4:
				if (console->ansi_buf[i] != ';') {
					--i;
					next = 7;
				} else
					next = 5;
				break;

			case 5:
				if (console->ansi_buf[i] >= '0' && console->ansi_buf[i] <= '9') {
					num2 = console->ansi_buf[i]-'0';
					next = 6;
				} else
					fail = 1;
				break;

			case 6:
				if (console->ansi_buf[i] >= '0' && console->ansi_buf[i] <= '9') {
					num2 *= 10;
					num2 += console->ansi_buf[i]-'0';
				} else {
					--i;
					next = 7;
				}
				break;

			case 7:
				if ((console->ansi_buf[i] >= 'A' && console->ansi_buf[i] <= 'H')
					|| console->ansi_buf[i] == 'J'
					|| console->ansi_buf[i] == 'K'
					|| console->ansi_buf[i] == 'h'
					|| console->ansi_buf[i] == 'l'
					|| console->ansi_buf[i] == 'm') {
					cchar = console->ansi_buf[i];
					flush = 1;
				} else
					fail = 1;
				break;
			}
		}

		if (fail) {
			for (i = 0; i < console->ansi_buf_size; ++i)
				console->putc(console->ansi_buf[i]);
			console->ansi_buf_size = 0;
			return;
		}

		if (flush) {
			if (!console->ansi_cursor_hidden && console->cursor_enable)
				console->cursor_enable(0);
			console->ansi_buf_size = 0;
			switch (cchar) {
			case 'A':
				/* move cursor num1 rows up */
				console_cursor_up(console, num1);
				break;
			case 'B':
				/* move cursor num1 rows down */
				console_cursor_down(console, num1);
				break;
			case 'C':
				/* move cursor num1 columns forward */
				console_cursor_right(console, num1);
				break;
			case 'D':
				/* move cursor num1 columns back */
				console_cursor_left(console, num1);
				break;
			case 'E':
				/* move cursor num1 rows up at begin of row */
				console_previous_line(console, num1);
				break;
			case 'F':
				/* move cursor num1 rows down at begin of row */
				console->new_line(num1);
				break;
			case 'G':
				/* move cursor to column num1 */
				console_cursor_set_position(console, -1, num1-1);
				break;
			case 'H':
				/* move cursor to row num1, column num2 */
				console_cursor_set_position(console, num1-1, num2-1);
				break;
			case 'J':
				/* clear console and move cursor to 0, 0 */
				console->clear();
				console_cursor_set_position(console, 0, 0);
				break;
			case 'K':
				/* clear line */
				if (num1 == 0)
					console->clear_line(*console->console_row,
							*console->console_col,
							-1);
				else if (num1 == 1)
					console->clear_line(*console->console_row,
							0, *console->console_col);
				else
					console->clear_line(*console->console_row,
							0, -1);
				break;
			case 'h':
				console->ansi_cursor_hidden = 0;
				break;
			case 'l':
				console->ansi_cursor_hidden = 1;
				break;
			case 'm':
				if (num1 == 0) { /* reset swapped colors */
					if (console->ansi_colors_need_revert && console->swap_colors) {
						console->swap_colors();
						console->ansi_colors_need_revert = 0;
					}
				} else if (num1 == 7) { /* once swap colors */
					if (!console->ansi_colors_need_revert && console->swap_colors) {
						console->swap_colors();
						console->ansi_colors_need_revert = 1;
					}
				}
				break;
			}
			if (!console->ansi_cursor_hidden && console->cursor_set)
				console->cursor_set();
		}
	} else {
		console->putc(c);
	}
}
