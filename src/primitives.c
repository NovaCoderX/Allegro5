/* Primitives addon: allegro_primitives.h.
 *
 * Follows Allegro's software primitives (prim_soft.c, tri_soft.c,
 * line_soft.c, high_primitives.c): shapes are built from the same
 * triangles, put through the target's transform, and filled with the same
 * rule (pixels whose centres are inside, top and left edges in, bottom and
 * right out), so pixel coverage matches Allegro's memory bitmaps.  In 16.16
 * fixed point: an axis-aligned rectangle (filled rectangles, thick
 * horizontal and vertical lines, rectangle outlines, rectangle pairs in
 * triangle lists) is filled as spans, and other triangles use exact
 * integer edges.  Only flat colours: a triangle takes its first vertex's
 * colour, and textures are not supported (Dune Dynasty uses neither).
 * One-pixel lines (thickness 0, which the game draws only at load time)
 * keep Allegro's float stepper.
 */

#include <math.h>

#include "allegro5/allegro.h"
#include "allegro5/allegro_primitives.h"
#include "a5_internal.h"

typedef struct A5_POINT {
   A5_FIXED x, y;
} A5_POINT;

bool al_init_primitives_addon(void)
{
   return true;
}

/* A point in the target's coordinates, through its transform. */
static A5_POINT to_target(const ALLEGRO_BITMAP *t, A5_FIXED x, A5_FIXED y)
{
   A5_POINT p;

   p.x = a5_fixed_mul(t->ta, x) + a5_fixed_mul(t->tc, y) + t->te;
   p.y = a5_fixed_mul(t->tb, x) + a5_fixed_mul(t->td, y) + t->tf;
   return p;
}

static A5_POINT vertex_to_target(const ALLEGRO_BITMAP *t, const ALLEGRO_VERTEX *v)
{
   return to_target(t, a5_float_to_fixed(v->x), a5_float_to_fixed(v->y));
}

/* The first pixel whose centre is at or right of (below) v: ceil(v - 0.5). */
static inline int pixel_start(A5_FIXED v)
{
   return (v - 0x8000 + 0xFFFF) >> 16;
}

/* Blends one span of a row with a colour. */
static void fill_span(Uint32 *row, int x0, int x1, Uint32 color, int kind)
{
   int x;

   if (kind == A5_BLEND_COPY || (kind == A5_BLEND_ALPHA && (color >> 24) == 255)) {
      a5_fill_pixels(row + x0, x1 - x0, color);
   }
   else if (kind == A5_BLEND_ALPHA) {
      /* A constant colour: its share of each channel is worked out once
       * (the shade under Dune Dynasty's dialogs covers the whole screen).
       */
      const Uint32 sa = color >> 24;
      const Uint32 ia = 255 - sa;
      const Uint32 crb = (color & 0x00FF00FF) * sa + 0x00800080;
      const Uint32 cag = ((color >> 8) & 0x00FF00FF) * sa + 0x00800080;

      if (sa == 0)
         return;

      /* a5_mix() with the colour's half worked out once. */
      for (x = x0; x < x1; x++) {
         const Uint32 d = row[x];
         Uint32 rb = crb + (d & 0x00FF00FF) * ia;
         Uint32 ag = cag + ((d >> 8) & 0x00FF00FF) * ia;

         rb = ((rb + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
         ag = (ag + ((ag >> 8) & 0x00FF00FF)) & 0xFF00FF00;
         row[x] = ag | rb;
      }
   }
   else {
      for (x = x0; x < x1; x++)
         row[x] = a5_blend_pixel(kind, color, row[x]);
   }
}

/* The pixels whose centres are in [x0, x1) x [y0, y1). */
static void fill_rect(ALLEGRO_BITMAP *target, A5_FIXED x0, A5_FIXED y0,
   A5_FIXED x1, A5_FIXED y1, Uint32 color)
{
   const int kind = a5_blend_kind();
   int px0 = pixel_start(x0), px1 = pixel_start(x1);
   int py0 = pixel_start(y0), py1 = pixel_start(y1);
   int y;

   if (px0 < target->cl) px0 = target->cl;
   if (py0 < target->ct) py0 = target->ct;
   if (px1 > target->cr) px1 = target->cr;
   if (py1 > target->cb) py1 = target->cb;

   for (y = py0; y < py1 && px0 < px1; y++)
      fill_span(a5_bitmap_row(target, y), px0, px1, color, kind);
}

/* x on the edge from a to b at height y, exactly (floor). */
static A5_FIXED edge_x(A5_POINT a, A5_POINT b, A5_FIXED y)
{
   return a.x + (A5_FIXED)a5_floor_div64((Sint64)(y - a.y) * (b.x - a.x), b.y - a.y);
}

/* Allegro's triangle_stepper() rule, with exact edges. */
static void fill_triangle(ALLEGRO_BITMAP *target, A5_POINT p1, A5_POINT p2,
   A5_POINT p3, Uint32 color)
{
   const int kind = a5_blend_kind();
   A5_POINT t;
   int y, y_start, y_end;

   if (p2.y < p1.y) { t = p1; p1 = p2; p2 = t; }
   if (p3.y < p1.y) { t = p1; p1 = p3; p3 = t; }
   if (p3.y < p2.y) { t = p2; p2 = p3; p3 = t; }

   y_start = pixel_start(p1.y);
   y_end = pixel_start(p3.y);
   if (y_start < target->ct)
      y_start = target->ct;
   if (y_end > target->cb)
      y_end = target->cb;

   for (y = y_start; y < y_end; y++) {
      const A5_FIXED yc = y * 65536 + 0x8000;
      const A5_FIXED xa = edge_x(p1, p3, yc);
      const A5_FIXED xb = (yc < p2.y) ? edge_x(p1, p2, yc)
         : (p3.y > p2.y) ? edge_x(p2, p3, yc) : p2.x;
      int x0 = pixel_start((xa < xb) ? xa : xb);
      int x1 = pixel_start((xa < xb) ? xb : xa);

      if (x0 < target->cl)
         x0 = target->cl;
      if (x1 > target->cr)
         x1 = target->cr;
      if (x0 < x1)
         fill_span(a5_bitmap_row(target, y), x0, x1, color, kind);
   }
}

/* If a, b, c, d go round an axis-aligned rectangle (a and c opposite
 * corners), fills it: the two triangles (a, b, c) and (a, c, d) cover
 * exactly its pixels.
 */
static bool fill_if_rect(ALLEGRO_BITMAP *target, A5_POINT a, A5_POINT b,
   A5_POINT c, A5_POINT d, Uint32 color)
{
   if (a.x == c.x || a.y == c.y)
      return false;

   if (!((b.x == a.x && b.y == c.y && d.x == c.x && d.y == a.y)
      || (b.x == c.x && b.y == a.y && d.x == a.x && d.y == c.y)))
      return false;

   fill_rect(target,
      (a.x < c.x) ? a.x : c.x, (a.y < c.y) ? a.y : c.y,
      (a.x < c.x) ? c.x : a.x, (a.y < c.y) ? c.y : a.y, color);
   return true;
}

static void plot(ALLEGRO_BITMAP *target, int x, int y, Uint32 color, int kind)
{
   if (x >= target->cl && x < target->cr && y >= target->ct && y < target->cb) {
      Uint32 *p = a5_bitmap_row(target, y) + x;

      *p = a5_blend_pixel(kind, color, *p);
   }
}

/* A one-pixel line, as Allegro's line_stepper(): the first pixel is drawn
 * and the last one is not when the ends are on pixel centres.
 */
static void hairline(ALLEGRO_BITMAP *target, float x1, float y1,
   float x2, float y2, Uint32 color)
{
   const int kind = a5_blend_kind();
   float dx, dy, err;
   int end_x, end_y, x, y;

   if (y2 < y1) {
      float t;
      t = x1; x1 = x2; x2 = t;
      t = y1; y1 = y2; y2 = t;
   }

   x1 -= 0.5001f;
   y1 -= 0.5001f;
   x2 -= 0.5001f;
   y2 -= 0.5001f;

   dx = x2 - x1;
   dy = y2 - y1;
   end_x = (int)floorf(x2 + 0.5f);
   end_y = (int)floorf(y2 + 0.5f);

#define FIRST \
   if ((x2 - x1) * ((float)x - x1) + (y2 - y1) * ((float)y - y1) >= 0) \
      plot(target, x, y, color, kind);
#define STEP \
   plot(target, x, y, color, kind);
#define LAST \
   if ((x1 - x2) * ((float)x - x2) + (y1 - y2) * ((float)y - y2) > 0) \
      plot(target, x, y, color, kind);
#define WORKER(var1, var2, comp, dvar1, dvar2, derr1, derr2, func) \
   { \
      if (err comp) { \
         var1 += dvar1; \
         err += derr1; \
      } \
      func \
      var2 += dvar2; \
      err += derr2; \
   }

   if (dx > 0) {
      if (dx > dy) {
         x = (int)floorf(x1 + 0.5f);
         y = (int)floorf(y1);
         err = (y1 - (float)y) * dx - (x1 - (float)x) * dy;

         if (x < end_x)
            WORKER(y, x, > 0.5f * dx, 1, 1, -dx, dy, FIRST)
         while (x < end_x)
            WORKER(y, x, > 0.5f * dx, 1, 1, -dx, dy, STEP)
         if (x <= end_x)
            WORKER(y, x, > 0.5f * dx, 1, 1, -dx, dy, LAST)
      }
      else {
         x = (int)floorf(x1);
         y = (int)floorf(y1 + 0.5f);
         err = (x1 - (float)x) * dy - (y1 - (float)y) * dx;

         if (y < end_y)
            WORKER(x, y, > 0.5f * dy, 1, 1, -dy, dx, FIRST)
         while (y < end_y)
            WORKER(x, y, > 0.5f * dy, 1, 1, -dy, dx, STEP)
         if (y <= end_y)
            WORKER(x, y, > 0.5f * dy, 1, 1, -dy, dx, LAST)
      }
   }
   else {
      if (-dx > dy) {
         x = (int)floorf(x1 + 0.5f);
         y = (int)floorf(y1);
         err = (y1 - (float)y) * dx - (x1 - (float)x) * dy;

         if (x > end_x)
            WORKER(y, x, <= 0.5f * dx, 1, -1, -dx, -dy, FIRST)
         while (x > end_x)
            WORKER(y, x, <= 0.5f * dx, 1, -1, -dx, -dy, STEP)
         if (x >= end_x)
            WORKER(y, x, <= 0.5f * dx, 1, -1, -dx, -dy, LAST)
      }
      else {
         x = (int)floorf(x1);
         y = (int)floorf(y1 + 0.5f);
         err = (x1 - (float)x) * dy - (y1 - (float)y) * dx;

         if (err > 0.5f * dy) {
            x += 1;
            err -= dy;
         }

         if (y < end_y)
            WORKER(x, y, <= -0.5f * dy, -1, 1, dy, dx, FIRST)
         while (y < end_y)
            WORKER(x, y, <= -0.5f * dy, -1, 1, dy, dx, STEP)
         if (y <= end_y)
            WORKER(x, y, <= -0.5f * dy, -1, 1, dy, dx, LAST)
      }
   }

#undef FIRST
#undef STEP
#undef LAST
#undef WORKER
}

static void line(ALLEGRO_BITMAP *target, const ALLEGRO_VERTEX *v1,
   const ALLEGRO_VERTEX *v2)
{
   const A5_POINT p1 = vertex_to_target(target, v1);
   const A5_POINT p2 = vertex_to_target(target, v2);
   const float scale = 1.0f / 65536.0f;

   hairline(target, p1.x * scale, p1.y * scale, p2.x * scale, p2.y * scale,
      a5_color_to_pixel(v1->color));
}

/* Allegro's al_draw_prim() and al_draw_indexed_prim(), for vertices
 * index[0] .. index[n - 1] (or 0 .. n - 1 without an index).
 */
static int draw_prim(const ALLEGRO_VERTEX *vtx, const int *index, int n, int type)
{
   ALLEGRO_BITMAP *target = al_get_target_bitmap();
   int i;

#define V(i) (&vtx[index ? index[i] : (i)])
#define P(i) vertex_to_target(target, V(i))
#define COLOR(i) a5_color_to_pixel(V(i)->color)

   if (target == NULL)
      return 0;

   switch (type) {
      case ALLEGRO_PRIM_LINE_LIST:
         for (i = 0; i < n - 1; i += 2)
            line(target, V(i), V(i + 1));
         return n / 2;

      case ALLEGRO_PRIM_LINE_STRIP:
         for (i = 1; i < n; i++)
            line(target, V(i - 1), V(i));
         return n - 1;

      case ALLEGRO_PRIM_LINE_LOOP:
         for (i = 1; i < n; i++)
            line(target, V(i - 1), V(i));
         if (n > 1)
            line(target, V(n - 1), V(0));
         return n;

      case ALLEGRO_PRIM_TRIANGLE_LIST:
         for (i = 0; i < n - 2; i += 3) {
            const A5_POINT a = P(i), b = P(i + 1), c = P(i + 2);

            /* (a, b, c), (a, c, d) making a rectangle: Dune Dynasty's
             * window borders.
             */
            if (i + 5 < n) {
               const A5_POINT a2 = P(i + 3), c2 = P(i + 4), d = P(i + 5);

               if (a2.x == a.x && a2.y == a.y && c2.x == c.x && c2.y == c.y
                  && COLOR(i) == COLOR(i + 3) && fill_if_rect(target, a, b, c, d, COLOR(i))) {
                  i += 3;
                  continue;
               }
            }
            fill_triangle(target, a, b, c, COLOR(i));
         }
         return n / 3;

      case ALLEGRO_PRIM_TRIANGLE_STRIP:
         for (i = 2; i < n; i++)
            fill_triangle(target, P(i - 2), P(i - 1), P(i), COLOR(i - 2));
         return n - 2;

      case ALLEGRO_PRIM_TRIANGLE_FAN:
         if (n == 4 && fill_if_rect(target, P(0), P(1), P(2), P(3), COLOR(0)))
            return 2;
         for (i = 2; i < n; i++)
            fill_triangle(target, P(0), P(i), P(i - 1), COLOR(0));
         return n - 2;

      default:
         return 0;
   }

#undef V
#undef P
#undef COLOR
}

int al_draw_indexed_prim(const void *vtxs, const ALLEGRO_VERTEX_DECL *decl,
   ALLEGRO_BITMAP *texture, const int *indices, int num_vtx, int type)
{
   if (decl != NULL || texture != NULL)
      return 0;

   return draw_prim(vtxs, indices, num_vtx, type);
}

/* sqrt of a 64-bit value, rounded down. */
static Uint32 isqrt64(Uint64 v)
{
   Uint64 bit = (Uint64)1 << 62, r = 0;

   while (bit > v)
      bit >>= 2;
   while (bit) {
      if (v >= r + bit) {
         v -= r + bit;
         r = (r >> 1) + bit;
      }
      else {
         r >>= 1;
      }
      bit >>= 2;
   }
   return (Uint32)r;
}

/* Allegro's al_draw_line(): a quad of the line's thickness, or a one-pixel
 * line for thickness 0.
 */
void al_draw_line(float x1, float y1, float x2, float y2,
   ALLEGRO_COLOR color, float thickness)
{
   ALLEGRO_BITMAP *target = al_get_target_bitmap();
   const A5_FIXED t = a5_float_to_fixed(thickness);
   const Uint32 pixel = a5_color_to_pixel(color);
   const A5_FIXED fx1 = a5_float_to_fixed(x1), fy1 = a5_float_to_fixed(y1);
   const A5_FIXED fx2 = a5_float_to_fixed(x2), fy2 = a5_float_to_fixed(y2);
   const A5_FIXED dx = fx2 - fx1, dy = fy2 - fy1;
   A5_FIXED tx, ty;
   A5_POINT p[4];

   if (target == NULL)
      return;

   if (t <= 0) {
      ALLEGRO_VERTEX v[2];

      v[0].x = x1; v[0].y = y1; v[0].color = color;
      v[1].x = x2; v[1].y = y2; v[1].color = color;
      draw_prim(v, NULL, 2, ALLEGRO_PRIM_LINE_LIST);
      return;
   }

   /* tx = t/2 dy / len, ty = -t/2 dx / len. */
   if (dx == 0 && dy == 0)
      return;
   if (dy == 0) {
      tx = 0;
      ty = (dx > 0) ? -t / 2 : t / 2;
   }
   else if (dx == 0) {
      tx = (dy > 0) ? t / 2 : -t / 2;
      ty = 0;
   }
   else {
      const A5_FIXED len = (A5_FIXED)isqrt64((Uint64)((Sint64)dx * dx + (Sint64)dy * dy));

      if (len == 0)
         return;
      tx = (A5_FIXED)a5_floor_div64((Sint64)(t / 2) * dy, len);
      ty = (A5_FIXED)a5_floor_div64(-(Sint64)(t / 2) * dx, len);
   }

   p[0] = to_target(target, fx1 + tx, fy1 + ty);
   p[1] = to_target(target, fx1 - tx, fy1 - ty);
   p[2] = to_target(target, fx2 - tx, fy2 - ty);
   p[3] = to_target(target, fx2 + tx, fy2 + ty);

   if (!fill_if_rect(target, p[0], p[1], p[2], p[3], pixel)) {
      fill_triangle(target, p[0], p[2], p[1], pixel);
      fill_triangle(target, p[0], p[3], p[2], pixel);
   }
}

/* Allegro's al_draw_rectangle(): a ring of the line's thickness (a strip of
 * triangles, four rectangles when nothing is rotated), or a loop of
 * one-pixel lines for thickness 0.
 */
void al_draw_rectangle(float x1, float y1, float x2, float y2,
   ALLEGRO_COLOR color, float thickness)
{
   ALLEGRO_BITMAP *target = al_get_target_bitmap();
   const A5_FIXED t = a5_float_to_fixed(thickness) / 2;
   const A5_FIXED fx1 = a5_float_to_fixed(x1), fy1 = a5_float_to_fixed(y1);
   const A5_FIXED fx2 = a5_float_to_fixed(x2), fy2 = a5_float_to_fixed(y2);
   const Uint32 pixel = a5_color_to_pixel(color);
   A5_POINT o0, o1, i0, i1, p[10];
   int i;

   if (target == NULL)
      return;

   if (t <= 0) {
      ALLEGRO_VERTEX v[4];

      v[0].x = x1; v[0].y = y1;
      v[1].x = x2; v[1].y = y1;
      v[2].x = x2; v[2].y = y2;
      v[3].x = x1; v[3].y = y2;
      for (i = 0; i < 4; i++)
         v[i].color = color;
      draw_prim(v, NULL, 4, ALLEGRO_PRIM_LINE_LOOP);
      return;
   }

   o0 = to_target(target, fx1 - t, fy1 - t);
   o1 = to_target(target, fx2 + t, fy2 + t);
   i0 = to_target(target, fx1 + t, fy1 + t);
   i1 = to_target(target, fx2 - t, fy2 - t);

   if (a5_is_axis_aligned(target) && o0.x < i0.x && i0.x < i1.x && i1.x < o1.x
      && o0.y < i0.y && i0.y < i1.y && i1.y < o1.y) {
      fill_rect(target, o0.x, o0.y, o1.x, i0.y, pixel);
      fill_rect(target, o0.x, i1.y, o1.x, o1.y, pixel);
      fill_rect(target, o0.x, i0.y, i0.x, i1.y, pixel);
      fill_rect(target, i1.x, i0.y, o1.x, i1.y, pixel);
      return;
   }

   /* Allegro's strip: outer and inner corners in turn. */
   p[0] = o0;
   p[1] = i0;
   p[2] = to_target(target, fx2 + t, fy1 - t);
   p[3] = to_target(target, fx2 - t, fy1 + t);
   p[4] = o1;
   p[5] = i1;
   p[6] = to_target(target, fx1 - t, fy2 + t);
   p[7] = to_target(target, fx1 + t, fy2 - t);
   p[8] = o0;
   p[9] = i0;
   for (i = 2; i < 10; i++)
      fill_triangle(target, p[i - 2], p[i - 1], p[i], pixel);
}

/* Allegro's al_draw_filled_rectangle(): a fan of two triangles. */
void al_draw_filled_rectangle(float x1, float y1, float x2, float y2,
   ALLEGRO_COLOR color)
{
   ALLEGRO_BITMAP *target = al_get_target_bitmap();
   const A5_FIXED fx1 = a5_float_to_fixed(x1), fy1 = a5_float_to_fixed(y1);
   const A5_FIXED fx2 = a5_float_to_fixed(x2), fy2 = a5_float_to_fixed(y2);
   const Uint32 pixel = a5_color_to_pixel(color);
   A5_POINT a, b, c, d;

   if (target == NULL)
      return;

   a = to_target(target, fx1, fy1);
   b = to_target(target, fx1, fy2);
   c = to_target(target, fx2, fy2);
   d = to_target(target, fx2, fy1);

   if (!fill_if_rect(target, a, b, c, d, pixel)) {
      fill_triangle(target, a, c, b, pixel);
      fill_triangle(target, a, d, c, pixel);
   }
}
