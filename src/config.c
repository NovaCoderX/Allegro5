/* Configuration files: config.h.
 *
 * Plain C on stdio, with Allegro 5.2's file format and behaviour for the
 * functions Dune Dynasty uses:
 *
 * - "[section]" starts a section; "key=value" lines before the first
 *   section are in the global section ("" or NULL).
 * - Keys and values are trimmed of surrounding blanks.
 * - Comments ('#' lines) and blank lines are kept in place, so saving a
 *   loaded file keeps its layout. A comment added with
 *   al_add_config_comment() is written as given.
 * - Setting an existing key replaces its value in place; a new key goes at
 *   the end of its section, a new section at the end of the file.
 * - Amiga extensions (allegro_amiga.h): a5_clear_config_section() and
 *   a5_append_config_line() write a section out again in a set order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allegro5/allegro.h"
#include "allegro5/allegro_amiga.h"

typedef struct A5_CONFIG_ENTRY {
   char *key;                    /* NULL for a comment or blank line */
   char *value;                  /* the comment text for a comment */
   struct A5_CONFIG_ENTRY *next;
} A5_CONFIG_ENTRY;

typedef struct A5_CONFIG_SECTION {
   char *name;                   /* "" for the global section */
   A5_CONFIG_ENTRY *head;
   A5_CONFIG_ENTRY *tail;
   struct A5_CONFIG_SECTION *next;
} A5_CONFIG_SECTION;

struct ALLEGRO_CONFIG {
   A5_CONFIG_SECTION *head;      /* always the global section */
   A5_CONFIG_SECTION *tail;
};

static char *config_strdup(const char *s)
{
   const size_t len = strlen(s) + 1;
   char *copy = malloc(len);

   if (copy)
      memcpy(copy, s, len);
   return copy;
}

/* Trims blanks from both ends of s, in place. */
static char *config_trim(char *s)
{
   char *end;

   while (*s == ' ' || *s == '\t')
      s++;

   end = s + strlen(s);
   while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
      end--;
   *end = '\0';

   return s;
}

static A5_CONFIG_SECTION *config_find_section(const ALLEGRO_CONFIG *config, const char *name)
{
   A5_CONFIG_SECTION *s;

   if (name == NULL)
      name = "";

   for (s = config->head; s; s = s->next) {
      if (strcmp(s->name, name) == 0)
         return s;
   }

   return NULL;
}

static void config_append(A5_CONFIG_SECTION *s, char *key, char *value, bool after_last_key);
static void config_set(ALLEGRO_CONFIG *config, const char *section,
   const char *key, const char *value, bool after_last_key);

static A5_CONFIG_SECTION *config_get_section(ALLEGRO_CONFIG *config, const char *name)
{
   A5_CONFIG_SECTION *s = config_find_section(config, name);

   if (s)
      return s;

   s = calloc(1, sizeof(*s));
   if (s == NULL)
      return NULL;

   s->name = config_strdup(name);
   if (s->name == NULL) {
      free(s);
      return NULL;
   }

   /* A blank line before a new section, as in a hand-written file. */
   if (config->tail->tail && (config->tail->tail->key || config->tail->tail->value[0] != '\0'))
      config_append(config->tail, NULL, config_strdup(""), false);

   config->tail->next = s;
   config->tail = s;
   return s;
}

/* Adds an entry at the end of a section, or with after_last_key after the
 * section's last key, so that a key set by the program stays above the
 * blank lines and comments that lead into the next section.
 */
static void config_append(A5_CONFIG_SECTION *s, char *key, char *value, bool after_last_key)
{
   A5_CONFIG_ENTRY *e = (value != NULL) ? calloc(1, sizeof(*e)) : NULL;
   A5_CONFIG_ENTRY *after = s->tail;

   if (e == NULL) {
      free(key);
      free(value);
      return;
   }

   e->key = key;
   e->value = value;

   if (after_last_key) {
      A5_CONFIG_ENTRY *i;

      for (i = s->head; i; i = i->next) {
         if (i->key)
            after = i;
      }
   }

   if (after) {
      e->next = after->next;
      after->next = e;
   }
   else {
      e->next = s->head;
      s->head = e;
   }

   if (e->next == NULL)
      s->tail = e;
}

ALLEGRO_CONFIG *al_create_config(void)
{
   ALLEGRO_CONFIG *config = calloc(1, sizeof(*config));

   if (config == NULL)
      return NULL;

   config->head = calloc(1, sizeof(*config->head));
   if (config->head)
      config->head->name = config_strdup("");

   if (config->head == NULL || config->head->name == NULL) {
      al_destroy_config(config);
      return NULL;
   }

   config->tail = config->head;
   return config;
}

void al_destroy_config(ALLEGRO_CONFIG *config)
{
   A5_CONFIG_SECTION *s;

   if (config == NULL)
      return;

   s = config->head;
   while (s) {
      A5_CONFIG_SECTION *next_section = s->next;
      A5_CONFIG_ENTRY *e = s->head;

      while (e) {
         A5_CONFIG_ENTRY *next_entry = e->next;

         free(e->key);
         free(e->value);
         free(e);
         e = next_entry;
      }

      free(s->name);
      free(s);
      s = next_section;
   }

   free(config);
}

ALLEGRO_CONFIG *al_load_config_file(const char *filename)
{
   ALLEGRO_CONFIG *config;
   A5_CONFIG_SECTION *section;
   char line[1024];
   FILE *fp;

   fp = fopen(filename, "r");
   if (fp == NULL)
      return NULL;

   config = al_create_config();
   if (config == NULL) {
      fclose(fp);
      return NULL;
   }

   section = config->head;

   while (fgets(line, sizeof(line), fp)) {
      char *s = config_trim(line);
      char *eq;

      if (*s == '\0' || *s == '#') {
         config_append(section, NULL, config_strdup(s), false);
      }
      else if (*s == '[') {
         char *end = strchr(s, ']');

         if (end)
            *end = '\0';
         section = config_get_section(config, config_trim(s + 1));
         if (section == NULL)
            break;
      }
      else if ((eq = strchr(s, '=')) != NULL) {
         *eq = '\0';
         config_set(config, section->name, config_trim(s), config_trim(eq + 1), false);
      }
      else {
         /* A key without '=': Allegro reads it as an empty value. */
         config_set(config, section->name, s, "", false);
      }
   }

   fclose(fp);
   return config;
}

bool al_save_config_file(const char *filename, const ALLEGRO_CONFIG *config)
{
   const A5_CONFIG_SECTION *s;
   FILE *fp;
   bool ok;

   fp = fopen(filename, "w");
   if (fp == NULL)
      return false;

   for (s = config->head; s; s = s->next) {
      const A5_CONFIG_ENTRY *e;

      if (s->name[0] != '\0')
         fprintf(fp, "[%s]\n", s->name);

      for (e = s->head; e; e = e->next) {
         if (e->key)
            fprintf(fp, "%s=%s\n", e->key, e->value);
         else
            fprintf(fp, "%s\n", e->value);
      }
   }

   ok = (ferror(fp) == 0);
   if (fclose(fp) != 0)
      ok = false;

   return ok;
}

const char *al_get_config_value(const ALLEGRO_CONFIG *config,
   const char *section, const char *key)
{
   const A5_CONFIG_SECTION *s;
   const A5_CONFIG_ENTRY *e;

   if (config == NULL)
      return NULL;

   s = config_find_section(config, section);
   if (s == NULL)
      return NULL;

   for (e = s->head; e; e = e->next) {
      if (e->key && strcmp(e->key, key) == 0)
         return e->value;
   }

   return NULL;
}

static void config_set(ALLEGRO_CONFIG *config, const char *section,
   const char *key, const char *value, bool after_last_key)
{
   A5_CONFIG_SECTION *s;
   A5_CONFIG_ENTRY *e;
   char *copy;

   if (config == NULL)
      return;

   s = config_get_section(config, section ? section : "");
   if (s == NULL)
      return;

   for (e = s->head; e; e = e->next) {
      if (e->key && strcmp(e->key, key) == 0) {
         copy = config_strdup(value);
         if (copy) {
            free(e->value);
            e->value = copy;
         }
         return;
      }
   }

   copy = config_strdup(key);
   if (copy)
      config_append(s, copy, config_strdup(value), after_last_key);
}

void al_set_config_value(ALLEGRO_CONFIG *config,
   const char *section, const char *key, const char *value)
{
   config_set(config, section, key, value, true);
}

void al_add_config_comment(ALLEGRO_CONFIG *config,
   const char *section, const char *comment)
{
   A5_CONFIG_SECTION *s;

   if (config == NULL)
      return;

   s = config_get_section(config, section ? section : "");
   if (s)
      config_append(s, NULL, config_strdup(comment), false);
}

static bool config_is_blank(const A5_CONFIG_ENTRY *e)
{
   return e->key == NULL && e->value[0] == '\0';
}

void a5_clear_config_section(ALLEGRO_CONFIG *config, const char *section)
{
   A5_CONFIG_SECTION *s;
   A5_CONFIG_ENTRY *e, *keep = NULL;

   if (config == NULL)
      return;

   s = config_find_section(config, section);
   if (s == NULL)
      return;

   /* Keep the run of blank lines at the end. */
   for (e = s->head; e; e = e->next) {
      if (!config_is_blank(e))
         keep = NULL;
      else if (keep == NULL)
         keep = e;
   }

   e = s->head;
   while (e && e != keep) {
      A5_CONFIG_ENTRY *next = e->next;

      free(e->key);
      free(e->value);
      free(e);
      e = next;
   }

   s->head = keep;
   if (keep == NULL)
      s->tail = NULL;
}

void a5_append_config_line(ALLEGRO_CONFIG *config, const char *section,
   const char *key, const char *value)
{
   A5_CONFIG_SECTION *s;
   A5_CONFIG_ENTRY *e, *after = NULL, *n;

   if (config == NULL || value == NULL)
      return;

   s = config_get_section(config, section ? section : "");
   if (s == NULL)
      return;

   n = calloc(1, sizeof(*n));
   if (n == NULL)
      return;

   n->key = key ? config_strdup(key) : NULL;
   n->value = config_strdup(value);
   if (n->value == NULL || (key && n->key == NULL)) {
      free(n->key);
      free(n->value);
      free(n);
      return;
   }

   for (e = s->head; e; e = e->next) {
      if (!config_is_blank(e))
         after = e;
   }

   if (after) {
      n->next = after->next;
      after->next = n;
   }
   else {
      n->next = s->head;
      s->head = n;
   }

   if (n->next == NULL)
      s->tail = n;
}
