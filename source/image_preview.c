/*
 * Implementation of the bottom-screen hover-preview declared in
 * image_preview.h.
 *
 * Threading: a single persistent worker thread polls
 * LY3DS_current_link_url() (the currently highlighted link, Lynx's closest
 * equivalent to mouse hover) a few times a second. When it points at a
 * PNG/JPEG, the worker fetches it with libcurl and decodes it with
 * libpng/libjpeg-turbo into a plain malloc'd RGBA8 buffer -- all pure CPU/
 * network work, no GPU calls, so it's safe to do off the thread that owns
 * the GPU command buffer. The actual C3D_Tex upload (which needs a
 * hardware GX transfer to get pixels into the GPU's tiled texture layout)
 * happens on bottom_ui's render thread instead, the same thread that
 * already owns every other GPU call in this app -- see upload_pending().
 *
 * Texture tiling: the 3DS GPU requires textures in an 8x8-tiled layout,
 * not plain row-major pixels. Rather than hand-writing that swizzle (easy
 * to get subtly wrong with no hardware here to check against), this uses
 * GX_DisplayTransfer(..., GX_TRANSFER_OUT_TILED(1), ...) -- the same
 * hardware DMA tiling path libctru itself uses internally -- to convert a
 * plain linear RGBA8 buffer straight into the C3D_Tex.
 */
#include <image_preview.h>
#include <bottom_ui.h>

#include <3ds.h>
#include <citro2d.h>
#include <curl/curl.h>
#include <png.h>
#include <jpeglib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

extern const char *LY3DS_current_link_url(void);	/* vendor/lynx/src/LYMainLoop.c */

#define BOTTOM_W 320
#define BOTTOM_H 240
#define BOX_W 300.0f
#define BOX_H 200.0f

#define WORKER_STACK_SIZE (64 * 1024)	/* curl+mbedtls+libpng/libjpeg call chains run deep */
#define WORKER_PRIORITY 0x31		/* slightly below bottom_ui's render thread (0x30) */
#define POLL_INTERVAL_NS (150ULL * 1000 * 1000)

#define MAX_FETCH_BYTES (6 * 1024 * 1024)	/* cap raw (compressed) download size */
#define MAX_IMAGE_DIM 512	/* cap decoded pixel dimensions -- also keeps the po2 texture small */

#define FADE_MS 350.0		/* guide<->image cross-fade duration */
#define INDICATOR_FADE_MS 200.0	/* loading indicator's own show/hide fade */
#define SPINNER_TICKS 8
#define SPINNER_PERIOD_MS 900.0
#define SPINNER_R_OUT 16.0f
#define SPINNER_R_IN 8.0f

typedef enum {
    PREVIEW_IDLE = 0,
    PREVIEW_LOADING,	/* worker fetching/decoding -- show guide + spinner */
    PREVIEW_FADING_IN,	/* texture just uploaded -- cross-fading guide out, image in */
    PREVIEW_READY,	/* fade complete -- image only */
    PREVIEW_FADING_OUT	/* un-hovered -- cross-fading image out, guide back in */
} preview_state_t;

static Thread g_thread;
static volatile int g_running = 0;
static volatile preview_state_t g_state = PREVIEW_IDLE;
static u64 g_start_ms;		/* for the spinner's animation clock */
static u64 g_fade_start_ms;	/* render-thread-owned: when the current guide<->image cross-fade began */

/* A simple "smoothly approach a target value over a duration" helper --
 * used for the loading indicator's own fade, which needs to keep animating
 * independently of (and sometimes overlapping) the guide<->image
 * cross-fade above, including reversing direction mid-fade without a pop
 * if the state changes again before it finishes. */
typedef struct {
    float from, to;
    u64 start_ms;
    double dur_ms;
} fade_t;

static float fade_eval(const fade_t *f)
{
    double elapsed;
    float t;

    if (f->to == f->from)
	return f->to;
    elapsed = (double) (osGetTime() - f->start_ms);
    t = (float) (elapsed / f->dur_ms);
    if (t <= 0.0f)
	return f->from;
    if (t >= 1.0f)
	return f->to;
    return f->from + (f->to - f->from) * t;
}

static void fade_to(fade_t *f, float target, double dur_ms)
{
    if (f->to == target)
	return;
    f->from = fade_eval(f);
    f->to = target;
    f->start_ms = osGetTime();
    f->dur_ms = dur_ms;
}

static fade_t g_indicator_fade = { 0.0f, 0.0f, 0, INDICATOR_FADE_MS };

/* Worker thread -> render thread handoff. Single writer (worker) / single
 * reader (render thread), same lock-free-flag style already used by
 * bookmarks.c and bottom_ui.c elsewhere in this project. */
static unsigned char *g_pending_pixels = NULL;	/* plain malloc'd, row-major RGBA8, w*h*4 bytes */
static int g_pending_w = 0, g_pending_h = 0;
static volatile int g_pending_ready = 0;
static volatile int g_pending_clear = 0;	/* hard/immediate clear (fetch or decode failed) */
static volatile int g_pending_fade_out = 0;	/* un-hovered -- fade out gracefully if something's showing */

/* Render-thread-owned GPU state -- touched only from image_preview_render_bottom(). */
static C3D_Tex g_tex;
static Tex3DS_SubTexture g_subtex;
static C2D_Image g_image;
static int g_tex_valid = 0;
static int g_img_w = 0, g_img_h = 0;

static C2D_TextBuf g_text_buf = NULL;

/* ---- URL policy: is this worth even trying to fetch? ---- */

static int url_is_image(const char *url)
{
    const char *end;
    const char *p;
    const char *ext = NULL;
    size_t extlen;

    if (!url || !url[0])
	return 0;
    end = url + strcspn(url, "?#");
    for (p = url; p < end; p++)
	if (*p == '.')
	    ext = p;
    if (!ext)
	return 0;
    extlen = (size_t) (end - ext);
    if (extlen == 4 && !strncasecmp(ext, ".png", 4))
	return 1;
    if (extlen == 4 && !strncasecmp(ext, ".jpg", 4))
	return 1;
    if (extlen == 5 && !strncasecmp(ext, ".jpeg", 5))
	return 1;
    return 0;
}

/* ---- content sniffing: trust magic bytes over the URL for *which*
 * decoder to use, in case a server mislabels something ---- */

static int is_png_data(const unsigned char *d, size_t n)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

    return n >= 8 && memcmp(d, sig, 8) == 0;
}

static int is_jpeg_data(const unsigned char *d, size_t n)
{
    return n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
}

/* ---- fetch ---- */

struct membuf {
    unsigned char *data;
    size_t len;
    size_t cap;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct membuf *mb = (struct membuf *) userdata;
    size_t n = size * nmemb;
    size_t newcap;

    if (mb->len + n > MAX_FETCH_BYTES)
	return 0;	/* abort transfer -- too large */
    if (mb->len + n > mb->cap) {
	newcap = mb->cap ? mb->cap * 2 : 65536;
	while (newcap < mb->len + n)
	    newcap *= 2;
	{
	    unsigned char *p = realloc(mb->data, newcap);

	    if (!p)
		return 0;
	    mb->data = p;
	    mb->cap = newcap;
	}
    }
    memcpy(mb->data + mb->len, ptr, n);
    mb->len += n;
    return n;
}

static int fetch_url(const char *url, unsigned char **out_data, size_t *out_len)
{
    CURL *curl;
    struct membuf mb;
    CURLcode res;

    memset(&mb, 0, sizeof(mb));
    curl = curl_easy_init();
    if (!curl)
	return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lynx3DS");
    /* No CA bundle on this port -- Lynx's own TLS shim (ssl_mbedtls_shim.c)
     * already runs with MBEDTLS_SSL_VERIFY_NONE for the same reason; match
     * that rather than introducing a stricter (and differently-broken)
     * policy just for image fetches. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || mb.len == 0) {
	free(mb.data);
	return 0;
    }
    *out_data = mb.data;
    *out_len = mb.len;
    return 1;
}

/* ---- decode ---- */

static unsigned char *decode_png(const unsigned char *data, size_t len, int *w, int *h)
{
    png_image image;
    unsigned char *buf;

    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, data, len))
	return NULL;

    /* Per 3dbrew's GPU color-format docs, non-24-bit PICA200 pixel formats
     * are little-endian, so "RGBA8" is actually stored in memory as A,B,G,R
     * byte order (alpha in the lowest byte) -- not a plain R/B swap.
     * Decoding straight to that order here avoids a separate channel-
     * shuffle pass before upload (see upload_pending()/GX_DisplayTransfer). */
    image.format = PNG_FORMAT_ABGR;
    buf = malloc(PNG_IMAGE_SIZE(image));
    if (!buf) {
	png_image_free(&image);
	return NULL;
    }
    if (!png_image_finish_read(&image, NULL, buf, 0, NULL)) {
	free(buf);
	png_image_free(&image);
	return NULL;
    }
    *w = (int) image.width;
    *h = (int) image.height;
    png_image_free(&image);
    return buf;
}

struct jpeg_error_ctx {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_error_ctx *err = (struct jpeg_error_ctx *) cinfo->err;

    longjmp(err->jmp, 1);
}

static unsigned char *decode_jpeg(const unsigned char *data, size_t len, int *w, int *h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_ctx jerr;
    unsigned char *buf = NULL;
    JSAMPROW rowptr[1];
    int row_stride;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    if (setjmp(jerr.jmp)) {
	jpeg_destroy_decompress(&cinfo);
	free(buf);
	return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long) len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_ABGR;	/* see decode_png()'s comment on GPU_RGBA8's actual byte order */
    jpeg_start_decompress(&cinfo);

    row_stride = (int) cinfo.output_width * cinfo.output_components;
    buf = malloc((size_t) row_stride * cinfo.output_height);
    if (!buf) {
	jpeg_destroy_decompress(&cinfo);
	return NULL;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
	rowptr[0] = buf + (size_t) cinfo.output_scanline * row_stride;
	jpeg_read_scanlines(&cinfo, rowptr, 1);
    }

    *w = (int) cinfo.output_width;
    *h = (int) cinfo.output_height;

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return buf;
}

/* ---- worker thread ---- */

static void worker_main(void *arg)
{
    char last_url[512];

    (void) arg;
    last_url[0] = '\0';

    while (g_running) {
	const char *raw = LY3DS_current_link_url();
	const char *img_url = (raw && url_is_image(raw)) ? raw : NULL;

	if (!img_url) {
	    if (last_url[0]) {
		last_url[0] = '\0';
		g_pending_fade_out = 1;
	    }
	} else if (strcmp(img_url, last_url) != 0) {
	    unsigned char *data = NULL;
	    size_t len = 0;
	    unsigned char *rgba = NULL;
	    int w = 0, h = 0;
	    int ok;

	    snprintf(last_url, sizeof(last_url), "%s", img_url);
	    g_state = PREVIEW_LOADING;

	    ok = fetch_url(img_url, &data, &len);
	    if (ok) {
		if (is_png_data(data, len))
		    rgba = decode_png(data, len, &w, &h);
		else if (is_jpeg_data(data, len))
		    rgba = decode_jpeg(data, len, &w, &h);
		free(data);
	    }

	    /* The hover target may have moved on while we were busy
	     * fetching/decoding -- if so, drop this result and loop right
	     * back around to chase the new target instead of sleeping. */
	    {
		const char *now_raw = LY3DS_current_link_url();
		const char *now_img = (now_raw && url_is_image(now_raw)) ? now_raw : NULL;

		if (!now_img || strcmp(now_img, img_url) != 0) {
		    free(rgba);
		    continue;
		}
	    }

	    if (rgba && w > 0 && h > 0 && w <= MAX_IMAGE_DIM && h <= MAX_IMAGE_DIM) {
		free(g_pending_pixels);
		g_pending_pixels = rgba;
		g_pending_w = w;
		g_pending_h = h;
		g_pending_ready = 1;
		/* Stay in PREVIEW_LOADING -- the render thread moves this to
		 * PREVIEW_FADING itself once it actually uploads the texture
		 * (see image_preview_render_bottom()), so the loading
		 * indicator keeps showing until there's really something to
		 * fade to. */
	    } else {
		/* Deliberately leave last_url set to this (failed) URL --
		 * otherwise, since it'd no longer match the still-hovered
		 * target, the check above would just retry it again next
		 * poll, every ~150ms, for as long as the same broken link
		 * stays hovered. Un-hovering (the `!img_url` branch) or
		 * moving to a genuinely different link still resets it, so
		 * a fresh hover later does get another attempt. */
		free(rgba);
		g_state = PREVIEW_IDLE;
		g_pending_clear = 1;
	    }
	}

	svcSleepThread(POLL_INTERVAL_NS);
    }
}

void image_preview_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_start_ms = osGetTime();
    g_running = 1;
    g_thread = threadCreate(worker_main, NULL, WORKER_STACK_SIZE, WORKER_PRIORITY, -1, false);
}

int image_preview_is_active(void)
{
    /* Even after g_state drops back to PREVIEW_IDLE, the loading indicator
     * may still be mid-fade-out (e.g. un-hovered while still loading, so
     * there was never an image to cross-fade against) -- keep rendering
     * until that's actually finished, or it'd just vanish with a pop. */
    return g_state != PREVIEW_IDLE || fade_eval(&g_indicator_fade) > 0.001f;
}

/* ---- GPU upload (render thread only) ---- */

static int next_po2(int v)
{
    int p = 8;	/* minimum sane size; also keeps GX transfer dims a multiple of 8 */

    while (p < v)
	p *= 2;
    return p;
}

static void upload_pending(void)
{
    int w = g_pending_w, h = g_pending_h;
    int pw = next_po2(w), ph = next_po2(h);
    u32 *staging;
    int y;

    if (g_tex_valid) {
	C3D_TexDelete(&g_tex);
	g_tex_valid = 0;
    }

    staging = (u32 *) linearAlloc((size_t) pw * ph * 4);
    if (!staging) {
	free(g_pending_pixels);
	g_pending_pixels = NULL;
	return;
    }
    memset(staging, 0, (size_t) pw * ph * 4);
    for (y = 0; y < h; y++)
	memcpy((unsigned char *) staging + (size_t) y * pw * 4,
	       g_pending_pixels + (size_t) y * w * 4,
	       (size_t) w * 4);

    free(g_pending_pixels);
    g_pending_pixels = NULL;

    GSPGPU_FlushDataCache(staging, (u32) ((size_t) pw * ph * 4));

    if (!C3D_TexInit(&g_tex, (u16) pw, (u16) ph, GPU_RGBA8)) {
	linearFree(staging);
	return;
    }

    GX_DisplayTransfer(staging, GX_BUFFER_DIM(pw, ph),
		       (u32 *) g_tex.data, GX_BUFFER_DIM(pw, ph),
		       GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
		       GX_TRANSFER_RAW_COPY(0) |
		       GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
		       GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
		       GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    gspWaitForPPF();
    linearFree(staging);

    g_subtex.width = (u16) w;
    g_subtex.height = (u16) h;
    g_subtex.left = 0.0f;
    g_subtex.top = 1.0f;
    g_subtex.right = (float) w / (float) pw;
    g_subtex.bottom = 1.0f - (float) h / (float) ph;

    g_image.tex = &g_tex;
    g_image.subtex = &g_subtex;
    g_img_w = w;
    g_img_h = h;
    g_tex_valid = 1;
}

static void draw_label(int x, int y, const char *s, u32 color)
{
    C2D_Text text;

    C2D_TextParse(&text, g_text_buf, s);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, (float) x, (float) y, 0.5f, 0.5f, 0.5f, color);
}

/* Draws the loaded image, scaled to fit within BOX_W x BOX_H (never
 * upscaled, so small icons don't come out blurry) and centered, at the
 * given opacity -- alpha < 1 during the guide-out/image-in cross-fade. */
static void draw_image(float alpha)
{
    float scale = fminf(BOX_W / (float) g_img_w, BOX_H / (float) g_img_h);
    float draw_w, draw_h, x, y;
    C2D_ImageTint tint;
    C2D_ImageTint *tintp = NULL;

    if (scale > 1.0f)
	scale = 1.0f;
    draw_w = g_img_w * scale;
    draw_h = g_img_h * scale;
    x = (BOTTOM_W - draw_w) / 2.0f;
    y = (BOTTOM_H - draw_h) / 2.0f;
    if (alpha < 1.0f) {
	C2D_AlphaImageTint(&tint, alpha);
	tintp = &tint;
    }
    C2D_DrawImageAt(g_image, x, y, 0.5f, tintp, scale, scale);
}

/* A small 8-tick rotating activity indicator (no image assets needed,
 * matching how the rest of this app draws its own UI primitives) -- shown
 * over the still-visible guide while a fetch/decode is in flight. */
static void draw_spinner(int cx, int cy, float alpha)
{
    double elapsed = (double) (osGetTime() - g_start_ms);
    float phase = (float) fmod(elapsed, SPINNER_PERIOD_MS) / (float) SPINNER_PERIOD_MS;
    int head = (int) (phase * SPINNER_TICKS);
    int i;

    for (i = 0; i < SPINNER_TICKS; i++) {
	float ang = (float) i / SPINNER_TICKS * 2.0f * (float) M_PI - (float) M_PI / 2.0f;
	int back = (head - i + SPINNER_TICKS) % SPINNER_TICKS;
	float bright = 1.0f - (float) back / SPINNER_TICKS;
	u8 v = (u8) (60 + bright * 195);
	u8 a = (u8) (255 * alpha);
	u32 color = C2D_Color32(v, v, v, a);
	float x0 = cx + cosf(ang) * SPINNER_R_IN;
	float y0 = cy + sinf(ang) * SPINNER_R_IN;
	float x1 = cx + cosf(ang) * SPINNER_R_OUT;
	float y1 = cy + sinf(ang) * SPINNER_R_OUT;

	C2D_DrawLine(x0, y0, color, x1, y1, color, 3.0f, 0.6f);
    }
}

static void draw_loading_indicator(float alpha)
{
    int cx = BOTTOM_W / 2;
    int cy = BOTTOM_H / 2 - 14;

    /* Dark backing so the spinner/text stay legible over whatever the
     * guide graphic happens to show underneath. */
    C2D_DrawRectSolid(cx - 80, cy - 34, 0.55f, 160, 68, C2D_Color32(0x00, 0x00, 0x00, (u8) (0x90 * alpha)));
    draw_spinner(cx, cy, alpha);
    draw_label(cx - 52, cy + 20, "Loading image...", C2D_Color32(0xe0, 0xe0, 0xe0, (u8) (255 * alpha)));
}

void image_preview_render_bottom(C3D_RenderTarget *target)
{
    static preview_state_t prev_state = PREVIEW_IDLE;

    if (!g_text_buf)
	g_text_buf = C2D_TextBufNew(256);

    if (g_pending_fade_out) {
	g_pending_fade_out = 0;
	if (g_tex_valid && (g_state == PREVIEW_FADING_IN || g_state == PREVIEW_READY)) {
	    /* Something's actually showing -- cross-fade it back out to the
	     * guide instead of cutting away abruptly. */
	    g_fade_start_ms = osGetTime();
	    g_state = PREVIEW_FADING_OUT;
	} else {
	    /* Nothing to fade from (still loading, no texture yet) --
	     * the loading indicator's own fade-out (below) covers this. */
	    if (g_tex_valid) {
		C3D_TexDelete(&g_tex);
		g_tex_valid = 0;
	    }
	    g_state = PREVIEW_IDLE;
	}
    }
    if (g_pending_clear) {
	if (g_tex_valid) {
	    C3D_TexDelete(&g_tex);
	    g_tex_valid = 0;
	}
	g_pending_clear = 0;
	g_state = PREVIEW_IDLE;
    }
    if (g_pending_ready) {
	upload_pending();
	g_pending_ready = 0;
	if (g_tex_valid) {
	    g_fade_start_ms = osGetTime();
	    g_state = PREVIEW_FADING_IN;
	}
    }

    /* The loading indicator fades independently of the guide<->image
     * cross-fade above -- it can still be finishing its own fade-out even
     * after g_state has already moved on (e.g. straight back to idle, if
     * there was never an image to cross-fade against). Comparing against
     * the state as of last frame catches entering *and* leaving LOADING
     * regardless of which of the branches above caused it. */
    if (g_state == PREVIEW_LOADING && prev_state != PREVIEW_LOADING)
	fade_to(&g_indicator_fade, 1.0f, INDICATOR_FADE_MS);
    else if (g_state != PREVIEW_LOADING && prev_state == PREVIEW_LOADING)
	fade_to(&g_indicator_fade, 0.0f, INDICATOR_FADE_MS);
    prev_state = g_state;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(target, C2D_Color32(0x10, 0x10, 0x14, 0xff));
    C2D_SceneBegin(target);

    if (g_state == PREVIEW_FADING_IN && g_tex_valid) {
	double elapsed = (double) (osGetTime() - g_fade_start_ms);
	float t = (float) (elapsed / FADE_MS);

	if (t >= 1.0f) {
	    t = 1.0f;
	    g_state = PREVIEW_READY;
	}
	bottom_ui_draw_content(1.0f - t);
	draw_image(t);
    } else if (g_state == PREVIEW_READY && g_tex_valid) {
	draw_image(1.0f);
    } else if (g_state == PREVIEW_FADING_OUT && g_tex_valid) {
	double elapsed = (double) (osGetTime() - g_fade_start_ms);
	float t = (float) (elapsed / FADE_MS);
	int done = (t >= 1.0f);

	if (done)
	    t = 1.0f;
	bottom_ui_draw_content(t);
	if (t < 1.0f)
	    draw_image(1.0f - t);
	if (done) {
	    C3D_TexDelete(&g_tex);
	    g_tex_valid = 0;
	    g_state = PREVIEW_IDLE;
	}
    } else {
	/* PREVIEW_LOADING, or a texture upload hasn't landed yet -- keep
	 * showing the normal guide underneath so the bottom screen never
	 * just goes blank while waiting. */
	bottom_ui_draw_content(1.0f);
    }

    {
	float ind_alpha = fade_eval(&g_indicator_fade);

	if (ind_alpha > 0.005f)
	    draw_loading_indicator(ind_alpha);
    }

    C3D_FrameEnd(0);
}
