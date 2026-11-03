/* Paths: path.h.
 *
 * A path is kept as its directory part, ending in '/' (or ':' for an
 * AmigaDOS volume), and its file name, so Amiga paths such as
 * "Work:Games/DuneDynasty/save/_SAVE001.DAT" go through unchanged.  The
 * file name starts after the last '/' or ':'.  Only the functions Dune
 * Dynasty uses; the strings returned live until the path changes or is
 * destroyed, as in Allegro.
 */

#include <stdlib.h>
#include <string.h>
#include "allegro5/allegro.h"

struct ALLEGRO_PATH {
   char *dir;
   char *filename;
   char *cstr;                   /* for al_path_cstr() */
   char *basename;               /* for al_get_path_basename() */
   char *tail;                   /* for al_get_path_tail() */
};

static char *copy_string(const char *s, size_t len)
{
   char *copy = malloc(len + 1);

   if (copy) {
      memcpy(copy, s, len);
      copy[len] = '\0';
   }
   return copy;
}

static bool is_separator(char c)
{
   return c == '/' || c == ':';
}

/* Splits str after its last separator; with dir_only all of it is the
 * directory, and a '/' is added if it does not end in a separator.
 */
static ALLEGRO_PATH *make_path(const char *str, bool dir_only)
{
   ALLEGRO_PATH *path = calloc(1, sizeof(*path));
   size_t len, split;

   if (path == NULL)
      return NULL;

   if (str == NULL)
      str = "";
   len = strlen(str);

   if (dir_only) {
      const bool add_sep = (len > 0 && !is_separator(str[len - 1]));

      path->dir = malloc(len + add_sep + 1);
      if (path->dir) {
         memcpy(path->dir, str, len);
         if (add_sep)
            path->dir[len++] = '/';
         path->dir[len] = '\0';
      }
      path->filename = copy_string("", 0);
   }
   else {
      split = len;
      while (split > 0 && !is_separator(str[split - 1]))
         split--;

      path->dir = copy_string(str, split);
      path->filename = copy_string(str + split, len - split);
   }

   if (path->dir == NULL || path->filename == NULL) {
      al_destroy_path(path);
      return NULL;
   }

   return path;
}

ALLEGRO_PATH *al_create_path(const char *str)
{
   return make_path(str, false);
}

ALLEGRO_PATH *al_create_path_for_directory(const char *str)
{
   return make_path(str, true);
}

void al_destroy_path(ALLEGRO_PATH *path)
{
   if (path == NULL)
      return;

   free(path->dir);
   free(path->filename);
   free(path->cstr);
   free(path->basename);
   free(path->tail);
   free(path);
}

/* Allegro keeps the string in the path too, hence the cast. */
const char *al_path_cstr(const ALLEGRO_PATH *path, char delim)
{
   ALLEGRO_PATH *p = (ALLEGRO_PATH *)path;
   const size_t dir_len = strlen(p->dir);
   const size_t name_len = strlen(p->filename);
   size_t i;

   free(p->cstr);
   p->cstr = malloc(dir_len + name_len + 1);
   if (p->cstr == NULL)
      return "";

   memcpy(p->cstr, p->dir, dir_len);
   memcpy(p->cstr + dir_len, p->filename, name_len + 1);

   if (delim != '/') {
      for (i = 0; i < dir_len; i++) {
         if (p->cstr[i] == '/')
            p->cstr[i] = delim;
      }
   }

   return p->cstr;
}

void al_set_path_filename(ALLEGRO_PATH *path, const char *filename)
{
   char *copy;

   if (filename == NULL)
      filename = "";

   copy = copy_string(filename, strlen(filename));
   if (copy) {
      free(path->filename);
      path->filename = copy;
   }
}

const char *al_get_path_filename(const ALLEGRO_PATH *path)
{
   return path->filename;
}

/* The file name without its extension. */
const char *al_get_path_basename(const ALLEGRO_PATH *path)
{
   ALLEGRO_PATH *p = (ALLEGRO_PATH *)path;
   const char *dot = strrchr(p->filename, '.');
   const size_t len = dot ? (size_t)(dot - p->filename) : strlen(p->filename);

   free(p->basename);
   p->basename = copy_string(p->filename, len);
   return p->basename ? p->basename : "";
}

/* From the last '.' of the file name, or "" if it has none. */
const char *al_get_path_extension(const ALLEGRO_PATH *path)
{
   const char *dot = strrchr(path->filename, '.');

   return dot ? dot : "";
}

/* The last directory name, or NULL if there is none. */
const char *al_get_path_tail(const ALLEGRO_PATH *path)
{
   ALLEGRO_PATH *p = (ALLEGRO_PATH *)path;
   size_t end = strlen(p->dir), start;

   if (end > 0 && p->dir[end - 1] == '/')
      end--;

   start = end;
   while (start > 0 && !is_separator(p->dir[start - 1]))
      start--;

   if (start == end)
      return NULL;

   free(p->tail);
   p->tail = copy_string(p->dir + start, end - start);
   return p->tail;
}
