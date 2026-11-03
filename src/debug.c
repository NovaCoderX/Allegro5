/* Trace output: debug.h.
 *
 * Allegro sends its trace to the handler a program registers with
 * al_register_trace_handler().  The wrapper's own trace (the stub trace,
 * see a5_stub.h) does the same, and goes to stderr when there is no handler.
 */

#include <stdarg.h>
#include <stdio.h>
#include "allegro5/allegro.h"
#include "a5_stub.h"

static void (*s_trace_handler)(const char *message) = NULL;

void al_register_trace_handler(void (*handler)(const char *))
{
   s_trace_handler = handler;
}

void a5_trace(const char *format, ...)
{
   char message[256];
   va_list ap;

   va_start(ap, format);
   vsnprintf(message, sizeof(message), format, ap);
   va_end(ap);

   if (s_trace_handler)
      s_trace_handler(message);
   else
      fputs(message, stderr);
}
