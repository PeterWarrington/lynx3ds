/*
 * Implementation of the minimal S-Lang shim declared in include/slang.h.
 *
 * Rendering: we keep our own virtual screen buffer (chars + color index per
 * cell); SLsmg_* calls just mutate that buffer. SLsmg_refresh() (repaint())
 * is the only point where we actually paint the top screen, via our own
 * proportional Open Sans pixel renderer (font_render.c) drawing straight
 * to the raw framebuffer -- not libctru's console, which only supports a
 * fixed 8x8 monospace font. See LICENSE-OPENSANS.md for the font's license.
 * The bottom screen is now bottom_ui.c's citro2d-rendered background +
 * fading controls guide, not a console at all; boot/debug logging still
 * goes to stdout (visible over 3dslink) but no longer to an on-screen
 * console.
 *
 * Input: SLang_getkey() blocks on 3DS hardware input (D-Pad/Circle-Pad/
 * buttons) and feeds Lynx standard xterm-style escape sequences (arrows,
 * the ~-terminated VT keys) through an internal byte queue, so Lynx's
 * existing escape-sequence parser in LYStrings.c (the non-USE_KEYMAPS path)
 * drives navigation unmodified. SELECT opens the software keyboard and
 * queues "g" + typed text + Enter, driving Lynx's own goto-URL prompt.
 */
#include <slang.h>
#include <font_render.h>

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

int SLtt_Screen_Rows = 30;
int SLtt_Screen_Cols = 50;
int SLtt_Use_Ansi_Colors = 1;
int SLtt_Blink_Mode = 0;
int SLsmg_Display_Eight_Bit = 160;
int SLsmg_Backspace_Moves = 1;
int SLsmg_Newline_Behavior = SLSMG_NEWLINE_MOVES;
int SLang_Error = 0;

#define MAX_ROWS 60
#define MAX_COLS 132

static char vch[MAX_ROWS][MAX_COLS];
static unsigned char vattr[MAX_ROWS][MAX_COLS];
static int cur_r = 0, cur_c = 0;
static int cur_color = 0;
static bool g_console_ready = false;

/* The 3DS top/bottom screens are double-buffered: nothing drawn/printed
 * ever reaches the display until the buffers are flushed *and* swapped.
 * Without this, the screen just stays on whatever was there before (e.g.
 * blank), which looks exactly like a crash/hang even if the app is
 * running fine. A single call presents both screens. */
void lynx3ds_present_frame(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

/* No-op now that the bottom screen is bottom_ui.c's citro2d rendering
 * rather than a libctru console -- kept so platform_3ds.c/posix_compat_3ds.c
 * don't need to special-case whether a console selection is meaningful
 * before printing a debug/diagnostic line (those still go to plain
 * stdout, visible over 3dslink, just not drawn on-screen anymore). */
void lynx3ds_select_bottom(void)
{
}

/* The top screen (Lynx's actual UI) is rendered entirely by our own
 * proportional-font pixel renderer (font_render.c), not libctru's fixed
 * 8x8 monospace console -- so it needs RGB565 framebuffer format (what
 * the renderer's alpha-blend math assumes) instead of the libctru default,
 * and no PrintConsole/consoleInit at all. TOP_COLS is an estimate (average
 * Open Sans Bold glyph width is ~5.3px, after left-bearing correction)
 * since a proportional font has no single true column count; Lynx just
 * needs *a* number for its own line-wrapping layout decisions. */
#define TOP_ROWS (FONT_SCREEN_H / FONT_LINE_H)
#define TOP_COLS (76 / FONT_SCALE)

static void ensure_console(void)
{
    if (g_console_ready)
	return;
    printf("lynx3ds: ensure_console: setting up top screen renderer...\n");
    fflush(stdout);

    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    font_render_init();
    g_console_ready = true;

    printf("lynx3ds: ensure_console: top screen ready, %dx%d (Open Sans Bold font)\n",
	   TOP_COLS, TOP_ROWS);
    fflush(stdout);

    SLtt_Screen_Rows = TOP_ROWS;
    SLtt_Screen_Cols = TOP_COLS;
    if (SLtt_Screen_Rows > MAX_ROWS)
	SLtt_Screen_Rows = MAX_ROWS;
    if (SLtt_Screen_Cols > MAX_COLS)
	SLtt_Screen_Cols = MAX_COLS;
    memset(vch, ' ', sizeof(vch));
    memset(vattr, 0, sizeof(vattr));
}

/* Callable early (e.g. from platform_3ds.c's constructor, right after
 * gfxInitDefault) so on-screen boot logging works from the very first
 * line, not just once Lynx itself first touches the screen driver. */
void lynx3ds_console_init(void)
{
    ensure_console();
}

/* ---- color handling ---- */

/*
 * Real S-Lang keeps a separate mono and color representation per object
 * and picks one based on terminal capability -- it does NOT mean the two
 * calls overwrite each other. Lynx's lynx_setup_colors() calls
 * SLtt_set_color() for objects 0-7 (real colors) and then, in the same
 * function, lynx_setup_attrs() calls SLtt_set_mono() for objects 1-7
 * (bold/reverse/underline bits). Since we *do* have color, has_color must
 * stay true after set_mono runs too, or every colored object silently
 * degrades to monochrome-only rendering.
 */
struct color_obj {
    char fg[16];
    char bg[16];
    int has_color;
    SLtt_Char_Type mono_attr;
};

static struct color_obj colors[16];

#define DEFAULT_FG565 0xef7d
#define DEFAULT_BG565 0x2104	

/*
 * Looks up a named color; returns 0 (and leaves *out untouched) for an
 * empty name or the literal "default" -- the caller keeps its own
 * fg/bg-appropriate default in that case. This matters: a single shared
 * fallback constant here would resolve Lynx's "default" background name
 * to whatever that constant is regardless of whether it's being used as
 * fg or bg, which is exactly how a black full-screen clear ended up
 * showing through gaps in an otherwise white-backgrounded page.
 */
static int color_name_lookup565(const char *name, unsigned short *out)
{
    if (!name || !name[0] || !strcasecmp(name, "default"))
	return 0;
    if (!strcasecmp(name, "black") || !strcasecmp(name, "gray") || !strcasecmp(name, "grey"))
	*out = DEFAULT_FG565;
    else if (!strcasecmp(name, "red"))
	*out = 0xf227;
    else if (!strcasecmp(name, "green"))
	*out = 0x04d6a;
    else if (!strcasecmp(name, "yellow") || !strcasecmp(name, "brown"))
	*out = 0xfde5;
    else if (!strcasecmp(name, "blue"))
	*out = 0x1b37;		/* lighter blue -- pure dark blue (0x001F) reads
				 * poorly against the dark-mode black background,
				 * and Lynx uses "blue" for bold/unvisited links */
    else if (!strcasecmp(name, "magenta"))
	*out = 0x69d6;
    else if (!strcasecmp(name, "cyan"))
	*out = 0x04b1;
    else if (!strcasecmp(name, "white"))
	*out = DEFAULT_BG565;
    else
	return 0;
    return 1;
}

/* Resolve a color-object index into concrete fg/bg RGB565. Bold/underline
 * are handled separately by the caller (direct access to colors[]).
 *
 * The reverse-video attribute bit is only applied as an fg/bg swap when
 * the object has *no* explicit color (i.e. we're falling back to plain
 * monochrome rendering) -- when Lynx has set real colors for an object
 * (e.g. object 7, "magenta on cyan" for bold+reverse+underline target-link
 * emphasis -- see LYstartTargetEmphasis/lynx_setup_colors), that color
 * already *is* the intended "reversed" look. Swapping it again on top
 * doesn't invert it back to normal; it corrupts a color pair Lynx chose
 * deliberately (this was actually what produced black-on-black rendering
 * for the currently-selected link).
 */
static void get_fg_bg565(int obj, unsigned short *fg, unsigned short *bg)
{
    unsigned short f = DEFAULT_FG565, b = DEFAULT_BG565;

    if (obj >= 0 && obj < 16) {
	struct color_obj *c = &colors[obj];
	unsigned short v;
	int has_fg = 0, has_bg = 0;

	if (c->has_color) {
	    has_fg = color_name_lookup565(c->fg, &v);
	    if (has_fg)
		f = v;
	    has_bg = color_name_lookup565(c->bg, &v);
	    if (has_bg)
		b = v;
	}
	if ((c->mono_attr & SLTT_REV_MASK) && !(has_fg || has_bg)) {
	    unsigned short t = f;

	    f = b;
	    b = t;
	}
    }
    /* Safety net: whatever combination of the above produced this, never
     * let foreground and background end up identical (invisible text). */
    if (f == b)
	b = (unsigned short) ~f;
    *fg = f;
    *bg = b;
}

void SLtt_set_color(int obj, char *name, char *fg, char *bg)
{
    (void) name;
    if (obj < 0 || obj >= 16)
	return;
    colors[obj].has_color = 1;
    snprintf(colors[obj].fg, sizeof(colors[obj].fg), "%s", fg ? fg : "");
    snprintf(colors[obj].bg, sizeof(colors[obj].bg), "%s", bg ? bg : "");
}

void SLtt_set_mono(int obj, char *name, SLtt_Char_Type attr)
{
    (void) name;
    if (obj < 0 || obj >= 16)
	return;
    /* Layer bold/reverse/underline on top of whatever color is (or isn't)
     * already set -- does not affect has_color. */
    colors[obj].mono_attr = attr;
}

void SLtt_add_color_attribute(int obj, int attr)
{
    if (obj < 0 || obj >= 16)
	return;
    colors[obj].mono_attr |= attr;
}

void SLtt_get_terminfo(void)
{
    ensure_console();
}

void SLtt_get_screen_size(void)
{
    ensure_console();
}

void SLtt_set_mouse_mode(int mode, int force)
{
    (void) mode;
    (void) force;
}

void SLtt_flush_output(void)
{
}

void SLtty_set_suspend_state(int state)
{
    (void) state;
}


/* ---- SLang tty layer (no real tty on 3DS, just satisfy the API) ---- */

int SLang_init_tty(int abort_char, int flow_ctrl, int opost)
{
    (void) abort_char;
    (void) flow_ctrl;
    (void) opost;
    ensure_console();
    return 0;
}

int SLang_reset_tty(void)
{
    return 0;
}

void SLang_exit_error(const char *fmt, ...)
{
    va_list ap;

    ensure_console();
    /* The top screen has no libctru console anymore (see ensure_console) --
     * a rare fatal-error message is simplest and most reliable printed to
     * the bottom screen's ordinary console rather than routed through the
     * pixel renderer's row/col buffer. */
    lynx3ds_select_bottom();
    printf("\x1b[2J\x1b[0;0Hlynx3ds: fatal: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    lynx3ds_present_frame();
    exit(1);
}

int SLang_get_error(void)
{
    return SLang_Error;
}

/* ---- input queue ---- */

#define KEYQ_SIZE 64
static int keyq[KEYQ_SIZE];
static int keyq_head = 0, keyq_tail = 0;

static void keyq_push(int c)
{
    int next = (keyq_tail + 1) % KEYQ_SIZE;

    if (next == keyq_head)
	return;		/* full, drop */
    keyq[keyq_tail] = c;
    keyq_tail = next;
}

static void keyq_push_str(const char *s)
{
    while (*s)
	keyq_push((unsigned char) *s++);
}

static int keyq_pop(void)
{
    int c;

    if (keyq_head == keyq_tail)
	return -1;
    c = keyq[keyq_head];
    keyq_head = (keyq_head + 1) % KEYQ_SIZE;
    return c;
}

static int keyq_empty(void)
{
    return keyq_head == keyq_tail;
}

/*
 * Pops up the software keyboard and feeds the typed string back in as
 * individual keystrokes, exactly as if a real keyboard had typed them,
 * optionally preceded by a fixed prefix (e.g. "g" for Lynx's goto-URL
 * keybinding).
 *
 * Called two ways:
 *  - SELECT (slang_shim's own poll_hardware_input) passes prefix="g", so
 *    SELECT is a direct, one-press "go to URL" action.
 *  - Lynx itself (LYForms.c:form_getstr(), see the __3DS__ hook there)
 *    passes prefix=NULL right when a form text field is activated, so the
 *    keyboard pops up automatically to fill *that* field -- injecting "g"
 *    there would just be a stray literal character typed into the field.
 */
void lynx3ds_trigger_swkbd(const char *prefix)
{
    static SwkbdState swkbd;
    static char buf[512];

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, "Enter text");
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Go", true);
    SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));

    if (button == SWKBD_BUTTON_RIGHT || button == SWKBD_BUTTON_CONFIRM) {
	if (prefix)
	    keyq_push_str(prefix);
	keyq_push_str(buf);
	keyq_push('\r');
    }
    ensure_console();
}

/* Keys poll_hardware_input() actually turns into queued bytes below --
 * shared with SLang_input_pending() so it doesn't report input as pending
 * for buttons/touch that poll_hardware_input will just ignore and keep
 * waiting on (which would make HTCheckForInterrupt()'s "just peek, don't
 * block" check fall through to a real blocking SLang_getkey() call, e.g.
 * hanging network progress the instant a finger rests on the Circle Pad
 * or touchscreen). */
#define HANDLED_KEYS (KEY_DUP | KEY_DDOWN | KEY_DRIGHT | KEY_DLEFT | \
		      KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_RIGHT | KEY_CPAD_LEFT | \
		      KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT)

/* Auto-repeat for holding Up/Down (D-Pad or Circle Pad) down, so scrolling
 * through a page doesn't need repeated individual presses. Timings are in
 * poll iterations, which pace to vblank (~60Hz) whenever nothing else is
 * happening -- see the "idle" branch below. */
#define REPEAT_INITIAL_DELAY 18	/* ~0.3s held before auto-repeat kicks in */
#define REPEAT_INTERVAL 5	/* ~0.08s between repeats once it starts */

#define TOUCH_SCROLL_STEP 6	/* px of drag per queued PGUP/PGDOWN */

/* Poll 3DS hardware input until we have something to deliver, translating
 * it into the byte(s) Lynx's escape-sequence parser understands. */
static void poll_hardware_input(void)
{
    static int hold_up = 0, hold_down = 0;
    static int touch_active = 0, touch_last_y = 0, touch_accum = 0;

    while (keyq_empty()) {
	if (!aptMainLoop()) {
	    keyq_push('Q');	/* quit without confirmation */
	    return;
	}
	hidScanInput();
	u32 kDown = hidKeysDown();
	u32 kHeld = hidKeysHeld();

	if (kHeld & (KEY_DUP | KEY_CPAD_UP))
	    hold_up++;
	else
	    hold_up = 0;
	if (kHeld & (KEY_DDOWN | KEY_CPAD_DOWN))
	    hold_down++;
	else
	    hold_down = 0;

	if ((kHeld & KEY_START) && (kHeld & KEY_SELECT)) {
	    lynx3ds_select_bottom();
	    printf("\n*** force quit (START+SELECT) ***\n");
	    fflush(stdout);
	    lynx3ds_present_frame();
	    _exit(0);		/* bypass our own exit() override -- this quit is real, not a bug to freeze-and-debug */
	}

	/*
	 * Touch-drag scrolling: reuses the exact same PGUP/PGDOWN escape
	 * sequences as L/R, which lynx.cfg's KEYMAP:PGUP:UP_TWO /
	 * KEYMAP:PGDOWN:DOWN_TWO already remaps to Lynx's small 2-line
	 * scroll actions rather than a full-page jump -- so dragging just
	 * has to keep sending these as the finger moves, and Lynx's own
	 * remapping gives the line-by-line feel. "Natural"/content-follows-
	 * finger direction: dragging up reveals what's below (PGDOWN),
	 * dragging down reveals what's above (PGUP).
	 */
	if (kHeld & KEY_TOUCH) {
	    touchPosition touch;

	    hidTouchRead(&touch);
	    if (!touch_active) {
		touch_active = 1;
		touch_accum = 0;
	    } else {
		touch_accum += (int) touch.py - touch_last_y;
		while (touch_accum >= TOUCH_SCROLL_STEP) {
		    keyq_push_str("\x1b[5~");	/* dragged down -> page up */
		    touch_accum -= TOUCH_SCROLL_STEP;
		}
		while (touch_accum <= -TOUCH_SCROLL_STEP) {
		    keyq_push_str("\x1b[6~");	/* dragged up -> page down */
		    touch_accum += TOUCH_SCROLL_STEP;
		}
	    }
	    touch_last_y = touch.py;
	    if (!keyq_empty())
		return;
	} else {
	    touch_active = 0;
	}

	if (kDown & KEY_DUP)
	    keyq_push_str("\x1b[A");
	else if (kDown & KEY_DDOWN)
	    keyq_push_str("\x1b[B");
	else if (kDown & KEY_DRIGHT)
	    keyq_push_str("\x1b[C");
	else if (kDown & KEY_DLEFT)
	    keyq_push_str("\x1b[D");
	else if (kDown & KEY_CPAD_UP)
	    keyq_push_str("\x1b[A");
	else if (kDown & KEY_CPAD_DOWN)
	    keyq_push_str("\x1b[B");
	else if (kDown & KEY_CPAD_RIGHT)
	    keyq_push_str("\x1b[C");
	else if (kDown & KEY_CPAD_LEFT)
	    keyq_push_str("\x1b[D");
	else if (kDown & KEY_A)
	    keyq_push('\r');
	else if (kDown & KEY_B)
	    keyq_push_str("\x1b[D");	/* back, same as left arrow */
	else if (kDown & KEY_L)
	    keyq_push_str("\x1b[5~");	/* page up */
	else if (kDown & KEY_R)
	    keyq_push_str("\x1b[6~");	/* page down */
	else if (kDown & KEY_SELECT)
	    lynx3ds_trigger_swkbd("g");	/* one-press "go to URL": opens Lynx's goto prompt and fills it in */
	else if (hold_up >= REPEAT_INITIAL_DELAY &&
		 (hold_up - REPEAT_INITIAL_DELAY) % REPEAT_INTERVAL == 0)
	    keyq_push_str("\x1b[A");
	else if (hold_down >= REPEAT_INITIAL_DELAY &&
		 (hold_down - REPEAT_INITIAL_DELAY) % REPEAT_INTERVAL == 0)
	    keyq_push_str("\x1b[B");
	else {
	    gspWaitForVBlank();
	    continue;
	}
	return;
    }
}

int SLang_getkey(void)
{
    ensure_console();
    poll_hardware_input();
    return keyq_pop();
}

int SLang_input_pending(int tsecs)
{
    (void) tsecs;
    if (!keyq_empty())
	return 1;
    hidScanInput();
    /* Must agree with what poll_hardware_input() will actually consume --
     * hidKeysHeld() is true for any touch/button (including ones
     * poll_hardware_input ignores), which made callers like
     * HTCheckForInterrupt() (used throughout network/file loading) think a
     * key was ready and fall through to a real, blocking SLang_getkey(),
     * stalling all progress until a *recognized* key was pressed. */
    return (hidKeysDown() & HANDLED_KEYS) ? 1 : 0;
}

void SLang_flush_input(void)
{
    keyq_head = keyq_tail = 0;
}

/* ---- keymap layer: near-stubs, real dispatch bypasses this (see the
 * __3DS__ carve-out for USE_KEYMAPS in LYCurses.h) ---- */

struct SLKeyMap_List_Type_s {
    int dummy;
};
static SLKeyMap_List_Type g_dummy_keymap;

SLKeyMap_List_Type *SLang_create_keymap(char *name, void *table)
{
    (void) name;
    (void) table;
    return &g_dummy_keymap;
}

void SLkm_define_keysym(char *string, unsigned code, SLKeyMap_List_Type *map)
{
    (void) string;
    (void) code;
    (void) map;
}

int SLang_undefine_key(char *keystr, SLKeyMap_List_Type *map)
{
    (void) keystr;
    (void) map;
    return 0;
}

int SLkp_init(void)
{
    return 0;
}

int SLkp_getkey(void)
{
    return SLang_getkey();
}

int SLexpand_escaped_string(char *out, char *first, char *last, int mode)
{
    (void) mode;
    int n = 0;

    while (first != last)
	out[n++] = *first++;
    out[n] = '\0';
    return n;
}

/* ---- screen management (SLsmg_*) ---- */

int SLsmg_init_smg(void)
{
    ensure_console();
    return 0;
}

void SLsmg_reset_smg(void)
{
    ensure_console();
    font_fill_rect(0, 0, FONT_SCREEN_W, FONT_SCREEN_H, DEFAULT_BG565);
    lynx3ds_present_frame();
}

static unsigned char cell_char(int r, int c)
{
    return vch[r][c] ? (unsigned char) vch[r][c] : ' ';
}

static void repaint(void)
{
    ensure_console();
    /* Clear the whole screen first: a proportional font doesn't necessarily
     * fill every pixel of the nominal COLS x ROWS grid (narrow characters,
     * or rows/margins beyond it), so per-cell background fills alone could
     * leave stale pixels at the edges. */
    font_fill_rect(0, 0, FONT_SCREEN_W, FONT_SCREEN_H, DEFAULT_BG565);

    for (int r = 0; r < SLtt_Screen_Rows; r++) {
	int px = 0;
	int py = r * FONT_LINE_H;

	if (py + FONT_LINE_H > FONT_SCREEN_H)
	    break;
	for (int c = 0; c < SLtt_Screen_Cols; c++) {
	    unsigned char ch = cell_char(r, c);
	    int col = vattr[r][c];
	    int wid = font_char_width(ch);
	    unsigned short fg, bg;

	    if (px + wid > FONT_SCREEN_W)
		break;
	    get_fg_bg565(col, &fg, &bg);
	    font_fill_rect(px, py, px + wid, py + FONT_LINE_H, bg);
	    font_draw_char(px, py, fg, ch);
	    /* We now render everything in Open Sans Bold (romfs/fonts/OpenSans-Bold/)
	     * rather than distinguishing bold text specially -- an earlier
	     * synthetic-bold trick (glyph drawn twice, offset 1px) looked muddy
	     * at this size, and per-attribute weight switching would need a
	     * second full glyph set kept in memory for little benefit here. */
	    if (col >= 0 && col < 16 && (colors[col].mono_attr & SLTT_ULINE_MASK))
		font_fill_rect(px, py + FONT_GLYPH_H, px + wid, py + FONT_GLYPH_H + 1, fg);
	    px += wid;
	}
    }

    /* Cursor: a small solid bar under the current cell. */
    if (cur_r >= 0 && cur_r < SLtt_Screen_Rows && cur_c >= 0 && cur_c < SLtt_Screen_Cols) {
	int cx = 0;
	int cy = cur_r * FONT_LINE_H;
	int cw;

	for (int c = 0; c < cur_c; c++)
	    cx += font_char_width(cell_char(cur_r, c));
	cw = font_char_width(cell_char(cur_r, cur_c));
	if (cw < 2)
	    cw = 2;
	font_fill_rect(cx, cy + FONT_GLYPH_H + 1, cx + cw, cy + FONT_LINE_H, DEFAULT_FG565);
    }

    lynx3ds_present_frame();
}

void SLsmg_suspend_smg(void)
{
}

void SLsmg_resume_smg(void)
{
    repaint();
}

void SLsmg_refresh(void)
{
    repaint();
}

void SLsmg_gotorc(int r, int c)
{
    if (r < 0)
	r = 0;
    if (c < 0)
	c = 0;
    if (r >= SLtt_Screen_Rows)
	r = SLtt_Screen_Rows - 1;
    if (c >= SLtt_Screen_Cols)
	c = SLtt_Screen_Cols - 1;
    cur_r = r;
    cur_c = c;
}

int SLsmg_get_row(void)
{
    return cur_r;
}

int SLsmg_get_column(void)
{
    return cur_c;
}

static void put_char_at_cursor(int ch)
{
    if (ch == '\n') {
	cur_r++;
	cur_c = 0;
	if (cur_r >= SLtt_Screen_Rows)
	    cur_r = SLtt_Screen_Rows - 1;
	return;
    }
    if (cur_r >= 0 && cur_r < SLtt_Screen_Rows && cur_c >= 0 && cur_c < SLtt_Screen_Cols) {
	vch[cur_r][cur_c] = (char) ch;
	vattr[cur_r][cur_c] = (unsigned char) cur_color;
	cur_c++;
    }
}

void SLsmg_write_char(int ch)
{
    put_char_at_cursor(ch);
}

void SLsmg_write_string(SLFUTURE_CONST char *s)
{
    while (*s)
	put_char_at_cursor((unsigned char) *s++);
}

void SLsmg_write_nstring(SLFUTURE_CONST char *s, unsigned n)
{
    unsigned i;

    for (i = 0; i < n && s[i]; i++)
	put_char_at_cursor((unsigned char) s[i]);
    for (; i < n; i++)
	put_char_at_cursor(' ');
}

int SLsmg_printf(SLFUTURE_CONST char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SLsmg_write_string(buf);
    return n;
}

void SLsmg_reverse_video(void)
{
    cur_color |= 2;
}

void SLsmg_normal_video(void)
{
    cur_color = 0;
}

void SLsmg_set_color(SLsmg_Color_Type color)
{
    cur_color = (int) color;
}

void SLsmg_erase_eol(void)
{
    int c;

    for (c = cur_c; c < SLtt_Screen_Cols; c++) {
	vch[cur_r][c] = ' ';
	vattr[cur_r][c] = (unsigned char) cur_color;
    }
}

void SLsmg_erase_eos(void)
{
    int r, c;

    for (c = cur_c; c < SLtt_Screen_Cols; c++) {
	vch[cur_r][c] = ' ';
	vattr[cur_r][c] = (unsigned char) cur_color;
    }
    for (r = cur_r + 1; r < SLtt_Screen_Rows; r++)
	for (c = 0; c < SLtt_Screen_Cols; c++) {
	    vch[r][c] = ' ';
	    vattr[r][c] = (unsigned char) cur_color;
	}
}

void SLsmg_fill_region(int r, int c, unsigned nr, unsigned nc, unsigned char ch)
{
    unsigned dr, dc;

    for (dr = 0; dr < nr; dr++) {
	int rr = r + (int) dr;

	if (rr < 0 || rr >= SLtt_Screen_Rows)
	    continue;
	for (dc = 0; dc < nc; dc++) {
	    int cc = c + (int) dc;

	    if (cc < 0 || cc >= SLtt_Screen_Cols)
		continue;
	    vch[rr][cc] = (char) ch;
	    vattr[rr][cc] = (unsigned char) cur_color;
	}
    }
}

void SLsmg_draw_box(int r, int c, unsigned nr, unsigned nc)
{
    if (nr == 0 || nc == 0)
	return;
    SLsmg_fill_region(r, c, 1, nc, '-');
    SLsmg_fill_region(r + (int) nr - 1, c, 1, nc, '-');
    SLsmg_fill_region(r, c, nr, 1, '|');
    SLsmg_fill_region(r, c + (int) nc - 1, nr, 1, '|');
}

void SLsmg_touch_lines(int r, int n)
{
    (void) r;
    (void) n;
}

void SLsmg_touch_screen(void)
{
}

void SLsmg_forward(int n)
{
    cur_c += n;
}
