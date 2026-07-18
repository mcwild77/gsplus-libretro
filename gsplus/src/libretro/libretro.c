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
 * Phase 1 ("it's alive"): boots a real Apple IIgs inside RetroArch. retro_run()
 * calls the core's run_16ms() (one emulated VBL frame), then drains the KEGS
 * framebuffer's dirty rectangles into a persistent XRGB8888 buffer for
 * retro_video_refresh, and hands the frame's audio to retro_audio_sample_batch.
 * Timing is owned by RetroArch: the core's own micro_sleep() pacer is disabled
 * via g_micro_sleep_disable so retro_run() is the sole frame clock.
 *
 * Only two shared core files are touched (both inert unless LIBRETRO_AUDIO /
 * the runtime flag are active): sound_driver.c routes audio here, and
 * sim65816.c gained the g_micro_sleep_disable flag. See
 * claudedocs/libretro-core-plan.md.
 *
 * Phase 2 adds input: a RetroArch keyboard callback feeds a ring buffer drained
 * each frame into adb_physical_key_update() (translated via g_retro_key_map[]),
 * the RetroPad drives the paddle/joystick and maps its D-pad/Start to the arrow
 * keys and Return, and RETRO_DEVICE_MOUSE deltas drive adb_update_mouse(). The
 * OSK (Select toggle) lands in Phase 4; the core is built -DSDL_INPUT, which
 * compiles out joystick_driver.c's native backends so we own joystick_*.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "libretro.h"

/* The KEGS core: typedefs (word32/byte/dword64), globals, and prototypes for
 * kegs_init, run_16ms, the video_ functions, and the rest. defc.h pulls in
 * protos.h -> protos_base.h. */
#include "defc.h"

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

/* ------------------------------------------------------------------ */
/*  Emulator-facing state                                              */
/* ------------------------------------------------------------------ */

/* Largest frame we advertise / allocate: the main Kimage's full size -- 640x400
 * active + IIgs borders + the status lines drawn below the picture (704x528).
 * Must equal the size video_init() gives g_mainwin_kimage: kegs_init() receives
 * these as the screen bounds, and anything smaller makes video_update_scale()
 * clamp x_width below a2_width -- with no_scale_window set, video_out_data()
 * would then blit 704-wide a2 rows into a narrower pitch and wrap rows. */
#define LR_MAX_WIDTH   X_A2_WINDOW_WIDTH
#define LR_MAX_HEIGHT  (X_A2_WINDOW_HEIGHT + MAX_STATUS_LINES*16 + 2)

extern int g_preferred_rate;         /* sound_driver.c: core sample rate (48000) */
extern int g_micro_sleep_disable;    /* sim65816.c: 1 => RetroArch owns pacing */
extern int g_audio_enable;           /* sound.c */

/* Active-picture margins inside the Kimage (video.c). The core framebuffer is
 * borders + 640x400 active area + status lines; these locate the active area so
 * the "Border" option can crop to just the picture or blacken the surround. */
extern int g_video_act_margin_left;
extern int g_video_act_margin_right;
extern int g_video_act_margin_top;

/* KEGS status lines under the picture (video.c). Forced off at boot: RetroArch
 * owns frontend UI, so the core shows just the bordered 704x462 picture. The
 * kimage/buffer keep MAX_STATUS_LINES headroom in case the in-emulator toggle
 * re-enables them; retro_run's aspect check adopts the resize either way. */
extern int g_status_enable;
extern int g_status_enable_previous;

/* Battery RAM (clock.c). g_bram_ptr points at the 256-byte bank for the running
 * ROM version; the boot-slot option pokes byte $28 and re-checksums it. */
extern byte *g_bram_ptr;

static Kimage  *g_lr_kimage;         /* main-window KEGS image */
static word32  *g_lr_vbuf;           /* persistent XRGB8888 framebuffer */
static int      g_lr_width;          /* current active width  (== pixels/line) */
static int      g_lr_height;         /* current active height */
static int      g_lr_kegs_inited;    /* KEGS globals can only init once/process */

/* ------------------------------------------------------------------ */
/*  Core options                                                       */
/* ------------------------------------------------------------------ */
/* Live option state, refreshed from the frontend by lr_apply_options(). */

enum { LR_ASPECT_43 = 0, LR_ASPECT_PP };     /* 4:3 vs square pixels */
enum { LR_DPAD_ARROWS = 0, LR_DPAD_JOYSTICK };
enum { LR_MOUSE_RELATIVE = 0, LR_MOUSE_DISABLED };
/* Border handling: draw the IIgs border, blacken it (keep 4:3 frame), or crop
 * it away (hand RetroArch just the 640x400 picture). */
enum { LR_BORDER_SHOW = 0, LR_BORDER_BLACK, LR_BORDER_CROP };

static int g_opt_audio    = 1;               /* g_audio_enable mirror */
static int g_opt_aspect   = LR_ASPECT_43;
static int g_opt_dpad     = LR_DPAD_ARROWS;
static int g_opt_mouse    = LR_MOUSE_RELATIVE;
static int g_opt_border   = LR_BORDER_SHOW;  /* how to treat the IIgs border */

/* Boot slot -> BRAM $28 value: 0 = "Default" sentinel meaning leave the ROM's
 * own choice untouched; otherwise the $28 encoding (0=Scan, 5/6/7=slot). */
static int g_opt_boot_slot   = 0;            /* option selection, sentinel 0 */
static int g_boot_slot_apply = 0;            /* $28 value pending application */
static int g_boot_slot_done  = 0;            /* already applied for this value */

/* ------------------------------------------------------------------ */
/*  Core options table                                                 */
/* ------------------------------------------------------------------ */
/* Declared with the v2 (categorized) API and downgraded to the v1
 * retro_variable API for older frontends by lr_set_core_options(). */

#define LR_KEY_AUDIO     "gsplus_audio"
#define LR_KEY_ASPECT    "gsplus_aspect"
#define LR_KEY_DPAD      "gsplus_dpad_mode"
#define LR_KEY_MOUSE     "gsplus_mouse_mode"
#define LR_KEY_BOOTSLOT  "gsplus_boot_slot"
#define LR_KEY_BORDER    "gsplus_border"

static const struct retro_core_option_v2_definition g_option_defs[] = {
   {
      LR_KEY_AUDIO,
      "Audio", NULL,
      "Enable Ensoniq/speaker sound output.", NULL, NULL,
      { { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } },
      "enabled"
   },
   {
      LR_KEY_ASPECT,
      "Aspect Ratio", NULL,
      "4:3 matches an Apple IIgs monitor; Pixel Perfect shows square pixels.",
      NULL, NULL,
      { { "4:3", "4:3 (monitor)" },
        { "pixel_perfect", "Pixel Perfect (1:1)" },
        { NULL, NULL } },
      "4:3"
   },
   {
      LR_KEY_BORDER,
      "Border", NULL,
      "Show draws the IIgs border; Black replaces it with a steady black frame "
      "(keeps 4:3); Crop hides it, showing just the 640x400 picture.",
      NULL, NULL,
      { { "show",  "Show" },
        { "black", "Black" },
        { "crop",  "Crop" },
        { NULL, NULL } },
      "show"
   },
   {
      LR_KEY_DPAD,
      "D-Pad", NULL,
      "Arrow Keys drives the emulated arrow keys (menus); Joystick drives the "
      "IIgs paddle digitally (games).",
      NULL, NULL,
      { { "arrows", "Arrow Keys" }, { "joystick", "Joystick" }, { NULL, NULL } },
      "arrows"
   },
   {
      LR_KEY_MOUSE,
      "Mouse", NULL,
      "Relative mode maps the RetroPad mouse device to the IIgs mouse.",
      NULL, NULL,
      { { "relative", "Relative" }, { "disabled", NULL }, { NULL, NULL } },
      "relative"
   },
   {
      LR_KEY_BOOTSLOT,
      "Startup Slot", NULL,
      "Force the IIgs startup slot via battery RAM (needs a reset to apply). "
      "Default leaves the machine's own Control Panel setting alone.",
      NULL, NULL,
      { { "default", "Default (Control Panel)" },
        { "scan",    "Scan" },
        { "7",       "Slot 7 (SmartPort/HD)" },
        { "6",       "Slot 6 (5.25\")" },
        { "5",       "Slot 5 (3.5\")" },
        { NULL, NULL } },
      "default"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, {{ NULL, NULL }}, NULL }
};

/* Register the option set, preferring the categorized v2 interface and falling
 * back to the flat v1 retro_variable list on older frontends. */
static void lr_set_core_options(void)
{
   unsigned version = 0;

   if (!environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) &&
       version >= 2)
   {
      struct retro_core_options_v2 opts;
      opts.categories  = NULL;   /* no categories -- flat list, but v2 sublabels */
      opts.definitions = (struct retro_core_option_v2_definition *)g_option_defs;
      if (environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts))
         return;
   }

   /* v1 fallback: build "desc; val0|val1|..." strings for SET_VARIABLES. */
   {
      static struct retro_variable vars[16];
      static char bufs[16][256];
      unsigned i, n = 0;

      for (i = 0; g_option_defs[i].key && n < 15; i++)
      {
         const struct retro_core_option_v2_definition *d = &g_option_defs[i];
         int pos, j;

         pos = snprintf(bufs[n], sizeof(bufs[n]), "%s; ", d->desc);
         for (j = 0; d->values[j].value && pos < (int)sizeof(bufs[n]) - 1; j++)
         {
            pos += snprintf(bufs[n] + pos, sizeof(bufs[n]) - pos, "%s%s",
                            j ? "|" : "", d->values[j].value);
         }
         vars[n].key   = d->key;
         vars[n].value = bufs[n];
         n++;
      }
      vars[n].key = NULL;
      vars[n].value = NULL;
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
   }
}

/* Read one option's current value, or NULL if the frontend has none. */
static const char *lr_get_option(const char *key)
{
   struct retro_variable var;
   var.key = key;
   var.value = NULL;
   if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      return var.value;
   return NULL;
}

/* Map the Border option string to an LR_BORDER_* mode (default: show). */
static int lr_border_from_option(const char *v)
{
   if (v && !strcmp(v, "crop"))  return LR_BORDER_CROP;
   if (v && !strcmp(v, "black")) return LR_BORDER_BLACK;
   return LR_BORDER_SHOW;
}

/* Map the startup-slot option string to a BRAM $28 value, or -1 for "default"
 * (leave the machine's own setting). */
static int lr_boot_slot_from_option(const char *v)
{
   if (!v || !strcmp(v, "default")) return -1;
   if (!strcmp(v, "scan"))          return 0;
   if (!strcmp(v, "5"))             return 5;
   if (!strcmp(v, "6"))             return 6;
   if (!strcmp(v, "7"))             return 7;
   return -1;
}

/* Pull every option's current value into the live g_opt_* state. Called once
 * after boot and whenever the frontend flags an options change. Returns 1 if
 * the video geometry (aspect/border) may have changed. */
static int lr_apply_options(void)
{
   const char *v;
   int geometry_dirty = 0;
   int new_aspect, new_border, slot;

   v = lr_get_option(LR_KEY_AUDIO);
   g_opt_audio = (v && !strcmp(v, "disabled")) ? 0 : 1;
   if (g_lr_kegs_inited)
      g_audio_enable = g_opt_audio;

   v = lr_get_option(LR_KEY_DPAD);
   g_opt_dpad = (v && !strcmp(v, "joystick")) ? LR_DPAD_JOYSTICK : LR_DPAD_ARROWS;

   v = lr_get_option(LR_KEY_MOUSE);
   g_opt_mouse = (v && !strcmp(v, "disabled")) ? LR_MOUSE_DISABLED
                                               : LR_MOUSE_RELATIVE;

   v = lr_get_option(LR_KEY_ASPECT);
   new_aspect = (v && !strcmp(v, "pixel_perfect")) ? LR_ASPECT_PP
                                                   : LR_ASPECT_43;
   if (new_aspect != g_opt_aspect) { g_opt_aspect = new_aspect; geometry_dirty = 1; }

   v = lr_get_option(LR_KEY_BORDER);
   new_border = lr_border_from_option(v);
   if (new_border != g_opt_border)
   {
      g_opt_border = new_border;
      geometry_dirty = 1;   /* crop toggles the reported base_width/height */
      /* Leaving Black/Crop for Show must repaint the border region, which the
       * core only redraws on dirty rects -- force a full refresh. */
      if (g_lr_kimage)
         video_set_x_refresh_needed(g_lr_kimage, 1);
   }

   v = lr_get_option(LR_KEY_BOOTSLOT);
   slot = lr_boot_slot_from_option(v);
   if (slot != g_opt_boot_slot)
   {
      g_opt_boot_slot   = slot;
      g_boot_slot_apply = slot;    /* -1 => nothing to do */
      g_boot_slot_done  = 0;       /* re-apply on next valid BRAM */
   }

   return geometry_dirty;
}

/* ------------------------------------------------------------------ */
/*  Boot slot via battery RAM                                          */
/* ------------------------------------------------------------------ */
/*
 * The IIgs reads its startup slot from battery-RAM byte $28 (0 = Scan,
 * 1..7 = slot, 8 = RAM disk, 9 = ROM disk). The ROM validates a checksum over
 * the BRAM on power-up and, if it fails, discards the whole BRAM and rewrites
 * defaults -- so poking $28 also means recomputing that checksum.
 *
 * The checksum lives at $FC (low), $FD (high), with $FE/$FF holding the same
 * word XOR $AAAA. Algorithm (from the IIgs firmware BCHECKSUM routine, verified
 * byte-for-byte against a ROM-written BRAM): a 16-bit accumulator starts at 0;
 * for X = $FA down to $00 (stepping one byte, so the words overlap) rotate the
 * accumulator left through carry, then add-with-carry the little-endian word at
 * (X, X+1). The subsequent `cpx #$FF` clears the carry into the next rotate, so
 * the add's carry-out does not chain into the following rotate.
 */
static word32 lr_bram_checksum(const byte *b)
{
   word32 acc = 0;
   int    carry_in = 0;   /* into the first rol; firmware enters with C=0 */
   int    x = 0xFA;

   for (;;)
   {
      int    rol_carry_out = (acc >> 15) & 1;
      word32 word;

      acc  = ((acc << 1) | carry_in) & 0xffff;   /* rol acc */
      word = b[x] | (b[x + 1] << 8);
      acc  = (acc + word + rol_carry_out) & 0xffff;   /* adc word,x */
      carry_in = 0;   /* cpx #$FF leaves C clear for the next rol */

      if (x == 0x00)
         break;
      x--;
   }
   return acc;
}

static int lr_bram_valid(const byte *b)
{
   word32 cs = lr_bram_checksum(b);
   return (b[0xfc] == (byte)(cs & 0xff)) && (b[0xfd] == (byte)((cs >> 8) & 0xff));
}

/* Apply a pending startup-slot change once the ROM (or the loaded config) has
 * produced a valid BRAM. Poking $28 into an invalid/uninitialized BRAM would be
 * wiped by the ROM's power-up reset, so this waits for a valid checksum, then
 * writes $28, re-checksums, and resets so the change takes effect. Runs each
 * frame until it succeeds (g_boot_slot_done). */
static void lr_apply_boot_slot(void)
{
   word32 cs;

   if (g_boot_slot_apply < 0 || g_boot_slot_done)
      return;                       /* "Default": leave the machine's setting */
   if (!g_bram_ptr || !lr_bram_valid(g_bram_ptr))
      return;                       /* wait until the BRAM is initialized */

   if (g_bram_ptr[0x28] != (byte)g_boot_slot_apply)
   {
      g_bram_ptr[0x28] = (byte)g_boot_slot_apply;
      cs = lr_bram_checksum(g_bram_ptr);
      g_bram_ptr[0xfc] = (byte)(cs & 0xff);
      g_bram_ptr[0xfd] = (byte)((cs >> 8) & 0xff);
      g_bram_ptr[0xfe] = (byte)((cs & 0xff) ^ 0xaa);
      g_bram_ptr[0xff] = (byte)(((cs >> 8) & 0xff) ^ 0xaa);
      do_reset();
      log_cb(RETRO_LOG_INFO,
             "GSplus: startup slot -> %d (BRAM $28); resetting.\n",
             g_boot_slot_apply);
   }
   g_boot_slot_done = 1;
}

/* ------------------------------------------------------------------ */
/*  Disk control (swap disks from the RetroArch menu / .m3u lists)     */
/* ------------------------------------------------------------------ */

#define LR_MAX_DISKS 16
#define LR_PATH_MAX  1024

static char     g_disk_paths[LR_MAX_DISKS][LR_PATH_MAX];
static unsigned g_disk_count;
static unsigned g_disk_index;        /* image the tray will insert */
static int      g_disk_ejected;      /* tray open? */
static int      g_disk_slot = 7;     /* IIgs slot the set swaps through */
static int      g_disk_drive;        /* drive 0 (== the user's "drive 1") */
static char     g_disk_initial_path[LR_PATH_MAX];   /* frontend restore hint */
static unsigned g_disk_initial_index;

/* Same slot mapping as drag-and-drop: 5.25" -> s6, 3.5" -> s5, else SmartPort. */
static int lr_disk_slot_for(const char *path)
{
   switch (cfg_guess_image_size(path))
   {
      case 1:  return 6;
      case 2:  return 5;
      default: return 7;
   }
}

static bool RETRO_CALLCONV lr_disk_set_eject(bool ejected)
{
   int want = ejected ? 1 : 0;

   if (!g_lr_kegs_inited)
      return false;
   if (want == g_disk_ejected)
      return true;
   g_disk_ejected = want;

   if (ejected)
   {
      iwm_eject_disk_by_num(g_disk_slot, g_disk_drive);
   }
   else if (g_disk_index < g_disk_count && g_disk_paths[g_disk_index][0])
   {
      g_disk_slot = lr_disk_slot_for(g_disk_paths[g_disk_index]);
      cfg_maybe_insert_disk(g_disk_slot, g_disk_drive,
                            g_disk_paths[g_disk_index]);
   }
   return true;
}

static bool RETRO_CALLCONV lr_disk_get_eject(void)
{
   return g_disk_ejected ? true : false;
}

static unsigned RETRO_CALLCONV lr_disk_get_image_index(void)
{
   return g_disk_index;
}

static bool RETRO_CALLCONV lr_disk_set_image_index(unsigned index)
{
   /* index == count is the frontend's "no disk"; the actual media change
    * happens when the tray closes (lr_disk_set_eject(false)). */
   if (index > g_disk_count)
      return false;
   g_disk_index = index;
   return true;
}

static unsigned RETRO_CALLCONV lr_disk_get_num_images(void)
{
   return g_disk_count;
}

static bool RETRO_CALLCONV lr_disk_replace_image_index(unsigned index,
      const struct retro_game_info *info)
{
   if (index >= g_disk_count)
      return false;

   if (info && info->path && info->path[0])
   {
      strncpy(g_disk_paths[index], info->path, LR_PATH_MAX - 1);
      g_disk_paths[index][LR_PATH_MAX - 1] = 0;
   }
   else
   {
      /* Remove this entry, shifting the rest down. */
      unsigned i;
      for (i = index; i + 1 < g_disk_count; i++)
         strcpy(g_disk_paths[i], g_disk_paths[i + 1]);
      g_disk_count--;
      if (g_disk_index >= g_disk_count && g_disk_index)
         g_disk_index = g_disk_count ? g_disk_count - 1 : 0;
   }
   return true;
}

static bool RETRO_CALLCONV lr_disk_add_image_index(void)
{
   if (g_disk_count >= LR_MAX_DISKS)
      return false;
   g_disk_paths[g_disk_count][0] = 0;
   g_disk_count++;
   return true;
}

static bool RETRO_CALLCONV lr_disk_set_initial_image(unsigned index,
      const char *path)
{
   g_disk_initial_index = index;
   if (path)
   {
      strncpy(g_disk_initial_path, path, LR_PATH_MAX - 1);
      g_disk_initial_path[LR_PATH_MAX - 1] = 0;
   }
   else
      g_disk_initial_path[0] = 0;
   return true;
}

static bool RETRO_CALLCONV lr_disk_get_image_path(unsigned index, char *s,
      size_t len)
{
   if (index >= g_disk_count || !g_disk_paths[index][0])
      return false;
   strncpy(s, g_disk_paths[index], len - 1);
   s[len - 1] = 0;
   return true;
}

static bool RETRO_CALLCONV lr_disk_get_image_label(unsigned index, char *s,
      size_t len)
{
   const char *base, *p;

   if (index >= g_disk_count || !g_disk_paths[index][0])
      return false;

   base = g_disk_paths[index];
   for (p = base; *p; p++)
   {
      if (*p == '/' || *p == '\\')
         base = p + 1;
   }
   strncpy(s, base, len - 1);
   s[len - 1] = 0;
   return true;
}

static void lr_register_disk_control(void)
{
   unsigned version = 0;

   struct retro_disk_control_ext_callback ext;
   ext.set_eject_state     = lr_disk_set_eject;
   ext.get_eject_state     = lr_disk_get_eject;
   ext.get_image_index     = lr_disk_get_image_index;
   ext.set_image_index     = lr_disk_set_image_index;
   ext.get_num_images      = lr_disk_get_num_images;
   ext.replace_image_index = lr_disk_replace_image_index;
   ext.add_image_index     = lr_disk_add_image_index;
   ext.set_initial_image   = lr_disk_set_initial_image;
   ext.get_image_path      = lr_disk_get_image_path;
   ext.get_image_label     = lr_disk_get_image_label;

   if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION,
                  &version) && version >= 1)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &ext);
   }
   else
   {
      struct retro_disk_control_callback base;
      base.set_eject_state     = lr_disk_set_eject;
      base.get_eject_state     = lr_disk_get_eject;
      base.get_image_index     = lr_disk_get_image_index;
      base.set_image_index     = lr_disk_set_image_index;
      base.get_num_images      = lr_disk_get_num_images;
      base.replace_image_index = lr_disk_replace_image_index;
      base.add_image_index     = lr_disk_add_image_index;
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &base);
   }
}

static void lr_disk_add(const char *path)
{
   if (g_disk_count >= LR_MAX_DISKS || !path || !path[0])
      return;
   strncpy(g_disk_paths[g_disk_count], path, LR_PATH_MAX - 1);
   g_disk_paths[g_disk_count][LR_PATH_MAX - 1] = 0;
   g_disk_count++;
}

static int lr_has_ext(const char *path, const char *ext)
{
   size_t lp = strlen(path), le = strlen(ext);
   const char *s = path + lp - le;
   size_t i;

   if (lp < le)
      return 0;
   for (i = 0; i < le; i++)
   {
      char a = s[i], b = ext[i];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (a != b)
         return 0;
   }
   return 1;
}

/* Build the swap list from the content path: an .m3u playlist expands into its
 * listed images (each resolved relative to the playlist directory); any other
 * file is a one-image set. */
static void lr_populate_disks(const char *content_path)
{
   g_disk_count = 0;
   g_disk_index = 0;
   g_disk_ejected = 0;

   if (lr_has_ext(content_path, ".m3u"))
   {
      FILE *f = fopen(content_path, "r");
      char  dir[LR_PATH_MAX];
      char  line[LR_PATH_MAX];
      const char *p, *slash;

      /* Directory of the .m3u, for resolving relative entries. */
      dir[0] = 0;
      slash = NULL;
      for (p = content_path; *p; p++)
         if (*p == '/' || *p == '\\')
            slash = p;
      if (slash)
      {
         size_t n = (size_t)(slash - content_path + 1);
         if (n >= sizeof(dir))
            n = sizeof(dir) - 1;
         memcpy(dir, content_path, n);
         dir[n] = 0;
      }

      if (f)
      {
         while (fgets(line, sizeof(line), f))
         {
            char full[LR_PATH_MAX];
            size_t n = strlen(line);

            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' '  || line[n - 1] == '\t'))
               line[--n] = 0;
            if (!n || line[0] == '#')
               continue;   /* blank or #EXTM3U/comment */

            if (line[0] == '/' || line[0] == '\\' ||
                (n > 1 && line[1] == ':'))          /* absolute */
               snprintf(full, sizeof(full), "%s", line);
            else
               snprintf(full, sizeof(full), "%s%s", dir, line);
            lr_disk_add(full);
         }
         fclose(f);
      }
   }

   if (g_disk_count == 0)
      lr_disk_add(content_path);   /* single image, or unreadable .m3u */

   /* Honor a frontend restore hint (last-used disk), matching by path. */
   if (g_disk_initial_path[0])
   {
      unsigned i;
      for (i = 0; i < g_disk_count; i++)
      {
         if (!strcmp(g_disk_paths[i], g_disk_initial_path))
         {
            g_disk_index = i;
            break;
         }
      }
   }
   else if (g_disk_initial_index < g_disk_count)
   {
      g_disk_index = g_disk_initial_index;
   }
}

/* ------------------------------------------------------------------ */
/*  Geometry (aspect ratio + border crop)                              */
/* ------------------------------------------------------------------ */

/* Locate the 640x400 active picture inside g_lr_vbuf, in current output-
 * resolution coordinates (the a2-space margins scaled by the output/a2 ratio).
 * Returns 1 and fills the clamped rect, or 0 if the geometry isn't known yet. */
static int lr_active_rect(int *x, int *y, int *w, int *h)
{
   int a2w, a2h;

   if (!g_lr_kimage)
      return 0;
   a2w = video_get_a2_width(g_lr_kimage);
   a2h = video_get_a2_height(g_lr_kimage);
   if (a2w <= 0 || a2h <= 0)
      return 0;

   *x = g_video_act_margin_left * g_lr_width  / a2w;
   *y = g_video_act_margin_top  * g_lr_height / a2h;
   *w = A2_WINDOW_WIDTH  * g_lr_width  / a2w;
   *h = A2_WINDOW_HEIGHT * g_lr_height / a2h;
   if (*w <= 0 || *h <= 0)
      return 0;
   if (*x + *w > g_lr_width)  *w = g_lr_width  - *x;
   if (*y + *h > g_lr_height) *h = g_lr_height - *y;
   return 1;
}

/* Compute the visible rectangle inside g_lr_vbuf. Full frame for Show/Black;
 * for Crop, just the 640x400 active picture. */
static void lr_compute_view(int *ox, int *oy, int *ow, int *oh)
{
   int x, y, w, h;

   *ox = 0;
   *oy = 0;
   *ow = g_lr_width;
   *oh = g_lr_height;

   if (g_opt_border == LR_BORDER_CROP && lr_active_rect(&x, &y, &w, &h))
   {
      *ox = x; *oy = y; *ow = w; *oh = h;
   }
}

/* Black border mode: overwrite everything outside the active picture with black
 * (XRGB8888 0) so the surround is a steady frame instead of the flashing IIgs
 * border. Runs each frame after the core's dirty rects land. */
static void lr_paint_border_black(void)
{
   int x, y, w, h, row;

   if (!g_lr_vbuf)
      return;
   /* If the active rect is unknown, leave the frame untouched rather than
    * blacking the whole picture. */
   if (!lr_active_rect(&x, &y, &w, &h))
      return;

   for (row = 0; row < y; row++)                    /* top band    */
      memset(g_lr_vbuf + (size_t)row * g_lr_width, 0,
             (size_t)g_lr_width * sizeof(word32));
   for (row = y + h; row < g_lr_height; row++)       /* bottom band */
      memset(g_lr_vbuf + (size_t)row * g_lr_width, 0,
             (size_t)g_lr_width * sizeof(word32));
   for (row = y; row < y + h; row++)                 /* side bands  */
   {
      word32 *line = g_lr_vbuf + (size_t)row * g_lr_width;
      if (x > 0)
         memset(line, 0, (size_t)x * sizeof(word32));
      if (x + w < g_lr_width)
         memset(line + x + w, 0,
                (size_t)(g_lr_width - (x + w)) * sizeof(word32));
   }
}

static float lr_aspect_ratio(int ow, int oh)
{
   if (g_opt_aspect == LR_ASPECT_PP && ow > 0 && oh > 0)
      return (float)ow / (float)oh;      /* square pixels */
   return 4.0f / 3.0f;
}

static void lr_update_geometry(void)
{
   struct retro_game_geometry geom;
   int ox, oy, ow, oh;

   if (!environ_cb)
      return;

   lr_compute_view(&ox, &oy, &ow, &oh);
   memset(&geom, 0, sizeof(geom));
   geom.base_width   = ow;
   geom.base_height  = oh;
   geom.max_width    = LR_MAX_WIDTH;
   geom.max_height   = LR_MAX_HEIGHT;
   geom.aspect_ratio = lr_aspect_ratio(ow, oh);
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
}

/* ------------------------------------------------------------------ */
/*  Basic entry points                                                 */
/* ------------------------------------------------------------------ */

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

static void RETRO_CALLCONV lr_keyboard_event(bool down, unsigned keycode,
      uint32_t character, uint16_t key_modifiers);

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   bool no_content = true;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   /* Advertise the core options up front so they appear even before content is
    * loaded (and with SET_SUPPORT_NO_GAME). */
   lr_set_core_options();

   /* Physical keyboard: RetroArch delivers key events through this callback
    * (they can fire outside retro_run), so we queue them and drain at frame
    * start. Full capture needs RetroArch's "Game Focus" mode -- otherwise the
    * frontend keeps some keys for its own hotkeys. */
   {
      struct retro_keyboard_callback kbd = { lr_keyboard_event };
      cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kbd);
   }
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
   info->library_version  = "0.3-phase3";
   info->need_fullpath    = true;   /* KEGS opens disk/ROM files by path */
   info->block_extract    = true;   /* .woz/.2mg etc. must not be unzipped */
   info->valid_extensions = "hdv|po|2mg|dsk|do|nib|woz|m3u";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   int ox, oy, ow, oh;

   if (g_lr_width > 0 && g_lr_height > 0)
   {
      lr_compute_view(&ox, &oy, &ow, &oh);
   }
   else
   {
      ow = 640;
      oh = 400;
   }

   memset(info, 0, sizeof(*info));
   info->timing.fps          = VBL_RATE;     /* ~59.9227 Hz */
   info->timing.sample_rate  = 48000.0;
   info->geometry.base_width   = ow;
   info->geometry.base_height  = oh;
   info->geometry.max_width    = LR_MAX_WIDTH;
   info->geometry.max_height   = LR_MAX_HEIGHT;
   info->geometry.aspect_ratio = lr_aspect_ratio(ow, oh);
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_init(void)
{
   /* RetroArch calls retro_run() once per frame, so the core must not also
    * sleep to pace itself -- disable its micro_sleep(). Everything else
    * (KEGS init, video/audio setup) is deferred to retro_load_game(), where
    * the environment callbacks needed to find the ROM/save dirs are live. */
   g_micro_sleep_disable = 1;
}

void retro_deinit(void)
{
   free(g_lr_vbuf);
   g_lr_vbuf = NULL;
}

/* ------------------------------------------------------------------ */
/*  Content load / unload                                              */
/* ------------------------------------------------------------------ */

/* Show an on-screen message, preferring the richer SET_MESSAGE_EXT interface
 * (priority, severity, duration in ms) and falling back to SET_MESSAGE. */
static void lr_show_message(const char *text, unsigned ms,
      enum retro_log_level level)
{
   unsigned version = 0;

   if (!environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version) &&
       version >= 1)
   {
      struct retro_message_ext msg;
      memset(&msg, 0, sizeof(msg));
      msg.msg      = text;
      msg.duration = ms;
      msg.priority = 3;
      msg.level    = level;
      msg.target   = RETRO_MESSAGE_TARGET_ALL;
      msg.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
      msg.progress = -1;
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
   }
   else
   {
      struct retro_message msg;
      msg.msg    = text;
      msg.frames = (unsigned)(VBL_RATE * ms / 1000.0);
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
   }
}

/* Build the KEGS command line and boot it. Returns 0 on success. */
static int lr_boot_kegs(void)
{
   const char *sysdir  = NULL;
   const char *savedir = NULL;
   char rompath[1024];
   char cfgpath[1024];
   int  have_rom = 0;

   char *argv[16];
   int   argc = 0;

   /* ROM: probe the frontend's system directory for the usual names. KEGS
    * translates "-rom <path>" into the g_cfg_rom_path config override. A miss
    * is non-fatal -- the core drops into its built-in config panel (rendered
    * through the normal framebuffer), a usable "install your ROM" screen. */
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) && sysdir)
   {
      static const char *const rom_names[] = {
         "gsplus/APPLE2GS.ROM", "APPLE2GS.ROM",
         "gsplus/ROM.03", "gsplus/ROM.01"
      };
      unsigned i;
      for (i = 0; i < sizeof(rom_names) / sizeof(rom_names[0]); i++)
      {
         FILE *f;
         snprintf(rompath, sizeof(rompath), "%s/%s", sysdir, rom_names[i]);
         f = fopen(rompath, "rb");
         if (f)
         {
            fclose(f);
            have_rom = 1;
            break;
         }
      }
   }

   /* Config: keep config.kegs in the save directory so we never litter the
    * frontend's cwd. Fall back to the system dir, then the cwd. */
   if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &savedir) || !savedir)
      savedir = sysdir;
   if (savedir)
      snprintf(cfgpath, sizeof(cfgpath), "%s/gsplus-config.kegs", savedir);
   else
      snprintf(cfgpath, sizeof(cfgpath), "gsplus-config.kegs");

   argv[argc++] = (char *)"gsplus_libretro";
   argv[argc++] = (char *)"-audio";
   argv[argc++] = (char *)"1";
   if (have_rom)
   {
      argv[argc++] = (char *)"-rom";
      argv[argc++] = rompath;
   }
   argv[argc++] = (char *)"-cfg";
   argv[argc++] = cfgpath;

   if (parse_argv(argc, argv, 1))
   {
      log_cb(RETRO_LOG_ERROR, "GSplus: parse_argv failed.\n");
      return -1;
   }

   if (kegs_init(32, LR_MAX_WIDTH, LR_MAX_HEIGHT, 1))
   {
      log_cb(RETRO_LOG_ERROR, "GSplus: kegs_init failed.\n");
      return -1;
   }

   if (!have_rom)
   {
      lr_show_message("No Apple IIgs ROM found. Put APPLE2GS.ROM (or ROM.01 / "
                      "ROM.03) in RetroArch's System directory, then reload.",
                      10000, RETRO_LOG_WARN);
      log_cb(RETRO_LOG_WARN,
             "GSplus: no ROM in system dir; showing config panel. Looked for "
             "APPLE2GS.ROM / ROM.03 / ROM.01 under the System directory.\n");
   }

   /* Video: tell the core our pixel layout (0x00RRGGBB with opaque alpha) and
    * build the palettes -- mirrors sdl_video_init(). Without video_set_palette()
    * the Apple II text/lores/hires palette stays black. */
   g_lr_kimage = video_get_kimage(0);

   /* Drop the status lines before reading the frame size, overriding any
    * persisted config value, so the first frame already has the final
    * 704x462 geometry. */
   g_status_enable          = 0;
   g_status_enable_previous = 0;
   video_update_status_enable(g_lr_kimage);

   g_lr_width  = video_get_x_width(g_lr_kimage);
   g_lr_height = video_get_x_height(g_lr_kimage);

   video_set_red_mask(0xff0000);
   video_set_green_mask(0x00ff00);
   video_set_blue_mask(0x0000ff);
   video_set_alpha_mask(0xff000000);
   video_set_palette();

   g_lr_vbuf = (word32 *)calloc((size_t)LR_MAX_WIDTH * LR_MAX_HEIGHT,
                                sizeof(word32));

   video_update_scale(g_lr_kimage, g_lr_width, g_lr_height, 1);
   video_set_active(g_lr_kimage, 1);
   /* Force a full first frame: static content (e.g. the config panel) is drawn
    * once during kegs_init and never re-dirties, so without this the frame
    * would stay blank. */
   video_set_x_refresh_needed(g_lr_kimage, 1);

   return 0;
}

static const struct retro_input_descriptor g_input_descriptors[];

bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "GSplus: XRGB8888 pixel format not supported.\n");
      return false;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
              (void *)g_input_descriptors);

   if (!g_lr_kegs_inited)
   {
      if (lr_boot_kegs())
         return false;
      g_lr_kegs_inited = 1;
   }
   else
   {
      /* KEGS globals cannot be re-initialized in-process; a second load just
       * remounts and resets. (RetroArch normally dlcloses the core between
       * content, so this path is only hit on a same-process reload.) */
      do_reset();
   }

   /* Build the disk-swap list from the content (expanding an .m3u playlist),
    * register the disk-control interface, then mount the initial image into its
    * guessed slot (5.25" -> s6, 3.5" -> s5, small/hard-drive images -> s7). */
   if (info && info->path && info->path[0])
      lr_populate_disks(info->path);
   else
   {
      g_disk_count = 0;
      g_disk_index = 0;
      g_disk_ejected = 0;
   }
   lr_register_disk_control();

   if (g_disk_count > 0 && g_disk_paths[g_disk_index][0])
   {
      g_disk_slot = lr_disk_slot_for(g_disk_paths[g_disk_index]);
      cfg_maybe_insert_disk(g_disk_slot, g_disk_drive,
                            g_disk_paths[g_disk_index]);
   }

   /* Now that the machine is up, read the initial option values (audio, dpad,
    * aspect, border, startup slot) and refresh the reported geometry. */
   lr_apply_options();
   lr_update_geometry();

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
/*  Input: keyboard, RetroPad, mouse                                   */
/* ------------------------------------------------------------------ */
/*
 * The IIgs core takes raw ADB key codes (0x00-0x7f). This table is the
 * SDL driver's g_sdl_key_map (sdl_driver.c) re-keyed from SDL scancodes to
 * RETROK_* values -- same a2 codes, so behaviour matches the SDL build.
 */
struct lr_key_map { unsigned retrok; int a2; };

static const struct lr_key_map g_retro_key_map[] = {
   { RETROK_ESCAPE, 0x35 },
   { RETROK_F1, 0x7a }, { RETROK_F2, 0x78 }, { RETROK_F3, 0x63 },
   { RETROK_F4, 0x76 }, { RETROK_F5, 0x60 }, { RETROK_F6, 0x61 },
   { RETROK_F7, 0x62 }, { RETROK_F8, 0x64 }, { RETROK_F9, 0x65 },
   { RETROK_F10, 0x6d }, { RETROK_F11, 0x67 }, { RETROK_F12, 0x6f },
   { RETROK_F13, 0x69 }, { RETROK_F14, 0x6b }, { RETROK_F15, 0x71 },
   { RETROK_PAUSE, 0x7f },

   { RETROK_BACKQUOTE, 0x32 },
   { RETROK_1, 0x12 }, { RETROK_2, 0x13 }, { RETROK_3, 0x14 },
   { RETROK_4, 0x15 }, { RETROK_5, 0x17 }, { RETROK_6, 0x16 },
   { RETROK_7, 0x1a }, { RETROK_8, 0x1c }, { RETROK_9, 0x19 },
   { RETROK_0, 0x1d }, { RETROK_MINUS, 0x1b }, { RETROK_EQUALS, 0x18 },
   { RETROK_BACKSPACE, 0x33 },

   { RETROK_INSERT, 0x72 }, { RETROK_HOME, 0x73 }, { RETROK_PAGEUP, 0x74 },

   { RETROK_TAB, 0x30 },
   { RETROK_q, 0x0c }, { RETROK_w, 0x0d }, { RETROK_e, 0x0e },
   { RETROK_r, 0x0f }, { RETROK_t, 0x11 }, { RETROK_y, 0x10 },
   { RETROK_u, 0x20 }, { RETROK_i, 0x22 }, { RETROK_o, 0x1f },
   { RETROK_p, 0x23 }, { RETROK_LEFTBRACKET, 0x21 },
   { RETROK_RIGHTBRACKET, 0x1e }, { RETROK_BACKSLASH, 0x2a },
   { RETROK_DELETE, 0x75 }, { RETROK_END, 0x77 }, { RETROK_PAGEDOWN, 0x79 },

   { RETROK_CAPSLOCK, 0x39 },
   { RETROK_a, 0x00 }, { RETROK_s, 0x01 }, { RETROK_d, 0x02 },
   { RETROK_f, 0x03 }, { RETROK_g, 0x05 }, { RETROK_h, 0x04 },
   { RETROK_j, 0x26 }, { RETROK_k, 0x28 }, { RETROK_l, 0x25 },
   { RETROK_SEMICOLON, 0x29 }, { RETROK_QUOTE, 0x27 },
   { RETROK_RETURN, 0x24 },

   { RETROK_LSHIFT, 0x38 }, { RETROK_RSHIFT, 0x38 },
   { RETROK_z, 0x06 }, { RETROK_x, 0x07 }, { RETROK_c, 0x08 },
   { RETROK_v, 0x09 }, { RETROK_b, 0x0b }, { RETROK_n, 0x2d },
   { RETROK_m, 0x2e }, { RETROK_COMMA, 0x2b }, { RETROK_PERIOD, 0x2f },
   { RETROK_SLASH, 0x2c },

   { RETROK_LCTRL, 0x36 }, { RETROK_RCTRL, 0x36 },
   { RETROK_LALT, 0x3a }, { RETROK_RALT, 0x3a },      /* Option / Solid-Apple */
   { RETROK_LMETA, 0x37 }, { RETROK_RMETA, 0x37 },    /* Open-Apple */
   { RETROK_LSUPER, 0x37 }, { RETROK_RSUPER, 0x37 },  /* Open-Apple */
   { RETROK_SPACE, 0x31 },
   { RETROK_UP, 0x3e }, { RETROK_DOWN, 0x3d },
   { RETROK_LEFT, 0x3b }, { RETROK_RIGHT, 0x3c },

   /* Numeric keypad */
   { RETROK_NUMLOCK, 0x47 }, { RETROK_KP_EQUALS, 0x51 },
   { RETROK_KP_DIVIDE, 0x4b }, { RETROK_KP_MULTIPLY, 0x43 },
   { RETROK_KP7, 0x59 }, { RETROK_KP8, 0x5b }, { RETROK_KP9, 0x5c },
   { RETROK_KP_MINUS, 0x4e },
   { RETROK_KP4, 0x56 }, { RETROK_KP5, 0x57 }, { RETROK_KP6, 0x58 },
   { RETROK_KP_PLUS, 0x45 },
   { RETROK_KP1, 0x53 }, { RETROK_KP2, 0x54 }, { RETROK_KP3, 0x55 },
   { RETROK_KP0, 0x52 }, { RETROK_KP_PERIOD, 0x41 }, { RETROK_KP_ENTER, 0x4c },

   { RETROK_UNKNOWN, -1 }   /* terminator */
};

static int lr_retrok_to_a2code(unsigned keycode)
{
   int i;
   for (i = 0; g_retro_key_map[i].a2 >= 0; i++)
   {
      if (g_retro_key_map[i].retrok == keycode)
         return g_retro_key_map[i].a2;
   }
   return -1;
}

/* Key-event ring buffer: RetroArch's keyboard callback can fire outside
 * retro_run(), so events are queued here and drained at the next frame start.
 * Single-producer/single-consumer, same thread in practice -- no locking. */
struct lr_key_event { unsigned keycode; uint16_t mods; int down; };
#define LR_KEY_RING_SIZE 256   /* power of two */
static struct lr_key_event g_key_ring[LR_KEY_RING_SIZE];
static unsigned g_key_ring_head;   /* producer writes here */
static unsigned g_key_ring_tail;   /* consumer reads here */

static void RETRO_CALLCONV lr_keyboard_event(bool down, unsigned keycode,
      uint32_t character, uint16_t key_modifiers)
{
   unsigned next;
   (void)character;

   next = (g_key_ring_head + 1) & (LR_KEY_RING_SIZE - 1);
   if (next == g_key_ring_tail)
      return;   /* ring full: drop the oldest-blocking event */

   g_key_ring[g_key_ring_head].keycode = keycode;
   g_key_ring[g_key_ring_head].mods    = key_modifiers;
   g_key_ring[g_key_ring_head].down    = down ? 1 : 0;
   g_key_ring_head = next;
}

/* Translate a RETROKMOD set into the IIgs c025 modifier register
 * (bit0 = shift, bit1 = control, bit2 = caps lock), like sdl_update_modifiers. */
static void lr_apply_key_modifiers(uint16_t mods)
{
   word32 c025_val = 0;

   if (mods & RETROKMOD_SHIFT)    c025_val |= 1;
   if (mods & RETROKMOD_CTRL)     c025_val |= 2;
   if (mods & RETROKMOD_CAPSLOCK) c025_val |= 4;
   adb_update_c025_mask(g_lr_kimage, c025_val, 7);
}

/* Drain queued key events into the emulated ADB keyboard. */
static void lr_drain_keyboard(void)
{
   if (!g_lr_kimage)
   {
      g_key_ring_tail = g_key_ring_head;   /* discard until the core is up */
      return;
   }

   while (g_key_ring_tail != g_key_ring_head)
   {
      struct lr_key_event *ev = &g_key_ring[g_key_ring_tail];
      int a2code = lr_retrok_to_a2code(ev->keycode);

      lr_apply_key_modifiers(ev->mods);
      if (a2code >= 0)
         adb_physical_key_update(g_lr_kimage, a2code, 0, ev->down ? 0 : 1);

      g_key_ring_tail = (g_key_ring_tail + 1) & (LR_KEY_RING_SIZE - 1);
   }
}

/* RetroPad digital buttons that stand in for IIgs keys: the D-pad drives the
 * arrow keys and Start is Return, so menus that expect a keyboard are navigable
 * with a pad alone. (Select is reserved for the Phase 4 OSK toggle.) The left
 * analog stick and A/B still drive the paddle joystick via joystick_update(). */
static const struct lr_key_map g_pad_key_map[] = {
   { RETRO_DEVICE_ID_JOYPAD_UP,    0x3e },   /* up arrow */
   { RETRO_DEVICE_ID_JOYPAD_DOWN,  0x3d },   /* down arrow */
   { RETRO_DEVICE_ID_JOYPAD_LEFT,  0x3b },   /* left arrow */
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, 0x3c },   /* right arrow */
   { RETRO_DEVICE_ID_JOYPAD_START, 0x24 },   /* Return */
   { 0, -1 }                                  /* terminator */
};
static int g_pad_key_prev[8];   /* pressed state, indexed by table slot */

static void lr_poll_pad_keys(void)
{
   int i;
   if (!g_lr_kimage || !input_state_cb)
      return;

   for (i = 0; g_pad_key_map[i].a2 >= 0; i++)
   {
      int a2       = g_pad_key_map[i].a2;
      int is_arrow = (a2 >= 0x3b && a2 <= 0x3e);
      int pressed;

      /* In "Joystick" D-pad mode the directions drive the paddle (see
       * joystick_update), so suppress the arrow-key emulation -- reporting them
       * as up also releases any arrow held when the mode was switched. */
      if (is_arrow && g_opt_dpad == LR_DPAD_JOYSTICK)
         pressed = 0;
      else
         pressed = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
                                  g_pad_key_map[i].retrok) ? 1 : 0;

      if (pressed != g_pad_key_prev[i])
      {
         adb_physical_key_update(g_lr_kimage, a2, 0, pressed ? 0 : 1);
         g_pad_key_prev[i] = pressed;
      }
   }
}

/* RETRO_DEVICE_MOUSE reports per-frame relative motion + button state; feed it
 * to the emulator in delta mode (buttons_valid | 0x1000), mirroring the SDL
 * mouse handler. Left -> IIgs button 0 (mask 1), right -> button 4. */
static int g_mouse_prev_mask;

static void lr_poll_mouse(void)
{
   int dx, dy, mask;
   if (!g_lr_kimage || !input_state_cb)
      return;
   if (g_opt_mouse == LR_MOUSE_DISABLED)
      return;

   dx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
   dy = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
   mask = 0;
   if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT))
      mask |= 1;
   if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT))
      mask |= 4;

   if (dx || dy || mask != g_mouse_prev_mask)
   {
      /* buttons_valid: bits 0 and 2 valid + 0x1000 = deltas, not absolute. */
      adb_update_mouse(g_lr_kimage, dx, dy, mask, 0x1000 | 1 | 4);
      g_mouse_prev_mask = mask;
   }
}

/* RetroArch input descriptors: label the pad buttons the core actually uses. */
static const struct retro_input_descriptor g_input_descriptors[] = {
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left (Arrow)" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up (Arrow)" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down (Arrow)" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right (Arrow)" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 0 (Open-Apple)" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 1 (Solid-Apple)" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Return" },
   { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
        RETRO_DEVICE_ID_ANALOG_X, "Joystick X" },
   { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
        RETRO_DEVICE_ID_ANALOG_Y, "Joystick Y" },
   { 0, 0, 0, 0, NULL }
};

/* ------------------------------------------------------------------ */
/*  Run loop                                                           */
/* ------------------------------------------------------------------ */

/* Growable S16 interleaved-stereo buffer for one frame's audio, filled by
 * libretro_send_audio() (called from the core each VBL) and drained here. */
static int16_t *g_lr_snd_buf;
static size_t   g_lr_snd_cap;    /* capacity in bytes */
static size_t   g_lr_snd_len;    /* used bytes */

void retro_run(void)
{
   int ret, i;
   int ox, oy, ow, oh;
   Change_rect rect;
   bool updated = false;

   /* Pick up any core-option changes made from the RetroArch menu, and apply a
    * pending startup-slot change once the BRAM is valid. */
   if (environ_cb &&
       environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      if (lr_apply_options())
         lr_update_geometry();
   }
   lr_apply_boot_slot();

   if (input_poll_cb)
      input_poll_cb();

   /* Feed queued keyboard events and current pad/mouse state to the emulated
    * ADB before advancing time, so this frame sees them. The analog paddle is
    * sampled lazily instead: joystick_update() reads live state whenever the
    * program strobes the paddle trigger ($C070). */
   lr_drain_keyboard();
   lr_poll_pad_keys();
   lr_poll_mouse();

   /* Advance the emulator by exactly one VBL frame. run_16ms() folds in
    * g_a2_fatal_err; a nonzero return means the emulated machine died. */
   ret = run_16ms();
   if (ret != 0)
   {
      log_cb(RETRO_LOG_ERROR, "GSplus: run_16ms returned %d; shutting down.\n",
             ret);
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
      return;
   }

   /* A IIgs video-mode switch changes the active resolution: adopt the new
    * geometry and ask the core to re-emit the whole screen. */
   if (g_lr_kimage &&
       video_change_aspect_needed(g_lr_kimage, g_lr_width, g_lr_height))
   {
      g_lr_width  = video_get_x_width(g_lr_kimage);
      g_lr_height = video_get_x_height(g_lr_kimage);
      video_update_scale(g_lr_kimage, g_lr_width, g_lr_height, 1);
      video_set_x_refresh_needed(g_lr_kimage, 1);
      lr_update_geometry();
   }

   /* Drain the core's dirty rectangles into our persistent framebuffer, then
    * hand RetroArch the visible rectangle (whole frame, or just the picture when
    * Border=Crop -- a pointer offset + smaller w/h, same full pitch). Border=
    * Black blackens the surround in place before the handoff. */
   lr_compute_view(&ox, &oy, &ow, &oh);
   if (g_lr_kimage && g_lr_vbuf && video_get_active(g_lr_kimage))
   {
      for (i = 0; i < MAX_CHANGE_RECTS; i++)
      {
         if (!video_out_data(g_lr_vbuf, g_lr_kimage, g_lr_width, &rect, i))
            break;
      }
      if (g_opt_border == LR_BORDER_BLACK)
         lr_paint_border_black();
      video_cb(g_lr_vbuf + (size_t)oy * g_lr_width + ox, ow, oh,
               (size_t)g_lr_width * sizeof(word32));
   }
   else
   {
      video_cb(NULL, ow, oh, (size_t)g_lr_width * sizeof(word32));
   }

   /* Audio: hand off exactly what the core produced this frame. audio_batch_cb
    * counts stereo sample *frames* (4 bytes each). If the core produced nothing
    * (audio disabled), emit one frame of silence to keep the A/V timebase. */
   if (audio_batch_cb)
   {
      if (g_lr_snd_len >= 4)
      {
         audio_batch_cb(g_lr_snd_buf, g_lr_snd_len / 4);
      }
      else
      {
         static const int16_t silence[2 * 801];
         audio_batch_cb(silence, 801);
      }
   }
   g_lr_snd_len = 0;
}

void retro_reset(void)
{
   if (g_lr_kegs_inited)
      do_reset();
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
/*  KEGS core seams (audio + inert joystick stubs)                     */
/* ------------------------------------------------------------------ */
/*
 * The core pushes audio once per VBL through child_send_samples(), which with
 * -DLIBRETRO_AUDIO calls libretro_send_audio() below. We append the interleaved
 * S16 samples to a growable buffer that retro_run() drains into audio_batch_cb.
 */

void libretro_snd_init(word32 *shmaddr)
{
   (void)shmaddr;   /* push model: samples arrive via libretro_send_audio() */
   sound_set_audio_rate(g_preferred_rate);
}

int libretro_send_audio(byte *ptr, int size)
{
   if (size <= 0)
      return size;

   if (g_lr_snd_len + (size_t)size > g_lr_snd_cap)
   {
      size_t newcap = g_lr_snd_cap ? g_lr_snd_cap * 2 : 8192;
      while (newcap < g_lr_snd_len + (size_t)size)
         newcap *= 2;
      g_lr_snd_buf = (int16_t *)realloc(g_lr_snd_buf, newcap);
      g_lr_snd_cap = newcap;
   }

   memcpy((byte *)g_lr_snd_buf + g_lr_snd_len, ptr, (size_t)size);
   g_lr_snd_len += (size_t)size;

   /* MUST return the full byte count: reliable_buf_write() calls exit(1)
    * otherwise. */
   return size;
}

/*
 * With -DSDL_INPUT the core's joystick_driver.c compiles out both its native
 * backends and its fallback stubs, so the frontend provides these three entry
 * points. They mirror the SDL gamepad backend (sdl_driver.c): the left analog
 * stick is the IIgs 2-axis paddle joystick, and the two face buttons are its
 * buttons 0/1. The core calls joystick_update() whenever a program strobes the
 * paddle trigger, so we sample RetroArch's live pad state on demand. Reporting
 * a connected native joystick (type1 = 1) lets the config's "Auto" joystick
 * mode route paddle reads here.
 */

extern int g_joystick_native_type1;   /* defined in paddles.c */
extern int g_joystick_native_type2;
extern int g_joystick_native_type;
extern int g_paddle_buttons;
extern int g_paddle_val[];

void joystick_init(void)
{
   /* libretro always presents a virtual RetroPad, so advertise joystick 1 as
    * present -- the frontend maps a real controller (or keyboard) onto it. */
   g_joystick_native_type1 = 1;
   g_joystick_native_type2 = -1;
   g_joystick_native_type  = 1;
}

/* Read the two face buttons into the low bits of g_paddle_buttons. RetroArch's
 * B (bottom) is button 0, A (right) is button 1 -- same layout the SDL backend
 * uses (SDL_GAMEPAD_BUTTON_SOUTH/EAST). */
void joystick_update_buttons(void)
{
   int buttons = 0;
   if (!input_state_cb)
      return;
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
      buttons |= 1;
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
      buttons |= 2;
   g_paddle_buttons = (g_paddle_buttons & ~3) | buttons;
}

void joystick_update(dword64 dfcyc)
{
   int i;

   /* Default: centered X/Y, both buttons up (0xc keeps the unused upper paddle
    * buttons high, matching the native backends). */
   for (i = 0; i < 4; i++)
      g_paddle_val[i] = 32767;
   g_paddle_buttons = 0xc;

   if (!input_state_cb)
      return;

   /* RetroArch analog axes are already -0x8000..0x7fff, exactly the paddle
    * range the core expects (0 = centered when the stick is at rest). */
   g_paddle_val[0] = input_state_cb(0, RETRO_DEVICE_ANALOG,
         RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
   g_paddle_val[1] = input_state_cb(0, RETRO_DEVICE_ANALOG,
         RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

   /* "Joystick" D-pad mode: digital directions deflect the paddle fully,
    * overriding the analog axis so games are playable without an analog stick. */
   if (g_opt_dpad == LR_DPAD_JOYSTICK)
   {
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
         g_paddle_val[0] = -0x8000;
      else if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
         g_paddle_val[0] = 0x7fff;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
         g_paddle_val[1] = -0x8000;
      else if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
         g_paddle_val[1] = 0x7fff;
   }

   joystick_update_buttons();
   paddle_update_trigger_dcycs(dfcyc);
}
