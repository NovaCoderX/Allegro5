/* Scale2x on 32-bit pixels, for the display scale's filter
 * (a5_set_new_display_filter(), allegro_amiga.h).
 *
 * Each source pixel E, with B above, D left, F right and H below (the
 * picture's edge repeated past it), becomes 2x2:
 *
 *    E0 E1     E0 = D if D == B, else E     E1 = F if B == F, else E
 *    E2 E3     E2 = D if D == H, else E     E3 = F if H == F, else E
 *
 * only where B != H and D != F; elsewhere all four are E.  Pixels are
 * compared whole, so blended ones (shadows, fades) rarely match their
 * neighbours and come out as nearest would.
 *
 * The advanced variant (scale2x_adv), after OpenDUNE's Amiga port
 * (OpenDUNE/src/video/hq2x.c), leaves E as a plain 2x2 block when it is
 * much brighter than the average of B, D, F and H, so thin white text keeps
 * its strokes.
 * Brightness is 3 R + 6 G + B (0 to 2550), the threshold 160, as there.
 * Each row's brightness is worked out once, into a buffer of three rows.
 *
 * Integers only.  The flat case (B == H or D == F), most of Dune's art, is
 * laid out as the straight path: Emu68 guesses a forward branch as not
 * taken, and a wrong guess leaves its translated code.
 */

#include <SDL.h>
#include "a5_internal.h"

#define LUMA_MAX_WIDTH    1024
#define LUMA_BRIGHT_EDGE  160

static int s_luma[3][LUMA_MAX_WIDTH];

static inline int luma(Uint32 p)
{
   const int r = (p >> 16) & 0xFF;
   const int g = (p >> 8) & 0xFF;
   const int b = p & 0xFF;

   return 3 * r + 6 * g + b;
}

static void luma_row(int *dst, const Uint32 *src, int w)
{
   int x;

   for (x = 0; x < w; x++)
      dst[x] = luma(src[x]);
}

/* One source pixel to its 2x2 block at o0 (upper row) and o1 (lower). */
static inline void block(Uint32 b, Uint32 d, Uint32 e, Uint32 f, Uint32 h,
   Uint32 *o0, Uint32 *o1)
{
   if (__builtin_expect(b != h && d != f, 0)) {
      o0[0] = (d == b) ? d : e;
      o0[1] = (b == f) ? f : e;
      o1[0] = (d == h) ? d : e;
      o1[1] = (h == f) ? f : e;
   }
   else {
      o0[0] = e;
      o0[1] = e;
      o1[0] = e;
      o1[1] = e;
   }
}

void a5_scale2x(const Uint32 *src, int src_pitch, int w, int h,
   Uint32 *dst, int dst_pitch, bool keep_bright_text)
{
   int x, y;

   if (w <= 0 || h <= 0)
      return;

   if (w > LUMA_MAX_WIDTH)
      keep_bright_text = false;

   /* Row r's brightness is in s_luma[r % 3]. */
   if (keep_bright_text)
      luma_row(s_luma[0], src, w);

   for (y = 0; y < h; y++) {
      const Uint32 *cur = src + y * src_pitch;
      const Uint32 *prev = (y > 0) ? cur - src_pitch : cur;
      const Uint32 *next = (y < h - 1) ? cur + src_pitch : cur;
      Uint32 *o0 = dst + 2 * y * dst_pitch;
      Uint32 *o1 = o0 + dst_pitch;
      const int *lp = NULL, *lc = NULL, *ln = NULL;

      if (keep_bright_text) {
         const int c = y % 3;

         lc = s_luma[c];
         lp = (y > 0) ? s_luma[(c + 2) % 3] : lc;
         if (y < h - 1) {
            luma_row(s_luma[(c + 1) % 3], next, w);
            ln = s_luma[(c + 1) % 3];
         }
         else {
            ln = lc;
         }
      }

      if (w == 1) {
         block(prev[0], cur[0], cur[0], cur[0], next[0], o0, o1);
         continue;
      }

      /* The left and right edges repeat the edge pixel. */
      block(prev[0], cur[0], cur[0], cur[1], next[0], o0, o1);

      if (keep_bright_text) {
         for (x = 1; x < w - 1; x++) {
            const Uint32 e = cur[x];
            const int around = (lp[x] + lc[x - 1] + lc[x + 1] + ln[x]) >> 2;

            if (__builtin_expect(lc[x] - around > LUMA_BRIGHT_EDGE, 0)) {
               o0[2 * x] = e;
               o0[2 * x + 1] = e;
               o1[2 * x] = e;
               o1[2 * x + 1] = e;
            }
            else {
               block(prev[x], cur[x - 1], e, cur[x + 1], next[x], o0 + 2 * x, o1 + 2 * x);
            }
         }
      }
      else {
         for (x = 1; x < w - 1; x++)
            block(prev[x], cur[x - 1], cur[x], cur[x + 1], next[x], o0 + 2 * x, o1 + 2 * x);
      }

      x = w - 1;
      block(prev[x], cur[x - 1], cur[x], cur[x], next[x], o0 + 2 * x, o1 + 2 * x);
   }
}
