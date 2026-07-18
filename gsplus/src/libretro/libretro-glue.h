/*
 * libretro-glue.h -- minimal compatibility shim for the vendored vice-libretro
 * drawing layer (libretro-graph.{c,h}, libretro-font.i) so it builds against the
 * GSplus libretro core instead of VICE's libretro-core.h.
 *
 * The vice-libretro graph code draws directly into a frontend framebuffer using
 * three globals: retro_bmp (the pixel buffer), retrow (pitch, in pixels) and
 * pix_bytes (2 or 4). GSplus's libretro.c owns those and points them at its
 * persistent XRGB8888 frame (g_lr_vbuf) before each print_vkbd() call.
 *
 * This file contains no VICE code; it only declares the handful of names the
 * ported drawing layer expects. See libretro-graph.c for the GPL/attribution.
 */
#ifndef LIBRETRO_GLUE_H
#define LIBRETRO_GLUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Largest frame the core ever produces: the main Kimage's full 704x462 picture
 * plus MAX_STATUS_LINES of headroom (see LR_MAX_* in libretro.c == 704x528).
 * graphed[] and the drawing clamps are sized from this; keep it >= the buffer
 * libretro.c allocates. A little slack is harmless. */
#define RETRO_BMP_WIDTH   704
#define RETRO_BMP_HEIGHT  544
#define RETRO_BMP_SIZE    (RETRO_BMP_WIDTH * RETRO_BMP_HEIGHT)

/* Set by libretro.c right before print_vkbd(): the destination frame, its pitch
 * in pixels, its height, and the bytes-per-pixel (always 4 / XRGB8888 here). */
extern unsigned char *retro_bmp;
extern int            retrow;
extern int            retroh;
extern int            pix_bytes;

/* Defined in libretro-graph.c. */
int RGBc(int r, int g, int b);

#endif /* LIBRETRO_GLUE_H */
