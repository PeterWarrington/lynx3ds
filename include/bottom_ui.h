/*
 * Bottom-screen UI: a static background image with a controls-guide
 * overlay that fades in and out on a sine wave, rendered via citro2d/
 * citro3d (GPU-accelerated), independent of the top screen's raw-
 * framebuffer Lynx renderer (see font_render.c) and the earlier plain
 * libctru-console debug view it replaces.
 */
#ifndef LYNX3DS_BOTTOM_UI_H
#define LYNX3DS_BOTTOM_UI_H

/*
 * Sets up C3D/C2D, the bottom-screen render target, loads the sprite
 * sheet (gfx/bottom_ui.t3s -> bg + fg images), and spawns a dedicated
 * background thread that continuously renders (background at full
 * opacity, guide overlay tinted with a sine-wave alpha) every vblank for
 * the rest of the app's life -- independent of whatever the main thread
 * (Lynx, the top screen) is doing. Call once, after gfxInitDefault().
 */
void bottom_ui_init(void);

#endif /* LYNX3DS_BOTTOM_UI_H */
