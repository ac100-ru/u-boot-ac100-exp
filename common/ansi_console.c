#include <ansi_console.h>

#define COL (*console->console_col)
#define ROW (*console->console_row)

#ifdef CONFIG_CONSOLE_ANSI_EXTENSION_ENABLED
static void console_cursor_fix(struct ansi_console_t* console)
{
	if (ROW < 0)
		ROW = 0;
	if (ROW >= console->rows)
		ROW = console->rows - 1;
	if (COL < 0)
		COL = 0;
	if (COL >= console->cols)
		COL = console->cols - 1;
}

static void console_cursor_set_position(struct ansi_console_t* console,
		int col, int row)
{
	if (ROW != -1)
		ROW = row;
	if (COL != -1)
		COL = col;
	console_cursor_fix(console);
}
#endif /* CONFIG_CONSOLE_ANSI_EXTENSION_ENABLED */

static inline void console_cursor_up(struct ansi_console_t* console, int n)
{
	ROW -= n;
	if (console->console_row < 0)
		console->console_row = 0;
}

static inline void console_cursor_down(struct ansi_console_t* console, int n)
{
	ROW += n;
	if (ROW >= console->rows)
		ROW = console->rows - 1;
}

static inline void console_cursor_left(struct ansi_console_t* console, int n)
{
	COL -= n;
	if (COL < 0)
		COL = 0;
}

static inline void console_cursor_right(struct ansi_console_t* console, int n)
{
	COL += n;
	if (COL >= console->cols)
		COL = console->cols - 1;
}

static inline void console_previous_line(struct ansi_console_t* console, int n)
{
	COL = 0;
	ROW -= n;

	/* Check if we need to scroll the terminal */
	if (ROW < 0) {
		if (console->scroll)
			console->scroll(1 - ROW);
	}
	else if (console->sync)
		console->sync();
}

static void console_new_line(struct ansi_console_t* console, int n)
{
	COL = 0;
	ROW += n;

	/* Check if we need to scroll the terminal */
	if (ROW >= console->rows) {
		if (console->scroll)
			console->scroll(console->rows - ROW + 1);
		ROW = console->rows - 1;
	}
	else if (console->sync)
		console->sync();
}

static void console_caret_return(struct ansi_console_t* console)
{
	COL = 0;
}

static inline void console_back(struct ansi_console_t* console)
{
	if (--COL < 0) {
		COL = console->cols-1 ;
		if (--ROW < 0)
			ROW = 0;
	}

	console->putc_cr(COL,
		ROW, ' ');
}


static void console_putc(struct ansi_console_t* console, const char c)
{
	switch (c) {
		case '\r':		/* back to first column */
			console_caret_return(console);
			break;

		case '\n':		/* next line */
			console_new_line(console, 1);
			break;

		case '\t':		/* tab 8 */
			COL |= 0x0008;
			COL &= ~0x0007;

			if (COL >= console->cols)
				console_new_line(console, 1);
			break;

		case '\b':		/* backspace */
			console_back(console);
			break;

		case 7:		/* bell */
			break;	/* ignored */

		default:		/* draw the char */
			console->putc_cr(COL, ROW, c);
			COL++;

			/* check for new line */
			if (COL >= console->cols)
				console_new_line(console, 1);
	}
}


void ansi_putc(struct ansi_console_t* console, const char c)
{
#ifdef CONFIG_CONSOLE_ANSI_EXTENSION_ENABLED
	int i;

	if (c == 27) {
		for (i = 0; i < console->ansi_buf_size; ++i)
			console_putc(console, console->ansi_buf[i]);
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
				console_putc(console, console->ansi_buf[i]);
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
				console_new_line(console, num1);
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
					console->clear_line(ROW, COL, -1);
				else if (num1 == 1)
					console->clear_line(ROW, 0, COL);
				else
					console->clear_line(ROW, 0, -1);
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
	} else
#endif /* CONFIG_ANSI_CONSOLE_EXTENSION_ENABLED */
		console_putc(console, c);
}
