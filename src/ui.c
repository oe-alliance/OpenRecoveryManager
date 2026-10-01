#define _GNU_SOURCE

#include "ui.h"

#include "i18n.h"
#include "lvgl.h"
#include "qrcodegen.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef FBIO_SET_MANUAL_BLIT
#define FBIO_SET_MANUAL_BLIT _IOW('F', 0x21, __u8)
#endif
#ifndef FBIO_BLIT
#define FBIO_BLIT 0x22
#endif

LV_FONT_DECLARE(orm_font_14)
LV_FONT_DECLARE(orm_font_16)
LV_FONT_DECLARE(orm_font_18)
LV_FONT_DECLARE(orm_font_20)
LV_FONT_DECLARE(orm_font_24)
LV_FONT_DECLARE(orm_font_28)
LV_FONT_DECLARE(orm_font_32)

#define COLOR_GROUND 0x0b1016
#define COLOR_SURFACE 0x121a23
#define COLOR_SUNKEN 0x070b10  /* Behind logs and the footer. */
#define COLOR_DIALOG 0x1a2530
#define COLOR_LINE 0x1d2733
#define COLOR_TEXT 0xe8edf2
#define COLOR_BODY 0xc9d4de
#define COLOR_MUTED 0x7d8fa1
#define COLOR_DIM 0x5d6b79
#define COLOR_FOCUS 0x2b7fd9
#define COLOR_FOCUS_IDLE 0x1b3a5c  /* The selection of the side without the keys. */
#define COLOR_WARN 0xf2b84b  /* Marks and notes. */
#define COLOR_WARN_GROUND 0x2a1f10  /* The dark amber of the crash card, a remote session, a marked choice. */
#define COLOR_WARN_LINE 0x6b4a17
#define COLOR_OK 0x4cc26a
#define COLOR_RED 0xef5b5b
#define COLOR_YELLOW 0xf2cc3d
#define COLOR_BLUE 0x5b9dff
#define COLOR_LINK 0x7cc4ff

#define KEYBOARD "\xE2\x8C\xA8"  /* U+2328 of DejaVu Sans. */
#define MAX_COLUMNS 8
#define DRAW_ROWS 120  /* Of the screen, drawn at once. */

enum font_role { FONT_LOG, FONT_SMALL, FONT_TEXT, FONT_ITEM, FONT_HEAD };
enum card_kind { CARD_INFO, CARD_WARN, CARD_ERROR };

static struct ui_context *active;  /* The open framebuffer. */
static lv_display_t *display;
static uint8_t *draw_buffer;
static int screen_width;
static int screen_height;
static lv_obj_t *header;
static lv_obj_t *header_title;
static lv_obj_t *header_remote;
static lv_obj_t *header_clock;
static lv_obj_t *menu;
static lv_obj_t *detail;
static lv_obj_t *footer;
static lv_obj_t *modal;
#define MAX_FOOTER_KEYS 16
static struct {
	lv_obj_t *pair;
	char name[16];
} footer_pairs[MAX_FOOTER_KEYS];  /* The keys of the footer, for ui_key_pressed(). */
static int footer_key_count;
/* While something long runs: a spinner at the top right and the time in the footer. */
static int footer_has_keys;
static int footer_has_ok;
static int busy_forced;
static int busy;
static uint32_t busy_since;
static lv_obj_t *spinner;
static lv_obj_t *elapsed;
static char footer_text[512];
static int menu_rows = 1;  /* Of the list drawn last. */

static char side_names[UI_MAX_ITEMS][64];
static char side_icons[UI_MAX_ITEMS][8];
static char side_marks[UI_MAX_ITEMS];
static lv_obj_t *side_rows[UI_MAX_ITEMS];
static int side_count;
static int side_selected;
static int side_focused = 1;

static time_t shown_minute = -1;
static int shown_generation = -1;  /* Of the language on the screen. */
static int shown_remote = -2;
static int (*remote_counts)(int *users, int *open);  /* A remote support session runs. */

/* The content of the screen, a screen drawn the same again is skipped: the loops draw after
 * every timeout of the keys. */
static uint64_t frame_hash;
static uint64_t below_hash;  /* Of the screen below a dialog, */
static char below_footer[512];  /* and its keys. */

static uint64_t hash_int(uint64_t hash, long value)
{
	for (size_t i = 0; i < sizeof(value); ++i)
		hash = (hash ^ ((unsigned long)value >> (8 * i) & 0xff)) * 1099511628211ULL;
	return hash;
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
	if (!text)
		return hash_int(hash, -1);
	for (; *text; ++text)
		hash = (hash ^ (unsigned char)*text) * 1099511628211ULL;
	return hash_int(hash, 0);  /* So "a", "bc" differs from "ab", "c". */
}

static int same_frame(uint64_t hash)
{
	if (hash == frame_hash)
		return 1;
	frame_hash = hash;
	return 0;
}

/* Sizes are for 1920 x 1080 and scale with the screen. */
static int px(int value)
{
	return value * screen_width / 1920;
}

static const lv_font_t *font(enum font_role role)
{
	static const lv_font_t *const full[] = {&orm_font_20, &orm_font_20, &orm_font_24, &orm_font_28, &orm_font_32};
	/* About two thirds for 1280 x 720, so the longer translations fit as well. */
	static const lv_font_t *const small[] = {&orm_font_14, &orm_font_14, &orm_font_16, &orm_font_18, &orm_font_24};
	return screen_width >= 1600 ? full[role] : small[role];
}

static int remote_state(void)
{
	int users = 0;
	int open = 0;
	if (!remote_counts || !remote_counts(&users, &open))
		return -1;
	return users * 1000 + open;
}

void ui_set_remote(int (*counts)(int *users, int *open))
{
	remote_counts = counts;
}

static uint32_t milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{
	const struct ui_context *ui = active;
	if (ui && ui->screen) {
		size_t width = (size_t)lv_area_get_width(area) * 4;
		for (int32_t y = area->y1; y <= area->y2; ++y) {
			size_t offset = (size_t)(y + (int)ui->var.yoffset) * ui->fix.line_length +
				(size_t)(area->x1 + (int)ui->var.xoffset) * 4;
			if (offset + width <= ui->memory_size)
				memcpy(ui->screen + offset, pixels + (size_t)(y - area->y1) * width, width);
		}
		if (lv_display_flush_is_last(disp) && ui->manual_blit)
			ioctl(ui->fd, FBIO_BLIT);
	}
	lv_display_flush_ready(disp);
}

/* Widgets */

static lv_obj_t *box(lv_obj_t *parent)
{
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_scrollable(obj, false);
	return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, enum font_role role, uint32_t color)
{
	lv_obj_t *obj = lv_label_create(parent);
	lv_label_set_text(obj, text ? text : "");
	lv_obj_set_style_text_font(obj, font(role), 0);
	lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
	if (i18n_rtl())  /* In the mirrored screen every text runs its own way, e.g. one not translated yet. */
		lv_obj_set_style_base_dir(obj, LV_BASE_DIR_AUTO, 0);
	return obj;
}

/* Keys, digits and icons stay as they are in a language from the right. */
static lv_obj_t *ltr(lv_obj_t *obj)
{
	lv_obj_set_style_base_dir(obj, LV_BASE_DIR_LTR, 0);
	return obj;
}

static lv_obj_t *paragraph(lv_obj_t *parent, const char *text, enum font_role role, uint32_t color)
{
	lv_obj_t *obj = label(parent, text, role, color);
	lv_obj_set_width(obj, LV_PCT(100));
	lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_line_space(obj, px(8), 0);
	return obj;
}

static lv_obj_t *column(lv_obj_t *parent, int gap)
{
	lv_obj_t *obj = box(parent);
	lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(obj, gap, 0);
	lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
	return obj;
}

static lv_obj_t *row(lv_obj_t *parent, int gap)
{
	lv_obj_t *obj = box(parent);
	lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(obj, gap, 0);
	lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
	return obj;
}

static void fill(lv_obj_t *obj, uint32_t color)
{
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
}

static int text_width(const char *text, size_t length, const lv_font_t *font_used)
{
	char part[512];
	lv_point_t size;
	if (length >= sizeof(part))
		length = sizeof(part) - 1;
	memcpy(part, text, length);
	part[length] = '\0';
	lv_text_get_size(&size, part, font_used, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x;
}

/* Every column but the last is as wide as its widest text, the last one takes the rest. */
static void measure_columns(const char *text, int widths[MAX_COLUMNS], const lv_font_t *font_used)
{
	int index = 0;
	const char *tab = text ? strchr(text, '\t') : NULL;
	while (tab && index < MAX_COLUMNS - 1) {
		int width = text_width(text, (size_t)(tab - text), font_used);
		if (width > widths[index])
			widths[index] = width;
		text = tab + 1;
		index++;
		tab = strchr(text, '\t');
	}
}

/* The columns of text into the row parent, the last one cut at its edge. */
static void columns(lv_obj_t *parent, const char *text, const int widths[MAX_COLUMNS], const char *align,
	enum font_role role, uint32_t color)
{
	char field[512];
	int index = 0;
	while (text) {
		const char *tab = index < MAX_COLUMNS - 1 ? strchr(text, '\t') : NULL;
		size_t length = tab ? (size_t)(tab - text) : strlen(text);
		lv_obj_t *obj;
		if (length >= sizeof(field))
			length = sizeof(field) - 1;
		memcpy(field, text, length);
		field[length] = '\0';
		obj = label(parent, field, role, color);
		if (tab) {
			lv_obj_set_width(obj, widths[index]);
			if (align && (int)strlen(align) > index && align[index] == 'r')
				lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, 0);
		} else {
			lv_obj_set_flex_grow(obj, 1);
			lv_obj_set_height(obj, lv_font_get_line_height(font(role)));
			lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_CLIP);
		}
		text = tab ? tab + 1 : NULL;
		index++;
	}
}

/* A track on the right of parent, the thumb shows the visible part and its place. */
static void scrollbar(lv_obj_t *parent, int height, int count, int first, int rows)
{
	lv_obj_t *track = box(parent);
	lv_obj_t *thumb = box(track);
	int size = count ? height * rows / count : height;
	int top;
	if (size < px(30))
		size = px(30);
	if (size > height)
		size = height;
	top = count > rows ? (height - size) * first / (count - rows) : 0;
	if (top > height - size)  /* The last page of a menu can start later. */
		top = height - size;
	lv_obj_set_size(track, px(6), height);
	lv_obj_set_style_radius(track, px(3), 0);
	fill(track, COLOR_LINE);
	lv_obj_set_size(thumb, px(6), size);
	lv_obj_set_y(thumb, top);
	lv_obj_set_style_radius(thumb, px(3), 0);
	fill(thumb, COLOR_MUTED);
}

static void card(lv_obj_t *parent, const char *title, const char *text, enum card_kind kind, int large)
{
	static const uint32_t grounds[] = {COLOR_SURFACE, COLOR_WARN_GROUND, 0x2a1414};
	static const uint32_t borders[] = {COLOR_LINE, COLOR_WARN_LINE, 0x6b2525};
	static const uint32_t titles[] = {COLOR_BLUE, COLOR_WARN, COLOR_RED};
	lv_obj_t *obj = column(parent, px(8));
	const char *rest = large ? strchr(text, '\n') : NULL;
	fill(obj, grounds[kind]);
	lv_obj_set_style_border_color(obj, lv_color_hex(borders[kind]), 0);
	lv_obj_set_style_border_width(obj, 2, 0);
	lv_obj_set_style_radius(obj, px(16), 0);
	lv_obj_set_style_pad_all(obj, px(28), 0);
	if (title)
		label(obj, title, FONT_SMALL, titles[kind]);
	if (rest) {  /* The first line large, e.g. the last start step. */
		char first[256];
		snprintf(first, sizeof(first), "%.*s", (int)(rest - text), text);
		paragraph(obj, first, FONT_HEAD, COLOR_TEXT);
		paragraph(obj, rest + 1, FONT_TEXT, kind == CARD_WARN ? 0xc8b490 : COLOR_BODY);
	} else
		paragraph(obj, text, large ? FONT_HEAD : FONT_TEXT, COLOR_TEXT);
}

/* The frame */

static void header_colors(int remote)
{
	fill(header, remote ? COLOR_WARN_GROUND : COLOR_GROUND);
	lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(header, lv_color_hex(COLOR_WARN_LINE), 0);
	lv_obj_set_style_border_width(header, remote ? 2 : 0, 0);
	lv_obj_set_hidden(header_remote, !remote);
}

/* The clock and the remote support session, 1 when they changed. */
static int header_update(void)
{
	time_t now = time(NULL);
	int state = remote_state();
	char text[64];
	struct tm local;
	if (now / 60 == shown_minute && state == shown_remote)
		return 0;
	shown_minute = now / 60;
	shown_remote = state;
	localtime_r(&now, &local);
	strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);  /* Without seconds, once a minute. */
	lv_label_set_text(header_clock, text);
	if (state >= 0) {
		snprintf(text, sizeof(text), "%d " KEYBOARD " %d", state / 1000, state % 1000);
		lv_label_set_text(header_remote, text);
	}
	header_colors(state >= 0);
	return 1;
}

static void sidebar_build(void);

/* A footer without keys, like "Please wait...", or ui_busy() means something long runs. */
static void busy_update(void)
{
	int now_busy = busy_forced || (!footer_has_keys && footer_text[0]);
	if (now_busy && !busy)
		busy_since = milliseconds();
	busy = now_busy;
	if (!busy) {
		if (spinner)
			lv_obj_delete(spinner);
		if (elapsed)
			lv_obj_delete(elapsed);
		spinner = NULL;
		elapsed = NULL;
		return;
	}
	if (!spinner) {
		spinner = lv_spinner_create(detail);
		lv_spinner_set_anim_params(spinner, 1200, 240);
		lv_obj_set_size(spinner, px(40), px(40));
		lv_obj_set_floating(spinner, true);
		lv_obj_align(spinner, i18n_rtl() ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, 0, 0);
		lv_obj_set_style_arc_width(spinner, px(5), LV_PART_MAIN);
		lv_obj_set_style_arc_width(spinner, px(5), LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_LINE), LV_PART_MAIN);
		lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_FOCUS), LV_PART_INDICATOR);
	}
	if (!elapsed) {
		lv_obj_set_flex_grow(box(footer), 1);
		elapsed = label(footer, "", FONT_SMALL, COLOR_MUTED);
		ltr(elapsed);
	}
	{
		uint32_t seconds = (milliseconds() - busy_since) / 1000;
		char text[16];
		snprintf(text, sizeof(text), "%u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
		if (strcmp(lv_label_get_text(elapsed), text))
			lv_label_set_text(elapsed, text);
	}
	lv_timer_handler();  /* Turns the spinner. */
}

int ui_offers_ok(void)
{
	return footer_has_ok;
}

/* Whether the footer key named name stands for the key named key: ARROWS for the arrows, 1-9 for a digit. */
static int footer_key_matches(const char *name, const char *key)
{
	if (!strcmp(name, key))
		return 1;
	if (!strcmp(name, "ARROWS"))
		return !strcmp(key, "UP") || !strcmp(key, "DOWN") || !strcmp(key, "LEFT") || !strcmp(key, "RIGHT");
	return key[0] >= '0' && key[0] <= '9' && !key[1] && name[0] >= '0' && name[0] <= '9';
}

void ui_key_pressed(const char *key)
{
	const struct timespec moment = {0, 120 * 1000000L};
	lv_obj_t *pair = NULL;
	if (!display || !key)
		return;
	for (int i = 0; i < footer_key_count; ++i)
		if (footer_key_matches(footer_pairs[i].name, key)) {
			pair = footer_pairs[i].pair;
			break;
		}
	if (!pair)
		return;
	lv_obj_set_style_bg_opa(pair, LV_OPA_COVER, 0);
	lv_refr_now(display);
	nanosleep(&moment, NULL);
	lv_obj_set_style_bg_opa(pair, LV_OPA_TRANSP, 0);  /* Drawn with the next screen. */
}

void ui_busy(const struct ui_context *ui, int on)
{
	(void)ui;
	busy_forced = on;
}

static void render(void)
{
	if (!display || !active || !active->screen)
		return;
	busy_update();
	if (shown_generation != i18n_generation()) {  /* A language from the right mirrors the whole screen. */
		shown_generation = i18n_generation();
		lv_obj_set_style_base_dir(lv_screen_active(), i18n_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
		lv_obj_set_style_border_side(menu, i18n_rtl() ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_RIGHT, 0);
		sidebar_build();
	}
	header_update();
	lv_refr_now(display);
}

static void sidebar_style(void)
{
	for (int i = 0; i < side_count; ++i) {
		int chosen = i == side_selected;
		uint32_t icon_color = 0x8ca0b4;  /* The icon, subdued like the digit. */
		uint32_t name_color = COLOR_TEXT;
		if (side_marks[i] == 2) {
			icon_color = COLOR_DIM;
			name_color = COLOR_DIM;
		} else if (chosen) {
			icon_color = 0xd7e7f7;
			name_color = 0xffffff;
		}
		lv_obj_set_style_bg_opa(side_rows[i], chosen ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(side_rows[i], lv_color_hex(side_focused ? COLOR_FOCUS : COLOR_FOCUS_IDLE), 0);
		lv_obj_set_style_text_color(lv_obj_get_child(side_rows[i], 1), lv_color_hex(icon_color), 0);
		lv_obj_set_style_text_color(lv_obj_get_child(side_rows[i], 2), lv_color_hex(name_color), 0);
	}
}

static void sidebar_build(void)
{
	lv_obj_clean(menu);
	for (int i = 0; i < side_count; ++i) {
		char digit[4];
		lv_obj_t *entry = box(menu);
		lv_obj_set_size(entry, LV_PCT(100), px(78));
		lv_obj_set_style_radius(entry, px(14), 0);
		lv_obj_set_style_pad_hor(entry, px(24), 0);
		lv_obj_set_flex_flow(entry, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(entry, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(entry, px(24), 0);
		snprintf(digit, sizeof(digit), "%d", (i + 1) % 10);
		lv_obj_set_width(ltr(label(entry, digit, FONT_TEXT, COLOR_MUTED)), px(20));
		lv_obj_set_width(ltr(label(entry, side_icons[i], FONT_ITEM, COLOR_MUTED)), px(36));
		label(entry, _(side_names[i]), FONT_ITEM, COLOR_TEXT);
		side_rows[i] = entry;
	}
	sidebar_style();
}

static void build_base(void)
{
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *body;
	lv_obj_clean(screen);
	spinner = NULL;
	elapsed = NULL;
	footer_key_count = 0;
	lv_obj_remove_style_all(screen);
	fill(screen, COLOR_GROUND);
	lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(screen, false);

	header = row(screen, px(24));
	lv_obj_set_height(header, px(96));
	lv_obj_set_style_pad_hor(header, px(60), 0);
	header_title = label(header, "Open Recovery Manager (ORM)", FONT_HEAD, COLOR_TEXT);
	lv_obj_set_flex_grow(header_title, 1);
	header_remote = ltr(label(header, "", FONT_ITEM, COLOR_WARN));
	lv_obj_set_style_pad_right(header_remote, px(40), 0);
	header_clock = ltr(label(header, "", FONT_ITEM, COLOR_BODY));

	body = row(screen, 0);
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	menu = column(body, px(6));
	lv_obj_set_size(menu, px(600), LV_PCT(100));
	lv_obj_set_style_pad_all(menu, px(36), 0);
	lv_obj_set_style_border_side(menu, LV_BORDER_SIDE_RIGHT, 0);
	lv_obj_set_style_border_width(menu, 2, 0);
	lv_obj_set_style_border_color(menu, lv_color_hex(COLOR_LINE), 0);
	detail = box(body);
	lv_obj_set_height(detail, LV_PCT(100));
	lv_obj_set_flex_grow(detail, 1);
	lv_obj_set_style_pad_hor(detail, px(72), 0);
	lv_obj_set_style_pad_ver(detail, px(56), 0);

	footer = row(screen, px(22));  /* With the padding of the keys 42 between them. */
	lv_obj_set_height(footer, px(84));
	lv_obj_set_style_pad_hor(footer, px(50), 0);
	fill(footer, COLOR_SUNKEN);

	modal = NULL;
	footer_text[0] = '\0';
	frame_hash = 0;
	shown_minute = -1;
	shown_generation = -1;
	sidebar_build();
}

/* The color of the key named by the first name bytes of text, like on the remote control. */
static uint32_t key_color(const char *text, size_t name)
{
	static const struct {
		const char *name;
		uint32_t color;
	} colors[] = {{"RED", COLOR_RED}, {"GREEN", COLOR_OK}, {"OK", COLOR_OK}, {"YELLOW", COLOR_YELLOW},
		{"BLUE", COLOR_BLUE}};
	uint32_t color = COLOR_TEXT;
	for (size_t k = 0; k < sizeof(colors) / sizeof(colors[0]); ++k)
		if (strlen(colors[k].name) == name && !strncmp(text, colors[k].name, name))
			color = colors[k].color;
	return color;
}

/* The keys, colored like on the remote control: "OK: Open   RED: Shutdown". */
static void footer_part(char *text)
{
	while (*text) {
		char *next = strstr(text, "   ");
		size_t name = strspn(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-");
		if (next)
			*next = '\0';
		if (name && text[name] == ':') {
			lv_obj_t *pair = box(footer);
			footer_has_keys = 1;
			if (name == 2 && !strncmp(text, "OK", 2))
				footer_has_ok = 1;
			uint32_t color = key_color(text, name);
			lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
			lv_obj_set_style_pad_column(pair, px(10), 0);
			lv_obj_set_style_pad_hor(pair, px(10), 0);  /* Room for the light of ui_key_pressed(). */
			lv_obj_set_style_pad_ver(pair, px(6), 0);
			lv_obj_set_style_radius(pair, px(10), 0);
			lv_obj_set_style_bg_color(pair, lv_color_hex(COLOR_FOCUS_IDLE), 0);
			lv_obj_set_size(pair, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
			text[name] = '\0';
			if (footer_key_count < MAX_FOOTER_KEYS) {
				footer_pairs[footer_key_count].pair = pair;
				snprintf(footer_pairs[footer_key_count].name, sizeof(footer_pairs[0].name), "%s", text);
				footer_key_count++;
			}
			ltr(label(pair, text, FONT_SMALL, color));  /* E.g. 1-8 as it is. */
			label(pair, text + name + 1 + strspn(text + name + 1, " "), FONT_SMALL, 0x9fb0c2);
		} else if (*text)  /* No key, e.g. "Please wait...". */
			label(footer, text, FONT_SMALL, COLOR_WARN);
		if (!next)
			break;
		text = next + 3 + strspn(next + 3, " ");
	}
}

static void footer_set(const char *text)
{
	char copy[512];
	char *right;
	if (!text)
		text = "";  /* Every screen names its keys. */
	if (!strcmp(text, footer_text))
		return;
	snprintf(footer_text, sizeof(footer_text), "%s", text);
	lv_obj_clean(footer);
	elapsed = NULL;  /* Cleaned with it; a new object may get its address. */
	footer_key_count = 0;
	footer_has_keys = 0;
	footer_has_ok = 0;
	snprintf(copy, sizeof(copy), "%s", text);
	right = strchr(copy, '\t');
	if (right)
		*right++ = '\0';
	footer_part(copy);
	if (right) {
		lv_obj_set_flex_grow(box(footer), 1);
		footer_part(right);
	}
}

/* Green after UI_GREEN, red after UI_RED, else amber. */
static uint32_t note_color(char mark)
{
	if (mark == UI_GREEN[0])
		return COLOR_OK;
	if (mark == UI_RED[0])
		return COLOR_RED;
	return COLOR_WARN;
}

/* "ORM - Flash online/local\t" UI_GREEN "note\tright": the title, its note and the right side. */
static void heading(lv_obj_t *parent, const char *title)
{
	char text[256];
	char *note;
	char *right = NULL;
	char *result;
	lv_obj_t *line = row(parent, 0);
	lv_obj_t *title_label;
	snprintf(text, sizeof(text), "%s", strncmp(title, "ORM - ", 6) ? title : title + 6);
	result = strchr(text, '\n');
	if (result)
		*result++ = '\0';
	note = strchr(text, '\t');
	if (note) {
		*note++ = '\0';
		right = strchr(note, '\t');
		if (right)
			*right++ = '\0';
	}
	title_label = label(line, text, FONT_HEAD, COLOR_TEXT);
	lv_obj_set_style_max_width(title_label, LV_PCT(100), 0);  /* A long title takes a second line. */
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_WRAP);
	if (note && *note) {
		uint32_t color = note_color(*note);
		if (*note == UI_GREEN[0] || *note == UI_RED[0])
			note++;
		label(line, note, FONT_HEAD, color);
	}
	if (right && *right) {
		lv_obj_set_flex_grow(box(line), 1);
		label(line, right, FONT_TEXT, COLOR_MUTED);
	}
	if (result && *result) {  /* The outcome of what ran, below the title. */
		uint32_t color = note_color(*result);
		if (*result == UI_GREEN[0] || *result == UI_RED[0])
			result++;
		paragraph(parent, result, FONT_TEXT, color);
	}
}

/* An empty screen on the right, or in the dialog. */
static lv_obj_t *page(const char *title)
{
	lv_obj_t *parent = modal ? modal : detail;
	lv_obj_t *root;
	lv_obj_clean(parent);
	if (parent == detail)
		spinner = NULL;  /* Cleaned with it. */
	root = column(parent, px(24));
	if (!modal)
		lv_obj_set_height(root, LV_PCT(100));
	if (title)
		heading(root, title);
	return root;
}

/* The rest of the height of root, for what fills it. */
static lv_obj_t *filler(lv_obj_t *root)
{
	lv_obj_t *obj = box(root);
	lv_obj_set_width(obj, LV_PCT(100));
	if (modal)
		lv_obj_set_height(obj, LV_SIZE_CONTENT);
	else
		lv_obj_set_flex_grow(obj, 1);
	return obj;
}

/* Opening */

/* The first framebuffer that opens, ui->fd stays negative without one. */
static void open_device(struct ui_context *ui)
{
	static const char *const devices[] = {
		"/dev/fb0", "/dev/fb/0", "/dev/fb1", NULL
	};
	for (int i = 0; devices[i]; ++i) {
		ui->fd = open(devices[i], O_RDWR | O_CLOEXEC);
		if (ui->fd >= 0) {
			snprintf(ui->device, sizeof(ui->device), "%s", devices[i]);
			return;
		}
	}
}

/* The display in the mode of ui, 0 without memory for it. */
static int display_setup(const struct ui_context *ui)
{
	if (display) {  /* Another mode, e.g. after ofgwrite. */
		lv_display_delete(display);
		free(draw_buffer);
	}
	screen_width = (int)ui->var.xres;
	screen_height = (int)ui->var.yres;
	draw_buffer = malloc((size_t)screen_width * DRAW_ROWS * 4);
	display = lv_display_create(screen_width, screen_height);
	if (!draw_buffer || !display)
		return 0;
	lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(display, draw_buffer, NULL, (uint32_t)screen_width * DRAW_ROWS * 4,
		LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush);
	build_base();
	return 1;
}

int ui_open(struct ui_context *ui)
{
	unsigned char manual = 1;

	if (!ui)
		return 0;
	memset(ui, 0, sizeof(*ui));
	ui->fd = -1;
	open_device(ui);
	if (ui->fd < 0)
		return 0;
	if (ioctl(ui->fd, FBIOGET_VSCREENINFO, &ui->var) < 0 ||
		ioctl(ui->fd, FBIOGET_FSCREENINFO, &ui->fix) < 0)
		goto failed;
	if (ui->var.bits_per_pixel != 32) {  /* Enigma2 sets 32 bits. */
		errno = ENOTSUP;
		goto failed;
	}
	ui->memory_size = ui->fix.smem_len;
	if (!ui->memory_size)
		ui->memory_size = (size_t)ui->fix.line_length * ui->var.yres_virtual;
	ui->screen = mmap(NULL, ui->memory_size, PROT_READ | PROT_WRITE,
		MAP_SHARED, ui->fd, 0);
	if (ui->screen == MAP_FAILED) {
		ui->screen = NULL;
		goto failed;
	}
	if (ioctl(ui->fd, FBIO_SET_MANUAL_BLIT, &manual) == 0)
		ui->manual_blit = 1;
	active = ui;
	if (!display) {
		lv_init();
		lv_tick_set_cb(milliseconds);
	}
	if (!display || screen_width != (int)ui->var.xres || screen_height != (int)ui->var.yres) {
		if (!display_setup(ui))
			goto failed;
	} else {
		lv_obj_invalidate(lv_screen_active());
		frame_hash = 0;
	}
	render();
	return 1;

failed:
	if (ui->screen)
		munmap(ui->screen, ui->memory_size);
	ui->screen = NULL;
	if (ui->fd >= 0)
		close(ui->fd);
	ui->fd = -1;
	active = NULL;
	return 0;
}

void ui_close(struct ui_context *ui)
{
	unsigned char manual = 0;
	if (!ui)
		return;
	if (active == ui)
		active = NULL;
	if (ui->screen) {
		msync(ui->screen, ui->memory_size, MS_SYNC);
		munmap(ui->screen, ui->memory_size);
		ui->screen = NULL;
	}
	if (ui->fd >= 0) {
		if (ui->manual_blit)
			ioctl(ui->fd, FBIO_SET_MANUAL_BLIT, &manual);
		close(ui->fd);
		ui->fd = -1;
	}
}

void ui_clear(struct ui_context *ui, struct ui_color color)
{
	uint32_t pixel = (uint32_t)color.a << 24 | (uint32_t)color.r << 16 | (uint32_t)color.g << 8 | color.b;
	if (!ui || !ui->screen)
		return;
	for (uint32_t y = 0; y < ui->var.yres; ++y) {
		size_t offset = (size_t)(y + ui->var.yoffset) * ui->fix.line_length + (size_t)ui->var.xoffset * 4;
		uint32_t *line = (uint32_t *)(ui->screen + offset);
		if (offset + (size_t)ui->var.xres * 4 > ui->memory_size)
			break;
		for (uint32_t x = 0; x < ui->var.xres; ++x)
			line[x] = pixel;
	}
	if (display)
		lv_obj_invalidate(lv_screen_active());
	frame_hash = 0;
}

void ui_present(struct ui_context *ui)
{
	if (!ui || ui->fd < 0 || !ui->screen)
		return;
	if (ui->manual_blit)
		ioctl(ui->fd, FBIO_BLIT);
	msync(ui->screen, ui->memory_size, MS_ASYNC);
}

/* The menu on the left */

/* 1 when the menu differs from the one shown. */
static int sidebar_changed(const char *const names[], int count, const char *marks)
{
	if (count != side_count)
		return 1;
	for (int i = 0; i < count; ++i)
		if (strcmp(side_names[i], names[i]) || side_marks[i] != (marks ? marks[i] : 0))
			return 1;
	return 0;
}

void ui_sidebar(const struct ui_context *ui, const char *const names[], const char *const icons[], int count,
	const char *marks)
{
	(void)ui;
	if (!sidebar_changed(names, count, marks))
		return;
	side_count = count < UI_MAX_ITEMS ? count : UI_MAX_ITEMS;
	for (int i = 0; i < side_count; ++i) {
		snprintf(side_names[i], sizeof(side_names[i]), "%s", names[i]);
		snprintf(side_icons[i], sizeof(side_icons[i]), "%s", icons ? icons[i] : "");
		side_marks[i] = marks ? marks[i] : 0;
	}
	if (display)
		sidebar_build();
}

void ui_sidebar_select(const struct ui_context *ui, int selected, int focused)
{
	(void)ui;
	if (selected == side_selected && focused == side_focused)
		return;
	side_selected = selected;
	side_focused = focused;
	if (display)
		sidebar_style();
}

int ui_sidebar_focus(const struct ui_context *ui, int focused)
{
	int was = side_focused;
	ui_sidebar_select(ui, side_selected, focused);
	render();
	return was;
}

/* The screens on the right */

void ui_preview(const struct ui_context *ui, const struct ui_card_page *p)
{
	uint64_t hash = hash_int(14695981039346656037ULL, 8);
	(void)ui;
	hash = hash_text(hash_text(hash_text(hash_text(hash_text(hash_text(hash_int(hash, p->warn), p->title),
		p->card_title), p->card), p->body), p->info), p->footer);
	if (display && !same_frame(hash)) {
		lv_obj_t *root = page(NULL);
		if (p->card) {
			card(root, p->card_title, p->card, p->warn ? CARD_WARN : CARD_INFO, 1);
			root = column(root, px(24));  /* In line with the text of the card. */
			lv_obj_set_width(root, LV_PCT(100));
			lv_obj_set_style_pad_hor(root, px(30), 0);
		}
		heading(root, p->title);
		if (p->body && *p->body)
			paragraph(root, p->body, FONT_TEXT, COLOR_BODY);
		if (p->info && *p->info)
			paragraph(root, p->info, FONT_SMALL, COLOR_MUTED);
		footer_set(p->footer);
	}
	render();
}

void ui_screen(const struct ui_context *ui, const char *title, const char *body,
	const char *footer_keys)
{
	(void)ui;
	if (display && !same_frame(hash_text(hash_text(hash_text(hash_int(14695981039346656037ULL, 1),
		title), body), footer_keys))) {
		lv_obj_t *root = page(title);
		paragraph(root, body, FONT_TEXT, COLOR_BODY);
		footer_set(footer_keys);
	}
	render();
}

void ui_menu(const struct ui_context *ui, const char *title, const char *body,
	const char *const items[], int item_count, int selected,
	const char *footer_keys)
{
	ui_menu_marked(ui, &(struct ui_menu){.title = title, .body = body, .items = items, .count = item_count,
		.selected = selected, .marked = -1, .footer = footer_keys});
}

void ui_menu_marked(const struct ui_context *ui, const struct ui_menu *m)
{
	/* The current one is named in the text, colors stay for the selection and for problems. */
	char current[256];
	struct ui_menu table = {.title = m->title, .body = m->body, .items = m->items, .count = m->count,
		.selected = m->selected, .marked = -1, .footer = m->footer};
	const char **named = m->marked >= 0 && m->marked < m->count ? malloc((size_t)m->count * sizeof(*named)) : NULL;
	if (!named) {
		ui_menu_table(ui, &table);
		return;
	}
	memcpy(named, m->items, (size_t)m->count * sizeof(*named));
	snprintf(current, sizeof(current), _("%s (current)"), m->items[m->marked]);
	named[m->marked] = current;
	table.items = named;
	ui_menu_table(ui, &table);
	free(named);
}

/* The items of a menu, see ui_menu_table. */
struct list_items {
	const char *header_text;
	const char *const *items;
	int count;
	int selected;
	const char *marks;
	int marked;
	const char *align;
};

/* The height of every item, a long last column takes more lines. */
static void list_heights(const lv_obj_t *area, const struct list_items *l, const int widths[MAX_COLUMNS], int height,
	int *heights)
{
	const lv_font_t *font_used = font(FONT_TEXT);
	for (int i = 0; i < l->count; ++i) {
		int last = lv_obj_get_content_width(area) - px(22) - px(48);
		const char *text = l->items[i] ? l->items[i] : "";
		const char *tab = strchr(text, '\t');
		int k = 0;
		lv_point_t size;
		while (tab && k < MAX_COLUMNS - 1) {
			last -= widths[k++] + px(36);
			text = tab + 1;
			tab = strchr(text, '\t');
		}
		lv_text_get_size(&size, text, font_used, 0, 0, last > px(100) ? last : px(100), LV_TEXT_FLAG_NONE);
		heights[i] = size.y + px(36) > height ? size.y + px(36) : height;
	}
}

/* The page with the selected item, pages filled from the top. */
static void list_page(int space, const int *heights, int count, int selected, int gap, int *first, int *visible)
{
	int start = 0;
	while (start < count) {
		int used = 0;
		int end = start;
		while (end < count && (end == start || used + heights[end] + gap <= space)) {
			used += heights[end] + gap;
			end++;
		}
		if (selected < end || end >= count) {
			*first = start;
			*visible = end - start;
			return;
		}
		start = end;
	}
}

/* Item i of the page. */
static void list_row(lv_obj_t *rows, const struct list_items *l, int i, const int widths[MAX_COLUMNS], int height)
{
	int is_marked = l->marks ? l->marks[i] == 1 : i == l->marked;
	uint32_t color = COLOR_TEXT;
	lv_obj_t *entry = row(rows, px(36));
	lv_obj_t *last;
	if (is_marked)
		color = COLOR_WARN;
	else if (l->marks && l->marks[i] == 2)
		color = COLOR_DIM;
	lv_obj_set_height(entry, height);
	lv_obj_set_style_radius(entry, px(12), 0);
	lv_obj_set_style_pad_hor(entry, px(24), 0);
	fill(entry, COLOR_SURFACE);
	if (i == l->selected) {
		fill(entry, is_marked ? COLOR_WARN_GROUND : COLOR_FOCUS);
		if (is_marked) {
			lv_obj_set_style_border_color(entry, lv_color_hex(COLOR_WARN), 0);
			lv_obj_set_style_border_width(entry, 2, 0);
		}
		color = 0xffffff;
	}
	columns(entry, l->items[i] ? l->items[i] : "", widths, l->align, FONT_TEXT, color);
	last = lv_obj_get_child(entry, -1);  /* Wrapped, see heights. */
	lv_label_set_long_mode(last, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_height(last, LV_SIZE_CONTENT);
}

/* Page by page like the lists of enigma2, a scrollbar shows the place. */
static void list(lv_obj_t *root, const struct list_items *l)
{
	const lv_font_t *font_used = font(FONT_TEXT);
	int widths[MAX_COLUMNS] = {0};
	int height = px(64);
	int gap = px(8);
	int visible = l->count;
	int first = 0;
	int *heights;
	lv_obj_t *area;
	lv_obj_t *rows;
	if (l->header_text)
		measure_columns(l->header_text, widths, font_used);
	for (int i = 0; i < l->count; ++i)  /* All items, so the columns do not jump between pages. */
		measure_columns(l->items[i], widths, font_used);
	heights = malloc((size_t)(l->count > 0 ? l->count : 1) * sizeof(*heights));
	if (l->header_text) {
		lv_obj_t *line = row(root, px(36));
		lv_obj_set_style_pad_hor(line, px(24), 0);
		columns(line, l->header_text, widths, l->align, FONT_TEXT, COLOR_MUTED);
	}
	area = filler(root);
	lv_obj_set_flex_flow(area, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(area, px(16), 0);
	lv_obj_update_layout(area);
	if (heights)
		list_heights(area, l, widths, height, heights);
	if (!modal && heights)
		list_page(lv_obj_get_content_height(area) + gap, heights, l->count, l->selected, gap, &first, &visible);
	if (first < 0 || first >= l->count)  /* list_page keeps it within the items. */
		first = 0;
	menu_rows = visible > 0 ? visible : 1;
	rows = column(area, gap);
	lv_obj_set_width(rows, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(rows, 1);
	for (int i = first; i < l->count && i < first + visible; ++i)
		list_row(rows, l, i, widths, heights ? heights[i] : height);
	if (visible < l->count) {
		lv_obj_update_layout(rows);
		scrollbar(area, lv_obj_get_height(rows), l->count, first, visible);
	}
	free(heights);
}

static void menu_table(const char *title, const char *body, const struct list_items *l, const char *footer_keys)
{
	uint64_t hash = hash_int(14695981039346656037ULL, 2);
	hash = hash_text(hash_text(hash_text(hash_text(hash, title), body), l->header_text), footer_keys);
	hash = hash_text(hash_int(hash_int(hash_int(hash, l->count), l->selected), l->marked), l->align);
	for (int i = 0; i < l->count; ++i)
		hash = hash_int(hash_text(hash, l->items[i]), l->marks ? l->marks[i] : 0);
	if (display && !same_frame(hash)) {
		lv_obj_t *root = page(title);
		if (body && *body)
			paragraph(root, body, FONT_TEXT, COLOR_BODY);
		footer_set(footer_keys);
		list(root, l);
	}
	render();
}

void ui_menu_table(const struct ui_context *ui, const struct ui_menu *m)
{
	const struct list_items l = {m->header, m->items, m->count, m->selected, m->marks, m->marked, m->align};
	(void)ui;
	menu_table(m->title, m->body, &l, m->footer);
}

int ui_menu_rows(void)
{
	return menu_rows;
}

size_t ui_text_fit(const char *text)
{
	const lv_font_t *font_used = font(FONT_LOG);
	size_t length = strlen(text);
	size_t low = 0;
	size_t high;
	int width;
	if (!display)
		return length;
	lv_obj_update_layout(detail);
	/* The padding and gap of the view and its scrollbar, see ui_text. */
	width = lv_obj_get_content_width(detail) - px(40) - px(16) - px(6);
	if (text_width(text, length, font_used) <= width)
		return length;
	high = length < 511 ? length : 511;
	while (low < high) {  /* The longest start that fits. */
		size_t middle = (low + high + 1) / 2;
		while (middle > low && (text[middle] & 0xc0) == 0x80)  /* Not inside a character. */
			middle--;
		if (middle == low)
			break;
		if (text_width(text, middle, font_used) <= width)
			low = middle;
		else
			high = middle - 1;
	}
	return low ? low : 1;
}

void ui_progress(const struct ui_context *ui, const char *title, const char *body,
	int percent, const char *detail_text, const char *footer_keys)
{
	(void)ui;
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	if (display && !same_frame(hash_text(hash_text(hash_int(hash_text(hash_text(
		hash_int(14695981039346656037ULL, 3), title), body), percent), detail_text), footer_keys))) {
		lv_obj_t *root = page(title);
		lv_obj_t *bar;
		lv_obj_t *done;
		paragraph(root, body, FONT_TEXT, COLOR_BODY);
		bar = box(root);
		lv_obj_set_size(bar, LV_PCT(100), px(20));
		lv_obj_set_style_radius(bar, px(10), 0);
		fill(bar, COLOR_SURFACE);
		done = box(bar);
		lv_obj_set_size(done, LV_PCT(percent), LV_PCT(100));
		lv_obj_set_style_radius(done, px(10), 0);
		fill(done, COLOR_FOCUS);
		if (detail_text && *detail_text)
			paragraph(root, detail_text, FONT_TEXT, COLOR_MUTED);
		footer_set(footer_keys);
	}
	render();
}

void ui_error(const struct ui_context *ui, const char *title, const char *message)
{
	(void)ui;
	if (display && !same_frame(hash_text(hash_text(hash_int(14695981039346656037ULL, 4), title), message))) {
		char footer_keys[64];
		lv_obj_t *root = page(title ? title : _("Error"));
		card(root, _("ERROR"), message ? message : "", CARD_ERROR, 0);
		snprintf(footer_keys, sizeof(footer_keys), "OK: %s", _("Back"));
		footer_set(footer_keys);
	}
	render();
}

/* The lines of ui_text in a sunken view, returns the rows that fit. */
static int text_view(const char *title, const char *header_text, const char *const lines[], int count, int first,
	const char *align, const char *footer_keys)
{
	const lv_font_t *font_used = font(FONT_LOG);
	int widths[MAX_COLUMNS] = {0};
	int line_height = lv_font_get_line_height(font_used);
	int gap = px(6);
	int height;
	int rows;
	lv_obj_t *root = page(title);
	lv_obj_t *view = filler(root);
	lv_obj_t *text;
	footer_set(footer_keys);
	fill(view, COLOR_SUNKEN);
	lv_obj_set_style_radius(view, px(12), 0);
	lv_obj_set_style_pad_all(view, px(20), 0);
	lv_obj_set_flex_flow(view, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(view, px(16), 0);
	lv_obj_update_layout(view);
	height = lv_obj_get_content_height(view);
	rows = (height + gap) / (line_height + gap) - (header_text ? 1 : 0);
	if (rows < 1)
		rows = 1;
	if (header_text)
		measure_columns(header_text, widths, font_used);
	for (int i = 0; i < count; ++i)  /* All lines, so the columns do not jump when scrolling. */
		if (lines[i] && strchr(lines[i], '\t'))
			measure_columns(lines[i], widths, font_used);
	text = column(view, gap);
	lv_obj_set_width(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	if (header_text)
		columns(row(text, px(36)), header_text, widths, align, FONT_LOG, COLOR_MUTED);
	for (int i = 0; i < rows && first + i < count; ++i)
		columns(row(text, px(36)), lines[first + i] ? lines[first + i] : "", widths, align, FONT_LOG,
			COLOR_BODY);
	if (count > rows)
		scrollbar(view, height, count, first, rows);
	return rows;
}

int ui_text(const struct ui_context *ui, const struct ui_text_page *p)
{
	static int last_rows = 1;
	uint64_t hash = hash_int(14695981039346656037ULL, 5);
	(void)ui;
	hash = hash_text(hash_text(hash_text(hash_text(hash, p->title), p->header), p->align), p->footer);
	hash = hash_int(hash_int(hash, p->count), p->first);
	for (int i = p->first; i < p->count && i < p->first + 200; ++i)  /* More rows never fit. */
		hash = hash_text(hash, p->lines[i]);
	if (display && !same_frame(hash))
		last_rows = text_view(p->title, p->header, p->lines, p->count, p->first, p->align, p->footer);
	render();
	return last_rows;
}

static void free_canvas(lv_event_t *event)
{
	lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(lv_event_get_target(event));
	if (buffer)
		lv_draw_buf_destroy(buffer);
}

/* The QR code with a quiet zone of 4 modules, in black on white. */
static lv_obj_t *qr_canvas(lv_obj_t *parent, const uint8_t *qrcode, int side)
{
	int size = qrcodegen_getSize(qrcode);
	int module = side / (size + 8);
	lv_draw_buf_t *buffer;
	lv_obj_t *canvas;
	uint8_t *data;
	if (module < 1)
		module = 1;
	side = module * (size + 8);
	buffer = lv_draw_buf_create((uint32_t)side, (uint32_t)side, LV_COLOR_FORMAT_I1, LV_STRIDE_AUTO);
	if (!buffer)
		return NULL;
	canvas = lv_canvas_create(parent);
	lv_canvas_set_draw_buf(canvas, buffer);
	lv_obj_add_event_cb(canvas, free_canvas, LV_EVENT_DELETE, NULL);
	lv_canvas_set_palette(canvas, 0, lv_color32_make(255, 255, 255, 255));
	lv_canvas_set_palette(canvas, 1, lv_color32_make(0, 0, 0, 255));
	data = buffer->data + 8;  /* After the palette. */
	memset(data, 0, (size_t)buffer->header.stride * (size_t)side);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x)
			if (qrcodegen_getModule(qrcode, x / module - 4, y / module - 4))
				data[(size_t)y * buffer->header.stride + (size_t)x / 8] |= (uint8_t)(0x80 >> (x % 8));
	return canvas;
}

void ui_remote_session(const struct ui_context *ui, const struct ui_remote_page *p)
{
	uint64_t hash = hash_int(14695981039346656037ULL, 6);
	(void)ui;
	hash = hash_text(hash_text(hash_text(hash_text(hash_text(hash_text(hash, p->title), p->body), p->link),
		p->warning), p->qr_hint), p->footer);
	if (p->qrcode) {
		int size = qrcodegen_getSize(p->qrcode);
		for (int module = 0; module < size * size; ++module)
			hash = hash_int(hash, qrcodegen_getModule(p->qrcode, module % size, module / size));
	}
	if (display && !same_frame(hash)) {
		lv_obj_t *root = page(p->title);
		lv_obj_t *line = row(root, px(48));
		lv_obj_t *text;
		lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		if (p->qrcode) {
			lv_obj_t *left = column(line, px(16));
			const lv_obj_t *canvas;
			lv_obj_set_width(left, LV_SIZE_CONTENT);
			lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			canvas = qr_canvas(left, p->qrcode, px(400));
			if (canvas && p->qr_hint && *p->qr_hint) {
				lv_obj_t *hint = paragraph(left, p->qr_hint, FONT_SMALL, COLOR_MUTED);
				lv_obj_update_layout(canvas);
				lv_obj_set_width(hint, lv_obj_get_width(canvas));
				lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
			}
		}
		text = column(line, px(20));
		lv_obj_set_width(text, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(text, 1);
		paragraph(text, p->body, FONT_TEXT, COLOR_BODY);
		if (p->link && *p->link)
			paragraph(text, p->link, FONT_TEXT, COLOR_LINK);
		if (p->warning && *p->warning)
			paragraph(text, p->warning, FONT_TEXT, COLOR_RED);
		footer_set(p->footer);
	}
	render();
}

/* Everything around the window of the other program, again and again: it clears the whole
 * screen when it starts. */
void ui_embed(const struct ui_context *ui, const char *title, const char *footer_keys, int width, int height)
{
	lv_area_t strips[4];
	int x1 = (screen_width - width) / 2;
	int y1 = (screen_height - height) / 2;
	int x2 = x1 + width - 1;
	int y2 = y1 + height - 1;
	(void)ui;
	if (!display || !active || !active->screen)
		return;
	if (!same_frame(hash_int(hash_text(hash_text(hash_int(14695981039346656037ULL, 7), title), footer_keys),
		width * 10000L + height))) {
		page(title);
		footer_set(footer_keys);
		render();  /* Before the window is there. */
		return;
	}
	strips[0] = (lv_area_t){0, 0, screen_width - 1, y1 - 1};
	strips[1] = (lv_area_t){0, y2 + 1, screen_width - 1, screen_height - 1};
	strips[2] = (lv_area_t){0, y1, x1 - 1, y2};
	strips[3] = (lv_area_t){x2 + 1, y1, screen_width - 1, y2};
	for (int i = 0; i < 4; ++i)
		if (strips[i].x2 >= strips[i].x1 && strips[i].y2 >= strips[i].y1)
			lv_obj_invalidate_area(lv_screen_active(), &strips[i]);
	render();
}

void ui_clock(const struct ui_context *ui)
{
	(void)ui;
	if (display && active && active->screen && header_update())
		lv_refr_now(display);
}

void ui_overlay(const struct ui_context *ui, int on)
{
	(void)ui;
	if (!display)
		return;
	if (on && !modal) {
		below_hash = frame_hash;
		frame_hash = 0;
		snprintf(below_footer, sizeof(below_footer), "%s", footer_text);
		modal = column(detail, 0);
		lv_obj_set_floating(modal, true);
		lv_obj_set_width(modal, LV_PCT(86));
		lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);
		fill(modal, COLOR_DIALOG);
		lv_obj_set_style_border_color(modal, lv_color_hex(COLOR_FOCUS), 0);
		lv_obj_set_style_border_width(modal, 3, 0);
		lv_obj_set_style_radius(modal, px(16), 0);
		lv_obj_set_style_pad_all(modal, px(40), 0);
	} else if (!on && modal) {
		lv_obj_delete(modal);
		modal = NULL;
		frame_hash = below_hash;
		footer_set(below_footer);
		render();
	}
}

void ui_redraw(const struct ui_context *ui)
{
	(void)ui;
	if (!display)
		return;
	lv_obj_invalidate(lv_screen_active());
	render();
}

void ui_keys(char *footer_keys, size_t size, const struct ui_key_names *keys)
{
	const char *const names[] = {"ARROWS", keys->digit_keys, "OK", "RED", "GREEN", "YELLOW", "BLUE", "BACK"};
	const char *const texts[] = {keys->arrows, keys->digits, keys->ok, keys->red, keys->green, keys->yellow,
		keys->blue, keys->back};
	size_t used = 0;
	if (!size)
		return;
	footer_keys[0] = '\0';
	for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i)
		if (texts[i] && names[i] && used < size)
			used += (size_t)snprintf(footer_keys + used, size - used, "%s%s: %s",
				used ? "   " : "", names[i], texts[i]);
}
