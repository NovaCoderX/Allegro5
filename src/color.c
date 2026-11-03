/* Colours: color.h.  Real implementation, as in Allegro's pixels.c, with
 * the conversion from a table.
 */

#include "allegro5/allegro.h"

/* n * (1.0f / 255.0f), as Allegro works it out, for every byte: worked out
 * by the compiler, so mapping a colour needs no floating point at run
 * time (a library call per operation in a software-float build).
 */
#define U8(n) ((n) * (1.0f / 255.0f))
#define U8x4(n) U8(n), U8(n + 1), U8(n + 2), U8(n + 3)
#define U8x16(n) U8x4(n), U8x4(n + 4), U8x4(n + 8), U8x4(n + 12)
#define U8x64(n) U8x16(n), U8x16(n + 16), U8x16(n + 32), U8x16(n + 48)

static const float s_u8_to_float[256] = {
   U8x64(0), U8x64(64), U8x64(128), U8x64(192)
};

ALLEGRO_COLOR al_map_rgba(
   unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
   ALLEGRO_COLOR color;
   color.r = s_u8_to_float[r];
   color.g = s_u8_to_float[g];
   color.b = s_u8_to_float[b];
   color.a = s_u8_to_float[a];
   return color;
}

ALLEGRO_COLOR al_map_rgb(
   unsigned char r, unsigned char g, unsigned char b)
{
   return al_map_rgba(r, g, b, 255);
}
