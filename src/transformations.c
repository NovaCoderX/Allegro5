/* Transformations: transformations.h.
 *
 * al_identity_transform() and al_build_transform() are the real code from
 * Allegro's transformations.c.  As in Allegro, a transform belongs to the
 * target bitmap it was used on, and applies again whenever that bitmap is
 * the target: Dune Dynasty keeps one sub-bitmap of the backbuffer per
 * screen area, each with its own scale.
 */

#include <math.h>
#include "allegro5/allegro.h"
#include "a5_internal.h"

void al_identity_transform(ALLEGRO_TRANSFORM *trans)
{
   trans->m[0][0] = 1;
   trans->m[0][1] = 0;
   trans->m[0][2] = 0;
   trans->m[0][3] = 0;

   trans->m[1][0] = 0;
   trans->m[1][1] = 1;
   trans->m[1][2] = 0;
   trans->m[1][3] = 0;

   trans->m[2][0] = 0;
   trans->m[2][1] = 0;
   trans->m[2][2] = 1;
   trans->m[2][3] = 0;

   trans->m[3][0] = 0;
   trans->m[3][1] = 0;
   trans->m[3][2] = 0;
   trans->m[3][3] = 1;
}

void al_build_transform(ALLEGRO_TRANSFORM *trans, float x, float y,
   float sx, float sy, float theta)
{
   float c, s;

   c = cosf(theta);
   s = sinf(theta);

   trans->m[0][0] = sx * c;
   trans->m[0][1] = sy * s;
   trans->m[0][2] = 0;
   trans->m[0][3] = 0;

   trans->m[1][0] = -sx * s;
   trans->m[1][1] = sy * c;
   trans->m[1][2] = 0;
   trans->m[1][3] = 0;

   trans->m[2][0] = 0;
   trans->m[2][1] = 0;
   trans->m[2][2] = 1;
   trans->m[2][3] = 0;

   trans->m[3][0] = x;
   trans->m[3][1] = y;
   trans->m[3][2] = 0;
   trans->m[3][3] = 1;
}

void al_use_transform(const ALLEGRO_TRANSFORM *trans)
{
   ALLEGRO_BITMAP *target = al_get_target_bitmap();

   if (target) {
      target->transform = *trans;
      a5_update_fixed_transform(target);
   }
}

/* Allegro applies a transform as x' = x m00 + y m10 + m30,
 * y' = x m01 + y m11 + m31; the wrapper draws in 2D, in 16.16.
 */
void a5_update_fixed_transform(ALLEGRO_BITMAP *bitmap)
{
   const ALLEGRO_TRANSFORM *t = &bitmap->transform;

   bitmap->ta = a5_float_to_fixed(t->m[0][0]);
   bitmap->tb = a5_float_to_fixed(t->m[0][1]);
   bitmap->tc = a5_float_to_fixed(t->m[1][0]);
   bitmap->td = a5_float_to_fixed(t->m[1][1]);
   bitmap->te = a5_float_to_fixed(t->m[3][0]);
   bitmap->tf = a5_float_to_fixed(t->m[3][1]);
}
