/**********************************************************************/
/*                    GSplus - Apple //gs Emulator                    */
/*                    Based on KEGS by Kent Dickey                    */
/*                                                                    */
/*      This code is covered by the GNU GPL v3                        */
/*      See the file COPYING.txt or https://www.gnu.org/licenses/     */
/**********************************************************************/

/*
 * libretro front-end for GSplus.
 *
 * Phase 0 (build skeleton): this file renders a moving test pattern and
 * exercises none of the KEGS core yet -- it exists to prove the build/link
 * wiring (all 30 platform-independent core objects link into a single
 * gsplus_libretro shared object) before any emulator code is called. See
 * claudedocs/libretro-core-plan.md.
 *
 * The KEGS core is compiled with -DSDL_INPUT, which compiles out the native
 * joystick backends in joystick_driver.c AND its fallback stubs (the frontend
 * is expected to provide joystick_init/update/update_buttons). So this file
 * supplies inert versions of them to close the link; real RetroPad handling
 * lands in Phase 2.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "libretro.h"

/* ------------------------------------------------------------------ */
/*  libretro callbacks                                                 */
/* ------------------------------------------------------------------ */

static retro_video_refresh_t   video_cb;
static retro_audio_sample_t    audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t     environ_cb;
static retro_input_poll_t      input_poll_cb;
static retro_input_state_t     input_state_cb;

static struct retro_log_callback logging;
static retro_log_printf_t      log_cb;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

/* Test-pattern framebuffer (Phase 0 only). */
#define LR_TEST_W 640
#define LR_TEST_H 480
static uint32_t *frame_buf;
static unsigned  frame_count;

/* ------------------------------------------------------------------ */
/*  Basic entry points                                                 */
/* ------------------------------------------------------------------ */

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   bool no_content = true;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;
}

void retro_set_video_refresh(retro_video_refresh_t cb)      { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)        { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)            { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)          { input_state_cb = cb; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "GSplus";
   info->library_version  = "0.0-phase0";
   info->need_fullpath    = true;
   info->block_extract    = true;
   info->valid_extensions = "hdv|po|2mg|dsk|do|nib|woz";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps          = 59.9227;   /* VBL_RATE */
   info->timing.sample_rate  = 48000.0;
   info->geometry.base_width  = LR_TEST_W;
   info->geometry.base_height = LR_TEST_H;
   info->geometry.max_width   = LR_TEST_W;
   info->geometry.max_height  = LR_TEST_H;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_init(void)
{
   frame_buf   = (uint32_t *)calloc((size_t)LR_TEST_W * LR_TEST_H, sizeof(uint32_t));
   frame_count = 0;
}

void retro_deinit(void)
{
   free(frame_buf);
   frame_buf = NULL;
}

/* ------------------------------------------------------------------ */
/*  Content load / unload                                              */
/* ------------------------------------------------------------------ */

bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "GSplus: XRGB8888 pixel format not supported.\n");
      return false;
   }

   (void)info;   /* Phase 0: content is ignored. */
   return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info,
      size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

void retro_unload_game(void)
{
}

/* ------------------------------------------------------------------ */
/*  Run loop (Phase 0: moving XRGB8888 test pattern)                   */
/* ------------------------------------------------------------------ */

static void render_test_pattern(void)
{
   const unsigned stride = LR_TEST_W;
   const unsigned shift  = frame_count;
   uint32_t *line = frame_buf;
   unsigned x, y;

   for (y = 0; y < LR_TEST_H; y++, line += stride)
   {
      unsigned band_y = ((y + shift) >> 5) & 1;
      for (x = 0; x < LR_TEST_W; x++)
      {
         unsigned band_x = ((x + shift) >> 5) & 1;
         line[x] = (band_x ^ band_y) ? 0x00ff0000u   /* red  */
                                     : 0x000000ffu;  /* blue */
      }
   }
}

void retro_run(void)
{
   if (input_poll_cb)
      input_poll_cb();

   render_test_pattern();
   frame_count++;

   video_cb(frame_buf, LR_TEST_W, LR_TEST_H, LR_TEST_W * sizeof(uint32_t));

   /* Phase 0: emit silence so the frontend has an audio timebase. */
   if (audio_batch_cb)
   {
      static int16_t silence[2 * 801];
      audio_batch_cb(silence, 801);
   }
}

void retro_reset(void)
{
   frame_count = 0;
}

/* ------------------------------------------------------------------ */
/*  Serialization / memory / cheats (unsupported)                     */
/* ------------------------------------------------------------------ */

unsigned retro_get_region(void)            { return RETRO_REGION_NTSC; }
size_t   retro_serialize_size(void)        { return 0; }
bool     retro_serialize(void *d, size_t s){ (void)d; (void)s; return false; }
bool     retro_unserialize(const void *d, size_t s){ (void)d; (void)s; return false; }
void    *retro_get_memory_data(unsigned id){ (void)id; return NULL; }
size_t   retro_get_memory_size(unsigned id){ (void)id; return 0; }
void     retro_cheat_reset(void)           { }
void     retro_cheat_set(unsigned i, bool e, const char *c)
{
   (void)i; (void)e; (void)c;
}

/* ------------------------------------------------------------------ */
/*  KEGS frontend seams                                                */
/* ------------------------------------------------------------------ */
/*
 * With -DSDL_INPUT the core's joystick_driver.c compiles out both its native
 * backends and its fallback stubs, expecting the frontend to provide these.
 * Phase 0 provides inert versions (no joystick attached) purely so the link
 * closes; real RetroPad-driven paddle input is Phase 2.
 *
 * defc.h is included here (not at the top) so its wide set of core typedefs
 * and macros stays out of the libretro-facing code above.
 */
#include "defc.h"

extern int g_joystick_native_type1;   /* defined in paddles.c */
extern int g_joystick_native_type2;
extern int g_joystick_native_type;
extern int g_paddle_buttons;
extern int g_paddle_val[];

void joystick_init(void)
{
   g_joystick_native_type1 = -1;
   g_joystick_native_type2 = -1;
   g_joystick_native_type  = -1;
}

void joystick_update(dword64 dfcyc)
{
   int i;
   (void)dfcyc;
   for (i = 0; i < 4; i++)
      g_paddle_val[i] = 32767;
   g_paddle_buttons = 0xc;
}

void joystick_update_buttons(void)
{
}
