/* Bitmap drawing: bitmap_draw.h.
 *
 * Follows Allegro's software renderer (memblit.c, tri_soft.c), which is
 * what Allegro uses for memory bitmaps, in integers and 16.16 fixed point
 * (no floating point: floats from the API are converted with integer
 * operations, and rotation takes its sine and cosine from a table).
 *
 * - The source region is clipped to the source's root bitmap, then
 *   truncated to whole pixels.
 * - Flips, the centre, scale, rotation, the position and the target's
 *   transform make one affine transform from the region to the target.
 * - With the copy blender, no tint and a transform that only moves, the
 *   region is copied to the position truncated to whole pixels.
 * - Otherwise the target pixels drawn are those whose centres are inside
 *   the transformed region (left and top edges in, right and bottom out),
 *   each taking the source pixel under its centre (nearest neighbour).
 */

#include <stdlib.h>
#include <string.h>
#include "allegro5/allegro.h"
#include "allegro5/allegro_amiga.h"
#include "a5_internal.h"

static const ALLEGRO_COLOR s_white = { 1.0f, 1.0f, 1.0f, 1.0f };

/* sin(i * pi / 512) for i = 0 .. 256 (a quarter turn), 16.16. */
static const A5_FIXED s_quarter_sine[257] = {
0, 402, 804, 1206, 1608, 2010, 2412, 2814,
   3216, 3617, 4019, 4420, 4821, 5222, 5623, 6023,
   6424, 6824, 7224, 7623, 8022, 8421, 8820, 9218,
   9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391,
   12785, 13180, 13573, 13966, 14359, 14751, 15143, 15534,
   15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
   19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699,
   22078, 22457, 22834, 23210, 23586, 23961, 24335, 24708,
   25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
   28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538,
   30893, 31248, 31600, 31952, 32303, 32652, 33000, 33347,
   33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
   36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716,
   39040, 39362, 39683, 40002, 40320, 40636, 40951, 41264,
   41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
   44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056,
   46341, 46624, 46906, 47186, 47464, 47741, 48015, 48288,
   48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
   50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398,
   52639, 52878, 53114, 53349, 53581, 53812, 54040, 54267,
   54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
   56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607,
   57798, 57986, 58172, 58356, 58538, 58718, 58896, 59071,
   59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
   60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568,
   61705, 61839, 61971, 62101, 62228, 62353, 62476, 62596,
   62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
   63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197,
   64277, 64354, 64429, 64501, 64571, 64639, 64704, 64766,
   64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
   65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436,
   65457, 65476, 65492, 65505, 65516, 65525, 65531, 65535,
   65536
};

/* sin of a 16.16 angle in 1024ths of a turn, interpolated. */
static A5_FIXED fixed_sine_1024(Sint32 k)
{
   const int i = (k >> 16) & 1023;
   const A5_FIXED frac = k & 0xFFFF;
   A5_FIXED s0, s1;
   int j;

   for (j = 0; j < 2; j++) {
      const int n = (i + j) & 1023;
      const int q = n >> 8, m = n & 255;
      const A5_FIXED v = (q & 1) ? s_quarter_sine[256 - m] : s_quarter_sine[m];

      if (j == 0)
         s0 = (q & 2) ? -v : v;
      else
         s1 = (q & 2) ? -v : v;
   }

   return s0 + (A5_FIXED)(((Sint64)(s1 - s0) * frac) >> 16);
}

/* sin and cos of an angle in radians, from the table: Dune Dynasty turns
 * in 256ths of a circle, which land on table entries.
 */
static void fixed_sincos(float angle, A5_FIXED *sine, A5_FIXED *cosine)
{
   /* Radians to 1024ths of a turn: times 1024 / 2 pi, in 16.16. */
   const Sint32 k = (Sint32)(((Sint64)a5_float_to_fixed(angle) * 10680707) >> 16);

   *sine = fixed_sine_1024(k);
   *cosine = fixed_sine_1024(k + (256 << 16));
}

static inline int clampi(int v, int lo, int hi)
{
   return (v < lo) ? lo : (v > hi) ? hi : v;
}

/* Copies a region one to one, with the copy blender and no tint. */
static void copy_region(ALLEGRO_BITMAP *target, const ALLEGRO_BITMAP *root,
   int sx, int sy, int sw, int sh, int dx, int dy)
{
   const bool same_root = ((target->parent ? target->parent : target) == root);
   int x0 = dx, y0 = dy, x1 = dx + sw, y1 = dy + sh;
   int y;

   if (x0 < target->cl) x0 = target->cl;
   if (y0 < target->ct) y0 = target->ct;
   if (x1 > target->cr) x1 = target->cr;
   if (y1 > target->cb) y1 = target->cb;
   if (x0 >= x1 || y0 >= y1)
      return;

   for (y = y0; y < y1; y++) {
      const Uint32 *src = root->memory + (sy + y - dy) * root->pitch + sx + (x0 - dx);
      Uint32 *dst = a5_bitmap_row(target, y) + x0;

      /* The source can be another part of the target's root. */
      if (same_root)
         memmove(dst, src, (size_t)(x1 - x0) * sizeof(Uint32));
      else
         a5_copy_pixels(dst, src, x1 - x0);
   }
}

/* The first pixel whose centre is at or right of (below) v: ceil(v - 0.5). */
static inline int pixel_start(A5_FIXED v)
{
   return (v - 0x8000 + 0xFFFF) >> 16;
}

static inline A5_FIXED pixel_centre(int x)
{
   return x * 65536 + 0x8000;
}

/* floor(n / d) as 16.16, shifts for Dune Dynasty's scales of 1 and 2. */
static A5_FIXED fixed_div(A5_FIXED n, A5_FIXED d)
{
   switch (d) {
      case A5_FIXED_ONE:      return n;
      case 2 * A5_FIXED_ONE:  return n >> 1;
      case -A5_FIXED_ONE:     return -n;
      case -2 * A5_FIXED_ONE: return -((n + 1) >> 1);
      default:                return (A5_FIXED)a5_floor_div64((Sint64)n * 65536, d);
   }
}

/* round(1 / d) as 16.16, halves rounding up. */
static A5_FIXED fixed_reciprocal(A5_FIXED d)
{
   switch (d) {
      case A5_FIXED_ONE:      return A5_FIXED_ONE;
      case 2 * A5_FIXED_ONE:  return A5_FIXED_ONE / 2;
      case -A5_FIXED_ONE:     return -A5_FIXED_ONE;
      case -2 * A5_FIXED_ONE: return -A5_FIXED_ONE / 2;
      default:                return (A5_FIXED)a5_floor_div64(((Sint64)1 << 33) + d, 2 * (Sint64)d);
   }
}

/* The column tables of draw_scaled(), kept between draws and grown when a
 * wider span comes along.  Static rather than on the stack, which Amiga
 * programs keep small; only the main thread draws.
 */
static int *s_run_col, *s_run_len;
static int s_run_size;

static bool reserve_runs(int n)
{
   int *col, *len;

   if (n <= s_run_size)
      return true;

   col = realloc(s_run_col, (size_t)n * sizeof(int));
   if (col)
      s_run_col = col;
   len = realloc(s_run_len, (size_t)n * sizeof(int));
   if (len)
      s_run_len = len;
   if (col == NULL || len == NULL)
      return false;

   s_run_size = n;
   return true;
}

/* The general case without rotation: scaling (negative for flips).
 *
 * The source column under each target column is worked out once per draw,
 * and grouped into runs of target columns that show the same source
 * pixel, so a 2x draw reads each source pixel once and writes it twice.
 * A row that samples the same source row as the one above is copied from
 * it when the result cannot depend on what was underneath: with the copy
 * blender, or with the alpha blender when the row above was all opaque
 * (the second row of every 2x tile).
 */
static void draw_scaled(ALLEGRO_BITMAP *target, const ALLEGRO_BITMAP *root,
   int sx, int sy, int sw, int sh, A5_FIXED a, A5_FIXED e, A5_FIXED d, A5_FIXED f,
   Uint32 tint)
{
   const int kind = a5_blend_kind();
   const bool tinted = (tint != 0xFFFFFFFF);
   A5_FIXED x0 = e, x1 = e + a * sw;
   A5_FIXED y0 = f, y1 = f + d * sh;
   int *run_col, *run_len;
   const Uint32 *prev_src = NULL;
   Uint32 *prev_dst = NULL;
   bool prev_reusable = false;
   int dx0, dx1, dy0, dy1, n, runs, i, k, y;
   A5_FIXED u, du, v, dv;

   if (x0 > x1) { const A5_FIXED t = x0; x0 = x1; x1 = t; }
   if (y0 > y1) { const A5_FIXED t = y0; y0 = y1; y1 = t; }

   /* Pixels whose centres are in [x0, x1) x [y0, y1). */
   dx0 = pixel_start(x0);
   dx1 = pixel_start(x1);
   dy0 = pixel_start(y0);
   dy1 = pixel_start(y1);

   if (dx0 < target->cl) dx0 = target->cl;
   if (dy0 < target->ct) dy0 = target->ct;
   if (dx1 > target->cr) dx1 = target->cr;
   if (dy1 > target->cb) dy1 = target->cb;
   if (dx0 >= dx1 || dy0 >= dy1)
      return;

   n = dx1 - dx0;
   if (!reserve_runs(n))
      return;
   run_col = s_run_col;
   run_len = s_run_len;

   /* Source column under each centre, stepped in 16.16 fixed point, as
    * runs of equal columns.
    */
   u = fixed_div(pixel_centre(dx0) - e, a);
   du = fixed_reciprocal(a);
   runs = 0;
   for (i = 0; i < n; i++, u += du) {
      const int col = clampi((int)(u >> 16), 0, sw - 1);

      if (runs > 0 && run_col[runs - 1] == col) {
         run_len[runs - 1]++;
      }
      else {
         run_col[runs] = col;
         run_len[runs] = 1;
         runs++;
      }
   }

   /* Source row under each centre, the same way. */
   v = fixed_div(pixel_centre(dy0) - f, d);
   dv = fixed_reciprocal(d);

   for (y = dy0; y < dy1; y++, v += dv) {
      const Uint32 *src = root->memory + (sy + clampi((int)(v >> 16), 0, sh - 1)) * root->pitch + sx;
      Uint32 *dst = a5_bitmap_row(target, y) + dx0;
      Uint32 *d = dst;

      if (src == prev_src && prev_reusable) {
         a5_copy_pixels(dst, prev_dst, n);
         prev_dst = dst;
         continue;
      }

      if (kind == A5_BLEND_COPY && !tinted) {
         if (runs == n && du == 65536) {
            if ((target->parent ? target->parent : target) == root)
               memmove(dst, src + run_col[0], (size_t)n * sizeof(Uint32));
            else
               a5_copy_pixels(dst, src + run_col[0], n);
         }
         else {
            for (k = 0; k < runs; k++) {
               const Uint32 s = src[run_col[k]];
               int c = run_len[k];

               do *d++ = s; while (--c);
            }
         }
         prev_reusable = true;
      }
      else if (kind == A5_BLEND_ALPHA && !tinted) {
         Uint32 all = 0xFFFFFFFF;

         for (k = 0; k < runs; k++) {
            const Uint32 s = src[run_col[k]];
            const Uint32 sa = s >> 24;
            int c = run_len[k];

            all &= s;
            if (sa == 255) {
               do *d++ = s; while (--c);
            }
            else if (sa == 0) {
               d += c;
            }
            else {
               do { *d = a5_blend_pixel(A5_BLEND_ALPHA, s, *d); d++; } while (--c);
            }
         }
         prev_reusable = ((all >> 24) == 255);
      }
      else if (kind == A5_BLEND_ALPHA) {
         /* Tinted text and shadows: transparent source pixels are skipped,
          * and the tint of a repeated source pixel is reused.
          */
         Uint32 last_s = 0, t = 0;

         for (k = 0; k < runs; k++) {
            const Uint32 s = src[run_col[k]];
            int c = run_len[k];

            if ((s >> 24) == 0) {
               d += c;
               continue;
            }
            if (s != last_s) {
               last_s = s;
               t = a5_tint_pixel(s, tint);
            }
            if ((t >> 24) == 255) {
               do *d++ = t; while (--c);
            }
            else {
               do { *d = a5_blend_pixel(A5_BLEND_ALPHA, t, *d); d++; } while (--c);
            }
         }
         prev_reusable = false;
      }
      else {
         for (k = 0; k < runs; k++) {
            Uint32 s = src[run_col[k]];
            int c = run_len[k];

            if (tinted)
               s = a5_tint_pixel(s, tint);
            do { *d = a5_blend_pixel(kind, s, *d); d++; } while (--c);
         }
         prev_reusable = (kind == A5_BLEND_COPY);
      }

      prev_src = src;
      prev_dst = dst;
   }
}

/* Rotation: each target pixel in the bounding box whose centre maps back
 * inside the region, with the transform x' = a x + c y + e,
 * y' = b x + d y + f, and its inverse stepped in 16.16.
 */
static void draw_rotated(ALLEGRO_BITMAP *target, const ALLEGRO_BITMAP *root,
   int sx, int sy, int sw, int sh, A5_FIXED a, A5_FIXED b, A5_FIXED c, A5_FIXED d,
   A5_FIXED e, A5_FIXED f, Uint32 tint)
{
   const int kind = a5_blend_kind();
   const bool tinted = (tint != 0xFFFFFFFF);
   const Sint64 det = ((Sint64)a * d - (Sint64)b * c) >> 16;
   A5_FIXED minx, maxx, miny, maxy, ia, ib, ic, id;
   int dx0, dx1, dy0, dy1, x, y, i;

   if (det == 0)
      return;

   minx = maxx = e;
   miny = maxy = f;
   for (i = 1; i < 4; i++) {
      const int cx = (i & 1) ? sw : 0, cy = (i & 2) ? sh : 0;
      const A5_FIXED px = a * cx + c * cy + e;
      const A5_FIXED py = b * cx + d * cy + f;

      if (px < minx) minx = px;
      if (px > maxx) maxx = px;
      if (py < miny) miny = py;
      if (py > maxy) maxy = py;
   }

   dx0 = pixel_start(minx);
   dx1 = pixel_start(maxx);
   dy0 = pixel_start(miny);
   dy1 = pixel_start(maxy);

   if (dx0 < target->cl) dx0 = target->cl;
   if (dy0 < target->ct) dy0 = target->ct;
   if (dx1 > target->cr) dx1 = target->cr;
   if (dy1 > target->cb) dy1 = target->cb;
   if (dx0 >= dx1 || dy0 >= dy1)
      return;

   /* The inverse of the linear part. */
   ia = (A5_FIXED)a5_floor_div64((Sint64)d * 65536, det);
   ib = (A5_FIXED)a5_floor_div64(-(Sint64)b * 65536, det);
   ic = (A5_FIXED)a5_floor_div64(-(Sint64)c * 65536, det);
   id = (A5_FIXED)a5_floor_div64((Sint64)a * 65536, det);

   for (y = dy0; y < dy1; y++) {
      const A5_FIXED ry = pixel_centre(y) - f;
      const A5_FIXED rx = pixel_centre(dx0) - e;
      A5_FIXED lx = (A5_FIXED)(((Sint64)ia * rx + (Sint64)ic * ry) >> 16);
      A5_FIXED ly = (A5_FIXED)(((Sint64)ib * rx + (Sint64)id * ry) >> 16);
      Uint32 *dst = a5_bitmap_row(target, y);

      for (x = dx0; x < dx1; x++, lx += ia, ly += ib) {
         Uint32 s;

         if (lx < 0 || ly < 0 || (lx >> 16) >= sw || (ly >> 16) >= sh)
            continue;

         s = root->memory[(sy + (ly >> 16)) * root->pitch + sx + (lx >> 16)];
         if (tinted)
            s = a5_tint_pixel(s, tint);
         dst[x] = a5_blend_pixel(kind, s, dst[x]);
      }
   }
}

static inline bool float_is_zero(float f)
{
   union { float f; Uint32 u; } v;

   v.f = f;
   return (v.u & 0x7FFFFFFF) == 0;
}

/* toward zero, as C converts a float to an int */
static inline int fixed_trunc(A5_FIXED v)
{
   return (v >= 0) ? (v >> 16) : -((-v) >> 16);
}

/* to / from as 16.16, shifts for Dune Dynasty's scales of 1 and 2. */
static A5_FIXED fixed_scale(A5_FIXED to, A5_FIXED from)
{
   if (to == from)
      return A5_FIXED_ONE;
   if (to == 2 * from)
      return 2 * A5_FIXED_ONE;
   return (A5_FIXED)a5_floor_div64((Sint64)to * 65536, from);
}

/* Allegro's _draw_tinted_rotated_scaled_bitmap_region() in fixed point:
 * the region, clipped to the source's root, is flipped, moved by -centre,
 * scaled, rotated, moved to (dx, dy) and put through the target's
 * transform, giving x' = a x + c y + e, y' = b x + d y + f.  dw and dh are
 * the scaled size, or the region's own size.  Everything but the angle is
 * 16.16 already.
 */
static void draw_bitmap(ALLEGRO_BITMAP *bitmap, ALLEGRO_COLOR tint,
   A5_FIXED cx, A5_FIXED cy, float angle, A5_FIXED sx, A5_FIXED sy, A5_FIXED sw,
   A5_FIXED sh, A5_FIXED dx, A5_FIXED dy, A5_FIXED dw, A5_FIXED dh, int flags)
{
   ALLEGRO_BITMAP *target = al_get_target_bitmap();
   const ALLEGRO_BITMAP *root;
   A5_FIXED fsx, fsy, fsw, fsh, orig_sw, orig_sh, clip_x = 0, clip_y = 0;
   A5_FIXED xscale, yscale, ox, oy, sine = 0, cosine = A5_FIXED_ONE;
   A5_FIXED r00, r01, r10, r11, x0, y0, a, b, c, d, e, f;
   Uint32 tint_pixel;
   int isx, isy, isw, ish;

   if (target == NULL || bitmap == NULL)
      return;

   root = bitmap->parent ? bitmap->parent : bitmap;
   fsx = sx;
   fsy = sy;
   fsw = orig_sw = sw;
   fsh = orig_sh = sh;
   if (orig_sw == 0 || orig_sh == 0)
      return;

   if (bitmap->parent) {
      fsx += bitmap->xofs * 65536;
      fsy += bitmap->yofs * 65536;
   }

   if (fsx < 0) {
      fsw += fsx;
      clip_x = -fsx;
      fsx = 0;
   }
   if (fsy < 0) {
      fsh += fsy;
      clip_y = -fsy;
      fsy = 0;
   }
   if (fsx + fsw > (root->w << 16))
      fsw = (root->w << 16) - fsx;
   if (fsy + fsh > (root->h << 16))
      fsh = (root->h << 16) - fsy;

   isx = fsx >> 16;
   isy = fsy >> 16;
   isw = fixed_trunc(fsw);
   ish = fixed_trunc(fsh);
   if (isw <= 0 || ish <= 0)
      return;

   xscale = fixed_scale(dw, orig_sw);
   yscale = fixed_scale(dh, orig_sh);

   /* Flip and centre: x -> (flip ? orig_sw - (x + clip_x) : x + clip_x) - cx,
    * then scale: r x + o.
    */
   ox = a5_fixed_mul(((flags & ALLEGRO_FLIP_HORIZONTAL) ? orig_sw - clip_x : clip_x) - cx,
      xscale);
   oy = a5_fixed_mul(((flags & ALLEGRO_FLIP_VERTICAL) ? orig_sh - clip_y : clip_y) - cy,
      yscale);
   if (flags & ALLEGRO_FLIP_HORIZONTAL)
      xscale = -xscale;
   if (flags & ALLEGRO_FLIP_VERTICAL)
      yscale = -yscale;

   /* Rotate, then move to (dx, dy). */
   if (!float_is_zero(angle))
      fixed_sincos(angle, &sine, &cosine);
   r00 = a5_fixed_mul(cosine, xscale);
   r01 = -a5_fixed_mul(sine, yscale);
   r10 = a5_fixed_mul(sine, xscale);
   r11 = a5_fixed_mul(cosine, yscale);
   x0 = a5_fixed_mul(cosine, ox) - a5_fixed_mul(sine, oy) + dx;
   y0 = a5_fixed_mul(sine, ox) + a5_fixed_mul(cosine, oy) + dy;

   /* The target's transform. */
   a = a5_fixed_mul(target->ta, r00) + a5_fixed_mul(target->tc, r10);
   c = a5_fixed_mul(target->ta, r01) + a5_fixed_mul(target->tc, r11);
   e = a5_fixed_mul(target->ta, x0) + a5_fixed_mul(target->tc, y0) + target->te;
   b = a5_fixed_mul(target->tb, r00) + a5_fixed_mul(target->td, r10);
   d = a5_fixed_mul(target->tb, r01) + a5_fixed_mul(target->td, r11);
   f = a5_fixed_mul(target->tb, x0) + a5_fixed_mul(target->td, y0) + target->tf;

   tint_pixel = a5_color_to_pixel(tint);

   if (b == 0 && c == 0) {
      if (a == A5_FIXED_ONE && d == A5_FIXED_ONE && tint_pixel == 0xFFFFFFFF
         && a5_blend_kind() == A5_BLEND_COPY) {
         copy_region(target, root, isx, isy, isw, ish, fixed_trunc(e), fixed_trunc(f));
      }
      else if (a != 0 && d != 0) {
         draw_scaled(target, root, isx, isy, isw, ish, a, e, d, f, tint_pixel);
      }
   }
   else {
      draw_rotated(target, root, isx, isy, isw, ish, a, b, c, d, e, f, tint_pixel);
   }
}

void al_draw_bitmap(ALLEGRO_BITMAP *bitmap, float dx, float dy, int flags)
{
   al_draw_tinted_bitmap(bitmap, s_white, dx, dy, flags);
}

void al_draw_bitmap_region(ALLEGRO_BITMAP *bitmap,
   float sx, float sy, float sw, float sh, float dx, float dy, int flags)
{
   al_draw_tinted_bitmap_region(bitmap, s_white, sx, sy, sw, sh, dx, dy, flags);
}

void al_draw_scaled_bitmap(ALLEGRO_BITMAP *bitmap,
   float sx, float sy, float sw, float sh,
   float dx, float dy, float dw, float dh, int flags)
{
   al_draw_tinted_scaled_bitmap(bitmap, s_white, sx, sy, sw, sh, dx, dy, dw, dh, flags);
}

void al_draw_rotated_bitmap(ALLEGRO_BITMAP *bitmap,
   float cx, float cy, float dx, float dy, float angle, int flags)
{
   al_draw_tinted_rotated_bitmap(bitmap, s_white, cx, cy, dx, dy, angle, flags);
}

#define FX(f) a5_float_to_fixed(f)

void al_draw_tinted_bitmap(ALLEGRO_BITMAP *bitmap, ALLEGRO_COLOR tint,
   float dx, float dy, int flags)
{
   if (bitmap) {
      const A5_FIXED w = bitmap->w * 65536, h = bitmap->h * 65536;

      draw_bitmap(bitmap, tint, 0, 0, 0.0f, 0, 0, w, h, FX(dx), FX(dy), w, h, flags);
   }
}

void al_draw_tinted_bitmap_region(ALLEGRO_BITMAP *bitmap, ALLEGRO_COLOR tint,
   float sx, float sy, float sw, float sh, float dx, float dy, int flags)
{
   const A5_FIXED w = FX(sw), h = FX(sh);

   draw_bitmap(bitmap, tint, 0, 0, 0.0f, FX(sx), FX(sy), w, h, FX(dx), FX(dy), w, h, flags);
}

void al_draw_tinted_scaled_bitmap(ALLEGRO_BITMAP *bitmap, ALLEGRO_COLOR tint,
   float sx, float sy, float sw, float sh,
   float dx, float dy, float dw, float dh, int flags)
{
   draw_bitmap(bitmap, tint, 0, 0, 0.0f, FX(sx), FX(sy), FX(sw), FX(sh), FX(dx), FX(dy),
      FX(dw), FX(dh), flags);
}

void al_draw_tinted_rotated_bitmap(ALLEGRO_BITMAP *bitmap, ALLEGRO_COLOR tint,
   float cx, float cy, float dx, float dy, float angle, int flags)
{
   if (bitmap) {
      const A5_FIXED w = bitmap->w * 65536, h = bitmap->h * 65536;

      draw_bitmap(bitmap, tint, FX(cx), FX(cy), angle, 0, 0, w, h, FX(dx), FX(dy), w, h, flags);
   }
}

/* Amiga extension (allegro_amiga.h): Dune II's sandworm and sonic-wave
 * ripple.  Where the mask, drawn at (dx, dy) through the target's
 * transform, has alpha of at least half, each target pixel takes the
 * target pixel shift units to its right (to its left for a negative
 * shift), as the scene was before: walking away from the side read from,
 * a pixel is always read before it is changed.  Reads stop at the edge of
 * the clip.  Scale and translation only; with rotation it draws nothing.
 */
void a5_draw_displaced(ALLEGRO_BITMAP *mask, float dx, float dy, float shift)
{
   ALLEGRO_BITMAP *target = al_get_target_bitmap();
   const ALLEGRO_BITMAP *root;
   A5_FIXED a, d, e, f, u0, du, v, dv;
   int dx0, dx1, dy0, dy1, s, x, y, n;

   if (target == NULL || mask == NULL || mask->w <= 0 || mask->h <= 0)
      return;
   if (target->tb != 0 || target->tc != 0 || target->ta <= 0 || target->td <= 0)
      return;

   root = mask->parent ? mask->parent : mask;
   a = target->ta;
   d = target->td;
   e = a5_fixed_mul(a, a5_float_to_fixed(dx)) + target->te;
   f = a5_fixed_mul(d, a5_float_to_fixed(dy)) + target->tf;
   s = (a5_fixed_mul(a, a5_float_to_fixed(shift)) + 0x8000) >> 16;
   if (s == 0)
      return;

   dx0 = pixel_start(e);
   dx1 = pixel_start(e + a * mask->w);
   dy0 = pixel_start(f);
   dy1 = pixel_start(f + d * mask->h);
   if (dx0 < target->cl) dx0 = target->cl;
   if (dy0 < target->ct) dy0 = target->ct;
   if (dx1 > target->cr) dx1 = target->cr;
   if (dy1 > target->cb) dy1 = target->cb;
   if (dx0 >= dx1 || dy0 >= dy1)
      return;

   n = dx1 - dx0;
   u0 = fixed_div(pixel_centre(dx0) - e, a);
   du = fixed_reciprocal(a);
   v = fixed_div(pixel_centre(dy0) - f, d);
   dv = fixed_reciprocal(d);

   for (y = dy0; y < dy1; y++, v += dv) {
      const Uint32 *m = root->memory + (mask->yofs + clampi((int)(v >> 16), 0, mask->h - 1)) * root->pitch
         + mask->xofs;
      Uint32 *row = a5_bitmap_row(target, y);

      if (s > 0) {
         A5_FIXED u = u0;

         for (x = dx0; x < dx1; x++, u += du) {
            if ((m[clampi((int)(u >> 16), 0, mask->w - 1)] >> 24) >= 128) {
               const int from = (x + s < target->cr) ? x + s : target->cr - 1;

               row[x] = row[from];
            }
         }
      }
      else {
         A5_FIXED u = u0 + du * (n - 1);

         for (x = dx1 - 1; x >= dx0; x--, u -= du) {
            if ((m[clampi((int)(u >> 16), 0, mask->w - 1)] >> 24) >= 128) {
               const int from = (x + s >= target->cl) ? x + s : target->cl;

               row[x] = row[from];
            }
         }
      }
   }
}
