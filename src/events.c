/* Event queues: events.h.
 *
 * A queue is a ring buffer of events plus the sources registered with it.
 * Timer ticks are generated when a queue they feed is checked or waited
 * on (see timer.c), so waiting means sleeping until the next tick or until
 * another thread emits an event.  On the main thread, checking or waiting
 * on a queue also pumps SDL's input (see a5_pump_sdl_events()).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "allegro5/allegro.h"
#include "a5_internal.h"
#include "a5_stub.h"

_Static_assert(sizeof(A5_EVENT_SOURCE) <= sizeof(ALLEGRO_EVENT_SOURCE),
   "A5_EVENT_SOURCE must fit in an ALLEGRO_EVENT_SOURCE");

#define INITIAL_CAPACITY  64   /* always a power of two */

/* The longest the main thread waits before pumping SDL's input again. */
#define PUMP_INTERVAL_MS  10

struct ALLEGRO_EVENT_QUEUE {
   ALLEGRO_EVENT *events;
   unsigned int capacity;
   unsigned int head;        /* index of the oldest event */
   unsigned int count;
   ALLEGRO_EVENT_SOURCE **sources;
   int num_sources;
   int max_sources;
   SDL_cond *cond;
};

static SDL_mutex *s_mutex = NULL;

bool a5_events_init(void)
{
   s_mutex = SDL_CreateMutex();
   return s_mutex != NULL;
}

void a5_events_shutdown(void)
{
   SDL_DestroyMutex(s_mutex);
   s_mutex = NULL;
}

void a5_lock(void)
{
   SDL_LockMutex(s_mutex);
}

void a5_unlock(void)
{
   SDL_UnlockMutex(s_mutex);
}

/*--------------------------------------------------------------*/

static ALLEGRO_EVENT *queue_slot(ALLEGRO_EVENT_QUEUE *queue, unsigned int i)
{
   return &queue->events[(queue->head + i) & (queue->capacity - 1)];
}

static void queue_push(ALLEGRO_EVENT_QUEUE *queue, const ALLEGRO_EVENT *event)
{
   if (queue->count == queue->capacity) {
      const unsigned int capacity = queue->capacity * 2;
      ALLEGRO_EVENT *events = malloc(capacity * sizeof(*events));
      unsigned int i;

      if (!events)
         return;  /* Out of memory: drop the event. */

      for (i = 0; i < queue->count; i++)
         events[i] = *queue_slot(queue, i);

      free(queue->events);
      queue->events = events;
      queue->capacity = capacity;
      queue->head = 0;
   }

   *queue_slot(queue, queue->count) = *event;
   queue->count++;
   SDL_CondBroadcast(queue->cond);
}

static void queue_pop(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *event)
{
   *event = *queue_slot(queue, 0);
   queue->head = (queue->head + 1) & (queue->capacity - 1);
   queue->count--;
}

static void queue_discard_events_of(ALLEGRO_EVENT_QUEUE *queue,
   const ALLEGRO_EVENT_SOURCE *source)
{
   unsigned int kept = 0;
   unsigned int i;

   for (i = 0; i < queue->count; i++) {
      const ALLEGRO_EVENT *event = queue_slot(queue, i);

      if (event->any.source != source)
         *queue_slot(queue, kept++) = *event;
   }

   queue->count = kept;
}

static bool queue_remove_source(ALLEGRO_EVENT_QUEUE *queue,
   const ALLEGRO_EVENT_SOURCE *source)
{
   int i;

   for (i = 0; i < queue->num_sources; i++) {
      if (queue->sources[i] == source) {
         queue->sources[i] = queue->sources[--queue->num_sources];
         return true;
      }
   }

   return false;
}

static void source_remove_queue(ALLEGRO_EVENT_SOURCE *source,
   const ALLEGRO_EVENT_QUEUE *queue)
{
   A5_EVENT_SOURCE *src = A5_SOURCE(source);
   int i;

   for (i = 0; i < src->num_queues; i++) {
      if (src->queues[i] == queue) {
         src->queues[i] = src->queues[--src->num_queues];
         return;
      }
   }
}

/* Finds when the next tick of the queue's timers is due; false if none of
 * them is running.
 */
static bool queue_next_tick(const ALLEGRO_EVENT_QUEUE *queue, double *when)
{
   bool found = false;
   int i;

   for (i = 0; i < queue->num_sources; i++) {
      const ALLEGRO_TIMER *timer = A5_SOURCE(queue->sources[i])->timer;
      double t;

      if (timer && a5_timer_next_tick_locked(timer, &t) && (!found || t < *when)) {
         *when = t;
         found = true;
      }
   }

   return found;
}

/* Generates every tick of the queue's timers that is due by now, oldest
 * first, so ticks from different timers stay in order.
 */
static void queue_update_timers(ALLEGRO_EVENT_QUEUE *queue, double now)
{
   for (;;) {
      ALLEGRO_TIMER *due = NULL;
      double due_time = now;
      int i;

      for (i = 0; i < queue->num_sources; i++) {
         ALLEGRO_TIMER *timer = A5_SOURCE(queue->sources[i])->timer;
         double t;

         if (timer && a5_timer_next_tick_locked(timer, &t) && t <= due_time) {
            due = timer;
            due_time = t;
         }
      }

      if (!due)
         break;

      a5_timer_tick_locked(due, now);
   }
}

/*--------------------------------------------------------------*/

void a5_event_source_init(ALLEGRO_EVENT_SOURCE *source)
{
   memset(source, 0, sizeof(*source));
}

void a5_event_source_free_locked(ALLEGRO_EVENT_SOURCE *source)
{
   A5_EVENT_SOURCE *src = A5_SOURCE(source);

   while (src->num_queues > 0) {
      ALLEGRO_EVENT_QUEUE *queue = src->queues[--src->num_queues];

      queue_remove_source(queue, source);
      queue_discard_events_of(queue, source);
   }
}

void a5_emit_event_locked(ALLEGRO_EVENT_SOURCE *source, ALLEGRO_EVENT *event)
{
   A5_EVENT_SOURCE *src = A5_SOURCE(source);
   int i;

   event->any.source = source;

   for (i = 0; i < src->num_queues; i++)
      queue_push(src->queues[i], event);
}

/* The most events from source waiting in any one queue it feeds. */
unsigned int a5_queued_events_locked(const ALLEGRO_EVENT_SOURCE *source)
{
   const A5_EVENT_SOURCE *src = A5_SOURCE((ALLEGRO_EVENT_SOURCE *)source);
   unsigned int most = 0;
   int q;

   for (q = 0; q < src->num_queues; q++) {
      ALLEGRO_EVENT_QUEUE *queue = src->queues[q];
      unsigned int n = 0;
      unsigned int i;

      for (i = 0; i < queue->count; i++) {
         if (queue_slot(queue, i)->any.source == source)
            n++;
      }

      if (n > most)
         most = n;
   }

   return most;
}

/*--------------------------------------------------------------*/

ALLEGRO_EVENT_QUEUE *al_create_event_queue(void)
{
   ALLEGRO_EVENT_QUEUE *queue = calloc(1, sizeof(*queue));

   if (!queue)
      return NULL;

   queue->capacity = INITIAL_CAPACITY;
   queue->events = malloc(queue->capacity * sizeof(*queue->events));
   queue->cond = SDL_CreateCond();

   if (!queue->events || !queue->cond) {
      if (queue->cond)
         SDL_DestroyCond(queue->cond);
      free(queue->events);
      free(queue);
      return NULL;
   }

   return queue;
}

void al_destroy_event_queue(ALLEGRO_EVENT_QUEUE *queue)
{
   if (!queue)
      return;

   a5_lock();
   while (queue->num_sources > 0)
      source_remove_queue(queue->sources[--queue->num_sources], queue);
   a5_unlock();

   SDL_DestroyCond(queue->cond);
   free(queue->sources);
   free(queue->events);
   free(queue);
}

void al_register_event_source(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT_SOURCE *source)
{
   A5_EVENT_SOURCE *src = A5_SOURCE(source);
   int i;

   a5_lock();

   for (i = 0; i < queue->num_sources; i++) {
      if (queue->sources[i] == source) {
         a5_unlock();
         return;
      }
   }

   if (src->num_queues == A5_MAX_SOURCE_QUEUES) {
      a5_trace("allegro5: an event source can feed at most %d queues\n",
         A5_MAX_SOURCE_QUEUES);
      a5_unlock();
      return;
   }

   if (queue->num_sources == queue->max_sources) {
      const int max_sources = queue->max_sources ? queue->max_sources * 2 : 8;
      ALLEGRO_EVENT_SOURCE **sources =
         realloc(queue->sources, max_sources * sizeof(*sources));

      if (!sources) {
         a5_unlock();
         return;
      }

      queue->sources = sources;
      queue->max_sources = max_sources;
   }

   queue->sources[queue->num_sources++] = source;
   src->queues[src->num_queues++] = queue;

   a5_unlock();
}

void al_unregister_event_source(ALLEGRO_EVENT_QUEUE *queue,
   ALLEGRO_EVENT_SOURCE *source)
{
   a5_lock();

   if (queue_remove_source(queue, source)) {
      source_remove_queue(source, queue);
      queue_discard_events_of(queue, source);
   }

   a5_unlock();
}

bool al_is_event_queue_empty(ALLEGRO_EVENT_QUEUE *queue)
{
   bool empty;

   a5_pump_sdl_events();
   a5_lock();
   queue_update_timers(queue, al_get_time());
   empty = (queue->count == 0);
   a5_unlock();

   return empty;
}

bool al_get_next_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   bool found;

   a5_pump_sdl_events();
   a5_lock();
   queue_update_timers(queue, al_get_time());

   found = (queue->count > 0);
   if (found)
      queue_pop(queue, ret_event);

   a5_unlock();
   return found;
}

void al_flush_event_queue(ALLEGRO_EVENT_QUEUE *queue)
{
   a5_pump_sdl_events();
   a5_lock();
   queue_update_timers(queue, al_get_time());
   queue->head = 0;
   queue->count = 0;
   a5_unlock();
}

/* As in Allegro, a NULL ret_event waits for an event without removing it.
 * Nothing else pumps SDL's input, so the main thread wakes at least every
 * PUMP_INTERVAL_MS to do it; other threads sleep until the next timer tick
 * or until an event is emitted.
 */
void al_wait_for_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   const bool pump = a5_is_main_thread();

   a5_pump_sdl_events();
   a5_lock();

   for (;;) {
      const double now = al_get_time();
      Uint32 ms = SDL_MUTEX_MAXWAIT;
      double when;

      queue_update_timers(queue, now);
      if (queue->count > 0)
         break;

      if (queue_next_tick(queue, &when)) {
         /* Round up, so we wake just after the tick rather than before. */
         ms = (Uint32)ceil((when - now) * 1000.0);
         if (ms == 0)
            ms = 1;
      }

      if (pump && ms > PUMP_INTERVAL_MS)
         ms = PUMP_INTERVAL_MS;

      if (ms == SDL_MUTEX_MAXWAIT)
         SDL_CondWait(queue->cond, s_mutex);
      else
         SDL_CondWaitTimeout(queue->cond, s_mutex, ms);

      if (pump) {
         a5_unlock();
         a5_pump_sdl_events();
         a5_lock();
      }
   }

   if (ret_event)
      queue_pop(queue, ret_event);

   a5_unlock();
}
