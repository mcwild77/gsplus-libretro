/*
 * gsplus_osk.c -- on-screen keyboard for the GSplus libretro core.
 *
 * Controller-driven virtual keyboard for the Apple IIgs. Draws with the ported
 * vice-libretro drawing layer (libretro-graph.{c,h} + gsplus-font.i, all GPL);
 * the key grid, Apple IIgs ADB layout and key emission are GSplus-original. The
 * transparent grid + cursor-navigation UX is modelled on the vice-libretro OSK.
 *
 * See gsplus_osk.h for the render / input contract with the host (libretro.c).
 */
#include "gsplus_osk.h"
#include "libretro-glue.h"
#include "libretro-graph.h"
#include "libretro.h"   /* RETRO_DEVICE_ID_JOYPAD_* */

/* Grid dimensions. The QWERTY page is 14x6 (a floating RESET row sits above the
 * number row); the numeric-keypad page is a compact 4-wide grid anchored to the
 * right (its top row is an ABC bar back to QWERTY). */
#define VKBDX 14
#define VKBDY 6
#define NUMPAD_COLS 4
#define NUMPAD_ROWS 6

/* Widest page -- keys are sized by this so they stay a consistent size when the
 * active page has fewer columns. */
#define OSK_MAX_COLS VKBDX

/* Special key codes (non-ADB). Negative so they never collide with a2 codes. */
#define OSK_SP_RESET  (-2)
#define OSK_SP_VOID   (-3)   /* non-interactive gap: draws nothing, cursor skips it */
#define OSK_SP_NUMPAD (-4)   /* switch to the numeric-keypad page                    */
#define OSK_SP_QWERTY (-5)   /* switch back to the QWERTY page                       */

/* Font-glyph labels (control codes indexing gsplus-font.i). */
#define G_OAPL  "\015"   /* 13 open-apple (outline)  */
#define G_SAPL  "\016"   /* 14 solid-apple (filled)  */
#define G_SHFT  "\014"   /* 12 shift                 */
#define G_RET   "\020"   /* 16 return                */
#define G_RST   "\021"   /* 17 reset (drawn red)     */
#define G_SPC   "\022"   /* 18 space bar             */
#define G_LEFT  "\033"   /* 27 left cursor           */
#define G_DOWN  "\034"   /* 28 down cursor           */
#define G_RIGHT "\035"   /* 29 right cursor          */
#define G_UP    "\036"   /* 30 up cursor             */

typedef struct { const char *label; const char *shift; int code; } osk_key_t;

/* Apple IIgs ADB layout, 14 columns wide to match a real IIgs keyboard. a2
 * codes match g_retro_key_map[] in libretro.c. Wide keys (TAB, RETURN, SHIFT,
 * Open-Apple, spacebar) span several cells by repeating the same entry across
 * adjacent columns -- the cursor visits each cell but they all emit one code. */
static const osk_key_t qwerty_keys[VKBDX * VKBDY] = {
   /* Row 0 -- floating row, top-left over ESC: RESET (red triangle, emulator
    * hard reset = instant ctrl-reset) and R (the real ADB Reset key, 0x7f --
    * inert alone; CTL+R warm-resets, OA+CTL+R cold-boots, like the physical
    * key). Rest is void so the game shows through beside them. */
   {G_RST,G_RST,OSK_SP_RESET},{"R","R",0x7f},{"","",OSK_SP_VOID},{"","",OSK_SP_VOID},
   {"","",OSK_SP_VOID},{"","",OSK_SP_VOID},{"","",OSK_SP_VOID},
   {"","",OSK_SP_VOID},{"","",OSK_SP_VOID},{"","",OSK_SP_VOID},{"","",OSK_SP_VOID},
   {"","",OSK_SP_VOID},{"","",OSK_SP_VOID},{"","",OSK_SP_VOID},

   /* Row 1 -- ESC, number row, Delete */
   {"ESC","ESC",0x35},{"1","!",0x12},{"2","@",0x13},{"3","#",0x14},{"4","$",0x15},
   {"5","%",0x17},{"6","^",0x16},{"7","&",0x1a},{"8","*",0x1c},{"9","(",0x19},
   {"0",")",0x1d},{"-","_",0x1b},{"=","+",0x18},{"DEL","DEL",0x33},

   /* Row 2 -- TAB, QWERTY top, 123 (switch to numeric keypad) */
   {"TAB","TAB",0x30},{"Q","Q",0x0c},{"W","W",0x0d},{"E","E",0x0e},{"R","R",0x0f},
   {"T","T",0x11},{"Y","Y",0x10},{"U","U",0x20},{"I","I",0x22},{"O","O",0x1f},
   {"P","P",0x23},{"[","{",0x21},{"]","}",0x1e},{"123","123",OSK_SP_NUMPAD},

   /* Row 3 -- CTRL, home row, RETURN (double) */
   {"CTL","CTL",0x36},{"A","A",0x00},{"S","S",0x01},{"D","D",0x02},{"F","F",0x03},
   {"G","G",0x05},{"H","H",0x04},{"J","J",0x26},{"K","K",0x28},{"L","L",0x25},
   {";",":",0x29},{"'","\"",0x27},{G_RET,G_RET,0x24},{G_RET,G_RET,0x24},

   /* Row 4 -- SHIFT (double), bottom letters, SHIFT (double) */
   {G_SHFT,G_SHFT,0x38},{G_SHFT,G_SHFT,0x38},{"Z","Z",0x06},{"X","X",0x07},{"C","C",0x08},
   {"V","V",0x09},{"B","B",0x0b},{"N","N",0x2d},{"M","M",0x2e},{",","<",0x2b},
   {".",">",0x2f},{"/","?",0x2c},{G_SHFT,G_SHFT,0x38},{G_SHFT,G_SHFT,0x38},

   /* Row 5 -- CAPS, Option (solid-apple), Open-Apple (double), `, spacebar
    * (quad), \, arrows in L R D U order */
   {"CAP","CAP",0x39},{G_SAPL,G_SAPL,0x3a},{G_OAPL,G_OAPL,0x37},{G_OAPL,G_OAPL,0x37},
   {"`","~",0x32},{G_SPC,G_SPC,0x31},{G_SPC,G_SPC,0x31},{G_SPC,G_SPC,0x31},
   {G_SPC,G_SPC,0x31},{"\\","|",0x2a},
   {G_LEFT,G_LEFT,0x3b},{G_RIGHT,G_RIGHT,0x3c},{G_DOWN,G_DOWN,0x3d},{G_UP,G_UP,0x3e},
};

/* Numeric-keypad page (4 wide). Emits the IIgs keypad's *own* ADB codes, which
 * are distinct from the number-row keys -- some software only accepts these. The
 * top row is a temporary ABC bar that returns to QWERTY (final placement TBD);
 * RETURN and the 0 key span two cells via the repeat trick. */
static const osk_key_t numpad_keys[NUMPAD_COLS * NUMPAD_ROWS] = {
   /* Row 0 -- ABC bar (back to QWERTY) */
   {"ABC","ABC",OSK_SP_QWERTY},{"","",OSK_SP_QWERTY},
   {"","",OSK_SP_QWERTY},{"","",OSK_SP_QWERTY},

   /* Row 1 -- Clear = / * */
   {"CLR","CLR",0x47},{"=","=",0x51},{"/","/",0x4b},{"*","*",0x43},

   /* Row 2 -- 7 8 9 + */
   {"7","7",0x59},{"8","8",0x5b},{"9","9",0x5c},{"+","+",0x45},

   /* Row 3 -- 4 5 6 - */
   {"4","4",0x56},{"5","5",0x57},{"6","6",0x58},{"-","-",0x4e},

   /* Row 4 -- 1 2 3 Enter */
   {"1","1",0x53},{"2","2",0x54},{"3","3",0x55},{G_RET,G_RET,0x4c},

   /* Row 5 -- 0 (double), ., Enter */
   {"0","0",0x52},{"0","0",0x52},{".",".",0x41},{G_RET,G_RET,0x4c},
};

/* Active page: 0 = QWERTY, 1 = numeric keypad. Accessors return the layout,
 * grid extents and anchor for whichever page is showing. */
static int g_page;

static const osk_key_t *cur_keys(void) { return g_page ? numpad_keys : qwerty_keys; }
static int cur_cols(void)         { return g_page ? NUMPAD_COLS : VKBDX; }
static int cur_rows(void)         { return g_page ? NUMPAD_ROWS : VKBDY; }
static int cur_anchor_right(void) { return g_page; }   /* numpad hugs the right */

/* Number of rows at the top that float free of the dim panel (the QWERTY page's
 * RESET row, so the game shows through beside the lone RESET button). */
static int cur_float_rows(void)   { return g_page ? 0 : 1; }

/* IIgs modifier a2 codes -- these keys act as sticky toggles on the OSK. */
#define A2_SHIFT 0x38
#define A2_CTRL  0x36
#define A2_CAPS  0x39
#define A2_SOLID 0x3a   /* Solid-Apple / Option */
#define A2_OPEN  0x37   /* Open-Apple / Command  */

static int is_modifier(int code)
{
   return code == A2_SHIFT || code == A2_CTRL || code == A2_CAPS
       || code == A2_SOLID || code == A2_OPEN;
}

/* ---- state ---------------------------------------------------------------- */

bool retro_vkbd = false;

static int  g_cx = 1;             /* cursor column (start on '1')          */
static int  g_cy = 1;             /* cursor row (row 0 is the RESET row)   */

static int  g_area_x, g_area_y, g_area_w, g_area_h;   /* target rect        */

static int  g_b_prev;             /* press-button edge state               */
static int  g_pressed_code = -1;  /* a2 code of a normal key held down      */

/* After a key tap, transient sticky modifiers (Shift/Ctrl/Option/Open-Apple)
 * are released one frame later rather than immediately -- the emulated machine
 * reads the modifier latch when it processes the keypress in run_16ms, so the
 * modifier must still be held down through that frame (like a real held key). */
static int  g_release_transient_next;

/* Sticky modifiers currently held (their key-down was sent, key-up pending). */
static int  g_sticky[8];
static int  g_sticky_n;

/* Cursor auto-repeat timing. */
#define OSK_MOVE_INIT_DELAY 220   /* ms held before auto-repeat begins */
#define OSK_MOVE_REPEAT      70    /* ms between auto-repeat steps       */
static int  g_dir_held;
static int  g_dir_repeat;
static long g_last_move_ms;

/* ---- sticky-modifier helpers ---------------------------------------------- */

static int sticky_index(int code)
{
   int i;
   for (i = 0; i < g_sticky_n; i++)
      if (g_sticky[i] == code)
         return i;
   return -1;
}

static int sticky_has(int code) { return sticky_index(code) >= 0; }

static void sticky_add(int code)
{
   if (sticky_has(code) || g_sticky_n >= (int)(sizeof(g_sticky)/sizeof(g_sticky[0])))
      return;
   g_sticky[g_sticky_n++] = code;
}

static void sticky_remove_at(int idx)
{
   int i;
   for (i = idx; i < g_sticky_n - 1; i++)
      g_sticky[i] = g_sticky[i + 1];
   g_sticky_n--;
}

/* Toggle a modifier: turn it on (latch bit + track) or off (clear + untrack).
 * Modifiers go through the c025 latch, not key events -- see osk_host_modifier. */
static void sticky_toggle(int code)
{
   int idx = sticky_index(code);
   if (idx >= 0)
   {
      osk_host_modifier(code, 0);
      sticky_remove_at(idx);
   }
   else
   {
      osk_host_modifier(code, 1);
      sticky_add(code);
   }
}

/* Release every held modifier except Caps Lock (a locking key). */
static void sticky_release_transient(void)
{
   int i;
   for (i = g_sticky_n - 1; i >= 0; i--)
   {
      if (g_sticky[i] == A2_CAPS)
         continue;
      osk_host_modifier(g_sticky[i], 0);
      sticky_remove_at(i);
   }
}

/* Release absolutely everything the OSK is holding (used when hiding). */
static void osk_release_all(void)
{
   int i;
   if (g_pressed_code >= 0)
   {
      osk_host_key(g_pressed_code, 0);
      g_pressed_code = -1;
   }
   for (i = g_sticky_n - 1; i >= 0; i--)
      osk_host_modifier(g_sticky[i], 0);
   g_sticky_n = 0;
   g_release_transient_next = 0;
}

/* ---- public control ------------------------------------------------------- */

void osk_set_area(int x, int y, int w, int h)
{
   g_area_x = x; g_area_y = y; g_area_w = w; g_area_h = h;
}

void toggle_vkbd(void)
{
   if (retro_vkbd)
      osk_release_all();
   retro_vkbd = !retro_vkbd;
   g_b_prev = 0;
   g_dir_held = 0;
   g_dir_repeat = 0;
}

/* ---- input ---------------------------------------------------------------- */

static void osk_move(int dx, int dy)
{
   const osk_key_t *keys = cur_keys();
   int cols = cur_cols(), rows = cur_rows();
   int nx = g_cx, ny = g_cy;
   int guard = cols * rows;   /* bail out if a whole line is void */

   /* Step in the requested direction, skipping non-interactive void cells so
    * the cursor never gets stranded on an empty gap (e.g. beside RESET). */
   do {
      nx += dx;
      if (nx < 0)      nx = cols - 1;
      else if (nx >= cols) nx = 0;
      ny += dy;
      if (ny < 0)      ny = rows - 1;
      else if (ny >= rows) ny = 0;
   } while (keys[ny * cols + nx].code == OSK_SP_VOID && --guard > 0);

   g_cx = nx;
   g_cy = ny;
}

/* Switch pages and drop the cursor on a sensible key in the new page. */
static void osk_set_page(int page)
{
   g_page = page ? 1 : 0;
   if (g_page) { g_cx = 1; g_cy = 3; }   /* numpad: land on "5"          */
   else        { g_cx = 1; g_cy = 1; }   /* qwerty: land on "1" (row 1)  */
}

static void osk_press_begin(void)
{
   const osk_key_t *k = &cur_keys()[g_cy * cur_cols() + g_cx];

   if (k->code == OSK_SP_VOID)
      return;                       /* empty gap: nothing to do (defensive) */
   if (k->code == OSK_SP_NUMPAD) { osk_set_page(1); return; }
   if (k->code == OSK_SP_QWERTY) { osk_set_page(0); return; }
   if (k->code == OSK_SP_RESET)
   {
      osk_host_action(OSK_ACTION_RESET);
      return;
   }
   if (is_modifier(k->code))
   {
      sticky_toggle(k->code);   /* edge-triggered toggle */
      return;
   }
   /* Emit a single tap (down + up in one edge) rather than holding the key
    * down for the whole button press. The emulated ADB has its own autorepeat
    * that engages once a key is held past g_adb_repeat_delay, so holding here
    * would spew the character; one down buffers exactly one keystroke and the
    * immediate up stops any repeat. */
   osk_host_key(k->code, 1);
   osk_host_key(k->code, 0);
   /* Keep any held modifier down through this frame's run_16ms (so the machine
    * sees e.g. Shift while processing the key); release it next frame. */
   g_release_transient_next = 1;
}

void input_vkbd(void)
{
   long now = osk_host_now_ms();
   int  dx  = 0, dy = 0;
   int  b;

   /* Release modifiers held over from last frame's tap, now that its run_16ms
    * has processed the keypress with the modifier still down. */
   if (g_release_transient_next)
   {
      sticky_release_transient();
      g_release_transient_next = 0;
   }

   if (osk_host_pad(RETRO_DEVICE_ID_JOYPAD_LEFT))       dx = -1;
   else if (osk_host_pad(RETRO_DEVICE_ID_JOYPAD_RIGHT)) dx =  1;
   if (osk_host_pad(RETRO_DEVICE_ID_JOYPAD_UP))         dy = -1;
   else if (osk_host_pad(RETRO_DEVICE_ID_JOYPAD_DOWN))  dy =  1;

   if (dx || dy)
   {
      if (!g_dir_held)
      {
         osk_move(dx, dy);
         g_dir_held    = 1;
         g_dir_repeat  = 0;
         g_last_move_ms = now;
      }
      else
      {
         long delay = g_dir_repeat ? OSK_MOVE_REPEAT : OSK_MOVE_INIT_DELAY;
         if (now - g_last_move_ms >= delay)
         {
            osk_move(dx, dy);
            g_dir_repeat  = 1;
            g_last_move_ms = now;
         }
      }
   }
   else
   {
      g_dir_held   = 0;
      g_dir_repeat = 0;
   }

   /* B taps the highlighted key (one keystroke per press edge). */
   b = osk_host_pad(RETRO_DEVICE_ID_JOYPAD_B) ? 1 : 0;
   if (b && !g_b_prev)
      osk_press_begin();
   g_b_prev = b;
}

/* ---- rendering ------------------------------------------------------------ */

/* Estimate the drawn pixel width of a label, mirroring draw_text()'s advance. */
static int osk_text_width(const char *s, int scale)
{
   int w = 0;
   for (; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      int cw = 6;
      if (c == 'l' || c == 'i')
         cw = 3;
      else if (c >= 'a' && c <= 'z' && c != 'm' && c != 'w')
         cw = 4;
      w += cw;
   }
   return w * scale;
}

void print_vkbd(void)
{
   int ax = g_area_x, ay = g_area_y, aw = g_area_w, ah = g_area_h;
   int x, y;
   int pad_x, XSIDE, YSIDE, kbw, kbh, x0, y0, fs;
   int cols = cur_cols(), rows = cur_rows();
   const osk_key_t *keys = cur_keys();
   bool shifted = sticky_has(A2_SHIFT);

   if (!retro_bmp || pix_bytes != 4)
      return;

   /* Default target: the whole frame. */
   if (aw <= 0 || ah <= 0)
   {
      ax = 0; ay = 0; aw = retrow; ah = retroh;
   }

   /* Layout: a centered grid anchored near the bottom of the target area. */
   pad_x = aw / 24;
   if (pad_x < 2) pad_x = 2;
   XSIDE = (aw - 2 * pad_x) / OSK_MAX_COLS;   /* size keys by the widest page */
   if (XSIDE < 6) XSIDE = 6;
   YSIDE = XSIDE * 3 / 4;
   if (YSIDE < 8) YSIDE = 8;
   kbw = XSIDE * cols;
   kbh = YSIDE * rows;
   /* Center the full-width QWERTY page; hug the right edge for the numpad. */
   if (cur_anchor_right())
      x0 = ax + aw - kbw - pad_x;
   else
      x0 = ax + (aw - kbw) / 2;
   y0  = ay + ah - kbh - ah / 12;
   if (x0 < 2) x0 = 2;
   if (y0 < ay + 2) y0 = ay + 2;
   /* Keep the whole grid (and its 1px borders) inside the frame. */
   if (x0 + kbw + 1 > retrow) x0 = retrow - kbw - 1;
   if (y0 + kbh + 1 > retroh) y0 = retroh - kbh - 1;
   if (x0 < 1) x0 = 1;
   if (y0 < 1) y0 = 1;

   fs = (XSIDE >= 28) ? 2 : 1;

   /* Dim panel behind the keyboard so labels read over any picture. Skip the
    * floating top row(s) (the RESET row) so the game shows through beside it. */
   {
      int pf = cur_float_rows();
      draw_fbox(x0 - pad_x / 2, y0 + pf * YSIDE - 4,
                kbw + pad_x, kbh - pf * YSIDE + 8,
                COLOR_BLACK_32, GRAPH_ALPHA_50);
   }

   for (y = 0; y < rows; y++)
   {
      for (x = 0; x < cols; x++)
      {
         const osk_key_t *k = &keys[y * cols + x];
         int kx = x0 + x * XSIDE;
         int ky = y0 + y * YSIDE;
         int sel = (x == g_cx && y == g_cy);
         const char *lbl = shifted ? k->shift : k->label;
         uint32_t bg = COLOR_32_32;
         uint32_t fg = COLOR_WHITE_32;
         int tw, tx, ty, th;
         libretro_graph_alpha_t bga = GRAPH_ALPHA_75;

         if (k->code == OSK_SP_VOID)
            continue;              /* empty gap: draw nothing at all */

         if (is_modifier(k->code))
            bg = sticky_has(k->code) ? COLOR_GREEN_32 : COLOR_64_32;

         if (sel)
         {
            bg  = COLOR_IIGS_32;
            fg  = COLOR_WHITE_32;
            bga = GRAPH_ALPHA_100;
         }

         /* Key face. */
         draw_fbox(kx + 1, ky + 1, XSIDE - 2, YSIDE - 2, bg, bga);

         /* 1px border (solid lines -- no graphed[] dependency). */
         draw_hline(kx, ky, XSIDE, 1, COLOR_10_32);
         draw_hline(kx, ky + YSIDE - 1, XSIDE, 1, COLOR_10_32);
         draw_vline(kx, ky, 1, YSIDE, COLOR_10_32);
         draw_vline(kx + XSIDE - 1, ky, 1, YSIDE, COLOR_10_32);

         /* Centered label. */
         tw = osk_text_width(lbl, fs);
         th = 8 * fs;
         tx = kx + (XSIDE - tw) / 2;
         ty = ky + (YSIDE - th) / 2 + fs;
         if (tx < kx + 1) tx = kx + 1;
         draw_text((uint16_t)tx, (uint16_t)ty, fg, bg, GRAPH_ALPHA_100,
                   GRAPH_BG_NONE, (uint8_t)fs, (uint8_t)fs, 10,
                   (const unsigned char *)lbl);
      }
   }
}
