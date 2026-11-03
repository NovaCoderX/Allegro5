/* Threads: threads.h, on SDL threads.
 *
 * Dune Dynasty runs its MIDI music on one (MPU_ThreadProc() in
 * audio_a5.c). As in Allegro, al_create_thread() makes a thread that
 * does not run until al_start_thread(); here the SDL thread is only
 * created then. al_join_thread() asks the thread to stop and waits for it,
 * and al_destroy_thread() joins first if that has not been done.
 *
 * On AmigaOS an SDL thread is a process of its own, with the stack size
 * given here (Amiga processes default to a few KB, and MPU_ThreadProc()
 * keeps its music buffer on the stack).
 */

#include "a5_internal.h"
#include "allegro5/allegro.h"

#define A5_THREAD_STACK_SIZE  (64 * 1024)

typedef enum A5_THREAD_STATE {
   A5_THREAD_CREATED,
   A5_THREAD_STARTED,
   A5_THREAD_JOINED
} A5_THREAD_STATE;

struct ALLEGRO_THREAD {
   void *(*proc)(ALLEGRO_THREAD *thread, void *arg);
   void *arg;
   void *retval;
   A5_THREAD_STATE state;
   SDL_Thread *thread;
   SDL_atomic_t should_stop;
};

static int thread_trampoline(void *data)
{
   ALLEGRO_THREAD *thread = data;

   thread->retval = thread->proc(thread, thread->arg);
   return 0;
}

ALLEGRO_THREAD *al_create_thread(
   void *(*proc)(ALLEGRO_THREAD *thread, void *arg), void *arg)
{
   ALLEGRO_THREAD *thread;

   if (proc == NULL)
      return NULL;

   thread = SDL_calloc(1, sizeof(*thread));
   if (thread == NULL)
      return NULL;

   thread->proc = proc;
   thread->arg = arg;
   thread->state = A5_THREAD_CREATED;
   SDL_AtomicSet(&thread->should_stop, 0);
   return thread;
}

void al_start_thread(ALLEGRO_THREAD *thread)
{
   if (thread == NULL || thread->state != A5_THREAD_CREATED)
      return;

#if SDL_VERSION_ATLEAST(2, 0, 9)
   thread->thread = SDL_CreateThreadWithStackSize(thread_trampoline,
      "allegro thread", A5_THREAD_STACK_SIZE, thread);
#else
   thread->thread = SDL_CreateThread(thread_trampoline, "allegro thread", thread);
#endif

   /* Allegro has no way to report this; the thread just never runs, and
    * reads as stopped. */
   if (thread->thread == NULL)
      SDL_AtomicSet(&thread->should_stop, 1);

   thread->state = A5_THREAD_STARTED;
}

void al_join_thread(ALLEGRO_THREAD *thread, void **ret_value)
{
   if (thread == NULL)
      return;

   if (thread->state != A5_THREAD_JOINED) {
      SDL_AtomicSet(&thread->should_stop, 1);
      if (thread->thread != NULL) {
         SDL_WaitThread(thread->thread, NULL);
         thread->thread = NULL;
      }
      thread->state = A5_THREAD_JOINED;
   }

   if (ret_value != NULL)
      *ret_value = thread->retval;
}

void al_set_thread_should_stop(ALLEGRO_THREAD *thread)
{
   if (thread != NULL)
      SDL_AtomicSet(&thread->should_stop, 1);
}

bool al_get_thread_should_stop(ALLEGRO_THREAD *thread)
{
   return thread == NULL || SDL_AtomicGet(&thread->should_stop) != 0;
}

void al_destroy_thread(ALLEGRO_THREAD *thread)
{
   if (thread == NULL)
      return;

   al_join_thread(thread, NULL);
   SDL_free(thread);
}
