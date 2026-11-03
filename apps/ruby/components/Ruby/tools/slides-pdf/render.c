/*
 * Render the slide deck on the host, through the same framebuffer console the
 * component uses.
 *
 * slides.rb's output arrives on stdin. It is fed to fb_console.c, drawing into
 * memory instead of a mapped framebuffer, and the screen is saved as a PPM each
 * time a slide is finished -- which is when the next one starts, since every
 * slide begins by clearing the screen. The capture is taken with the clear
 * already received but not yet carried out, so it holds the slide as it was.
 *
 * Usage: render <scripts dir> <output dir>
 *
 * Geometry comes from FB_WIDTH, FB_HEIGHT and FB_SCALE at compile time, so the
 * pages match what ruby.camkes gives the device.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"

const int fb_width = FB_WIDTH;
const int fb_height = FB_HEIGHT;
const int fb_pitch = FB_WIDTH * 4;
const int fb_scale = FB_SCALE;
const int fb_rotate = 0;

static uint32_t pixels[FB_WIDTH * FB_HEIGHT];
void *fb = pixels;

/* fb_console.c names the archive bounds; the lookup below never uses them. */
char _cpio_archive[1];
char _cpio_archive_end[1];

static const char *script_dir;

/*
 * The archive holds the scripts directory flat, so a name in it is a file
 * there. Each file is read once and kept, as the archive's contents would be.
 */
const void *cpio_get_file(const void *archive, unsigned long len, const char *name,
                          unsigned long *size)
{
    char path[4096];
    FILE *f;
    long n;
    char *data;

    (void)archive;
    (void)len;

    snprintf(path, sizeof(path), "%s/%s", script_dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)n + 1);
    if (data == NULL || fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    if (size != NULL) {
        *size = (unsigned long)n;
    }
    return data;
}

static int save_page(const char *out_dir, int page)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/slide-%03d.ppm", out_dir, page);
    f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        return -1;
    }

    fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
    for (long i = 0; i < (long)FB_WIDTH * FB_HEIGHT; i++) {
        uint8_t rgb[3] = {
            (uint8_t)(pixels[i] >> 16), (uint8_t)(pixels[i] >> 8), (uint8_t)pixels[i]
        };
        fwrite(rgb, 1, sizeof(rgb), f);
    }

    return fclose(f);
}

int main(int argc, char **argv)
{
    static const char clear[] = "\033[2J";
    char buf[65536];
    size_t n;
    size_t matched = 0;
    size_t pending = 0;
    int page = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <scripts dir> <output dir>\n", argv[0]);
        return 2;
    }
    script_dir = argv[1];

    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        size_t start = 0;

        for (size_t i = 0; i < n; i++) {
            matched = buf[i] == clear[matched] ? matched + 1 : (buf[i] == clear[0] ? 1 : 0);
            if (matched != sizeof(clear) - 1) {
                continue;
            }
            matched = 0;

            /* Everything up to the final byte of the clear, so the console holds
             * it unexecuted while the page is saved. */
            console_fb_write(buf + start, i - start);
            pending += i - start;
            start = i;

            /* The first clear comes before anything has been drawn. */
            if (pending > sizeof(clear)) {
                if (save_page(argv[2], ++page) != 0) {
                    return 1;
                }
            }
            pending = 0;
        }

        console_fb_write(buf + start, n - start);
        pending += n - start;
    }

    /* What follows the last clear is the tool's goodbye, not a slide. */
    fprintf(stderr, "%d pages\n", page);
    return page > 0 ? 0 : 1;
}
