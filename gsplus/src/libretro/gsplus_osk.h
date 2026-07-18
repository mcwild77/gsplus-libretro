/*
 * gsplus_osk.h -- on-screen keyboard for the GSplus libretro core.
 *
 * A controller-driven virtual keyboard for the Apple IIgs, so handheld / no-
 * keyboard users can type. The look and feel (transparent key grid, cursor
 * navigation with auto-repeat, sticky modifiers via a long press) follows the
 * vice-libretro OSK, and it draws with the ported vice-libretro drawing layer
 * (libretro-graph.{c,h}, gsplus-font.i, all GPL). The layout and key emission
 * are GSplus-original: an Apple IIgs ADB layout that injects raw ADB key codes.
 *
 * Rendering contract: the caller sets retro_bmp / retrow / retroh / pix_bytes
 * (via libretro-glue.h) to the destination frame, optionally sets the OSK's
 * target area with osk_set_area(), then calls print_vkbd() once per frame after
 * the emulator picture has been drawn and before handing the frame to RetroArch.
 *
 * Input contract: the host implements the osk_host_* callbacks below. input_vkbd()
 * is called once per frame while the OSK is visible; it reads the pad through
 * osk_host_pad() and emits keys through osk_host_key() / osk_host_action().
 */
#ifndef GSPLUS_OSK_H
#define GSPLUS_OSK_H

#include <stdbool.h>

/* Visible state: true while the OSK is shown. */
extern bool retro_vkbd;

/* Draw the OSK onto the framebuffer described by libretro-glue.h globals. */
void print_vkbd(void);

/* Consume one frame of pad input (cursor move, key press). Call while visible. */
void input_vkbd(void);

/* Show / hide. */
void toggle_vkbd(void);

/* Confine the keyboard to a sub-rectangle of the frame (e.g. the cropped active
 * picture). Coordinates are in framebuffer pixels at the current pitch (retrow).
 * Pass w<=0 to reset to the full frame. */
void osk_set_area(int x, int y, int w, int h);

/* ---- host callbacks (implemented by libretro.c) ---------------------------- */

/* Read a RetroPad digital button (RETRO_DEVICE_ID_JOYPAD_*). Returns nonzero if
 * currently pressed. */
int  osk_host_pad(unsigned retro_id);

/* Inject an emulated ADB key: a2code is a raw ADB scan code (0x00-0x7f),
 * down != 0 for a press, 0 for a release. */
void osk_host_key(int a2code, int down);

/* Hold/release a sticky modifier (shift/ctrl/caps/option/open-apple): sends it
 * as a raw ADB key event AND sets it in the emulated modifier latch, so it works
 * both when KEGS translates keys itself (autopoll on) and when the emulated OS
 * does (autopoll off, e.g. GS/OS). a2code is the modifier's ADB scan code;
 * down != 0 holds it, 0 releases it. Use this instead of osk_host_key(). */
void osk_host_modifier(int a2code, int down);

/* Special (non-ADB-key) actions triggered from the OSK. */
enum { OSK_ACTION_RESET = 0 };
void osk_host_action(int action);

/* Millisecond monotonic clock for timing (auto-repeat, long-press). */
long osk_host_now_ms(void);

#endif /* GSPLUS_OSK_H */
