/* Amiga extensions of the Allegro 5 wrapper (not part of Allegro). */
#ifndef __al_included_allegro5_allegro_amiga_h
#define __al_included_allegro5_allegro_amiga_h

#include "allegro5/allegro.h"

#ifdef __cplusplus
   extern "C" {
#endif

/* The next display's window (or fullscreen screen) is this many times the
 * size passed to al_create_display(), 1 to 4, and every flip scales the
 * picture up by it: the program draws at the small size, a quarter of the
 * pixels at 2, and sees mouse positions at that size too.
 */
void a5_set_new_display_scale(int scale);

/* How the next display's flips scale the picture up, at a display scale
 * of 2 (any other scale is always nearest):
 *
 * - A5_FILTER_NEAREST: each pixel a 2x2 block.
 * - A5_FILTER_SCALE2X: Scale2x, which rounds off diagonal edges.
 * - A5_FILTER_SCALE2X_ADV: Scale2x, but a pixel much brighter than the
 *   pixels round it stays a 2x2 block, so thin white text stays sharp
 *   (after OpenDUNE's Amiga port).
 */
enum {
   A5_FILTER_NEAREST,
   A5_FILTER_SCALE2X,
   A5_FILTER_SCALE2X_ADV
};

void a5_set_new_display_filter(int filter);

/* Dune II's sandworm and sonic-wave ripple, which Dune Dynasty draws with an
 * OpenGL or Direct3D stencil: where mask (drawn at dx, dy through the
 * target's transform) has alpha of at least half, each target pixel takes
 * the target pixel shift units to its right, as the scene was.  Scale and
 * translation only.
 */
void a5_draw_displaced(ALLEGRO_BITMAP *mask, float dx, float dy, float shift);

/* A section's entries, keys and comments, removed, but for the blank lines
 * at its end, keeping its place in the file; then a5_append_config_line()
 * adds a key (or with key NULL, a comment, given with its '#') after the
 * section's last line that is not blank, so a section can be written out
 * again in a set order, comments and all.
 */
void a5_clear_config_section(ALLEGRO_CONFIG *config, const char *section);
void a5_append_config_line(ALLEGRO_CONFIG *config, const char *section,
   const char *key, const char *value);

#ifdef __cplusplus
   }
#endif

#endif
