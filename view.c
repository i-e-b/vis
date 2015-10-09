/*
 * Copyright (c) 2014 Marc André Tanner <mat at brain-dump.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include "editor.h"
#include "view.h"
#include "syntax.h"
#include "text.h"
#include "text-motions.h"
#include "text-util.h"
#include "util.h"

struct Selection {
	Mark anchor;             /* position where the selection was created */
	Mark cursor;             /* other selection endpoint where it changes */
	View *view;              /* associated view to which this selection belongs */
	Selection *prev, *next;  /* previsous/next selections in no particular order */
};

struct Cursor {             /* cursor position */
	Filepos pos;        /* in bytes from the start of the file */
	int row, col;       /* in terms of zero based screen coordinates */
	int lastcol;        /* remembered column used when moving across lines */
	Line *line;         /* screen line on which cursor currently resides */
	Mark mark;          /* mark used to keep track of current cursor position */
	Selection *sel;     /* selection (if any) which folows the cursor upon movement */
	Mark lastsel_anchor;/* previously used selection data, */
	Mark lastsel_cursor;/* used to restore it */
	Register reg;       /* per cursor register to support yank/put operation */
	View *view;         /* associated view to which this cursor belongs */
	Cursor *prev, *next;/* previous/next cursors in no particular order */
};

/* Viewable area, showing part of a file. Keeps track of cursors and selections.
 * At all times there exists at least one cursor, which is placed in the visible viewport.
 * Additional cursors can be created and positioned anywhere in the file. */
struct View {
	Text *text;         /* underlying text management */
	UiWin *ui;
	ViewEvent *events;
	int width, height;  /* size of display area */
	Filepos start, end; /* currently displayed area [start, end] in bytes from the start of the file */
	Filepos start_last; /* previously used start of visible area, used to update the mark */
	Mark start_mark;    /* mark to keep track of the start of the visible area */
	size_t lines_size;  /* number of allocated bytes for lines (grows only) */
	Line *lines;        /* view->height number of lines representing view content */
	Line *topline;      /* top of the view, first line currently shown */
	Line *lastline;     /* last currently used line, always <= bottomline */
	Line *bottomline;   /* bottom of view, might be unused if lastline < bottomline */
	Cursor *cursor;     /* main cursor, always placed within the visible viewport */
	Line *line;         /* used while drawing view content, line where next char will be drawn */
	int col;            /* used while drawing view content, column where next char will be drawn */
	Syntax *syntax;     /* syntax highlighting definitions for this view or NULL */
	SyntaxSymbol *symbols[SYNTAX_SYMBOL_LAST]; /* symbols to use for white spaces etc */
	int tabwidth;       /* how many spaces should be used to display a tab character */
	Cursor *cursors;    /* all cursors currently active */
	Selection *selections; /* all selected regions */
};

static SyntaxSymbol symbols_none[] = {
	{ " " }, /* spaces */
	{ " " }, /* tab first cell */
	{ " " }, /* tab remaining cells */
	{ " " }, /* eol */
	{ "~" }, /* eof */
};

static SyntaxSymbol symbols_default[] = {
	{ "\xC2\xB7" },     /* spaces */
	{ "\xE2\x96\xB6" }, /* tab first cell */
	{ " " },            /* tab remaining cells */
	{ "\xE2\x8F\x8E" }, /* eol */
	{ "~" },            /* eof */
};

static Cell cell_unused;
static Cell cell_blank = { .data = " " };

static void view_clear(View *view);
static bool view_addch(View *view, Cell *cell);
static bool view_coord_get(View *view, size_t pos, Line **retline, int *retrow, int *retcol); 
static void view_cursors_free(Cursor *c);
/* set/move current cursor position to a given (line, column) pair */
static size_t cursor_set(Cursor *cursor, Line *line, int col);

void view_tabwidth_set(View *view, int tabwidth) {
	view->tabwidth = tabwidth;
	view_draw(view);
}

/* reset internal view data structures (cell matrix, line offsets etc.) */
static void view_clear(View *view) {
	if (view->start != view->start_last) {
		view->start_mark = text_mark_set(view->text, view->start);
		view->start_last = view->start;
	} else {
		size_t start = text_mark_get(view->text, view->start_mark);
		if (start != EPOS)
			view->start = start;
	}
	view->topline = view->lines;
	view->topline->lineno = text_lineno_by_pos(view->text, view->start);
	view->lastline = view->topline;

	/* reset all other lines */
	size_t line_size = sizeof(Line) + view->width*sizeof(Cell);
	size_t end = view->height * line_size;
	Line *prev = NULL;
	for (size_t i = 0; i < end; i += line_size) {
		Line *line = (Line*)(((char*)view->lines) + i);
		line->width = 0;
		line->len = 0;
		line->prev = prev;
		if (prev)
			prev->next = line;
		prev = line;
	}
	view->bottomline = prev ? prev : view->topline;
	view->bottomline->next = NULL;
	view->line = view->topline;
	view->col = 0;
}

Filerange view_viewport_get(View *view) {
	return (Filerange){ .start = view->start, .end = view->end };
}

/* try to add another character to the view, return whether there was space left */
static bool view_addch(View *view, Cell *cell) {
	if (!view->line)
		return false;

	int width;
	size_t lineno = view->line->lineno;

	switch (cell->data[0]) {
	case '\t':
		cell->istab = true;
		cell->width = 1;
		width = view->tabwidth - (view->col % view->tabwidth);
		for (int w = 0; w < width; w++) {
			if (view->col + 1 > view->width) {
				view->line = view->line->next;
				view->col = 0;
				if (!view->line)
					return false;
				view->line->lineno = lineno;
			}

			cell->len = w == 0 ? 1 : 0;
			int t = w == 0 ? SYNTAX_SYMBOL_TAB : SYNTAX_SYMBOL_TAB_FILL;
			strncpy(cell->data, view->symbols[t]->symbol, sizeof(cell->data)-1);
			cell->attr = view->symbols[t]->style;
			view->line->cells[view->col] = *cell;
			view->line->len += cell->len;
			view->line->width += cell->width;
			view->col++;
		}
		cell->len = 1;
		return true;
	case '\n':
		cell->width = 1;
		if (view->col + cell->width > view->width) {
			view->line = view->line->next;
			view->col = 0;
			if (!view->line)
				return false;
			view->line->lineno = lineno;
		}

		strncpy(cell->data, view->symbols[SYNTAX_SYMBOL_EOL]->symbol, sizeof(cell->data)-1);
		cell->attr = view->symbols[SYNTAX_SYMBOL_EOL]->style;

		view->line->cells[view->col] = *cell;
		view->line->len += cell->len;
		view->line->width += cell->width;
		for (int i = view->col + 1; i < view->width; i++)
			view->line->cells[i] = cell_blank;

		view->line = view->line->next;
		if (view->line)
			view->line->lineno = lineno + 1;
		view->col = 0;
		return true;
	default:
		if ((unsigned char)cell->data[0] < 128 && !isprint((unsigned char)cell->data[0])) {
			/* non-printable ascii char, represent it as ^(char + 64) */
			*cell = (Cell) {
				.data = { '^', cell->data[0] + 64, '\0' },
				.len = 1,
				.width = 2,
				.istab = false,
				.attr = cell->attr,
			};
		}

		if (cell->data[0] == ' ') {
			strncpy(cell->data, view->symbols[SYNTAX_SYMBOL_SPACE]->symbol, sizeof(cell->data)-1);
			cell->attr = view->symbols[SYNTAX_SYMBOL_SPACE]->style;

		}

		if (view->col + cell->width > view->width) {
			for (int i = view->col; i < view->width; i++)
				view->line->cells[i] = cell_blank;
			view->line = view->line->next;
			view->col = 0;
		}

		if (view->line) {
			view->line->width += cell->width;
			view->line->len += cell->len;
			view->line->lineno = lineno;
			view->line->cells[view->col] = *cell;
			view->col++;
			/* set cells of a character which uses multiple columns */
			for (int i = 1; i < cell->width; i++)
				view->line->cells[view->col++] = cell_unused;
			return true;
		}
		return false;
	}
}

CursorPos view_cursor_getpos(View *view) {
	Cursor *cursor = view->cursor;
	Line *line = cursor->line;
	CursorPos pos = { .line = line->lineno, .col = cursor->col };
	while (line->prev && line->prev->lineno == pos.line) {
		line = line->prev;
		pos.col += line->width;
	}
	pos.col++;
	return pos;
}

static void cursor_to(Cursor *c, size_t pos) {
	Text *txt = c->view->text;
	c->mark = text_mark_set(txt, pos);
	if (pos != c->pos)
		c->lastcol = 0;
	c->pos = pos;
	if (c->sel) {
		size_t anchor = text_mark_get(txt, c->sel->anchor);
		size_t cursor = text_mark_get(txt, c->sel->cursor);
		/* do we have to change the orientation of the selection? */
		if (pos < anchor && anchor < cursor) {
			/* right extend -> left extend  */
			anchor = text_char_next(txt, anchor);
			c->sel->anchor = text_mark_set(txt, anchor);
		} else if (cursor < anchor && anchor <= pos) {
			/* left extend  -> right extend */
			anchor = text_char_prev(txt, anchor);
			c->sel->anchor = text_mark_set(txt, anchor);
		}
		if (anchor <= pos)
			pos = text_char_next(txt, pos);
		c->sel->cursor = text_mark_set(txt, pos);
	}
	if (!view_coord_get(c->view, pos, &c->line, &c->row, &c->col)) {
		if (c->view->cursor == c) {
			c->line = c->view->topline;
			c->row = 0;
			c->col = 0;
		}
		return;
	}
	// TODO: minimize number of redraws
	view_draw(c->view);
}

static bool view_coord_get(View *view, size_t pos, Line **retline, int *retrow, int *retcol) {
	int row = 0, col = 0;
	size_t cur = view->start;
	Line *line = view->topline;

	if (pos < view->start || pos > view->end) {
		if (retline) *retline = NULL;
		if (retrow) *retrow = -1;
		if (retcol) *retcol = -1;
		return false;
	}

	while (line && line != view->lastline && cur < pos) {
		if (cur + line->len > pos)
			break;
		cur += line->len;
		line = line->next;
		row++;
	}

	if (line) {
		int max_col = MIN(view->width, line->width);
		while (cur < pos && col < max_col) {
			cur += line->cells[col].len;
			/* skip over columns occupied by the same character */
			while (++col < max_col && line->cells[col].len == 0);
		}
	} else {
		line = view->bottomline;
		row = view->height - 1;
	}

	if (retline) *retline = line; 
	if (retrow) *retrow = row;
	if (retcol) *retcol = col;
	return true;
}

/* move the cursor to the character at pos bytes from the begining of the file.
 * if pos is not in the current viewport, redraw the view to make it visible */
void view_cursor_to(View *view, size_t pos) {
	view_cursors_to(view->cursor, pos);
}

/* redraw the complete with data starting from view->start bytes into the file.
 * stop once the screen is full, update view->end, view->lastline */
void view_draw(View *view) {
	view_clear(view);
	/* current absolute file position */
	size_t pos = view->start;
	/* number of bytes to read in one go */
	size_t text_len = view->width * view->height;
	/* current buffer to work with */
	char text[text_len+1];
	/* remaining bytes to process in buffer*/
	size_t rem = text_bytes_get(view->text, pos, text_len, text);
	/* NUL terminate because regex(3) function expect it */
	text[rem] = '\0';
	/* current position into buffer from which to interpret a character */
	char *cur = text;
	/* syntax definition to use */
	Syntax *syntax = view->syntax;
	/* matched tokens for each syntax rule */
	regmatch_t match[syntax ? LENGTH(syntax->rules) : 1][1], *matched = NULL;
	memset(match, 0, sizeof match);
	/* default and current curses attributes to use */
	int default_attrs = 0, attrs = default_attrs;
	/* start from known multibyte state */
	mbstate_t mbstate = { 0 };

	while (rem > 0) {

		/* current 'parsed' character' */
		wchar_t wchar;
		Cell cell;
		memset(&cell, 0, sizeof cell);
	
		if (syntax) {
			if (matched && cur >= text + matched->rm_eo) {
				/* end of current match */
				matched = NULL;
				attrs = default_attrs;
				for (int i = 0; i < LENGTH(syntax->rules); i++) {
					if (match[i][0].rm_so == -1)
						continue; /* no match on whole text */
					/* reset matches which overlap with matched */
					if (text + match[i][0].rm_so <= cur && cur < text + match[i][0].rm_eo) {
						match[i][0].rm_so = 0;
						match[i][0].rm_eo = 0;
					}
				}
			}

			if (!matched) {
				size_t off = cur - text; /* number of already processed bytes */
				for (int i = 0; i < LENGTH(syntax->rules); i++) {
					SyntaxRule *rule = &syntax->rules[i];
					if (!rule->rule)
						break;
					if (match[i][0].rm_so == -1)
						continue; /* no match on whole text */
					if (off >= (size_t)match[i][0].rm_eo) {
						/* past match, continue search from current position */
						if (regexec(&rule->regex, cur, 1, match[i], 0) ||
						    match[i][0].rm_so == match[i][0].rm_eo) {
							match[i][0].rm_so = -1;
							match[i][0].rm_eo = -1;
							continue;
						}
						match[i][0].rm_so += off;
						match[i][0].rm_eo += off;
					}

					if (text + match[i][0].rm_so <= cur && cur < text + match[i][0].rm_eo) {
						/* within matched expression */
						matched = &match[i][0];
						attrs = rule->style;
						break; /* first match views */
					}
				}
			}
		}

		size_t len = mbrtowc(&wchar, cur, rem, &mbstate);
		if (len == (size_t)-1 && errno == EILSEQ) {
			/* ok, we encountered an invalid multibyte sequence,
			 * replace it with the Unicode Replacement Character
			 * (FFFD) and skip until the start of the next utf8 char */
			for (len = 1; rem > len && !ISUTF8(cur[len]); len++);
			cell = (Cell){ .data = "\xEF\xBF\xBD", .len = len, .width = 1, .istab = false };
		} else if (len == (size_t)-2) {
			/* not enough bytes available to convert to a
			 * wide character. advance file position and read
			 * another junk into buffer.
			 */
			rem = text_bytes_get(view->text, pos, text_len, text);
			text[rem] = '\0';
			cur = text;
			continue;
		} else if (len == 0) {
			/* NUL byte encountered, store it and continue */
			cell = (Cell){ .data = "\x00", .len = 1, .width = 0, .istab = false };
		} else {
			for (size_t i = 0; i < len; i++)
				cell.data[i] = cur[i];
			cell.data[len] = '\0';
			cell.istab = false;
			cell.len = len;
			cell.width = wcwidth(wchar);
			if (cell.width == -1)
				cell.width = 1;
		}

		if (cur[0] == '\r' && rem > 1 && cur[1] == '\n') {
			/* convert views style newline \r\n into a single char with len = 2 */
			cell = (Cell){ .data = "\n", .len = 2, .width = 1, .istab = false };
		}

		cell.attr = attrs;
		if (!view_addch(view, &cell))
			break;

 		rem -= cell.len;
		cur += cell.len;
		pos += cell.len;
	}

	/* set end of vieviewg region */
	view->end = pos;
	view->lastline = view->line ? view->line : view->bottomline;
	if (view->line) {
		for (int x = view->col; x < view->width; x++)
			view->line->cells[x] = cell_blank;
	}

	for (Line *l = view->lastline->next; l; l = l->next) {
		strncpy(l->cells[0].data, view->symbols[SYNTAX_SYMBOL_EOF]->symbol, sizeof(l->cells[0].data));
		l->cells[0].attr = view->symbols[SYNTAX_SYMBOL_EOF]->style;
		for (int x = 1; x < view->width; x++)
			l->cells[x] = cell_blank;
		l->width = 1;
		l->len = 0;
	}

	for (Selection *s = view->selections; s; s = s->next) {
		Filerange sel = view_selections_get(s);
		if (text_range_valid(&sel)) { 
			Line *start_line; int start_col;
			Line *end_line; int end_col;
			view_coord_get(view, sel.start, &start_line, NULL, &start_col);
			view_coord_get(view, sel.end, &end_line, NULL, &end_col);
			if (start_line || end_line) {
				if (!start_line) {
					start_line = view->topline;
					start_col = 0;
				}
				if (!end_line) {
					end_line = view->lastline;
					end_col = end_line->width;
				}
				for (Line *l = start_line; l != end_line->next; l = l->next) {
					int col = (l == start_line) ? start_col : 0;
					int end = (l == end_line) ? end_col : l->width;
					while (col < end) {
						l->cells[col++].selected = true;
					}
				}
			}

			if (view->events && view->events->selection)
				view->events->selection(view->events->data, &sel);
		}
	}

	for (Cursor *c = view->cursors; c; c = c->next) {
		size_t pos = view_cursors_pos(c);
		if (view_coord_get(view, pos, &c->line, &c->row, &c->col)) {
			c->line->cells[c->col].cursor = true;
			if (view->ui && view->syntax) {
				Line *line_match; int col_match;
				size_t pos_match = text_bracket_match_except(view->text, pos, "<>");
				if (pos != pos_match && view_coord_get(view, pos_match, &line_match, NULL, &col_match)) {
					line_match->cells[col_match].selected = true;
				}
			}
		} else if (c == view->cursor) {
			c->line = view->topline;
			c->row = 0;
			c->col = 0;
		}
	}

	if (view->ui)
		view->ui->draw_text(view->ui, view->topline);
}

bool view_resize(View *view, int width, int height) {
	size_t lines_size = height*(sizeof(Line) + width*sizeof(Cell));
	if (lines_size > view->lines_size) {
		Line *lines = realloc(view->lines, lines_size);
		if (!lines)
			return false;
		view->lines = lines;
		view->lines_size = lines_size;
	}
	view->width = width;
	view->height = height;
	if (view->lines)
		memset(view->lines, 0, view->lines_size);
	view_draw(view);
	return true;
}

int view_height_get(View *view) {
	return view->height;
}

int view_width_get(View *view) {
	return view->width;
}

void view_free(View *view) {
	if (!view)
		return;
	while (view->cursors)
		view_cursors_free(view->cursors);
	while (view->selections)
		view_selections_free(view->selections);
	free(view->lines);
	free(view);
}

void view_reload(View *view, Text *text) {
	view->text = text;
	view_selections_clear(view);
	view_cursor_to(view, 0);
}

View *view_new(Text *text, ViewEvent *events) {
	if (!text)
		return NULL;
	View *view = calloc(1, sizeof(View));
	if (!view)
		return NULL;
	if (!view_cursors_new(view)) {
		view_free(view);
		return NULL;
	}
		
	view->text = text;
	view->events = events;
	view->tabwidth = 8;
	view_symbols_set(view, 0);
	
	if (!view_resize(view, 1, 1)) {
		view_free(view);
		return NULL;
	}

	view_cursor_to(view, 0);

	return view;
}

void view_ui(View *view, UiWin* ui) {
	view->ui = ui;
}

static size_t cursor_set(Cursor *cursor, Line *line, int col) {
	int row = 0;
	View *view = cursor->view;
	size_t pos = view->start;
	/* get row number and file offset at start of the given line */
	for (Line *cur = view->topline; cur && cur != line; cur = cur->next) {
		pos += cur->len;
		row++;
	}

	/* for characters which use more than 1 column, make sure we are on the left most */
	while (col > 0 && line->cells[col].len == 0)
		col--;
	while (col < line->width && line->cells[col].istab)
		col++;

	/* calculate offset within the line */
	for (int i = 0; i < col; i++)
		pos += line->cells[i].len;

	cursor->col = col;
	cursor->row = row;
	cursor->line = line;

	cursor_to(cursor, pos);

	return pos;
}

bool view_viewport_down(View *view, int n) {
	Line *line;
	if (view->end == text_size(view->text))
		return false;
	if (n >= view->height) {
		view->start = view->end;
	} else {
		for (line = view->topline; line && n > 0; line = line->next, n--)
			view->start += line->len;
	}
	view_draw(view);
	return true;
}

bool view_viewport_up(View *view, int n) {
	/* scrolling up is somewhat tricky because we do not yet know where
	 * the lines start, therefore scan backwards but stop at a reasonable
	 * maximum in case we are dealing with a file without any newlines
	 */
	if (view->start == 0)
		return false;
	size_t max = view->width * view->height;
	char c;
	Iterator it = text_iterator_get(view->text, view->start - 1);

	if (!text_iterator_byte_get(&it, &c))
		return false;
	size_t off = 0;
	/* skip newlines immediately before display area */
	if (c == '\n' && text_iterator_byte_prev(&it, &c))
		off++;
	if (c == '\r' && text_iterator_byte_prev(&it, &c))
		off++;
	do {
		if (c == '\n' && --n == 0)
			break;
		if (++off > max)
			break;
	} while (text_iterator_byte_prev(&it, &c));
	if (c == '\r')
		off++;
	view->start -= off;
	view_draw(view);
	return true;
}

void view_redraw_top(View *view) {
	Line *line = view->cursor->line;
	for (Line *cur = view->topline; cur && cur != line; cur = cur->next)
		view->start += cur->len;
	view_draw(view);
	view_cursor_to(view, view->cursor->pos);
}

void view_redraw_center(View *view) {
	int center = view->height / 2;
	size_t pos = view->cursor->pos;
	for (int i = 0; i < 2; i++) {
		int linenr = 0;
		Line *line = view->cursor->line;
		for (Line *cur = view->topline; cur && cur != line; cur = cur->next)
			linenr++;
		if (linenr < center) {
			view_slide_down(view, center - linenr);
			continue;
		}
		for (Line *cur = view->topline; cur && cur != line && linenr > center; cur = cur->next) {
			view->start += cur->len;
			linenr--;
		}
		break;
	}
	view_draw(view);
	view_cursor_to(view, pos);
}

void view_redraw_bottom(View *view) {
	Line *line = view->cursor->line;
	if (line == view->lastline)
		return;
	int linenr = 0;
	size_t pos = view->cursor->pos;
	for (Line *cur = view->topline; cur && cur != line; cur = cur->next)
		linenr++;
	view_slide_down(view, view->height - linenr - 1);
	view_cursor_to(view, pos);
}

size_t view_slide_up(View *view, int lines) {
	Cursor *cursor = view->cursor;
	if (view_viewport_down(view, lines)) {
		if (cursor->line == view->topline)
			cursor_set(cursor, view->topline, cursor->col);
		else
			view_cursor_to(view, cursor->pos);
	} else {
		view_screenline_down(cursor);
	}
	return cursor->pos;
}

size_t view_slide_down(View *view, int lines) {
	Cursor *cursor = view->cursor;
	if (view_viewport_up(view, lines)) {
		if (cursor->line == view->lastline)
			cursor_set(cursor, view->lastline, cursor->col);
		else
			view_cursor_to(view, cursor->pos);
	} else {
		view_screenline_up(cursor);
	}
	return cursor->pos;
}

size_t view_scroll_up(View *view, int lines) {
	Cursor *cursor = view->cursor;
	if (view_viewport_up(view, lines)) {
		Line *line = cursor->line < view->lastline ? cursor->line : view->lastline;
		cursor_set(cursor, line, view->cursor->col);
	} else {
		view_cursor_to(view, 0);
	}
	return cursor->pos;
}

size_t view_scroll_down(View *view, int lines) {
	Cursor *cursor = view->cursor;
	if (view_viewport_down(view, lines)) {
		Line *line = cursor->line > view->topline ? cursor->line : view->topline;
		cursor_set(cursor, line, cursor->col);
	} else {
		view_cursor_to(view, text_size(view->text));
	}
	return cursor->pos;
}

size_t view_line_up(Cursor *cursor) {
	if (cursor->line && cursor->line->prev && cursor->line->prev->prev &&
	    cursor->line->lineno != cursor->line->prev->lineno &&
	    cursor->line->prev->lineno != cursor->line->prev->prev->lineno)
		return view_screenline_up(cursor);
	size_t pos = text_line_up(cursor->view->text, cursor->pos);
	view_cursors_to(cursor, pos);
	return pos;
}

size_t view_line_down(Cursor *cursor) {
	if (cursor->line && (!cursor->line->next || cursor->line->next->lineno != cursor->line->lineno))
		return view_screenline_down(cursor);
	size_t pos = text_line_down(cursor->view->text, cursor->pos);
	view_cursors_to(cursor, pos);
	return pos;
}

size_t view_screenline_up(Cursor *cursor) {
	int lastcol = cursor->lastcol;
	if (!lastcol)
		lastcol = cursor->col;
	if (!cursor->line->prev)
		view_scroll_up(cursor->view, 1);
	if (cursor->line->prev)
		cursor_set(cursor, cursor->line->prev, lastcol);
	cursor->lastcol = lastcol;
	return cursor->pos;
}

size_t view_screenline_down(Cursor *cursor) {
	int lastcol = cursor->lastcol;
	if (!lastcol)
		lastcol = cursor->col;
	if (!cursor->line->next && cursor->line == cursor->view->bottomline)
		view_scroll_down(cursor->view, 1);
	if (cursor->line->next)
		cursor_set(cursor, cursor->line->next, lastcol);
	cursor->lastcol = lastcol;
	return cursor->pos;
}

size_t view_screenline_begin(Cursor *cursor) {
	if (!cursor->line)
		return cursor->pos;
	return cursor_set(cursor, cursor->line, 0);
}

size_t view_screenline_middle(Cursor *cursor) {
	if (!cursor->line)
		return cursor->pos;
	return cursor_set(cursor, cursor->line, cursor->line->width / 2);
}

size_t view_screenline_end(Cursor *cursor) {
	if (!cursor->line)
		return cursor->pos;
	int col = cursor->line->width - 1;
	return cursor_set(cursor, cursor->line, col >= 0 ? col : 0);
}

size_t view_cursor_get(View *view) {
	return view_cursors_pos(view->cursor);
}

const Line *view_lines_get(View *view) {
	return view->topline;
}

void view_scroll_to(View *view, size_t pos) {
	view_cursors_scroll_to(view->cursor, pos);
}

void view_syntax_set(View *view, Syntax *syntax) {
	view->syntax = syntax;
	for (int i = 0; i < LENGTH(view->symbols); i++) {
		if (syntax && syntax->symbols[i].symbol)
			view->symbols[i] = &syntax->symbols[i];
		else
			view->symbols[i] = &symbols_none[i];
	}
	if (syntax) {
		for (const char **style = syntax->styles; *style; style++) {
			view->ui->syntax_style(view->ui, style - syntax->styles, *style);
		}
	}
}

Syntax *view_syntax_get(View *view) {
	return view->syntax;
}

void view_symbols_set(View *view, int flags) {
	for (int i = 0; i < LENGTH(view->symbols); i++) {
		if (flags & (1 << i)) {
			if (view->syntax && view->syntax->symbols[i].symbol)
				view->symbols[i] = &view->syntax->symbols[i];
			else
				view->symbols[i] = &symbols_default[i];
		} else {
			view->symbols[i] = &symbols_none[i];
		}
	}
}

int view_symbols_get(View *view) {
	int flags = 0;
	for (int i = 0; i < LENGTH(view->symbols); i++) {
		if (view->symbols[i] != &symbols_none[i])
			flags |= (1 << i);
	}
	return flags;
}

size_t view_screenline_goto(View *view, int n) {
	size_t pos = view->start;
	for (Line *line = view->topline; --n > 0 && line != view->lastline; line = line->next)
		pos += line->len;
	return pos;
}

Cursor *view_cursors_new(View *view) {
	Cursor *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	c->view = view;
	c->next = view->cursors;
	if (view->cursors)
		view->cursors->prev = c;
	view->cursors = c;
	view->cursor = c;
	return c;
}

int view_cursors_count(View *view) {
	int i = 0;
	for (Cursor *c = view_cursors(view); c; c = view_cursors_next(c))
		i++;
	return i;
}

static void view_cursors_free(Cursor *c) {
	if (!c)
		return;
	register_release(&c->reg);
	if (c->prev)
		c->prev->next = c->next;
	if (c->next)
		c->next->prev = c->prev;
	if (c->view->cursors == c)
		c->view->cursors = c->next;
	if (c->view->cursor == c)
		c->view->cursor = c->next ? c->next : c->prev;
	free(c);
}

void view_cursors_dispose(Cursor *c) {
	if (!c)
		return;
	View *view = c->view;
	if (view->cursors && view->cursors->next) {
		view_selections_free(c->sel);
		view_cursors_free(c);
		view_draw(view);
	}
}

Cursor *view_cursors(View *view) {
	return view->cursors;
}

Cursor *view_cursor(View *view) {
	return view->cursor;
}

Cursor *view_cursors_prev(Cursor *c) {
	return c->prev;
}

Cursor *view_cursors_next(Cursor *c) {
	return c->next;
}

size_t view_cursors_pos(Cursor *c) {
	return text_mark_get(c->view->text, c->mark);
}

Register *view_cursors_register(Cursor *c) {
	return &c->reg;
}

void view_cursors_scroll_to(Cursor *c, size_t pos) {
	View *view = c->view;
	if (view->cursor == c) {
		while (pos < view->start && view_viewport_up(view, 1));
		while (pos > view->end && view_viewport_down(view, 1));
	}
	view_cursors_to(c, pos);
}

void view_cursors_to(Cursor *c, size_t pos) {
	View *view = c->view;
	if (c->view->cursors == c) {
		c->mark = text_mark_set(view->text, pos);

		size_t max = text_size(view->text);
		if (pos == max && view->end != max) {
			/* do not display an empty screen when shoviewg the end of the file */
			view->start = pos;
			view_viewport_up(view, view->height / 2);
		} else {
			/* set the start of the viewable region to the start of the line on which
			 * the cursor should be placed. if this line requires more space than
			 * available in the view then simply start displaying text at the new
			 * cursor position */
			for (int i = 0;  i < 2 && (pos < view->start || pos > view->end); i++) {
				view->start = i == 0 ? text_line_begin(view->text, pos) : pos;
				view_draw(view);
			}
		}
	}

	cursor_to(c, pos);
}

void view_cursors_selection_start(Cursor *c) {
	if (c->sel)
		return;
	size_t pos = view_cursors_pos(c);
	if (pos == EPOS || !(c->sel = view_selections_new(c->view)))
		return;
	Text *txt = c->view->text;
	c->sel->anchor = text_mark_set(txt, pos);
	c->sel->cursor = text_mark_set(txt, text_char_next(txt, pos));
	view_draw(c->view);
}

void view_cursors_selection_restore(Cursor *c) {
	Text *txt = c->view->text;
	if (c->sel)
		return;
	Filerange sel = text_range_new(
		text_mark_get(txt, c->lastsel_anchor),
		text_mark_get(txt, c->lastsel_cursor)
	);
	if (!text_range_valid(&sel))
		return;
	if (!(c->sel = view_selections_new(c->view)))
		return;
	view_selections_set(c->sel, &sel);
	view_cursors_selection_sync(c);
	view_draw(c->view);
}

void view_cursors_selection_stop(Cursor *c) {
	c->sel = NULL;
}

void view_cursors_selection_clear(Cursor *c) {
	view_selections_free(c->sel);
	view_draw(c->view);
}

void view_cursors_selection_swap(Cursor *c) {
	if (!c->sel)
		return;
	view_selections_swap(c->sel);
	view_cursors_selection_sync(c);
}

void view_cursors_selection_sync(Cursor *c) {
	if (!c->sel)
		return;
	Text *txt = c->view->text;
	size_t anchor = text_mark_get(txt, c->sel->anchor);
	size_t cursor = text_mark_get(txt, c->sel->cursor);
	bool right_extending = anchor < cursor;
	if (right_extending)
		cursor = text_char_prev(txt, cursor);
	view_cursors_to(c, cursor);
}

Filerange view_cursors_selection_get(Cursor *c) {
	return view_selections_get(c->sel);
}

void view_cursors_selection_set(Cursor *c, Filerange *r) {
	if (!text_range_valid(r))
		return;
	if (!c->sel)
		c->sel = view_selections_new(c->view);
	if (!c->sel)
		return;
	
	view_selections_set(c->sel, r);
}

Selection *view_selections_new(View *view) {
	Selection *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;

	s->view = view;
	s->next = view->selections;
	if (view->selections)
		view->selections->prev = s;
	view->selections = s;
	return s;
}

void view_selections_free(Selection *s) {
	if (!s)
		return;
	if (s->prev)
		s->prev->next = s->next;
	if (s->next)
		s->next->prev = s->prev;
	if (s->view->selections == s)
		s->view->selections = s->next;
	// XXX: add backlink Selection->Cursor?
	for (Cursor *c = s->view->cursors; c; c = c->next) {
		if (c->sel == s) {
			c->lastsel_anchor = s->anchor;
			c->lastsel_cursor = s->cursor;
			c->sel = NULL;
		}
	}
	free(s);
}

void view_selections_clear(View *view) {
	while (view->selections)
		view_selections_free(view->selections);
	view_draw(view);
}

void view_cursors_clear(View *view) {
	for (Cursor *c = view->cursors, *next; c; c = next) {
		next = c->next;
		if (c != view->cursor) {
			view_selections_free(c->sel);
			view_cursors_free(c);
		}
	}
	view_draw(view);
} 

void view_selections_swap(Selection *s) {
	Mark temp = s->anchor;
	s->anchor = s->cursor;
	s->cursor = temp;
}

Selection *view_selections(View *view) {
	return view->selections;
}

Selection *view_selections_prev(Selection *s) {
	return s->prev;
}

Selection *view_selections_next(Selection *s) {
	return s->next;
}

Filerange view_selections_get(Selection *s) {
	if (!s)
		return text_range_empty();
	Text *txt = s->view->text;
	size_t anchor = text_mark_get(txt, s->anchor);
	size_t cursor = text_mark_get(txt, s->cursor);
	return text_range_new(anchor, cursor);
}

void view_selections_set(Selection *s, Filerange *r) {
	if (!text_range_valid(r))
		return;
	Text *txt = s->view->text;
	size_t anchor = text_mark_get(txt, s->anchor);
	size_t cursor = text_mark_get(txt, s->cursor);
	bool left_extending = anchor > cursor;
	if (left_extending) {
		s->anchor = text_mark_set(txt, r->end);
		s->cursor = text_mark_set(txt, r->start);
	} else {
		s->anchor = text_mark_set(txt, r->start);
		s->cursor = text_mark_set(txt, r->end);
	}
	view_draw(s->view);
}
