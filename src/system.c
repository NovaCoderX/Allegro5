/* System, time and memory: system.h, altime.h, memory.h. */

#include <stdlib.h>
#include <SDL.h>
#include "allegro5/allegro.h"
#include "a5_internal.h"

static bool s_installed = false;
static SDL_threadID s_main_thread;
static Uint64 s_time_start;
static double s_time_scale;

bool al_install_system(int version, int (*atexit_ptr)(void (*)(void)))
{
   if (s_installed)
      return true;

   /* SDL_Init(0) only starts SDL's clock (timer.device and the EClock on
    * AmigaOS).  SDL_INIT_TIMER would also start SDL's timer thread, which
    * the wrapper's timers do not use: see timer.c.
    */
   if (SDL_Init(0) != 0)
      return false;

   if (!a5_events_init()) {
      SDL_Quit();
      return false;
   }

   s_main_thread = SDL_ThreadID();
   s_time_start = SDL_GetPerformanceCounter();
   s_time_scale = 1.0 / (double)SDL_GetPerformanceFrequency();
   s_installed = true;

   if (atexit_ptr)
      atexit_ptr(al_uninstall_system);

   return true;
}

void al_uninstall_system(void)
{
   if (!s_installed)
      return;

   s_installed = false;
   a5_events_shutdown();
   SDL_Quit();
}

bool a5_is_main_thread(void)
{
   return s_installed && SDL_ThreadID() == s_main_thread;
}

/* Hands SDL's pending events to the modules that turn them into Allegro
 * events.  Called by the event queue functions, as Allegro's own SDL
 * backend does.  SDL_PumpEvents() once, then SDL_PeepEvents(), because
 * SDL_PollEvent() would pump again for every event.
 */
void a5_pump_sdl_events(void)
{
   SDL_Event event;

   if (!a5_is_main_thread())
      return;

   SDL_PumpEvents();

   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
      switch (event.type) {
         case SDL_KEYDOWN:
         case SDL_KEYUP:
            a5_keyboard_handle_event(&event);
            break;

         case SDL_MOUSEMOTION:
         case SDL_MOUSEBUTTONDOWN:
         case SDL_MOUSEBUTTONUP:
         case SDL_MOUSEWHEEL:
            a5_mouse_handle_event(&event);
            break;

         case SDL_WINDOWEVENT:
         case SDL_QUIT:
            a5_display_handle_event(&event);
            break;
      }
   }
}

/* Allegro uses the name for standard paths and default window titles; the
 * Amiga build needs neither (paths come from g_amiga_path, and the game
 * sets its own window title).
 */
void al_set_app_name(const char *app_name)
{
}

/* Seconds since al_install_system(), from SDL's performance counter (the
 * EClock on AmigaOS).
 */
double al_get_time(void)
{
   if (!s_installed)
      return 0.0;

   return (double)(SDL_GetPerformanceCounter() - s_time_start) * s_time_scale;
}

/* Real implementation: Allegro's default allocator is plain malloc(). */
void *al_malloc_with_context(size_t n, int line, const char *file,
   const char *func)
{
   return malloc(n);
}

/* Real implementation: free() (the audio addon frees sample buffers the
 * program allocated with al_malloc()). */
void al_free_with_context(void *ptr, int line, const char *file,
   const char *func)
{
   free(ptr);
}
