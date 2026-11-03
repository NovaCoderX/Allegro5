/* Mouse and mouse cursors: mouse.h, mouse_cursor.h.
 *
 * Mouse events come from SDL's mouse events, as in Allegro's SDL backend.
 * There are no custom cursors: SDL2-NovaMesa ignores cursor images, so
 * Dune Dynasty draws its own (its software cursor) and the wrapper only
 * shows or hides the system pointer.
 */

#include <string.h>
#include <SDL.h>
#include "allegro5/allegro.h"
#include "a5_internal.h"

static bool s_installed = false;
static A5_EVENT_SOURCE_ALIGN ALLEGRO_EVENT_SOURCE s_source;

/* Where the mouse is, the wheel totals, and the buttons held. */
static int s_x, s_y, s_z, s_w;
static unsigned int s_buttons;

/* With a display scale, positions are divided down to the display's size,
 * and movements too, keeping what is left over for the next one so slow
 * movement is not lost.
 */
static int s_rest_x, s_rest_y;

static int scale_movement(int rel, int *rest, int scale)
{
   int moved;

   *rest += rel;
   moved = *rest / scale;
   *rest -= moved * scale;
   return moved;
}

/* SDL numbers the middle button 2 and the right 3; Allegro the other way. */
static unsigned int get_button(Uint8 sdl_button)
{
   switch (sdl_button) {
      case SDL_BUTTON_LEFT:   return 1;
      case SDL_BUTTON_RIGHT:  return 2;
      case SDL_BUTTON_MIDDLE: return 3;
      case SDL_BUTTON_X1:     return 4;
      case SDL_BUTTON_X2:     return 5;
      default:                return 0;
   }
}

void a5_mouse_handle_event(const SDL_Event *e)
{
   const int scale = a5_display_scale();
   ALLEGRO_EVENT event;

   if (!s_installed)
      return;

   memset(&event, 0, sizeof(event));
   event.mouse.type = ALLEGRO_EVENT_MOUSE_AXES;

   switch (e->type) {
      case SDL_MOUSEMOTION:
         s_x = e->motion.x / scale;
         s_y = e->motion.y / scale;
         event.mouse.dx = scale_movement(e->motion.xrel, &s_rest_x, scale);
         event.mouse.dy = scale_movement(e->motion.yrel, &s_rest_y, scale);
         break;

      case SDL_MOUSEWHEEL:
         s_z += e->wheel.y;
         s_w += e->wheel.x;
         event.mouse.dz = e->wheel.y;
         event.mouse.dw = e->wheel.x;
         break;

      default:
         event.mouse.button = get_button(e->button.button);
         if (event.mouse.button == 0)
            return;

         s_x = e->button.x / scale;
         s_y = e->button.y / scale;

         if (e->type == SDL_MOUSEBUTTONDOWN) {
            event.mouse.type = ALLEGRO_EVENT_MOUSE_BUTTON_DOWN;
            s_buttons |= 1 << (event.mouse.button - 1);
         }
         else {
            event.mouse.type = ALLEGRO_EVENT_MOUSE_BUTTON_UP;
            s_buttons &= ~(1 << (event.mouse.button - 1));
         }
         break;
   }

   event.mouse.timestamp = al_get_time();
   event.mouse.display = a5_current_display();
   event.mouse.x = s_x;
   event.mouse.y = s_y;
   event.mouse.z = s_z;
   event.mouse.w = s_w;
   event.mouse.pressure = s_buttons ? 1.0f : 0.0f;

   a5_lock();
   a5_emit_event_locked(&s_source, &event);
   a5_unlock();
}

bool al_install_mouse(void)
{
   if (s_installed)
      return true;

   if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0)
      return false;

   a5_event_source_init(&s_source);
   s_installed = true;
   return true;
}

ALLEGRO_EVENT_SOURCE *al_get_mouse_event_source(void)
{
   return s_installed ? &s_source : NULL;
}

/* As Allegro's SDL backend: SDL then reports the new position with its
 * next motion event.
 */
bool al_set_mouse_xy(ALLEGRO_DISPLAY *display, int x, int y)
{
   SDL_Window *window = a5_display_window(display);

   if (window == NULL)
      return false;

   SDL_WarpMouseInWindow(window, x * a5_display_scale(), y * a5_display_scale());
   return true;
}

ALLEGRO_MOUSE_CURSOR *al_create_mouse_cursor(ALLEGRO_BITMAP *sprite,
   int xfocus, int yfocus)
{
   (void)sprite;
   (void)xfocus;
   (void)yfocus;
   return NULL;
}

void al_destroy_mouse_cursor(ALLEGRO_MOUSE_CURSOR *cursor)
{
   (void)cursor;
}

bool al_set_mouse_cursor(ALLEGRO_DISPLAY *display, ALLEGRO_MOUSE_CURSOR *cursor)
{
   (void)display;
   (void)cursor;
   return false;
}

bool al_show_mouse_cursor(ALLEGRO_DISPLAY *display)
{
   if (display == NULL)
      return false;

   SDL_ShowCursor(SDL_ENABLE);
   return true;
}

bool al_hide_mouse_cursor(ALLEGRO_DISPLAY *display)
{
   if (display == NULL)
      return false;

   SDL_ShowCursor(SDL_DISABLE);
   return true;
}
