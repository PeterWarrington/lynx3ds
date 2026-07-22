/*
 * Bottom-screen "hover preview": when the currently highlighted link (see
 * LY3DS_current_link_url() in vendor/lynx/src/LYMainLoop.c) points at a
 * PNG or JPEG image, fetches and decodes it on a background thread (libcurl
 * + libpng/libjpeg-turbo, both devkitPro portlibs) and shows it on the
 * bottom screen in place of the normal fading guide, for as long as that
 * link stays highlighted.
 */
#ifndef LYNX3DS_IMAGE_PREVIEW_H
#define LYNX3DS_IMAGE_PREVIEW_H

#include <citro2d.h>

/* Spawns the background fetch/decode worker thread. Call once, after
 * bottom_ui_init() (needs C3D/C2D already initialized) and after socInit()
 * (needs sockets up before it could ever actually fetch anything). */
void image_preview_init(void);

/* True if there's currently something to show (loading text or an image) --
 * checked by bottom_ui's render thread to decide whether to call
 * image_preview_render_bottom() instead of its own normal render. */
int image_preview_is_active(void);

/* Called every vblank from bottom_ui's render thread in place of its own
 * render while image_preview_is_active() -- draws a full frame (begin to
 * end) into the given render target, mirroring bottom_ui_render_once()'s
 * and bookmarks_render_bottom()'s own structure. */
void image_preview_render_bottom(C3D_RenderTarget *target);

#endif /* LYNX3DS_IMAGE_PREVIEW_H */
