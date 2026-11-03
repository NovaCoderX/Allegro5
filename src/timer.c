/* Timers: timer.h.
 *
 * Timers have no thread of their own.  A timer's ticks are worked out from
 * al_get_time() whenever something looks at it: a queue it feeds being
 * checked or waited on, or its count being read.  Ticks that fell due in
 * the meantime are generated together, as Allegro's timer thread does when
 * it runs late, so counts and events come out the same, without an extra
 * task waking up for every tick.
 */

#include <stdlib.h>
#include "allegro5/allegro.h"
#include "a5_internal.h"

struct ALLEGRO_TIMER {
   ALLEGRO_EVENT_SOURCE es;   /* first, so a timer event's source is the timer */
   bool started;
   double speed_secs;
   int64_t count;
   double next_tick;          /* al_get_time() of the next tick, while started */
};

static void timer_update_locked(ALLEGRO_TIMER *timer, double now)
{
   while (timer->started && timer->next_tick <= now)
      a5_timer_tick_locked(timer, now);
}

bool a5_timer_next_tick_locked(const ALLEGRO_TIMER *timer, double *when)
{
   if (!timer->started)
      return false;

   *when = timer->next_tick;
   return true;
}

/* Generates the timer's next tick, which is due. */
void a5_timer_tick_locked(ALLEGRO_TIMER *timer, double now)
{
   ALLEGRO_EVENT event;

   timer->count++;

   event.timer.type = ALLEGRO_EVENT_TIMER;
   event.timer.timestamp = now;
   event.timer.count = timer->count;
   event.timer.error = now - timer->next_tick;
   a5_emit_event_locked(&timer->es, &event);

   timer->next_tick += timer->speed_secs;
}

/*--------------------------------------------------------------*/

ALLEGRO_TIMER *al_create_timer(double speed_secs)
{
   ALLEGRO_TIMER *timer;

   if (speed_secs <= 0.0)
      return NULL;

   timer = calloc(1, sizeof(*timer));
   if (!timer)
      return NULL;

   a5_event_source_init(&timer->es);
   A5_SOURCE(&timer->es)->timer = timer;
   timer->speed_secs = speed_secs;
   return timer;
}

void al_destroy_timer(ALLEGRO_TIMER *timer)
{
   if (!timer)
      return;

   a5_lock();
   a5_event_source_free_locked(&timer->es);
   a5_unlock();

   free(timer);
}

/* As in Allegro, starting restarts the wait for the next tick but keeps
 * the count.
 */
void al_start_timer(ALLEGRO_TIMER *timer)
{
   a5_lock();

   if (!timer->started) {
      timer->started = true;
      timer->next_tick = al_get_time() + timer->speed_secs;
   }

   a5_unlock();
}

void al_stop_timer(ALLEGRO_TIMER *timer)
{
   a5_lock();
   timer_update_locked(timer, al_get_time());
   timer->started = false;
   a5_unlock();
}

int64_t al_get_timer_count(const ALLEGRO_TIMER *timer)
{
   /* The const in Allegro's prototype assumes a timer thread keeps the
    * count current; here reading it is what brings it up to date.
    */
   ALLEGRO_TIMER *t = (ALLEGRO_TIMER *)timer;
   int64_t count;

   a5_lock();
   timer_update_locked(t, al_get_time());
   count = t->count;
   a5_unlock();

   return count;
}

/* As in Allegro, the tick in progress is lengthened or shortened by the
 * change in speed.
 */
void al_set_timer_speed(ALLEGRO_TIMER *timer, double speed_secs)
{
   if (speed_secs <= 0.0)
      return;

   a5_lock();

   if (timer->started) {
      timer_update_locked(timer, al_get_time());
      timer->next_tick += speed_secs - timer->speed_secs;
   }

   timer->speed_secs = speed_secs;
   a5_unlock();
}

ALLEGRO_EVENT_SOURCE *al_get_timer_event_source(ALLEGRO_TIMER *timer)
{
   return &timer->es;
}
