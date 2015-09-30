/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

struct screen *window_copy_init(struct window_pane *);
void	window_copy_free(struct window_pane *);
void	window_copy_resize(struct window_pane *, u_int, u_int);
void	window_copy_key(struct window_pane *, struct session *, int);
int	window_copy_key_input(struct window_pane *, int);
int	window_copy_key_numeric_prefix(struct window_pane *, int);
void	window_copy_mouse(struct window_pane *, struct session *,
	    struct mouse_event *);

void	window_copy_redraw_selection(struct window_pane *, u_int);
void	window_copy_redraw_lines(struct window_pane *, u_int, u_int);
void	window_copy_redraw_screen(struct window_pane *);
void	window_copy_write_line(struct window_pane *, struct screen_write_ctx *,
	    u_int);
void	window_copy_write_lines(struct window_pane *,
	    struct screen_write_ctx *, u_int, u_int);

void	window_copy_scroll_to(struct window_pane *, u_int, u_int);
int	window_copy_search_compare(struct grid *, u_int, u_int, struct grid *,
	    u_int, int);
int	window_copy_search_lr(struct grid *, struct grid *, u_int *, u_int,
	    u_int, u_int, int);
int	window_copy_search_rl(struct grid *, struct grid *, u_int *, u_int,
	    u_int, u_int, int);
void	window_copy_search_up(struct window_pane *, const char *);
void	window_copy_search_down(struct window_pane *, const char *);
void	window_copy_goto_line(struct window_pane *, const char *);
void	window_copy_update_cursor(struct window_pane *, u_int, u_int);
void	window_copy_start_selection(struct window_pane *);
int	window_copy_update_selection(struct window_pane *);
void   *window_copy_get_selection(struct window_pane *, size_t *);
void	window_copy_copy_buffer(struct window_pane *, const char *, void *,
	    size_t);
void	window_copy_copy_pipe(struct window_pane *, struct session *,
	    const char *, const char *);
int	window_copy_copy_selection(struct window_pane *, const char *);
void	window_copy_append_selection(struct window_pane *, const char *);
void	window_copy_clear_selection(struct window_pane *);
int	window_copy_copy_line(struct window_pane *, char **, size_t *, u_int,
	    u_int, u_int);
int	window_copy_in_set(struct window_pane *, u_int, u_int, const char *);
u_int	window_copy_find_length(struct window_pane *, u_int);
void	window_copy_cursor_start_of_line(struct window_pane *);
void	window_copy_cursor_back_to_indentation(struct window_pane *);
void	window_copy_cursor_end_of_line(struct window_pane *);
void	window_copy_other_end(struct window_pane *);
void	window_copy_cursor_left(struct window_pane *);
void	window_copy_cursor_right(struct window_pane *);
void	window_copy_cursor_up(struct window_pane *, int);
void	window_copy_cursor_down(struct window_pane *, int);
void	window_copy_cursor_jump(struct window_pane *);
void	window_copy_cursor_jump_back(struct window_pane *);
void	window_copy_cursor_jump_to(struct window_pane *, int);
void	window_copy_cursor_jump_to_back(struct window_pane *, int);
void	window_copy_cursor_next_word(struct window_pane *, const char *);
void	window_copy_cursor_next_word_end(struct window_pane *, const char *);
void	window_copy_cursor_previous_word(struct window_pane *, const char *);
void	window_copy_scroll_up(struct window_pane *, u_int);
void	window_copy_scroll_down(struct window_pane *, u_int);
void	window_copy_left_prune(struct window_pane *);
void	window_copy_right_prune(struct window_pane *);
void	window_copy_change_joinmode(struct window_pane *);

const struct window_mode window_copy_mode = {
	window_copy_init,
	window_copy_free,
	window_copy_resize,
	window_copy_key,
	window_copy_mouse,
	NULL,
};

enum window_copy_input_type {
	WINDOW_COPY_OFF,
	WINDOW_COPY_NAMEDBUFFER,
	WINDOW_COPY_NUMERICPREFIX,
	WINDOW_COPY_SEARCHUP,
	WINDOW_COPY_SEARCHDOWN,
	WINDOW_COPY_JUMPFORWARD,
	WINDOW_COPY_JUMPBACK,
	WINDOW_COPY_JUMPTOFORWARD,
	WINDOW_COPY_JUMPTOBACK,
	WINDOW_COPY_GOTOLINE,
};

enum window_copy_join_mode {
	WINDOW_COPY_JOIN_NEWLINE,
	WINDOW_COPY_JOIN_NONE,
	WINDOW_COPY_JOIN_SPACE,
	WINDOW_COPY_JOIN_COMMA,
	WINDOW_COPY_JOIN_MAX
};

struct window_copy_join_mode_data {
	const char *header;
	const char *delimiter;
} join_modes[WINDOW_COPY_JOIN_MAX] = {
	{
		"",
		"\n",
	},
	{
		" [joined]",
		"",
	},
	{
		" [joined with spaces]",
		" ",
	},
	{
		" [joined with commas]",
		",",
	},
};

/*
 * Copy-mode's visible screen (the "screen" field) is filled from one of
 * two sources: the original contents of the pane (used when we
 * actually enter via the "copy-mode" command, to copy the contents of
 * the current pane), or else a series of lines containing the output
 * from an output-writing tmux command (such as any of the "show-*" or
 * "list-*" commands).
 *
 * In either case, the full content of the copy-mode grid is pointed at
 * by the "backing" field, and is copied into "screen" as needed (that
 * is, when scrolling occurs). When copy-mode is backed by a pane,
 * backing points directly at that pane's screen structure (&wp->base);
 * when backed by a list of output-lines from a command, it points at
 * a newly-allocated screen structure (which is deallocated when the
 * mode ends).
 */
struct window_copy_mode_data {
	struct screen	screen;

	struct screen  *backing;
	int		backing_written; /* backing display has started */

	struct mode_key_data mdata;

	u_int		oy;

	u_int		selx;
	u_int		sely;

	u_int		cx;
	u_int		cy;

	int		leftprunex_set;
	u_int		leftprunex;
	int		rightprunex_set;
	u_int		rightprunex;

	enum window_copy_join_mode joinmode;

	enum window_copy_input_type inputtype;
	const char     *inputprompt;
	char	       *inputstr;

	int		numprefix;

	enum window_copy_input_type searchtype;
	char	       *searchstr;

	enum window_copy_input_type jumptype;
	char		jumpchar;
};

struct screen *
window_copy_init(struct window_pane *wp)
{
	struct window_copy_mode_data	*data;
	struct screen			*s;

	wp->modedata = data = xmalloc(sizeof *data);
	data->oy = 0;
	data->cx = 0;
	data->cy = 0;

	data->leftprunex_set = 0;
	data->rightprunex_set = 0;

	data->joinmode = WINDOW_COPY_JOIN_NEWLINE;

	data->backing_written = 0;

	data->inputtype = WINDOW_COPY_OFF;
	data->inputprompt = NULL;
	data->inputstr = xstrdup("");
	data->numprefix = -1;

	data->searchtype = WINDOW_COPY_OFF;
	data->searchstr = NULL;

	if (wp->fd != -1)
		bufferevent_disable(wp->event, EV_READ|EV_WRITE);

	data->jumptype = WINDOW_COPY_OFF;
	data->jumpchar = '\0';

	s = &data->screen;
	screen_init(s, screen_size_x(&wp->base), screen_size_y(&wp->base), 0);
	if (options_get_number(&wp->window->options, "mode-mouse"))
		s->mode |= MODE_MOUSE_STANDARD;

	mode_key_init(&data->mdata, &mode_key_tree_vi_copy);

	data->backing = NULL;

	return (s);
}

void
window_copy_init_from_pane(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	struct screen_write_ctx	 	 ctx;
	u_int				 i;

	if (wp->mode != &window_copy_mode)
		fatalx("not in copy mode");

	data->backing = &wp->base;
	data->cx = data->backing->cx;
	data->cy = data->backing->cy;

	s->cx = data->cx;
	s->cy = data->cy;

	screen_write_start(&ctx, NULL, s);
	for (i = 0; i < screen_size_y(s); i++)
		window_copy_write_line(wp, &ctx, i);
	screen_write_cursormove(&ctx, data->cx, data->cy);
	screen_write_stop(&ctx);
}

void
window_copy_init_for_output(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;

	data->backing = xmalloc(sizeof *data->backing);
	screen_init(data->backing, screen_size_x(&wp->base),
	    screen_size_y(&wp->base), UINT_MAX);
}

void
window_copy_free(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;

	if (wp->fd != -1)
		bufferevent_enable(wp->event, EV_READ|EV_WRITE);

	free(data->searchstr);
	free(data->inputstr);

	if (data->backing != &wp->base) {
		screen_free(data->backing);
		free(data->backing);
	}
	screen_free(&data->screen);

	free(data);
}

void
window_copy_add(struct window_pane *wp, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	window_copy_vadd(wp, fmt, ap);
	va_end(ap);
}

void
window_copy_vadd(struct window_pane *wp, const char *fmt, va_list ap)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*backing = data->backing;
	struct screen_write_ctx	 	 back_ctx, ctx;
	struct grid_cell		 gc;
	int				 utf8flag;
	u_int				 old_hsize, old_cy;

	if (backing == &wp->base)
		return;

	utf8flag = options_get_number(&wp->window->options, "utf8");
	memcpy(&gc, &grid_default_cell, sizeof gc);

	old_hsize = screen_hsize(data->backing);
	screen_write_start(&back_ctx, NULL, backing);
	if (data->backing_written) {
		/*
		 * On the second or later line, do a CRLF before writing
		 * (so it's on a new line).
		 */
		screen_write_carriagereturn(&back_ctx);
		screen_write_linefeed(&back_ctx, 0);
	} else
		data->backing_written = 1;
	old_cy = backing->cy;
	screen_write_vnputs(&back_ctx, 0, &gc, utf8flag, fmt, ap);
	screen_write_stop(&back_ctx);

	data->oy += screen_hsize(data->backing) - old_hsize;

	screen_write_start(&ctx, wp, &data->screen);

	/*
	 * If the history has changed, draw the top line.
	 * (If there's any history at all, it has changed.)
	 */
	if (screen_hsize(data->backing))
		window_copy_redraw_lines(wp, 0, 1);

	/* Write the new lines. */
	window_copy_redraw_lines(wp, old_cy, backing->cy - old_cy + 1);

	screen_write_stop(&ctx);
}

void
window_copy_pageup(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	u_int				 n;

	n = 1;
	if (screen_size_y(s) > 2)
		n = screen_size_y(s) - 2;
	if (data->oy + n > screen_hsize(data->backing))
		data->oy = screen_hsize(data->backing);
	else
		data->oy += n;
	window_copy_update_selection(wp);
	window_copy_redraw_screen(wp);
}

void
window_copy_resize(struct window_pane *wp, u_int sx, u_int sy)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	struct screen_write_ctx	 	 ctx;

	screen_resize(s, sx, sy, 1);
	if (data->backing != &wp->base)
		screen_resize(data->backing, sx, sy, 1);

	if (data->cy > sy - 1)
		data->cy = sy - 1;
	if (data->cx > sx)
		data->cx = sx;
	if (data->oy > screen_hsize(data->backing))
		data->oy = screen_hsize(data->backing);

	window_copy_clear_selection(wp);

	screen_write_start(&ctx, NULL, s);
	window_copy_write_lines(wp, &ctx, 0, screen_size_y(s) - 1);
	screen_write_stop(&ctx);

	window_copy_redraw_screen(wp);
}

void
window_copy_key(struct window_pane *wp, struct session *sess, int key)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	u_int				 n;
	int				 np, first;
	enum mode_key_cmd		 cmd;
	const char			*arg, *ss;

	np = data->numprefix;
	if (np <= 0)
		np = 1;

	if (data->inputtype == WINDOW_COPY_JUMPFORWARD ||
	    data->inputtype == WINDOW_COPY_JUMPBACK ||
	    data->inputtype == WINDOW_COPY_JUMPTOFORWARD ||
	    data->inputtype == WINDOW_COPY_JUMPTOBACK) {
		/* Ignore keys with modifiers. */
		if ((key & KEYC_MASK_MOD) == 0) {
			data->jumpchar = key;
			if (data->inputtype == WINDOW_COPY_JUMPFORWARD) {
				for (; np != 0; np--)
					window_copy_cursor_jump(wp);
			}
			if (data->inputtype == WINDOW_COPY_JUMPBACK) {
				for (; np != 0; np--)
					window_copy_cursor_jump_back(wp);
			}
			if (data->inputtype == WINDOW_COPY_JUMPTOFORWARD) {
				first = 1;
				for (; np != 0; np--) {
					window_copy_cursor_jump_to(wp, first);
					first = 0;
				}
			}
			if (data->inputtype == WINDOW_COPY_JUMPTOBACK) {
				first = 1;
				for (; np != 0; np--) {
					window_copy_cursor_jump_to_back(wp, first);
					first = 0;
				}
			}
		}
		data->jumptype = data->inputtype;
		data->inputtype = WINDOW_COPY_OFF;
		window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
		return;
	} else if (data->inputtype == WINDOW_COPY_NUMERICPREFIX) {
		if (window_copy_key_numeric_prefix(wp, key) == 0)
			return;
		data->inputtype = WINDOW_COPY_OFF;
		window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
	} else if (data->inputtype != WINDOW_COPY_OFF) {
		if (window_copy_key_input(wp, key) != 0)
			goto input_off;
		return;
	}

	cmd = mode_key_lookup(&data->mdata, key, &arg);
	switch (cmd) {
	case MODEKEYCOPY_APPENDSELECTION:
		if (sess != NULL) {
			window_copy_append_selection(wp, NULL);
			window_pane_reset_mode(wp);
			return;
		}
		break;
	case MODEKEYCOPY_CANCEL:
		window_pane_reset_mode(wp);
		return;
	case MODEKEYCOPY_OTHEREND:
		if (np % 2)
			window_copy_other_end(wp);
		break;
	case MODEKEYCOPY_LEFT:
		for (; np != 0; np--)
			window_copy_cursor_left(wp);
		break;
	case MODEKEYCOPY_RIGHT:
		for (; np != 0; np--)
			window_copy_cursor_right(wp);
		break;
	case MODEKEYCOPY_UP:
		for (; np != 0; np--)
			window_copy_cursor_up(wp, 0);
		break;
	case MODEKEYCOPY_DOWN:
		for (; np != 0; np--)
			window_copy_cursor_down(wp, 0);
		break;
	case MODEKEYCOPY_SCROLLUP:
		for (; np != 0; np--)
			window_copy_cursor_up(wp, 1);
		break;
	case MODEKEYCOPY_SCROLLDOWN:
		for (; np != 0; np--)
			window_copy_cursor_down(wp, 1);
		break;
	case MODEKEYCOPY_PREVIOUSPAGE:
		for (; np != 0; np--)
			window_copy_pageup(wp);
		break;
	case MODEKEYCOPY_NEXTPAGE:
		n = 1;
		if (screen_size_y(s) > 2)
			n = screen_size_y(s) - 2;
		for (; np != 0; np--) {
			if (data->oy < n)
				data->oy = 0;
			else
				data->oy -= n;
		}
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_HALFPAGEUP:
		n = screen_size_y(s) / 2;
		for (; np != 0; np--) {
			if (data->oy + n > screen_hsize(data->backing))
				data->oy = screen_hsize(data->backing);
			else
				data->oy += n;
		}
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_HALFPAGEDOWN:
		n = screen_size_y(s) / 2;
		for (; np != 0; np--) {
			if (data->oy < n)
				data->oy = 0;
			else
				data->oy -= n;
		}
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_TOPLINE:
		data->cx = 0;
		data->cy = 0;
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_MIDDLELINE:
		data->cx = 0;
		data->cy = (screen_size_y(s) - 1) / 2;
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_BOTTOMLINE:
		data->cx = 0;
		data->cy = screen_size_y(s) - 1;
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_HISTORYTOP:
		data->cx = 0;
		data->cy = 0;
		data->oy = screen_hsize(data->backing);
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_HISTORYBOTTOM:
		data->cx = 0;
		data->cy = screen_size_y(s) - 1;
		data->oy = 0;
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_STARTSELECTION:
		window_copy_start_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_SELECTLINE:
		/* FALLTHROUGH */
	case MODEKEYCOPY_COPYLINE:
		window_copy_cursor_start_of_line(wp);
		/* FALLTHROUGH */
	case MODEKEYCOPY_COPYENDOFLINE:
		window_copy_start_selection(wp);
		for (; np > 1; np--)
			window_copy_cursor_down(wp, 0);
		window_copy_cursor_end_of_line(wp);
		window_copy_redraw_screen(wp);

		/* If a copy command then copy the selection and exit. */
		if (sess != NULL &&
		    (cmd == MODEKEYCOPY_COPYLINE ||
		    cmd == MODEKEYCOPY_COPYENDOFLINE)) {
			window_copy_copy_selection(wp, NULL);
			window_pane_reset_mode(wp);
			return;
		}
		break;
	case MODEKEYCOPY_CLEARSELECTION:
		window_copy_clear_selection(wp);
		window_copy_redraw_screen(wp);
		break;
	case MODEKEYCOPY_COPYPIPE:
		if (sess != NULL) {
			window_copy_copy_pipe(wp, sess, NULL, arg);
			window_pane_reset_mode(wp);
			return;
		}
		break;
	case MODEKEYCOPY_COPYSELECTION:
		if (sess != NULL) {
			window_copy_copy_selection(wp, NULL);
			window_pane_reset_mode(wp);
			return;
		}
		break;
	case MODEKEYCOPY_STARTOFLINE:
		window_copy_cursor_start_of_line(wp);
		break;
	case MODEKEYCOPY_BACKTOINDENTATION:
		window_copy_cursor_back_to_indentation(wp);
		break;
	case MODEKEYCOPY_ENDOFLINE:
		window_copy_cursor_end_of_line(wp);
		break;
	case MODEKEYCOPY_NEXTLWORD:
		for (; np != 0; np--)
			window_copy_cursor_next_word(wp, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
		break;
	case MODEKEYCOPY_NEXTLWORDEND:
		for (; np != 0; np--)
			window_copy_cursor_next_word_end(wp, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
		break;
	case MODEKEYCOPY_NEXTUWORD:
		for (; np != 0; np--)
			window_copy_cursor_next_word(wp, "");
		break;
	case MODEKEYCOPY_NEXTUWORDEND:
		for (; np != 0; np--)
			window_copy_cursor_next_word_end(wp, "");
		break;
	case MODEKEYCOPY_PREVIOUSLWORD:
		for (; np != 0; np--)
			window_copy_cursor_previous_word(wp, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
		break;
	case MODEKEYCOPY_PREVIOUSUWORD:
		for (; np != 0; np--)
			window_copy_cursor_previous_word(wp, "");
		break;
	case MODEKEYCOPY_JUMP:
		data->inputtype = WINDOW_COPY_JUMPFORWARD;
		data->inputprompt = "Jump Forward";
		*data->inputstr = '\0';
		window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
		return; /* skip numprefix reset */
	case MODEKEYCOPY_JUMPAGAIN:
		if (data->jumptype == WINDOW_COPY_JUMPFORWARD) {
			for (; np != 0; np--)
				window_copy_cursor_jump(wp);
		} else if (data->jumptype == WINDOW_COPY_JUMPBACK) {
			for (; np != 0; np--)
				window_copy_cursor_jump_back(wp);
		} else if (data->jumptype == WINDOW_COPY_JUMPTOFORWARD) {
			for (; np != 0; np--)
				window_copy_cursor_jump_to(wp, 0);
		} else if (data->jumptype == WINDOW_COPY_JUMPTOBACK) {
			for (; np != 0; np--)
				window_copy_cursor_jump_to_back(wp, 0);
		}
		break;
	case MODEKEYCOPY_JUMPREVERSE:
		if (data->jumptype == WINDOW_COPY_JUMPFORWARD) {
			for (; np != 0; np--)
				window_copy_cursor_jump_back(wp);
		} else if (data->jumptype == WINDOW_COPY_JUMPBACK) {
			for (; np != 0; np--)
				window_copy_cursor_jump(wp);
		} else if (data->jumptype == WINDOW_COPY_JUMPTOFORWARD) {
			for (; np != 0; np--)
				window_copy_cursor_jump_to_back(wp, 0);
		} else if (data->jumptype == WINDOW_COPY_JUMPTOBACK) {
			for (; np != 0; np--)
				window_copy_cursor_jump_to(wp, 0);
		}
		break;
	case MODEKEYCOPY_JUMPBACK:
		data->inputtype = WINDOW_COPY_JUMPBACK;
		data->inputprompt = "Jump Back";
		*data->inputstr = '\0';
		window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
		return; /* skip numprefix reset */
	case MODEKEYCOPY_JUMPTO:
		data->inputtype = WINDOW_COPY_JUMPTOFORWARD;
		data->inputprompt = "Jump To";
		*data->inputstr = '\0';
		window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
		return; /* skip numprefix reset */
	case MODEKEYCOPY_JUMPTOBACK:
		data->inputtype = WINDOW_COPY_JUMPTOBACK;
		data->inputprompt = "Jump To Back";
		*data->inputstr = '\0';
		window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
		return; /* skip numprefix reset */
	case MODEKEYCOPY_SEARCHUP:
		data->inputtype = WINDOW_COPY_SEARCHUP;
		data->inputprompt = "Search Up";
		goto input_on;
	case MODEKEYCOPY_SEARCHDOWN:
		data->inputtype = WINDOW_COPY_SEARCHDOWN;
		data->inputprompt = "Search Down";
		goto input_on;
	case MODEKEYCOPY_SEARCHAGAIN:
	case MODEKEYCOPY_SEARCHREVERSE:
		switch (data->searchtype) {
		case WINDOW_COPY_OFF:
		case WINDOW_COPY_GOTOLINE:
		case WINDOW_COPY_JUMPFORWARD:
		case WINDOW_COPY_JUMPBACK:
		case WINDOW_COPY_JUMPTOFORWARD:
		case WINDOW_COPY_JUMPTOBACK:
		case WINDOW_COPY_NAMEDBUFFER:
		case WINDOW_COPY_NUMERICPREFIX:
			break;
		case WINDOW_COPY_SEARCHUP:
			ss = data->searchstr;
			if (cmd == MODEKEYCOPY_SEARCHAGAIN) {
				for (; np != 0; np--)
					window_copy_search_up(wp, ss);
			} else {
				for (; np != 0; np--)
					window_copy_search_down(wp, ss);
			}
			break;
		case WINDOW_COPY_SEARCHDOWN:
			ss = data->searchstr;
			if (cmd == MODEKEYCOPY_SEARCHAGAIN) {
				for (; np != 0; np--)
					window_copy_search_down(wp, ss);
			} else {
				for (; np != 0; np--)
					window_copy_search_up(wp, ss);
			}
			break;
		}
		break;
	case MODEKEYCOPY_GOTOLINE:
		data->inputtype = WINDOW_COPY_GOTOLINE;
		data->inputprompt = "Goto Line";
		*data->inputstr = '\0';
		goto input_on;
	case MODEKEYCOPY_STARTNAMEDBUFFER:
		data->inputtype = WINDOW_COPY_NAMEDBUFFER;
		data->inputprompt = "Buffer";
		*data->inputstr = '\0';
		goto input_on;
	case MODEKEYCOPY_STARTNUMBERPREFIX:
		key &= KEYC_MASK_KEY;
		if (key >= '0' && key <= '9') {
			data->inputtype = WINDOW_COPY_NUMERICPREFIX;
			data->numprefix = 0;
			window_copy_key_numeric_prefix(wp, key);
			return;
		}
		break;
	case MODEKEYCOPY_LEFTPRUNE:
		window_copy_left_prune(wp);
		break;
	case MODEKEYCOPY_RIGHTPRUNE:
		window_copy_right_prune(wp);
		break;
	case MODEKEYCOPY_CHANGEJOINMODE:
		window_copy_change_joinmode(wp);
		break;
	case MODEKEYCOPY_STARTORCOPYSELECTION:
		if (sess != NULL) {
			if(window_copy_copy_selection(wp, NULL))
				window_pane_reset_mode(wp);
			else {
				window_copy_start_selection(wp);
				window_copy_redraw_screen(wp);
			}
			return;
		}
		break;
	default:
		break;
	}

	data->numprefix = -1;
	return;

input_on:
	mode_key_init(&data->mdata, &mode_key_tree_vi_edit);

	window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
	return;

input_off:
	mode_key_init(&data->mdata, &mode_key_tree_vi_copy);

	data->inputtype = WINDOW_COPY_OFF;
	data->inputprompt = NULL;

	window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
}

int
window_copy_key_input(struct window_pane *wp, int key)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	size_t				 inputlen, n;
	int				 np;
	struct paste_buffer		*pb;
	u_char				 ch;

	switch (mode_key_lookup(&data->mdata, key, NULL)) {
	case MODEKEYEDIT_CANCEL:
		data->numprefix = -1;
		return (-1);
	case MODEKEYEDIT_BACKSPACE:
		inputlen = strlen(data->inputstr);
		if (inputlen > 0)
			data->inputstr[inputlen - 1] = '\0';
		break;
	case MODEKEYEDIT_DELETELINE:
		*data->inputstr = '\0';
		break;
	case MODEKEYEDIT_PASTE:
		if ((pb = paste_get_top()) == NULL)
			break;
		for (n = 0; n < pb->size; n++) {
			ch = (u_char) pb->data[n];
			if (ch < 32 || ch == 127)
				break;
		}
		inputlen = strlen(data->inputstr);

		data->inputstr = xrealloc(data->inputstr, inputlen + n + 1);
		memcpy(data->inputstr + inputlen, pb->data, n);
		data->inputstr[inputlen + n] = '\0';
		break;
	case MODEKEYEDIT_ENTER:
		np = data->numprefix;
		if (np <= 0)
			np = 1;

		switch (data->inputtype) {
		case WINDOW_COPY_OFF:
		case WINDOW_COPY_JUMPFORWARD:
		case WINDOW_COPY_JUMPBACK:
		case WINDOW_COPY_JUMPTOFORWARD:
		case WINDOW_COPY_JUMPTOBACK:
		case WINDOW_COPY_NUMERICPREFIX:
			break;
		case WINDOW_COPY_SEARCHUP:
			for (; np != 0; np--)
				window_copy_search_up(wp, data->inputstr);
			data->searchtype = data->inputtype;
			data->searchstr = xstrdup(data->inputstr);
			break;
		case WINDOW_COPY_SEARCHDOWN:
			for (; np != 0; np--)
				window_copy_search_down(wp, data->inputstr);
			data->searchtype = data->inputtype;
			data->searchstr = xstrdup(data->inputstr);
			break;
		case WINDOW_COPY_NAMEDBUFFER:
			window_copy_copy_selection(wp, data->inputstr);
			*data->inputstr = '\0';
			window_pane_reset_mode(wp);
			return (0);
		case WINDOW_COPY_GOTOLINE:
			window_copy_goto_line(wp, data->inputstr);
			*data->inputstr = '\0';
			break;
		}
		data->numprefix = -1;
		return (1);
	case MODEKEY_OTHER:
		if (key < 32 || key > 126)
			break;
		inputlen = strlen(data->inputstr) + 2;

		data->inputstr = xrealloc(data->inputstr, inputlen);
		data->inputstr[inputlen - 2] = key;
		data->inputstr[inputlen - 1] = '\0';
		break;
	default:
		break;
	}

	window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
	return (0);
}

int
window_copy_key_numeric_prefix(struct window_pane *wp, int key)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;

	key &= KEYC_MASK_KEY;
	if (key < '0' || key > '9')
		return (1);

	if (data->numprefix >= 100) 	/* no more than three digits */
		return (0);
	data->numprefix = data->numprefix * 10 + key - '0';

	window_copy_redraw_lines(wp, screen_size_y(s) - 1, 1);
	return (0);
}

void
window_copy_mouse(struct window_pane *wp, struct session *sess,
    struct mouse_event *m)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	u_int				 i, old_cy;

	if (m->x >= screen_size_x(s))
		return;
	if (m->y >= screen_size_y(s))
		return;

	/* If mouse wheel (buttons 4 and 5), scroll. */
	if (m->event == MOUSE_EVENT_WHEEL) {
		for (i = 0; i < m->scroll; i++) {
			if (m->wheel == MOUSE_WHEEL_UP)
				window_copy_cursor_up(wp, 1);
			else {
				window_copy_cursor_down(wp, 1);

				/*
				 * We reached the bottom, leave copy mode, but
				 * only if no selection is in progress.
				 */
				if (data->oy == 0 && !s->sel.flag)
					goto reset_mode;
			}
		}
		return;
	}

	/*
	 * If already reading motion, move the cursor while buttons are still
	 * pressed, or stop the selection on their release.
	 */
	if (s->mode & MODE_MOUSE_BUTTON) {
		if (~m->event & MOUSE_EVENT_UP) {
			old_cy = data->cy;
			window_copy_update_cursor(wp, m->x, m->y);
			if (window_copy_update_selection(wp))
				window_copy_redraw_selection(wp, old_cy);
			return;
		}
		goto reset_mode;
	}

	/* Otherwise if other buttons pressed, start selection and motion. */
	if (~m->event & MOUSE_EVENT_UP) {
		s->mode &= ~MODE_MOUSE_STANDARD;
		s->mode |= MODE_MOUSE_BUTTON;

		window_copy_update_cursor(wp, m->x, m->y);
		window_copy_start_selection(wp);
		window_copy_redraw_screen(wp);
	}

	return;

reset_mode:
	s->mode &= ~MODE_MOUSE_BUTTON;
	s->mode |= MODE_MOUSE_STANDARD;
	if (sess != NULL) {
		window_copy_copy_selection(wp, NULL);
		window_pane_reset_mode(wp);
	}
}

void
window_copy_scroll_to(struct window_pane *wp, u_int px, u_int py)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct grid			*gd = data->backing->grid;
	u_int				 offset, gap;

	data->cx = px;

	gap = gd->sy / 4;
	if (py < gd->sy) {
		offset = 0;
		data->cy = py;
	} else if (py > gd->hsize + gd->sy - gap) {
		offset = gd->hsize;
		data->cy = py - gd->hsize;
	} else {
		offset = py + gap - gd->sy;
		data->cy = py - offset;
	}
	data->oy = gd->hsize - offset;

	window_copy_update_selection(wp);
	window_copy_redraw_screen(wp);
}

int
window_copy_search_compare(struct grid *gd, u_int px, u_int py,
    struct grid *sgd, u_int spx, int cis)
{
	const struct grid_cell	*gc, *sgc;
	struct utf8_data	 ud, sud;

	gc = grid_peek_cell(gd, px, py);
	grid_cell_get(gc, &ud);
	sgc = grid_peek_cell(sgd, spx, 0);
	grid_cell_get(sgc, &sud);

	if (ud.size != sud.size || ud.width != sud.width)
		return (0);

	if (cis && ud.size == 1)
		return (tolower(ud.data[0]) == sud.data[0]);

	return (memcmp(ud.data, sud.data, ud.size) == 0);
}

int
window_copy_search_lr(struct grid *gd,
    struct grid *sgd, u_int *ppx, u_int py, u_int first, u_int last, int cis)
{
	u_int	ax, bx, px;
	int	matched;

	for (ax = first; ax < last; ax++) {
		if (ax + sgd->sx >= gd->sx)
			break;
		for (bx = 0; bx < sgd->sx; bx++) {
			px = ax + bx;
			matched = window_copy_search_compare(gd, px, py, sgd,
			    bx, cis);
			if (!matched)
				break;
		}
		if (bx == sgd->sx) {
			*ppx = ax;
			return (1);
		}
	}
	return (0);
}

int
window_copy_search_rl(struct grid *gd,
    struct grid *sgd, u_int *ppx, u_int py, u_int first, u_int last, int cis)
{
	u_int	ax, bx, px;
	int	matched;

	for (ax = last + 1; ax > first; ax--) {
		if (gd->sx - (ax - 1) < sgd->sx)
			continue;
		for (bx = 0; bx < sgd->sx; bx++) {
			px = ax - 1 + bx;
			matched = window_copy_search_compare(gd, px, py, sgd,
			    bx, cis);
			if (!matched)
				break;
		}
		if (bx == sgd->sx) {
			*ppx = ax - 1;
			return (1);
		}
	}
	return (0);
}

void
window_copy_search_up(struct window_pane *wp, const char *searchstr)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = data->backing, ss;
	struct screen_write_ctx		 ctx;
	struct grid			*gd = s->grid, *sgd;
	struct grid_cell	 	 gc;
	size_t				 searchlen;
	u_int				 i, last, fx, fy, px;
	int				 utf8flag, n, wrapped, wrapflag, cis;
	const char			*ptr;

	if (*searchstr == '\0')
		return;
	utf8flag = options_get_number(&wp->window->options, "utf8");
	wrapflag = options_get_number(&wp->window->options, "wrap-search");
	searchlen = screen_write_strlen(utf8flag, "%s", searchstr);

	screen_init(&ss, searchlen, 1, 0);
	screen_write_start(&ctx, NULL, &ss);
	memcpy(&gc, &grid_default_cell, sizeof gc);
	screen_write_nputs(&ctx, -1, &gc, utf8flag, "%s", searchstr);
	screen_write_stop(&ctx);

	fx = data->cx;
	fy = gd->hsize - data->oy + data->cy;

	if (fx == 0) {
		if (fy == 0)
			return;
		fx = gd->sx - 1;
		fy--;
	} else
		fx--;
	n = wrapped = 0;

	cis = 1;
	for (ptr = searchstr; *ptr != '\0'; ptr++) {
		if (*ptr != tolower((u_char)*ptr)) {
			cis = 0;
			break;
		}
	}

retry:
	sgd = ss.grid;
	for (i = fy + 1; i > 0; i--) {
		last = screen_size_x(s);
		if (i == fy + 1)
			last = fx;
		n = window_copy_search_rl(gd, sgd, &px, i - 1, 0, last, cis);
		if (n) {
			window_copy_scroll_to(wp, px, i - 1);
			break;
		}
	}
	if (wrapflag && !n && !wrapped) {
		fx = gd->sx - 1;
		fy = gd->hsize + gd->sy - 1;
		wrapped = 1;
		goto retry;
	}

	screen_free(&ss);
}

void
window_copy_search_down(struct window_pane *wp, const char *searchstr)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = data->backing, ss;
	struct screen_write_ctx		 ctx;
	struct grid			*gd = s->grid, *sgd;
	struct grid_cell	 	 gc;
	size_t				 searchlen;
	u_int				 i, first, fx, fy, px;
	int				 utf8flag, n, wrapped, wrapflag, cis;
	const char			*ptr;

	if (*searchstr == '\0')
		return;
	utf8flag = options_get_number(&wp->window->options, "utf8");
	wrapflag = options_get_number(&wp->window->options, "wrap-search");
	searchlen = screen_write_strlen(utf8flag, "%s", searchstr);

	screen_init(&ss, searchlen, 1, 0);
	screen_write_start(&ctx, NULL, &ss);
	memcpy(&gc, &grid_default_cell, sizeof gc);
	screen_write_nputs(&ctx, -1, &gc, utf8flag, "%s", searchstr);
	screen_write_stop(&ctx);

	fx = data->cx;
	fy = gd->hsize - data->oy + data->cy;

	if (fx == gd->sx - 1) {
		if (fy == gd->hsize + gd->sy)
			return;
		fx = 0;
		fy++;
	} else
		fx++;
	n = wrapped = 0;

	cis = 1;
	for (ptr = searchstr; *ptr != '\0'; ptr++) {
		if (*ptr != tolower((u_char)*ptr)) {
			cis = 0;
			break;
		}
	}

retry:
	sgd = ss.grid;
	for (i = fy + 1; i < gd->hsize + gd->sy + 1; i++) {
		first = 0;
		if (i == fy + 1)
			first = fx;
		n = window_copy_search_lr(gd, sgd, &px, i - 1, first, gd->sx,
		    cis);
		if (n) {
			window_copy_scroll_to(wp, px, i - 1);
			break;
		}
	}
	if (wrapflag && !n && !wrapped) {
		fx = 0;
		fy = 0;
		wrapped = 1;
		goto retry;
	}

	screen_free(&ss);
}

void
window_copy_goto_line(struct window_pane *wp, const char *linestr)
{
	struct window_copy_mode_data	*data = wp->modedata;
	const char			*errstr;
	u_int				 lineno;

	lineno = strtonum(linestr, 0, screen_hsize(data->backing), &errstr);
	if (errstr != NULL)
		return;

	data->oy = lineno;
	window_copy_update_selection(wp);
	window_copy_redraw_screen(wp);
}

void
window_copy_write_line(struct window_pane *wp, struct screen_write_ctx *ctx,
    u_int py)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	struct options			*oo = &wp->window->options;
	struct grid_cell		 gc;
	char				 hdr[512];
	size_t				 last, xoff = 0, size = 0, limit;

	style_apply(&gc, oo, "mode-style");

	last = screen_size_y(s) - 1;
	if (py == 0) {
		size = xsnprintf(hdr, sizeof hdr,
		    "[%u/%u]%s", data->oy, screen_hsize(data->backing),
		    join_modes[data->joinmode].header);
		if (size > screen_size_x(s))
			size = screen_size_x(s);
		screen_write_cursormove(ctx, screen_size_x(s) - size, 0);
		screen_write_puts(ctx, &gc, "%s", hdr);
	} else if (py == last && data->inputtype != WINDOW_COPY_OFF) {
		limit = sizeof hdr;
		if (limit > screen_size_x(s) + 1)
			limit = screen_size_x(s) + 1;
		if (data->inputtype == WINDOW_COPY_NUMERICPREFIX) {
			xoff = size = xsnprintf(hdr, limit,
			    "Repeat: %u", data->numprefix);
		} else {
			xoff = size = xsnprintf(hdr, limit,
			    "%s: %s", data->inputprompt, data->inputstr);
		}
		screen_write_cursormove(ctx, 0, last);
		screen_write_puts(ctx, &gc, "%s", hdr);
	} else
		size = 0;

	if (size < screen_size_x(s)) {
		screen_write_cursormove(ctx, xoff, py);
		screen_write_copy(ctx, data->backing, xoff,
		    (screen_hsize(data->backing) - data->oy) + py,
		    screen_size_x(s) - size, 1);
	}
}

void
window_copy_write_lines(struct window_pane *wp, struct screen_write_ctx *ctx,
    u_int py, u_int ny)
{
	u_int	yy;

	for (yy = py; yy < py + ny; yy++)
		window_copy_write_line(wp, ctx, py);
}

void
window_copy_redraw_selection(struct window_pane *wp, u_int old_y)
{
	struct window_copy_mode_data	*data = wp->modedata;
	u_int				 new_y, start, end;

	new_y = data->cy;
	if (old_y <= new_y) {
		start = old_y;
		end = new_y;
	} else {
		start = new_y;
		end = old_y;
	}
	window_copy_redraw_lines(wp, start, end - start + 1);
}

void
window_copy_redraw_lines(struct window_pane *wp, u_int py, u_int ny)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen_write_ctx	 	 ctx;
	u_int				 i;

	screen_write_start(&ctx, wp, NULL);
	for (i = py; i < py + ny; i++)
		window_copy_write_line(wp, &ctx, i);
	screen_write_cursormove(&ctx, data->cx, data->cy);
	screen_write_stop(&ctx);
}

void
window_copy_redraw_screen(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;

	window_copy_redraw_lines(wp, 0, screen_size_y(&data->screen));
}

void
window_copy_update_cursor(struct window_pane *wp, u_int cx, u_int cy)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	struct screen_write_ctx		 ctx;
	u_int				 old_cx, old_cy;

	old_cx = data->cx; old_cy = data->cy;
	data->cx = cx; data->cy = cy;
	if (old_cx == screen_size_x(s))
		window_copy_redraw_lines(wp, old_cy, 1);
	if (data->cx == screen_size_x(s))
		window_copy_redraw_lines(wp, data->cy, 1);
	else {
		screen_write_start(&ctx, wp, NULL);
		screen_write_cursormove(&ctx, data->cx, data->cy);
		screen_write_stop(&ctx);
	}
}

void
window_copy_start_selection(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;

	data->selx = data->cx;
	data->sely = screen_hsize(data->backing) + data->cy - data->oy;

	s->sel.flag = 1;
	window_copy_update_selection(wp);
}

int
window_copy_update_selection(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	struct options			*oo = &wp->window->options;
	struct grid_cell		 gc;
	u_int				 sx, sy, ty, leftprunex, rightprunex;

	if (!s->sel.flag)
		return (0);

	/* Set colours. */
	style_apply(&gc, oo, "mode-style");

	/* Find top of screen. */
	ty = screen_hsize(data->backing) - data->oy;

	/* Adjust the selection. */
	sx = data->selx;
	sy = data->sely;
	if (sy < ty) {					/* above screen */
		sx = 0;
		sy = 0;
	} else if (sy > ty + screen_size_y(s) - 1) {	/* below screen */
		sx = screen_size_x(s) - 1;
		sy = screen_size_y(s) - 1;
	} else
		sy -= ty;
	sy = screen_hsize(s) + sy;
	leftprunex = data->leftprunex_set ? data->leftprunex : 0;
	rightprunex = data->rightprunex_set ? data->rightprunex : (screen_size_x(s) - 1);

	screen_set_selection(s,
	    sx, sy, data->cx, screen_hsize(s) + data->cy,
	    leftprunex, rightprunex, &gc);

	return (1);
}

void *
window_copy_get_selection(struct window_pane *wp, size_t *len)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	char				*buf;
	size_t				 off;
	u_int				 i, sx, sy, ex, ey, ey_last, t, trailing_delimiter;
	u_int				 firstsx, lastex, restex, restsx;

	if (!s->sel.flag)
		return (NULL);

	buf = xmalloc(1);
	off = 0;

	*buf = '\0';

	/*
	 * The selection extends from selx,sely to (adjusted) cx,cy on
	 * the base screen.
	 */

	/* Find start and end. */
	sx = data->selx;
	sy = data->sely;
	ex = data->cx;
	ey = screen_hsize(data->backing) + data->cy - data->oy;

	/* And flip them where appropriate. */
	if ((sy > ey) || (sy == ey && sx > ex)) {
		t = sx;
		sx = ex;
		ex = t;

		t = sy;
		sy = ey;
		ey = t;
	}

	/* Trim ex to end of line. */
	ey_last = window_copy_find_length(wp, ey);
	if (ex > ey_last)
		ex = ey_last;

	/*
	 * Deal with rectangle-copy if necessary; four situations: start of
	 * first line (firstsx), end of last line (lastex), start (restsx) and
	 * end (restex) of all other lines.
	 */
	lastex = ex + 1;
	restex = screen_size_x(s);
	firstsx = sx;
	restsx = 0;

	if (data->leftprunex_set) {
		if (firstsx < data->leftprunex) {
			firstsx = data->leftprunex;
		}
		if (restsx < data->leftprunex) {
			restsx = data->leftprunex;
		}
	}
	if (data->rightprunex_set) {
		if (restex > data->rightprunex + 1) {
			restex = data->rightprunex + 1;
		}
		if (lastex > data->rightprunex + 1) {
			lastex = data->rightprunex + 1;
		}
	}

	/* Copy the lines. */
	trailing_delimiter = 0;
	for (i = sy; i <= ey; ++i) {
		trailing_delimiter = window_copy_copy_line(wp, &buf, &off, i,
		    (i == sy ? firstsx : restsx),
		    (i == ey ? lastex : restex));
	}
	if (trailing_delimiter) {
		off -= strlen(join_modes[data->joinmode].delimiter); /* remove final delimiter */
	}

	*len = off;
	return (buf);
}

void
window_copy_copy_buffer(struct window_pane *wp, const char *bufname, void *buf,
    size_t len)
{
	struct screen_write_ctx	ctx;

	if (options_get_number(&global_options, "set-clipboard")) {
		screen_write_start(&ctx, wp, NULL);
		screen_write_setselection(&ctx, buf, len);
		screen_write_stop(&ctx);
	}

	if (paste_set(buf, len, bufname, NULL) != 0)
		free(buf);
}

void
window_copy_copy_pipe(struct window_pane *wp, struct session *sess,
    const char *bufname, const char *arg)
{
	void			*buf;
	size_t			 len;
	struct job		*job;
	struct format_tree	*ft;
	char			*expanded;

	buf = window_copy_get_selection(wp, &len);
	if (buf == NULL)
		return;

	ft = format_create();
	format_defaults(ft, NULL, sess, NULL, wp);
	expanded = format_expand(ft, arg);

	job = job_run(expanded, sess, NULL, NULL, NULL);
	bufferevent_write(job->event, buf, len);

	free(expanded);
	format_free(ft);

	window_copy_copy_buffer(wp, bufname, buf, len);
}

int
window_copy_copy_selection(struct window_pane *wp, const char *bufname)
{
	void	*buf;
	size_t	 len;

	buf = window_copy_get_selection(wp, &len);
	if (buf == NULL)
		return 0;

	window_copy_copy_buffer(wp, bufname, buf, len);

	return 1;
}

void
window_copy_append_selection(struct window_pane *wp, const char *bufname)
{
	char				*buf;
	struct paste_buffer		*pb;
	size_t				 len;
	struct screen_write_ctx		 ctx;

	buf = window_copy_get_selection(wp, &len);
	if (buf == NULL)
		return;

	if (options_get_number(&global_options, "set-clipboard")) {
		screen_write_start(&ctx, wp, NULL);
		screen_write_setselection(&ctx, buf, len);
		screen_write_stop(&ctx);
	}

	if (bufname == NULL || *bufname == '\0') {
		pb = paste_get_top();
		if (pb != NULL)
			bufname = pb->name;
	} else
		pb = paste_get_name(bufname);
	if (pb != NULL) {
		buf = xrealloc(buf, len + pb->size);
		memmove(buf + pb->size, buf, len);
		memcpy(buf, pb->data, pb->size);
		len += pb->size;
	}
	if (paste_set(buf, len, bufname, NULL) != 0)
		free(buf);
}

int
window_copy_copy_line(struct window_pane *wp,
    char **buf, size_t *off, u_int sy, u_int sx, u_int ex)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct grid			*gd = data->backing->grid;
	const struct grid_cell		*gc;
	struct grid_line		*gl;
	struct utf8_data		 ud;
	u_int				 i, xx, wrapped = 0;
	const char			*s;

	if (sx > ex)
		return 0;

	/*
	 * Work out if the line was wrapped at the screen edge and all of it is
	 * on screen.
	 */
	gl = &gd->linedata[sy];
	if (gl->flags & GRID_LINE_WRAPPED && gl->cellsize <= gd->sx)
		wrapped = 1;

	/* If the line was wrapped, don't strip spaces (use the full length). */
	if (wrapped)
		xx = gl->cellsize;
	else
		xx = window_copy_find_length(wp, sy);
	if (ex > xx)
		ex = xx;
	if (sx > xx)
		sx = xx;

	if (sx < ex) {
		for (i = sx; i < ex; i++) {
			gc = grid_peek_cell(gd, i, sy);
			if (gc->flags & GRID_FLAG_PADDING)
				continue;
			grid_cell_get(gc, &ud);
			if (ud.size == 1 && (gc->attr & GRID_ATTR_CHARSET)) {
				s = tty_acs_get(NULL, ud.data[0]);
				if (s != NULL && strlen(s) <= sizeof ud.data) {
					ud.size = strlen(s);
					memcpy(ud.data, s, ud.size);
				}
			}

			*buf = xrealloc(*buf, (*off) + ud.size);
			memcpy(*buf + *off, ud.data, ud.size);
			*off += ud.size;
		}
	}

	/* Only add a delimiter if the line wasn't wrapped. */
	if (!wrapped || ex != xx) {
		*buf = xrealloc(*buf, (*off) + strlen(join_modes[data->joinmode].delimiter));
		memcpy(*buf + *off, join_modes[data->joinmode].delimiter, strlen(join_modes[data->joinmode].delimiter));
		*off += strlen(join_modes[data->joinmode].delimiter);
		return 1;
	}
	return 0;
}

void
window_copy_clear_selection(struct window_pane *wp)
{
	struct window_copy_mode_data   *data = wp->modedata;
	u_int				px, py;

	screen_clear_selection(&data->screen);

	py = screen_hsize(data->backing) + data->cy - data->oy;
	px = window_copy_find_length(wp, py);
	if (data->cx > px)
		window_copy_update_cursor(wp, px, data->cy);
}

int
window_copy_in_set(struct window_pane *wp, u_int px, u_int py, const char *set)
{
	struct window_copy_mode_data	*data = wp->modedata;
	const struct grid_cell		*gc;
	struct utf8_data		 ud;

	gc = grid_peek_cell(data->backing->grid, px, py);
	grid_cell_get(gc, &ud);
	if (ud.size != 1 || gc->flags & GRID_FLAG_PADDING)
		return (0);
	if (*ud.data == 0x00 || *ud.data == 0x7f)
		return (0);
	return (strchr(set, *ud.data) != NULL);
}

u_int
window_copy_find_length(struct window_pane *wp, u_int py)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = data->backing;
	const struct grid_cell		*gc;
	struct utf8_data		 ud;
	u_int				 px;

	/*
	 * If the pane has been resized, its grid can contain old overlong
	 * lines. grid_peek_cell does not allow accessing cells beyond the
	 * width of the grid, and screen_write_copy treats them as spaces, so
	 * ignore them here too.
	 */
	px = s->grid->linedata[py].cellsize;
	if (px > screen_size_x(s))
		px = screen_size_x(s);
	while (px > 0) {
		gc = grid_peek_cell(s->grid, px - 1, py);
		grid_cell_get(gc, &ud);
		if (ud.size != 1 || *ud.data != ' ')
			break;
		px--;
	}
	return (px);
}

void
window_copy_cursor_start_of_line(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	struct grid			*gd = back_s->grid;
	u_int				 py;

	if (data->cx == 0) {
		py = screen_hsize(back_s) + data->cy - data->oy;
		while (py > 0 &&
		    gd->linedata[py-1].flags & GRID_LINE_WRAPPED) {
			window_copy_cursor_up(wp, 0);
			py = screen_hsize(back_s) + data->cy - data->oy;
		}
	}
	window_copy_update_cursor(wp, 0, data->cy);
	if (window_copy_update_selection(wp))
		window_copy_redraw_lines(wp, data->cy, 1);
}

void
window_copy_cursor_back_to_indentation(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	u_int				 px, py, xx;
	const struct grid_cell		*gc;
	struct utf8_data		 ud;

	px = 0;
	py = screen_hsize(data->backing) + data->cy - data->oy;
	xx = window_copy_find_length(wp, py);

	while (px < xx) {
		gc = grid_peek_cell(data->backing->grid, px, py);
		grid_cell_get(gc, &ud);
		if (ud.size != 1 || *ud.data != ' ')
			break;
		px++;
	}

	window_copy_update_cursor(wp, px, data->cy);
	if (window_copy_update_selection(wp))
		window_copy_redraw_lines(wp, data->cy, 1);
}

void
window_copy_cursor_end_of_line(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	u_int				 px, py;

	py = screen_hsize(back_s) + data->cy - data->oy;
	px = window_copy_find_length(wp, py);

	/* Actually, vi '$' doesn't really go "off" the end of the line... */
	if (px > 0)
		--px;

	window_copy_update_cursor(wp, px, data->cy);

	if (window_copy_update_selection(wp))
		window_copy_redraw_lines(wp, data->cy, 1);
}

void
window_copy_other_end(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	u_int				 selx, sely, cx, cy, yy, hsize;

	if (!s->sel.flag)
		return;

	selx = data->selx;
	sely = data->sely;
	cx = data->cx;
	cy = data->cy;
	yy = screen_hsize(data->backing) + data->cy - data->oy;

	data->selx = cx;
	data->sely = yy;
	data->cx = selx;

	hsize = screen_hsize(data->backing);
	if (sely < hsize - data->oy) {
		data->oy = hsize - sely;
		data->cy = 0;
	} else if (sely > hsize - data->oy + screen_size_y(s)) {
		data->oy = hsize - sely + screen_size_y(s) - 1;
		data->cy = screen_size_y(s) - 1;
	} else
		data->cy = cy + sely - yy;

	window_copy_redraw_screen(wp);
}

void
window_copy_cursor_left(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;

	if (data->cx > 0) {
		window_copy_update_cursor(wp, data->cx - 1, data->cy);
		if (window_copy_update_selection(wp))
			window_copy_redraw_lines(wp, data->cy, 1);
	}
}

void
window_copy_cursor_right(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	u_int				 px;

	px = screen_size_x(s) - 1;

	if (data->cx < px) {
		window_copy_update_cursor(wp, data->cx + 1, data->cy);
		if (window_copy_update_selection(wp))
			window_copy_redraw_lines(wp, data->cy, 1);
	}
}

void
window_copy_cursor_up(struct window_pane *wp, int scroll_only)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;

	if (scroll_only || data->cy == 0) {
		window_copy_scroll_down(wp, 1);
		if (scroll_only) {
			if (data->cy == screen_size_y(s) - 1)
				window_copy_redraw_lines(wp, data->cy, 1);
			else
				window_copy_redraw_lines(wp, data->cy, 2);
		}
	} else {
		window_copy_update_cursor(wp, data->cx, data->cy - 1);
		if (window_copy_update_selection(wp)) {
			if (data->cy == screen_size_y(s) - 1)
				window_copy_redraw_lines(wp, data->cy, 1);
			else
				window_copy_redraw_lines(wp, data->cy, 2);
		}
	}
}

void
window_copy_cursor_down(struct window_pane *wp, int scroll_only)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;

	if (scroll_only || data->cy == screen_size_y(s) - 1) {
		window_copy_scroll_up(wp, 1);
		if (scroll_only && data->cy > 0)
			window_copy_redraw_lines(wp, data->cy - 1, 2);
	} else {
		window_copy_update_cursor(wp, data->cx, data->cy + 1);
		if (window_copy_update_selection(wp))
			window_copy_redraw_lines(wp, data->cy - 1, 2);
	}
}

void
window_copy_cursor_jump(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	const struct grid_cell		*gc;
	struct utf8_data		 ud;
	u_int				 px, py, xx;

	px = data->cx + 1;
	py = screen_hsize(back_s) + data->cy - data->oy;
	xx = window_copy_find_length(wp, py);

	while (px < xx) {
		gc = grid_peek_cell(back_s->grid, px, py);
		grid_cell_get(gc, &ud);
		if (!(gc->flags & GRID_FLAG_PADDING) &&
		    ud.size == 1 && *ud.data == data->jumpchar) {
			window_copy_update_cursor(wp, px, data->cy);
			if (window_copy_update_selection(wp))
				window_copy_redraw_lines(wp, data->cy, 1);
			return;
		}
		px++;
	}
}

void
window_copy_cursor_jump_back(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	const struct grid_cell		*gc;
	struct utf8_data		 ud;
	u_int				 px, py;

	px = data->cx;
	py = screen_hsize(back_s) + data->cy - data->oy;

	if (px > 0)
		px--;

	for (;;) {
		gc = grid_peek_cell(back_s->grid, px, py);
		grid_cell_get(gc, &ud);
		if (!(gc->flags & GRID_FLAG_PADDING) &&
		    ud.size == 1 && *ud.data == data->jumpchar) {
			window_copy_update_cursor(wp, px, data->cy);
			if (window_copy_update_selection(wp))
				window_copy_redraw_lines(wp, data->cy, 1);
			return;
		}
		if (px == 0)
			break;
		px--;
	}
}

void
window_copy_cursor_jump_to(struct window_pane *wp, int allow_still)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	const struct grid_cell		*gc;
	struct utf8_data		 ud;
	u_int				 px, py, xx;

	px = data->cx + 1 + (allow_still ? 0 : 1);
	py = screen_hsize(back_s) + data->cy - data->oy;
	xx = window_copy_find_length(wp, py);

	while (px < xx) {
		gc = grid_peek_cell(back_s->grid, px, py);
		grid_cell_get(gc, &ud);
		if (!(gc->flags & GRID_FLAG_PADDING) &&
		    ud.size == 1 && *ud.data == data->jumpchar) {
			window_copy_update_cursor(wp, px - 1, data->cy);
			if (window_copy_update_selection(wp))
				window_copy_redraw_lines(wp, data->cy, 1);
			return;
		}
		px++;
	}
}

void
window_copy_cursor_jump_to_back(struct window_pane *wp, int allow_still)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	const struct grid_cell		*gc;
	struct utf8_data		 ud;
	u_int				 px, py;

	px = data->cx;
	py = screen_hsize(back_s) + data->cy - data->oy;

	if (px > 0)
		px--;
	if (px > 0 && !allow_still)
		px--;

	for (;;) {
		gc = grid_peek_cell(back_s->grid, px, py);
		grid_cell_get(gc, &ud);
		if (!(gc->flags & GRID_FLAG_PADDING) &&
		    ud.size == 1 && *ud.data == data->jumpchar) {
			window_copy_update_cursor(wp, px + 1, data->cy);
			if (window_copy_update_selection(wp))
				window_copy_redraw_lines(wp, data->cy, 1);
			return;
		}
		if (px == 0)
			break;
		px--;
	}
}

void
window_copy_cursor_next_word(struct window_pane *wp, const char *class1)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	u_int				 px, py, xx, yy;
	int				 state = -1;

	px = data->cx;
	py = screen_hsize(back_s) + data->cy - data->oy;
	xx = window_copy_find_length(wp, py);
	yy = screen_hsize(back_s) + screen_size_y(back_s) - 1;

	while (1) {
		if (px > xx) {
			if (py == yy) {
				return;
			}
			window_copy_cursor_down(wp, 0);
			px = 0;
			py = screen_hsize(back_s) + data->cy - data->oy;
			xx = window_copy_find_length(wp, py);
			continue;
		}
		if (px == xx || window_copy_in_set(wp, px, py, " ")) {
			state = 0;
		} else if (window_copy_in_set(wp, px, py, class1)) {
			if (state == 0 || state == 2) {
				break;
			}
			state = 1;
		} else {
			if (state == 0 || state == 1) {
				break;
			}
			state = 2;
		}
		++px;
	}

	window_copy_update_cursor(wp, px, data->cy);
	if (window_copy_update_selection(wp))
		window_copy_redraw_lines(wp, data->cy, 1);
}

void
window_copy_cursor_next_word_end(struct window_pane *wp,
    const char *class1)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*back_s = data->backing;
	u_int				 px, py, xx, yy;
	int				 state = -1;

	px = data->cx;
	py = screen_hsize(back_s) + data->cy - data->oy;
	xx = window_copy_find_length(wp, py);
	yy = screen_hsize(back_s) + screen_size_y(back_s) - 1;

	while (1) {
		if (px == xx - 1 || window_copy_in_set(wp, px + 1, py, " ")) {
			if (state > 0) {
				break;
			}
			state = 0;
		} else if (window_copy_in_set(wp, px + 1, py, class1)) {
			if (state == 2) {
				break;
			}
			state = 1;
		} else {
			if (state == 1) {
				break;
			}
			state = 2;
		}

		if (px == xx - 1) {
			if (py == yy) {
				return;
			}
			window_copy_cursor_down(wp, 0);
			px = 0;
			py = screen_hsize(back_s) + data->cy - data->oy;
			xx = window_copy_find_length(wp, py);
		} else {
			++px;
		}
	}

	window_copy_update_cursor(wp, px, data->cy);
	if (window_copy_update_selection(wp))
		window_copy_redraw_lines(wp, data->cy, 1);
}

/* Move to the previous place where a word begins. */
void
window_copy_cursor_previous_word(struct window_pane *wp,
    const char *class1)
{
	struct window_copy_mode_data	*data = wp->modedata;
	u_int				 px, py;
	int				 state = -1;

	px = data->cx;
	py = screen_hsize(data->backing) + data->cy - data->oy;

	while (1) {
		if (px == 0 || window_copy_in_set(wp, px - 1, py, " ")) {
			if (state > 0) {
				break;
			}
			state = 0;
		} else if (window_copy_in_set(wp, px - 1, py, class1)) {
			if (state == 2) {
				break;
			}
			state = 1;
		} else {
			if (state == 1) {
				break;
			}
			state = 2;
		}

		if (px == 0) {
			if (data->cy == 0 &&
			    (screen_hsize(data->backing) == 0 ||
			    data->oy >= screen_hsize(data->backing) - 1))
				return;
			window_copy_cursor_up(wp, 0);
			py = screen_hsize(data->backing) + data->cy - data->oy;
			px = window_copy_find_length(wp, py);
		}
		else {
			--px;
		}
	}

	window_copy_update_cursor(wp, px, data->cy);
	if (window_copy_update_selection(wp))
		window_copy_redraw_lines(wp, data->cy, 1);
}

void
window_copy_scroll_up(struct window_pane *wp, u_int ny)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	struct screen_write_ctx		 ctx;

	if (data->oy < ny)
		ny = data->oy;
	if (ny == 0)
		return;
	data->oy -= ny;

	window_copy_update_selection(wp);

	screen_write_start(&ctx, wp, NULL);
	screen_write_cursormove(&ctx, 0, 0);
	screen_write_deleteline(&ctx, ny);
	window_copy_write_lines(wp, &ctx, screen_size_y(s) - ny, ny);
	window_copy_write_line(wp, &ctx, 0);
	if (screen_size_y(s) > 1)
		window_copy_write_line(wp, &ctx, 1);
	if (screen_size_y(s) > 3)
		window_copy_write_line(wp, &ctx, screen_size_y(s) - 2);
	if (s->sel.flag && screen_size_y(s) > ny)
		window_copy_write_line(wp, &ctx, screen_size_y(s) - ny - 1);
	screen_write_cursormove(&ctx, data->cx, data->cy);
	screen_write_stop(&ctx);
}

void
window_copy_scroll_down(struct window_pane *wp, u_int ny)
{
	struct window_copy_mode_data	*data = wp->modedata;
	struct screen			*s = &data->screen;
	struct screen_write_ctx		 ctx;

	if (ny > screen_hsize(data->backing))
		return;

	if (data->oy > screen_hsize(data->backing) - ny)
		ny = screen_hsize(data->backing) - data->oy;
	if (ny == 0)
		return;
	data->oy += ny;

	window_copy_update_selection(wp);

	screen_write_start(&ctx, wp, NULL);
	screen_write_cursormove(&ctx, 0, 0);
	screen_write_insertline(&ctx, ny);
	window_copy_write_lines(wp, &ctx, 0, ny);
	if (s->sel.flag && screen_size_y(s) > ny)
		window_copy_write_line(wp, &ctx, ny);
	else if (ny == 1) /* nuke position */
		window_copy_write_line(wp, &ctx, 1);
	screen_write_cursormove(&ctx, data->cx, data->cy);
	screen_write_stop(&ctx);
}

void
window_copy_left_prune(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	if (!data->rightprunex_set || data->cx <= data->rightprunex) {
		data->leftprunex = data->cx;
		data->leftprunex_set = 1;
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
	}
}

void
window_copy_right_prune(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	if (!data->leftprunex_set || data->cx >= data->leftprunex) {
		data->rightprunex = data->cx;
		data->rightprunex_set = 1;
		window_copy_update_selection(wp);
		window_copy_redraw_screen(wp);
	}
}

void
window_copy_change_joinmode(struct window_pane *wp)
{
	struct window_copy_mode_data	*data = wp->modedata;
	data->joinmode = (data->joinmode + 1) % WINDOW_COPY_JOIN_MAX;
	window_copy_redraw_screen(wp);
}
