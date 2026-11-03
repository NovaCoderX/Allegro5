/* Display, render target and blending: display.h, drawing.h, blender.h.
 *
 * One display: an SDL window, or with ALLEGRO_FULLSCREEN or
 * ALLEGRO_FULLSCREEN_WINDOW an SDL_WINDOW_FULLSCREEN one.  Never
 * SDL_WINDOW_FULLSCREEN_DESKTOP: SDL2-NovaMesa then opens the screen at the
 * desktop size and scales to it, which is slow.  The wrapper draws in
 * software straight into the window surface, which SDL2-NovaMesa keeps in
 * fast RAM as ARGB8888 and copies to the RTG screen on
 * SDL_UpdateWindowSurface().  ALLEGRO_OPENGL is ignored.
 *
 * With a display scale (a5_set_new_display_scale(), allegro_amiga.h) the
 * window is that many times bigger than the display: the backbuffer is a
 * bitmap of the display's size, and each flip scales it up into the window
 * surface, each pixel written that many times across and each row once,
 * then copied for the rows below.  At a scale of 2 the flip can use
 * Scale2x instead (a5_set_new_display_filter(), scale2x.c).
 */

#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#if defined(SDL_VIDEO_DRIVER_AMIGAOS3)
#include <SDL_syswm.h>
#endif
#if defined(__AMIGA__)
#include <proto/exec.h>
#endif
#include "allegro5/allegro.h"
#include "allegro5/allegro_amiga.h"
#include "a5_internal.h"
#include "a5_stub.h"

struct ALLEGRO_DISPLAY {
   ALLEGRO_EVENT_SOURCE es;      /* first, so it is aligned for A5_SOURCE */
   SDL_Window *window;
   SDL_Surface *surface;         /* the window surface */
   SDL_Surface *shadow;          /* drawn into when the window surface is
                                    not 32-bit RGB, and blitted on flip */
   int flags;
   int scale;                    /* window size / display size */
   int filter;                   /* A5_FILTER_*, at a scale of 2 */
   ALLEGRO_BITMAP *backbuffer;
};

static int s_new_display_flags = ALLEGRO_WINDOWED;
static int s_new_display_scale = 1;
static int s_new_display_filter = A5_FILTER_NEAREST;
static ALLEGRO_DISPLAY *s_display = NULL;
static ALLEGRO_BITMAP *s_target = NULL;

/* The blender; Allegro's default is premultiplied alpha. */
static int s_blend_op = ALLEGRO_ADD;
static int s_blend_src = ALLEGRO_ONE;
static int s_blend_dst = ALLEGRO_INVERSE_ALPHA;
static int s_blend_kind = A5_BLEND_PREMULTIPLIED;

ALLEGRO_DISPLAY *a5_current_display(void)
{
   return s_display;
}

void a5_set_new_display_scale(int scale)
{
   s_new_display_scale = (scale >= 1 && scale <= 4) ? scale : 1;
}

void a5_set_new_display_filter(int filter)
{
   s_new_display_filter = (filter == A5_FILTER_SCALE2X || filter == A5_FILTER_SCALE2X_ADV)
      ? filter : A5_FILTER_NEAREST;
}

int a5_display_scale(void)
{
   return s_display ? s_display->scale : 1;
}

/* Copies n pixels between two places that do not overlap: CopyMemQuick()
 * on the Amiga, exec's fastest copy (pixels are always longword aligned),
 * as the owner's NovaMesa port and V-WARS do.
 */
void a5_copy_pixels(Uint32 *dst, const Uint32 *src, int n)
{
   if (n <= 0)
      return;
#if defined(__AMIGA__)
   CopyMemQuick((APTR)src, dst, (ULONG)n * sizeof(Uint32));
#else
   memcpy(dst, src, (size_t)n * sizeof(Uint32));
#endif
}

SDL_Window *a5_display_window(ALLEGRO_DISPLAY *display)
{
   return display ? display->window : NULL;
}

void al_set_new_display_flags(int flags)
{
   s_new_display_flags = flags;
}

/* Vsync, stencil and the other options are for OpenGL and Direct3D. */
void al_set_new_display_option(int option, int value, int importance)
{
   (void)option;
   (void)value;
   (void)importance;
}

static bool is_32bit_rgb(const SDL_Surface *surface)
{
   return surface->format->format == SDL_PIXELFORMAT_ARGB8888
      || surface->format->format == SDL_PIXELFORMAT_RGB888;
}

ALLEGRO_DISPLAY *al_create_display(int w, int h)
{
   ALLEGRO_DISPLAY *display;
   SDL_Surface *pixels;
   Uint32 window_flags = 0;

   if (s_display != NULL || w <= 0 || h <= 0)
      return NULL;

   if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
      return NULL;

   display = calloc(1, sizeof(*display));
   if (display == NULL)
      goto fail;

   display->flags = s_new_display_flags & ~ALLEGRO_OPENGL;
   if (display->flags & (ALLEGRO_FULLSCREEN | ALLEGRO_FULLSCREEN_WINDOW))
      window_flags |= SDL_WINDOW_FULLSCREEN;
   else
      display->flags |= ALLEGRO_WINDOWED;
   display->scale = s_new_display_scale;
   display->filter = s_new_display_filter;

   display->window = SDL_CreateWindow("", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, w * display->scale, h * display->scale, window_flags);
   if (display->window == NULL) {
      a5_trace("al_create_display: no %dx%d window: %s\n",
         w * display->scale, h * display->scale, SDL_GetError());
      goto fail;
   }

#if defined(SDL_VIDEO_DRIVER_AMIGAOS3)
   /* SDL2-NovaMesa can come back from going fullscreen with no Intuition
    * window at all (when P96 has no mode of the size), while SDL carries on
    * as if there were one, and the program would seem to quit without a
    * word.  As in V-WARS.
    */
   {
      SDL_SysWMinfo info;

      SDL_VERSION(&info.version);
      if (SDL_GetWindowWMInfo(display->window, &info) && info.subsystem == SDL_SYSWM_OS3
         && info.info.os3.window == NULL) {
         a5_trace("al_create_display: SDL has no Intuition window for a %dx%d %s: %s\n",
            w * display->scale, h * display->scale,
            (window_flags & SDL_WINDOW_FULLSCREEN) ? "screen" : "window", SDL_GetError());
         goto fail;
      }
   }
#endif

   display->surface = SDL_GetWindowSurface(display->window);
   if (display->surface == NULL)
      goto fail;

   pixels = display->surface;
   if (!is_32bit_rgb(pixels)) {
      display->shadow = SDL_CreateRGBSurfaceWithFormat(0,
         display->surface->w, display->surface->h, 32, SDL_PIXELFORMAT_ARGB8888);
      if (display->shadow == NULL)
         goto fail;
      pixels = display->shadow;
   }

   if (display->scale == 1) {
      display->backbuffer = a5_create_bitmap_over(pixels->pixels,
         pixels->w, pixels->h, pixels->pitch / (int)sizeof(Uint32));
   }
   else {
      /* The display's size, whatever mode the window really got: the flip
       * scales up as much of it as fits.
       */
      display->backbuffer = al_create_bitmap(w, h);
      if (display->backbuffer)
         display->backbuffer->owns_memory = false;
   }
   if (display->backbuffer == NULL)
      goto fail;

   a5_trace("display %dx%d, window %dx%d, %s\n", display->backbuffer->w,
      display->backbuffer->h, display->surface->w, display->surface->h,
      SDL_GetPixelFormatName(display->surface->format->format));

   SDL_FillRect(pixels, NULL, 0);
   a5_event_source_init(&display->es);

   s_display = display;
   s_target = display->backbuffer;
   return display;

fail:
   if (display) {
      if (display->shadow)
         SDL_FreeSurface(display->shadow);
      if (display->window)
         SDL_DestroyWindow(display->window);
      free(display);
   }
   SDL_QuitSubSystem(SDL_INIT_VIDEO);
   return NULL;
}

/* The backbuffer into the window surface (or its shadow), scale times. */
static void scale_up(ALLEGRO_DISPLAY *display)
{
   SDL_Surface *out = display->shadow ? display->shadow : display->surface;
   const ALLEGRO_BITMAP *bb = display->backbuffer;
   const int s = display->scale;
   const int out_pitch = out->pitch / (int)sizeof(Uint32);
   int w = bb->w, h = bb->h, x, y, k;

   if (w * s > out->w)
      w = out->w / s;
   if (h * s > out->h)
      h = out->h / s;

   if (s == 2 && display->filter != A5_FILTER_NEAREST) {
      a5_scale2x(bb->memory, bb->pitch, w, h, (Uint32 *)out->pixels, out_pitch,
         display->filter == A5_FILTER_SCALE2X_ADV);
      return;
   }

   for (y = 0; y < h; y++) {
      const Uint32 *src = bb->memory + y * bb->pitch;
      Uint32 *row = (Uint32 *)out->pixels + y * s * out_pitch;
      Uint32 *d = row;

      if (s == 2) {
         for (x = 0; x < w; x++) {
            const Uint32 p = src[x];

            d[0] = p;
            d[1] = p;
            d += 2;
         }
      }
      else {
         for (x = 0; x < w; x++) {
            for (k = 0; k < s; k++)
               *d++ = src[x];
         }
      }

      for (k = 1; k < s; k++)
         a5_copy_pixels(row + k * out_pitch, row, w * s);
   }
}

void al_flip_display(void)
{
   ALLEGRO_DISPLAY *display = s_display;
   SDL_Surface *surface;

   if (display == NULL)
      return;

   if (display->scale > 1)
      scale_up(display);

   if (display->shadow)
      SDL_BlitSurface(display->shadow, NULL, display->surface, NULL);

   SDL_UpdateWindowSurface(display->window);

   /* SDL may have replaced the window surface (it does when the window
    * changes size); the backbuffer then has to draw into the new one.
    */
   surface = SDL_GetWindowSurface(display->window);
   if (surface != NULL && surface != display->surface) {
      display->surface = surface;
      if (display->scale == 1 && !display->shadow && surface->w == display->backbuffer->w
         && surface->h == display->backbuffer->h && is_32bit_rgb(surface)) {
         a5_set_bitmap_memory(display->backbuffer, surface->pixels,
            surface->pitch / (int)sizeof(Uint32));
      }
   }
}

ALLEGRO_BITMAP *al_get_backbuffer(ALLEGRO_DISPLAY *display)
{
   return display ? display->backbuffer : NULL;
}

ALLEGRO_EVENT_SOURCE *al_get_display_event_source(ALLEGRO_DISPLAY *display)
{
   return display ? &display->es : NULL;
}

int al_get_display_flags(ALLEGRO_DISPLAY *display)
{
   return display ? display->flags : 0;
}

int al_get_display_width(ALLEGRO_DISPLAY *display)
{
   return display ? display->backbuffer->w : 0;
}

int al_get_display_height(ALLEGRO_DISPLAY *display)
{
   return display ? display->backbuffer->h : 0;
}

/* Switching between windowed and fullscreen is not supported; Dune
 * Dynasty's Alt+Enter then leaves the display as it is.
 */
bool al_set_display_flag(ALLEGRO_DISPLAY *display, int flag, bool onoff)
{
   (void)display;
   (void)flag;
   (void)onoff;
   return false;
}

void al_set_display_icon(ALLEGRO_DISPLAY *display, ALLEGRO_BITMAP *icon)
{
   (void)display;
   (void)icon;
}

void al_set_window_title(ALLEGRO_DISPLAY *display, const char *title)
{
   if (display)
      SDL_SetWindowTitle(display->window, title);
}

/* SDL window events as Allegro display events, as in Allegro's SDL
 * backend.  Called from a5_pump_sdl_events().
 */
void a5_display_handle_event(const SDL_Event *e)
{
   ALLEGRO_DISPLAY *display = s_display;
   ALLEGRO_EVENT event;

   if (display == NULL)
      return;

   memset(&event, 0, sizeof(event));

   if (e->type == SDL_QUIT) {
      event.display.type = ALLEGRO_EVENT_DISPLAY_CLOSE;
   }
   else if (e->type == SDL_WINDOWEVENT) {
      switch (e->window.event) {
         case SDL_WINDOWEVENT_CLOSE:
            event.display.type = ALLEGRO_EVENT_DISPLAY_CLOSE;
            break;

         case SDL_WINDOWEVENT_EXPOSED:
            if (!(display->flags & ALLEGRO_GENERATE_EXPOSE_EVENTS))
               return;
            event.display.type = ALLEGRO_EVENT_DISPLAY_EXPOSE;
            event.display.width = display->backbuffer->w;
            event.display.height = display->backbuffer->h;
            break;

         case SDL_WINDOWEVENT_FOCUS_GAINED:
            event.display.type = ALLEGRO_EVENT_DISPLAY_SWITCH_IN;
            break;

         case SDL_WINDOWEVENT_FOCUS_LOST:
            event.display.type = ALLEGRO_EVENT_DISPLAY_SWITCH_OUT;
            break;

         default:
            return;
      }
   }
   else {
      return;
   }

   event.display.timestamp = al_get_time();

   a5_lock();
   a5_emit_event_locked(&display->es, &event);
   a5_unlock();
}

/*--------------------------------------------------------------------------*/

void al_set_target_bitmap(ALLEGRO_BITMAP *bitmap)
{
   s_target = bitmap;
}

void al_set_target_backbuffer(ALLEGRO_DISPLAY *display)
{
   if (display)
      s_target = display->backbuffer;
}

ALLEGRO_BITMAP *al_get_target_bitmap(void)
{
   return s_target;
}

void a5_bitmap_destroyed(ALLEGRO_BITMAP *bitmap)
{
   if (s_target == bitmap)
      s_target = NULL;
}

/* Drawing is never deferred, so there is nothing to hold. */
void al_hold_bitmap_drawing(bool hold)
{
   (void)hold;
}

/* Fills the target's clipping rectangle, without blending or transform. */
void al_clear_to_color(ALLEGRO_COLOR color)
{
   ALLEGRO_BITMAP *target = s_target;
   const ALLEGRO_BITMAP *root;
   const Uint32 pixel = a5_color_to_pixel(color);
   const int w = target ? target->cr - target->cl : 0;
   int y;

   if (target == NULL || w <= 0 || target->cb <= target->ct)
      return;

   root = target->parent ? target->parent : target;

   /* The whole backbuffer, as the menus clear it after every flip: copied
    * from a frame of the colour made ready beforehand, as V-WARS does.
    */
   if (s_display && root == s_display->backbuffer && target == root
      && w == root->w && target->cb - target->ct == root->h && root->pitch == root->w) {
      static Uint32 *clear_frame = NULL;
      static int clear_size = 0;
      static Uint32 clear_pixel = 0;
      const int n = root->w * root->h;

      if (clear_size != n) {
         free(clear_frame);
         clear_frame = malloc((size_t)n * sizeof(Uint32));
         clear_size = clear_frame ? n : 0;
         if (clear_frame)
            a5_fill_pixels(clear_frame, n, pixel);
         clear_pixel = pixel;
      }
      else if (clear_pixel != pixel) {
         a5_fill_pixels(clear_frame, n, pixel);
         clear_pixel = pixel;
      }

      if (clear_frame) {
         a5_copy_pixels(root->memory, clear_frame, n);
         return;
      }
   }

   /* The whole of a bitmap without row padding is one span. */
   if (w == root->pitch) {
      a5_fill_pixels(a5_bitmap_row(target, target->ct) + target->cl,
         w * (target->cb - target->ct), pixel);
      return;
   }

   for (y = target->ct; y < target->cb; y++)
      a5_fill_pixels(a5_bitmap_row(target, y) + target->cl, w, pixel);
}

/*--------------------------------------------------------------------------*/

void al_set_blender(int op, int source, int dest)
{
   s_blend_op = op;
   s_blend_src = source;
   s_blend_dst = dest;

   if (op != ALLEGRO_ADD)
      s_blend_kind = A5_BLEND_GENERIC;
   else if (source == ALLEGRO_ONE && dest == ALLEGRO_ZERO)
      s_blend_kind = A5_BLEND_COPY;
   else if (source == ALLEGRO_ALPHA && dest == ALLEGRO_INVERSE_ALPHA)
      s_blend_kind = A5_BLEND_ALPHA;
   else if (source == ALLEGRO_ONE && dest == ALLEGRO_INVERSE_ALPHA)
      s_blend_kind = A5_BLEND_PREMULTIPLIED;
   else if (source == ALLEGRO_ALPHA && dest == ALLEGRO_ONE)
      s_blend_kind = A5_BLEND_ADDITIVE;
   else
      s_blend_kind = A5_BLEND_GENERIC;
}

int a5_blend_kind(void)
{
   return s_blend_kind;
}

/* A factor out of 255 for one channel, as Allegro's get_factor(); alpha
 * selects get_alpha_factor(), which uses the alphas for the colour modes.
 * The blend colour is Allegro's default, white.
 */
static Uint32 blend_factor(int mode, Uint32 sc, Uint32 dc, Uint32 sa, Uint32 da, bool alpha)
{
   switch (mode) {
      case ALLEGRO_ZERO:                return 0;
      case ALLEGRO_ONE:                 return 255;
      case ALLEGRO_ALPHA:               return sa;
      case ALLEGRO_INVERSE_ALPHA:       return 255 - sa;
      case ALLEGRO_SRC_COLOR:           return alpha ? sa : sc;
      case ALLEGRO_DEST_COLOR:          return alpha ? da : dc;
      case ALLEGRO_INVERSE_SRC_COLOR:   return 255 - (alpha ? sa : sc);
      case ALLEGRO_INVERSE_DEST_COLOR:  return 255 - (alpha ? da : dc);
      case ALLEGRO_CONST_COLOR:         return 255;
      case ALLEGRO_INVERSE_CONST_COLOR: return 0;
      default:                          return 0;
   }
}

static Uint32 blend_channel(int shift, Uint32 s, Uint32 d)
{
   const bool alpha = (shift == 24);
   const Uint32 sc = (s >> shift) & 0xFF;
   const Uint32 dc = (d >> shift) & 0xFF;
   const Uint32 sf = blend_factor(s_blend_src, sc, dc, s >> 24, d >> 24, alpha);
   const Uint32 df = blend_factor(s_blend_dst, sc, dc, s >> 24, d >> 24, alpha);
   const long x = (long)(sc * sf);
   const long y = (long)(dc * df);
   long r;

   switch (s_blend_op) {
      case ALLEGRO_SRC_MINUS_DEST: r = x - y; break;
      case ALLEGRO_DEST_MINUS_SRC: r = y - x; break;
      default:                     r = x + y; break;
   }

   if (r <= 0)
      return 0;
   if (r >= 255 * 255)
      return 255;
   return a5_div255((unsigned int)r);
}

Uint32 a5_blend_generic(Uint32 src, Uint32 dst)
{
   return (blend_channel(24, src, dst) << 24) | (blend_channel(16, src, dst) << 16)
      | (blend_channel(8, src, dst) << 8) | blend_channel(0, src, dst);
}
