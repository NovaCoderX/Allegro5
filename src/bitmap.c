/* Bitmaps: bitmap.h, bitmap_lock.h, bitmap_io.h.
 *
 * Every bitmap is 32-bit ARGB in host byte order (see a5_internal.h);
 * memory and video bitmaps are the same thing.  Locks in that format point
 * straight at the pixels; locks in ABGR_8888_LE, the one Dune Dynasty asks
 * for (bytes R, G, B, A), go through a buffer that is converted on lock and
 * back on unlock.
 */

#include <stdlib.h>
#include <string.h>
#include <SDL_image.h>
#include "allegro5/allegro.h"
#include "a5_internal.h"
#include "a5_stub.h"

static int s_new_bitmap_flags = ALLEGRO_VIDEO_BITMAP;

void al_set_new_bitmap_flags(int flags)
{
   s_new_bitmap_flags = flags;
}

int al_get_new_bitmap_flags(void)
{
   return s_new_bitmap_flags;
}

/* Keeps a bitmap's clipping rectangle inside its root, since a sub-bitmap
 * may extend past its parent (Dune Dynasty's screen shake moves them).
 */
static void clamp_clip_to_root(ALLEGRO_BITMAP *bitmap)
{
   const ALLEGRO_BITMAP *root = bitmap->parent ? bitmap->parent : bitmap;

   if (bitmap->cl < -bitmap->xofs)
      bitmap->cl = -bitmap->xofs;
   if (bitmap->ct < -bitmap->yofs)
      bitmap->ct = -bitmap->yofs;
   if (bitmap->cr > root->w - bitmap->xofs)
      bitmap->cr = root->w - bitmap->xofs;
   if (bitmap->cb > root->h - bitmap->yofs)
      bitmap->cb = root->h - bitmap->yofs;
   if (bitmap->cr < bitmap->cl)
      bitmap->cr = bitmap->cl;
   if (bitmap->cb < bitmap->ct)
      bitmap->cb = bitmap->ct;
}

static ALLEGRO_BITMAP *new_bitmap(int w, int h)
{
   ALLEGRO_BITMAP *bitmap = calloc(1, sizeof(*bitmap));

   if (bitmap == NULL)
      return NULL;

   bitmap->w = w;
   bitmap->h = h;
   bitmap->flags = s_new_bitmap_flags;
   bitmap->cr = w;
   bitmap->cb = h;
   al_identity_transform(&bitmap->transform);
   a5_update_fixed_transform(bitmap);
   return bitmap;
}

/* A bitmap over pixels the wrapper does not own: the window surface. */
ALLEGRO_BITMAP *a5_create_bitmap_over(Uint32 *memory, int w, int h, int pitch)
{
   ALLEGRO_BITMAP *bitmap = new_bitmap(w, h);

   if (bitmap == NULL)
      return NULL;

   bitmap->flags = ALLEGRO_VIDEO_BITMAP;
   bitmap->memory = memory;
   bitmap->pitch = pitch;
   return bitmap;
}

void a5_set_bitmap_memory(ALLEGRO_BITMAP *bitmap, Uint32 *memory, int pitch)
{
   bitmap->memory = memory;
   bitmap->pitch = pitch;
}

/* New bitmaps are transparent black, as Allegro's memory bitmaps are in
 * practice; Dune Dynasty clears its atlases anyway.
 */
ALLEGRO_BITMAP *al_create_bitmap(int w, int h)
{
   ALLEGRO_BITMAP *bitmap;

   if (w <= 0 || h <= 0)
      return NULL;

   bitmap = new_bitmap(w, h);
   if (bitmap == NULL)
      return NULL;

   bitmap->memory = calloc((size_t)w * h, sizeof(Uint32));
   if (bitmap->memory == NULL) {
      free(bitmap);
      return NULL;
   }

   bitmap->pitch = w;
   bitmap->owns_memory = true;
   return bitmap;
}

ALLEGRO_BITMAP *al_create_sub_bitmap(ALLEGRO_BITMAP *parent,
   int x, int y, int w, int h)
{
   ALLEGRO_BITMAP *bitmap;

   if (parent == NULL)
      return NULL;

   if (parent->parent) {
      x += parent->xofs;
      y += parent->yofs;
      parent = parent->parent;
   }

   bitmap = new_bitmap(w, h);
   if (bitmap == NULL)
      return NULL;

   bitmap->flags = parent->flags;
   bitmap->parent = parent;
   bitmap->xofs = x;
   bitmap->yofs = y;
   clamp_clip_to_root(bitmap);
   return bitmap;
}

ALLEGRO_BITMAP *al_clone_bitmap(ALLEGRO_BITMAP *bitmap)
{
   ALLEGRO_BITMAP *clone;
   int y;

   if (bitmap == NULL)
      return NULL;

   clone = al_create_bitmap(bitmap->w, bitmap->h);
   if (clone == NULL)
      return NULL;

   /* The part of a sub-bitmap that is inside its root. */
   {
      const ALLEGRO_BITMAP *root = bitmap->parent ? bitmap->parent : bitmap;
      const int x0 = (bitmap->xofs < 0) ? -bitmap->xofs : 0;
      const int y0 = (bitmap->yofs < 0) ? -bitmap->yofs : 0;
      int x1 = bitmap->w;
      int y1 = bitmap->h;

      if (x1 > root->w - bitmap->xofs)
         x1 = root->w - bitmap->xofs;
      if (y1 > root->h - bitmap->yofs)
         y1 = root->h - bitmap->yofs;

      for (y = y0; y < y1 && x0 < x1; y++) {
         memcpy(clone->memory + y * clone->pitch + x0,
            a5_bitmap_row(bitmap, y) + x0, (size_t)(x1 - x0) * sizeof(Uint32));
      }
   }

   return clone;
}

void al_destroy_bitmap(ALLEGRO_BITMAP *bitmap)
{
   if (bitmap == NULL)
      return;

   /* The backbuffer belongs to its display. */
   if (bitmap->parent == NULL && !bitmap->owns_memory)
      return;

   if (bitmap->locked)
      al_unlock_bitmap(bitmap);

   a5_bitmap_destroyed(bitmap);

   if (bitmap->owns_memory)
      free(bitmap->memory);
   free(bitmap);
}

int al_get_bitmap_width(ALLEGRO_BITMAP *bitmap)
{
   return bitmap->w;
}

int al_get_bitmap_height(ALLEGRO_BITMAP *bitmap)
{
   return bitmap->h;
}

bool al_is_sub_bitmap(ALLEGRO_BITMAP *bitmap)
{
   return bitmap->parent != NULL;
}

void al_set_clipping_rectangle(int x, int y, int width, int height)
{
   ALLEGRO_BITMAP *bitmap = al_get_target_bitmap();

   if (bitmap == NULL)
      return;

   if (x < 0) {
      width += x;
      x = 0;
   }
   if (y < 0) {
      height += y;
      y = 0;
   }
   if (x + width > bitmap->w)
      width = bitmap->w - x;
   if (y + height > bitmap->h)
      height = bitmap->h - y;
   if (width < 0)
      width = 0;
   if (height < 0)
      height = 0;

   bitmap->cl = x;
   bitmap->ct = y;
   bitmap->cr = x + width;
   bitmap->cb = y + height;
   clamp_clip_to_root(bitmap);
}

/*--------------------------------------------------------------------------*/

ALLEGRO_LOCKED_REGION *al_lock_bitmap_region(ALLEGRO_BITMAP *bitmap,
   int x, int y, int width, int height, int format, int flags)
{
   const ALLEGRO_BITMAP *root;
   ALLEGRO_LOCKED_REGION *region;

   if (bitmap == NULL || bitmap->locked)
      return NULL;

   root = bitmap->parent ? bitmap->parent : bitmap;
   if (x < 0 || y < 0 || width <= 0 || height <= 0
      || x + width > bitmap->w || y + height > bitmap->h
      || x + bitmap->xofs < 0 || y + bitmap->yofs < 0
      || x + bitmap->xofs + width > root->w || y + bitmap->yofs + height > root->h) {
      return NULL;
   }

   region = &bitmap->locked_region;

   switch (format) {
      case ALLEGRO_PIXEL_FORMAT_ANY:
      case ALLEGRO_PIXEL_FORMAT_ANY_NO_ALPHA:
      case ALLEGRO_PIXEL_FORMAT_ANY_WITH_ALPHA:
      case ALLEGRO_PIXEL_FORMAT_ANY_32_NO_ALPHA:
      case ALLEGRO_PIXEL_FORMAT_ANY_32_WITH_ALPHA:
      case ALLEGRO_PIXEL_FORMAT_ARGB_8888:
         bitmap->lock_buffer = NULL;
         region->data = a5_bitmap_row(bitmap, y) + x;
         region->format = ALLEGRO_PIXEL_FORMAT_ARGB_8888;
         region->pitch = root->pitch * (int)sizeof(Uint32);
         break;

      case ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE: {
         int i, j;

         bitmap->lock_buffer = malloc((size_t)width * height * 4);
         if (bitmap->lock_buffer == NULL)
            return NULL;

         /* Always filled in, even for ALLEGRO_LOCK_WRITEONLY, so pixels the
          * caller does not write keep their colour on unlock.
          */
         for (j = 0; j < height; j++) {
            const Uint32 *src = a5_bitmap_row(bitmap, y + j) + x;
            unsigned char *dst = bitmap->lock_buffer + (size_t)j * width * 4;

            for (i = 0; i < width; i++) {
               const Uint32 p = src[i];

               dst[0] = (unsigned char)(p >> 16);
               dst[1] = (unsigned char)(p >> 8);
               dst[2] = (unsigned char)p;
               dst[3] = (unsigned char)(p >> 24);
               dst += 4;
            }
         }

         region->data = bitmap->lock_buffer;
         region->format = ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE;
         region->pitch = width * 4;
         break;
      }

      default:
         return NULL;
   }

   region->pixel_size = 4;
   bitmap->locked = true;
   bitmap->lock_x = x;
   bitmap->lock_y = y;
   bitmap->lock_w = width;
   bitmap->lock_h = height;
   bitmap->lock_flags = flags;
   return region;
}

ALLEGRO_LOCKED_REGION *al_lock_bitmap(ALLEGRO_BITMAP *bitmap,
   int format, int flags)
{
   if (bitmap == NULL)
      return NULL;

   return al_lock_bitmap_region(bitmap, 0, 0, bitmap->w, bitmap->h, format, flags);
}

void al_unlock_bitmap(ALLEGRO_BITMAP *bitmap)
{
   if (bitmap == NULL || !bitmap->locked)
      return;

   if (bitmap->lock_buffer) {
      if (!(bitmap->lock_flags & ALLEGRO_LOCK_READONLY)) {
         int i, j;

         for (j = 0; j < bitmap->lock_h; j++) {
            const unsigned char *src = bitmap->lock_buffer + (size_t)j * bitmap->lock_w * 4;
            Uint32 *dst = a5_bitmap_row(bitmap, bitmap->lock_y + j) + bitmap->lock_x;

            for (i = 0; i < bitmap->lock_w; i++) {
               dst[i] = ((Uint32)src[3] << 24) | ((Uint32)src[0] << 16)
                  | ((Uint32)src[1] << 8) | src[2];
               src += 4;
            }
         }
      }

      free(bitmap->lock_buffer);
      bitmap->lock_buffer = NULL;
   }

   bitmap->locked = false;
}

/*--------------------------------------------------------------------------*/

/* Image files, through SDL2_image (the owner's 68k build, which reads PNG
 * with its own stb_image and writes it with miniz, so it needs no libpng
 * or zlib).  Dune Dynasty loads gfx/rubblemask.png and saves screenshots.
 *
 * A loaded image becomes a bitmap of the new bitmap flags, its alpha
 * premultiplied as Allegro does unless ALLEGRO_NO_PREMULTIPLIED_ALPHA is in
 * the new bitmap flags.  Saving writes PNG only (the extension is not
 * checked); a bitmap on the display's backbuffer is written without alpha,
 * since the backbuffer's alpha bytes are whatever blending left there.
 */
ALLEGRO_BITMAP *al_load_bitmap(const char *filename)
{
   const bool premultiply = !(s_new_bitmap_flags & ALLEGRO_NO_PREMULTIPLIED_ALPHA);
   SDL_Surface *loaded, *argb;
   ALLEGRO_BITMAP *bitmap;
   int x, y;

   if (filename == NULL)
      return NULL;

   loaded = IMG_Load(filename);
   if (loaded == NULL) {
      a5_trace("al_load_bitmap: %s: %s\n", filename, IMG_GetError());
      return NULL;
   }

   argb = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0);
   SDL_FreeSurface(loaded);
   if (argb == NULL) {
      a5_trace("al_load_bitmap: %s: %s\n", filename, SDL_GetError());
      return NULL;
   }

   bitmap = al_create_bitmap(argb->w, argb->h);
   if (bitmap != NULL && SDL_LockSurface(argb) == 0) {
      for (y = 0; y < argb->h; y++) {
         const Uint32 *src = (const Uint32 *)((const Uint8 *)argb->pixels + y * argb->pitch);
         Uint32 *dst = a5_bitmap_row(bitmap, y);

         for (x = 0; x < argb->w; x++) {
            const Uint32 p = src[x];
            const unsigned int a = p >> 24;

            if (a == 255 || !premultiply) {
               dst[x] = p;
            }
            else {
               dst[x] = (p & 0xFF000000)
                  | (a5_div255(((p >> 16) & 0xFF) * a) << 16)
                  | (a5_div255(((p >> 8) & 0xFF) * a) << 8)
                  | a5_div255((p & 0xFF) * a);
            }
         }
      }
      SDL_UnlockSurface(argb);
   }

   SDL_FreeSurface(argb);
   return bitmap;
}

bool al_save_bitmap(const char *filename, ALLEGRO_BITMAP *bitmap)
{
   ALLEGRO_DISPLAY *display = a5_current_display();
   ALLEGRO_BITMAP *backbuffer = display ? al_get_backbuffer(display) : NULL;
   const ALLEGRO_BITMAP *root;
   SDL_Surface *surface;
   bool opaque, ok;

   if (filename == NULL || bitmap == NULL || bitmap->w <= 0 || bitmap->h <= 0)
      return false;

   root = bitmap->parent ? bitmap->parent : bitmap;
   opaque = (backbuffer != NULL && root == (backbuffer->parent ? backbuffer->parent : backbuffer));

   surface = SDL_CreateRGBSurfaceWithFormatFrom(a5_bitmap_row(bitmap, 0), bitmap->w, bitmap->h,
      32, root->pitch * (int)sizeof(Uint32),
      opaque ? SDL_PIXELFORMAT_RGB888 : SDL_PIXELFORMAT_ARGB8888);
   if (surface == NULL) {
      a5_trace("al_save_bitmap: %s: %s\n", filename, SDL_GetError());
      return false;
   }

   ok = (IMG_SavePNG(surface, filename) == 0);
   if (!ok)
      a5_trace("al_save_bitmap: %s: %s\n", filename, IMG_GetError());

   SDL_FreeSurface(surface);
   return ok;
}
