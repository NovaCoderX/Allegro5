/* Internals shared between the wrapper's modules. */
#ifndef A5_INTERNAL_H
#define A5_INTERNAL_H

#include <SDL.h>
#include "allegro5/allegro.h"

/* What the wrapper keeps inside an ALLEGRO_EVENT_SOURCE's padding: the
 * queues it feeds and, for a timer's source, the timer.  may_alias, since
 * it is accessed through that int padding, which strict aliasing would
 * otherwise let GCC reorder.
 */
#define A5_MAX_SOURCE_QUEUES  8

typedef struct __attribute__((may_alias)) A5_EVENT_SOURCE {
   ALLEGRO_EVENT_QUEUE *queues[A5_MAX_SOURCE_QUEUES];
   int num_queues;
   ALLEGRO_TIMER *timer;
} A5_EVENT_SOURCE;

#define A5_SOURCE(source)  ((A5_EVENT_SOURCE *)(source))

/* For event sources that are not the first member of a malloc'd object. */
#define A5_EVENT_SOURCE_ALIGN  _Alignas(A5_EVENT_SOURCE)

union SDL_Event;

/* system.c.  SDL's input is pumped by the task that owns the window, which
 * is the one that called al_install_system(); elsewhere pumping does nothing.
 */
bool a5_is_main_thread(void);
void a5_pump_sdl_events(void);

/* keyboard.c and mouse.c: turn SDL input events into Allegro events. */
void a5_keyboard_handle_event(const union SDL_Event *event);
void a5_mouse_handle_event(const union SDL_Event *event);

/* events.c.  One lock guards every queue, event source and timer, since
 * the game and its music thread both use them.
 */
bool a5_events_init(void);
void a5_events_shutdown(void);
void a5_lock(void);
void a5_unlock(void);
void a5_event_source_init(ALLEGRO_EVENT_SOURCE *source);

/* Call these with the lock held. */
void a5_event_source_free_locked(ALLEGRO_EVENT_SOURCE *source);
void a5_emit_event_locked(ALLEGRO_EVENT_SOURCE *source, ALLEGRO_EVENT *event);
unsigned int a5_queued_events_locked(const ALLEGRO_EVENT_SOURCE *source);

/* timer.c.  Call these with the lock held. */
bool a5_timer_next_tick_locked(const ALLEGRO_TIMER *timer, double *when);
void a5_timer_tick_locked(ALLEGRO_TIMER *timer, double now);

/* Bitmaps (bitmap.c).  Pixels are 32-bit ARGB (0xAARRGGBB) in host byte
 * order, the format of SDL2-NovaMesa's window surface, so the backbuffer is
 * a bitmap over that surface.  A sub-bitmap shares its root's pixels.
 */
struct ALLEGRO_BITMAP {
   int w, h;
   int flags;
   Uint32 *memory;               /* root: the pixels */
   int pitch;                    /* root: in pixels */
   bool owns_memory;             /* root: not the window surface */
   ALLEGRO_BITMAP *parent;       /* a sub-bitmap's root, else NULL */
   int xofs, yofs;               /* position in the root */

   /* Clipping rectangle, right and bottom exclusive.  Like the transform,
    * it belongs to the bitmap and applies while it is the target.
    */
   int cl, ct, cr, cb;
   ALLEGRO_TRANSFORM transform;

   /* The same transform in 16.16 fixed point (see A5_FIXED), so drawing
    * needs no floating point: x' = ta x + tc y + te, y' = tb x + td y + tf.
    */
   Sint32 ta, tb, tc, td, te, tf;

   /* Lock; buffer is NULL when the region points at the pixels. */
   bool locked;
   int lock_x, lock_y, lock_w, lock_h, lock_flags;
   ALLEGRO_LOCKED_REGION locked_region;
   unsigned char *lock_buffer;
};

ALLEGRO_BITMAP *a5_create_bitmap_over(Uint32 *memory, int w, int h, int pitch);
void a5_set_bitmap_memory(ALLEGRO_BITMAP *bitmap, Uint32 *memory, int pitch);

/* The first pixel of row y of a bitmap (in its own coordinates). */
static inline Uint32 *a5_bitmap_row(const ALLEGRO_BITMAP *bitmap, int y)
{
   const ALLEGRO_BITMAP *root = bitmap->parent ? bitmap->parent : bitmap;

   return root->memory + (y + bitmap->yofs) * root->pitch + bitmap->xofs;
}

/* display.c: the display, the target bitmap and the blender. */
ALLEGRO_DISPLAY *a5_current_display(void);
int a5_display_scale(void);
void a5_copy_pixels(Uint32 *dst, const Uint32 *src, int n);

/* Scale2x from src (w x h) into dst (2w x 2h), scale2x.c; with
 * keep_bright_text, a pixel much brighter than its neighbours stays a
 * plain 2x2 block.
 */
void a5_scale2x(const Uint32 *src, int src_pitch, int w, int h,
   Uint32 *dst, int dst_pitch, bool keep_bright_text);
SDL_Window *a5_display_window(ALLEGRO_DISPLAY *display);
void a5_display_handle_event(const union SDL_Event *event);
void a5_bitmap_destroyed(ALLEGRO_BITMAP *bitmap);

/* How drawing combines a source pixel with the destination, from the
 * blender: the four combinations Dune Dynasty uses have their own code.
 */
enum {
   A5_BLEND_COPY,                /* ADD, ONE, ZERO */
   A5_BLEND_ALPHA,               /* ADD, ALPHA, INVERSE_ALPHA */
   A5_BLEND_PREMULTIPLIED,       /* ADD, ONE, INVERSE_ALPHA (the default) */
   A5_BLEND_ADDITIVE,            /* ADD, ALPHA, ONE */
   A5_BLEND_GENERIC
};

int a5_blend_kind(void);
Uint32 a5_blend_generic(Uint32 src, Uint32 dst);

/* Sets n pixels to one value, eight at a time. */
static inline void a5_fill_pixels(Uint32 *p, int n, Uint32 pixel)
{
   while (n >= 8) {
      p[0] = pixel; p[1] = pixel; p[2] = pixel; p[3] = pixel;
      p[4] = pixel; p[5] = pixel; p[6] = pixel; p[7] = pixel;
      p += 8;
      n -= 8;
   }
   while (n-- > 0)
      *p++ = pixel;
}

/* 16.16 fixed point.  The drawing code avoids floating point, which on the
 * 68060 is slow (and in a software-float build a library call per
 * operation); values arriving as floats through the Allegro API are
 * converted with integer operations on their bits.
 */
typedef Sint32 A5_FIXED;

#define A5_FIXED_ONE 0x10000

/* A float as 16.16, rounded to nearest; saturates beyond +-32767. */
static inline A5_FIXED a5_float_to_fixed(float f)
{
   union { float f; Uint32 u; } v;
   Uint32 mant;
   int shift;
   A5_FIXED r;

   v.f = f;
   /* f = 1.mant * 2^(exp - 127), so f * 2^16 = mant24 << (exp - 127 - 7). */
   shift = (int)((v.u >> 23) & 0xFF) - 134;
   if (shift < -25)
      return 0;
   if (shift > 7)
      return (v.u >> 31) ? -0x7FFFFFFF : 0x7FFFFFFF;

   mant = (v.u & 0x7FFFFF) | 0x800000;
   if (shift >= 0)
      r = (A5_FIXED)(mant << shift);
   else
      r = (A5_FIXED)((mant + (1UL << (-shift - 1))) >> -shift);

   return (v.u >> 31) ? -r : r;
}

/* a * b for 16.16 values, with the scales Dune Dynasty uses (1 and 2)
 * done without a 64-bit multiply, which the 68060 does in software.
 */
static inline A5_FIXED a5_fixed_mul(A5_FIXED a, A5_FIXED b)
{
   if (a == 0 || b == 0)
      return 0;
   if (a == A5_FIXED_ONE)
      return b;
   if (b == A5_FIXED_ONE)
      return a;
   if (a == -A5_FIXED_ONE)
      return -b;
   if (b == -A5_FIXED_ONE)
      return -a;
   if (a == 2 * A5_FIXED_ONE)
      return 2 * b;
   if (b == 2 * A5_FIXED_ONE)
      return 2 * a;
   return (A5_FIXED)(((Sint64)a * b) >> 16);
}

/* floor(n / d) for d != 0. */
static inline Sint64 a5_floor_div64(Sint64 n, Sint64 d)
{
   Sint64 q = n / d;

   if ((n % d != 0) && ((n < 0) != (d < 0)))
      q--;
   return q;
}

/* round(x / 255) for 0 <= x <= 255 * 255. */
static inline unsigned int a5_div255(unsigned int x)
{
   x += 128;
   return (x + (x >> 8)) >> 8;
}

/* An ALLEGRO_COLOR as a pixel, rounding (Allegro truncates, which gives
 * 254 for some colours made with al_map_rgb(255, ...)), in integers.
 */
static inline unsigned int a5_color_channel(float c)
{
   const A5_FIXED f = a5_float_to_fixed(c);

   if (f <= 0)
      return 0;
   if (f >= A5_FIXED_ONE)
      return 255;
   return (unsigned int)(f * 255 + 0x8000) >> 16;
}

static inline Uint32 a5_color_to_pixel(ALLEGRO_COLOR c)
{
   return ((Uint32)a5_color_channel(c.a) << 24) | (a5_color_channel(c.r) << 16)
      | (a5_color_channel(c.g) << 8) | a5_color_channel(c.b);
}

/* src multiplied by tint, channel by channel. */
static inline Uint32 a5_tint_pixel(Uint32 src, Uint32 tint)
{
   return ((Uint32)a5_div255((src >> 24) * (tint >> 24)) << 24)
      | (a5_div255(((src >> 16) & 0xFF) * ((tint >> 16) & 0xFF)) << 16)
      | (a5_div255(((src >> 8) & 0xFF) * ((tint >> 8) & 0xFF)) << 8)
      | a5_div255((src & 0xFF) * (tint & 0xFF));
}

/* One channel of src * sf + dst * df, both factors out of 255, clamped. */
static inline Uint32 a5_blend_add(Uint32 s, Uint32 sf, Uint32 d, Uint32 df)
{
   const Uint32 x = s * sf + d * df;

   return (x >= 255 * 255) ? 255 : a5_div255(x);
}

/* Each channel of (s * sa + d * ia) / 255, rounded, for sa + ia = 255: red
 * and blue in one multiply and alpha and green in another, as V-WARS
 * darkens, since each channel's product fits in its own 16 bits.  The
 * rounding is a5_div255() done in both halves at once.
 */
static inline Uint32 a5_mix(Uint32 s, Uint32 d, Uint32 sa, Uint32 ia)
{
   Uint32 rb = (s & 0x00FF00FF) * sa + (d & 0x00FF00FF) * ia + 0x00800080;
   Uint32 ag = ((s >> 8) & 0x00FF00FF) * sa + ((d >> 8) & 0x00FF00FF) * ia + 0x00800080;

   rb = ((rb + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
   ag = (ag + ((ag >> 8) & 0x00FF00FF)) & 0xFF00FF00;
   return ag | rb;
}

/* Allegro's blending, as in its software renderer, for one pixel. */
static inline Uint32 a5_blend_pixel(int kind, Uint32 s, Uint32 d)
{
   Uint32 sa, ia;

   switch (kind) {
      case A5_BLEND_COPY:
         return s;

      case A5_BLEND_ALPHA:
         sa = s >> 24;
         if (sa == 255)
            return s;
         if (sa == 0)
            return d;
         return a5_mix(s, d, sa, 255 - sa);

      case A5_BLEND_PREMULTIPLIED:
         ia = 255 - (s >> 24);
         return (a5_blend_add(s >> 24, 255, d >> 24, ia) << 24)
            | (a5_blend_add((s >> 16) & 0xFF, 255, (d >> 16) & 0xFF, ia) << 16)
            | (a5_blend_add((s >> 8) & 0xFF, 255, (d >> 8) & 0xFF, ia) << 8)
            | a5_blend_add(s & 0xFF, 255, d & 0xFF, ia);

      case A5_BLEND_ADDITIVE:
         sa = s >> 24;
         if (sa == 0)
            return d;
         return (a5_blend_add(sa, sa, d >> 24, 255) << 24)
            | (a5_blend_add((s >> 16) & 0xFF, sa, (d >> 16) & 0xFF, 255) << 16)
            | (a5_blend_add((s >> 8) & 0xFF, sa, (d >> 8) & 0xFF, 255) << 8)
            | a5_blend_add(s & 0xFF, sa, d & 0xFF, 255);

      default:
         return a5_blend_generic(s, d);
   }
}

/* transformations.c: sets the bitmap's fixed-point transform from its
 * float one.
 */
void a5_update_fixed_transform(ALLEGRO_BITMAP *bitmap);

/* A transform with no rotation or shear: the fast drawing paths. */
static inline bool a5_is_axis_aligned(const ALLEGRO_BITMAP *bitmap)
{
   return bitmap->tb == 0 && bitmap->tc == 0;
}

#endif
