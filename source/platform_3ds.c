/*
 * 3DS service init/teardown, run automatically before/after Lynx's own
 * main() via constructor/destructor attributes so LYMain.c stays unmodified.
 */
#include <3ds.h>
#include <bottom_ui.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>

#define SOC_BUFFERSIZE 0x100000

static u32 *soc_buffer = NULL;

extern void lynx3ds_console_init(void);
extern void lynx3ds_present_frame(void);
extern void lynx3ds_select_bottom(void);

static void log_step(const char *msg)
{
    lynx3ds_select_bottom();
    printf("lynx3ds: %s\n", msg);
    fflush(stdout);
    lynx3ds_present_frame();
}

/* Lynx's own temp files (partial downloads, etc.) accumulate under here
 * across runs since there's no shell/cron to clean them up on 3DS -- so
 * wipe it on every launch instead. */
static void clean_tmp_dir(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *ent;
    char full[512];

    if (d == NULL)
	return;
    while ((ent = readdir(d)) != NULL) {
	if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
	    continue;
	snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
	remove(full);
    }
    closedir(d);
}

__attribute__((constructor))
static void lynx3ds_platform_init(void)
{
    gfxInitDefault();
    /*
     * Double buffering stays on (the default): we draw a full frame (clear
     * + per-cell fills + glyphs, many pixel writes) into the back buffer,
     * then lynx3ds_present_frame() swaps it in atomically. Single-buffering
     * was tried earlier to fix stale-content-until-keypress, but that was
     * really just from not swapping reliably after every real redraw (now
     * fixed) -- drawing directly into the buffer actively being scanned
     * out at 60Hz is what was causing visible tearing.
     */
    lynx3ds_console_init();	/* so boot logging is visible on-screen from here on */
    log_step("gfxInitDefault done");

    /*
     * Bring up the bottom-screen UI as early as possible -- it's
     * statically linked (no romfs needed), so this can run immediately.
     * It spawns its own render thread, so it keeps animating for the
     * whole rest of startup (Lynx parsing its config/history/etc. on the
     * main thread) instead of a blank/flickering screen until Lynx
     * reaches its input loop.
     */
    bottom_ui_init();

    {
	Result rc = romfsInit();
	char buf[64];

	snprintf(buf, sizeof(buf), "romfsInit rc=%08lx", (unsigned long) rc);
	log_step(buf);
    }

    /*
     * Lynx assumes Unix-style absolute paths ("/..."), e.g. LYisAbsPath()
     * just checks path[0] == '/'. Our device-prefixed paths (sdmc:/, romfs:/)
     * don't look like that. Making sdmc:/ the default device lets plain
     * "/..." paths (HOME, temp files, bookmarks, history, cookies, ...)
     * resolve against the SD card without patching Lynx's path logic.
     */
    chdir("sdmc:/");
    mkdir("/3ds", 0777);
    mkdir("/3ds/lynx", 0777);
    mkdir("/3ds/lynx/tmp", 0777);
    clean_tmp_dir("/3ds/lynx/tmp");
    setenv("HOME", "/3ds/lynx", 1);
    setenv("LYNX_TEMP_SPACE", "/3ds/lynx/tmp/", 1);
    log_step("sdmc:/ chdir + /3ds/lynx dirs + HOME set");

    soc_buffer = (u32 *) memalign(0x1000, SOC_BUFFERSIZE);
    if (soc_buffer != NULL) {
	Result rc = socInit(soc_buffer, SOC_BUFFERSIZE);
	char buf[64];

	snprintf(buf, sizeof(buf), "socInit rc=%08lx", (unsigned long) rc);
	log_step(buf);
    } else {
	log_step("soc_buffer alloc FAILED");
    }
    log_step("platform init done, entering Lynx main()");
}

__attribute__((destructor))
static void lynx3ds_platform_fini(void)
{
    if (soc_buffer != NULL) {
	socExit();
	free(soc_buffer);
	soc_buffer = NULL;
    }
    romfsExit();
    gfxExit();
}
