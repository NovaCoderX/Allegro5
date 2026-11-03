/* File system entries and directories: fshook.h.
 *
 * On AmigaOS straight on dos.library (Lock, Examine, ExNext, CreateDir), so
 * paths are passed as AmigaDOS reads them; elsewhere on POSIX, for testing
 * on the host.  An entry's name is its full path: a directory's entries are
 * named by joining the directory's path and the file name, as in Allegro.
 *
 * Dune Dynasty builds directory paths with a trailing '/' ("save/"), which
 * is dropped before AmigaDOS sees it.
 */

#include <stdlib.h>
#include <string.h>
#include "allegro5/allegro.h"

#if defined(__AMIGA__)
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* Allegro's public ALLEGRO_FS_ENTRY is only a vtable pointer; the wrapper's
 * entries extend it.
 */
typedef struct A5_FS_ENTRY {
   ALLEGRO_FS_ENTRY fs;
   char *path;
   uint32_t mode;
#if defined(__AMIGA__)
   BPTR lock;                    /* while the directory is open */
   struct FileInfoBlock *fib;
#else
   DIR *dir;
#endif
} A5_FS_ENTRY;

#define ENTRY(e) ((A5_FS_ENTRY *)(e))

/* A copy of path without a trailing '/', unless that would leave a volume
 * ("Work:") or the root ("/") without its separator.
 */
static char *system_path(const char *path)
{
   const size_t len = strlen(path);
   char *copy = malloc(len + 1);

   if (copy == NULL)
      return NULL;

   memcpy(copy, path, len + 1);
   if (len > 1 && copy[len - 1] == '/' && copy[len - 2] != ':' && copy[len - 2] != '/')
      copy[len - 1] = '\0';
   return copy;
}

#if defined(__AMIGA__)

static uint32_t fib_mode(const struct FileInfoBlock *fib)
{
   uint32_t mode = (fib->fib_DirEntryType > 0) ? ALLEGRO_FILEMODE_ISDIR : ALLEGRO_FILEMODE_ISFILE;

   /* The AmigaDOS protection bits are set when the action is denied. */
   if (!(fib->fib_Protection & FIBF_READ))
      mode |= ALLEGRO_FILEMODE_READ;
   if (!(fib->fib_Protection & FIBF_WRITE))
      mode |= ALLEGRO_FILEMODE_WRITE;
   if (!(fib->fib_Protection & FIBF_EXECUTE))
      mode |= ALLEGRO_FILEMODE_EXECUTE;
   return mode;
}

/* The mode of whatever is at path, or 0 if there is nothing. */
static uint32_t path_mode(const char *path)
{
   char *name = system_path(path);
   struct FileInfoBlock *fib;
   uint32_t mode = 0;
   BPTR lock;

   if (name == NULL)
      return 0;

   lock = Lock((CONST_STRPTR)name, ACCESS_READ);
   if (lock) {
      fib = AllocDosObject(DOS_FIB, NULL);
      if (fib) {
         if (Examine(lock, fib))
            mode = fib_mode(fib);
         FreeDosObject(DOS_FIB, fib);
      }
      UnLock(lock);
   }

   free(name);
   return mode;
}

static bool make_one_directory(const char *name)
{
   BPTR lock = Lock((CONST_STRPTR)name, ACCESS_READ);

   if (lock == 0)
      lock = CreateDir((CONST_STRPTR)name);
   if (lock == 0)
      return false;

   UnLock(lock);
   return true;
}

#else

static uint32_t stat_mode(const struct stat *st)
{
   uint32_t mode = S_ISDIR(st->st_mode) ? ALLEGRO_FILEMODE_ISDIR : ALLEGRO_FILEMODE_ISFILE;

   if (st->st_mode & S_IRUSR) mode |= ALLEGRO_FILEMODE_READ;
   if (st->st_mode & S_IWUSR) mode |= ALLEGRO_FILEMODE_WRITE;
   if (st->st_mode & S_IXUSR) mode |= ALLEGRO_FILEMODE_EXECUTE;
   return mode;
}

static uint32_t path_mode(const char *path)
{
   char *name = system_path(path);
   struct stat st;
   uint32_t mode = 0;

   if (name == NULL)
      return 0;

   if (stat(name, &st) == 0)
      mode = stat_mode(&st);

   free(name);
   return mode;
}

static bool make_one_directory(const char *name)
{
   struct stat st;

   if (stat(name, &st) == 0)
      return S_ISDIR(st.st_mode);
   return mkdir(name, 0755) == 0;
}

#endif

static ALLEGRO_FS_ENTRY *new_entry(char *path, uint32_t mode)
{
   A5_FS_ENTRY *e = calloc(1, sizeof(*e));

   if (e == NULL) {
      free(path);
      return NULL;
   }

   e->path = path;
   e->mode = mode;
   return &e->fs;
}

/* As in Allegro, the entry exists even when nothing is at path. */
ALLEGRO_FS_ENTRY *al_create_fs_entry(const char *path)
{
   const size_t len = strlen(path);
   char *copy = malloc(len + 1);

   if (copy == NULL)
      return NULL;

   memcpy(copy, path, len + 1);
   return new_entry(copy, path_mode(path));
}

void al_destroy_fs_entry(ALLEGRO_FS_ENTRY *e)
{
   if (e == NULL)
      return;

   al_close_directory(e);
   free(ENTRY(e)->path);
   free(ENTRY(e));
}

const char *al_get_fs_entry_name(ALLEGRO_FS_ENTRY *e)
{
   return ENTRY(e)->path;
}

uint32_t al_get_fs_entry_mode(ALLEGRO_FS_ENTRY *e)
{
   return ENTRY(e)->mode;
}

#if defined(__AMIGA__)

bool al_open_directory(ALLEGRO_FS_ENTRY *fs)
{
   A5_FS_ENTRY *e = ENTRY(fs);
   char *name;

   if (e->lock)
      return false;

   name = system_path(e->path);
   if (name == NULL)
      return false;

   e->lock = Lock((CONST_STRPTR)name, ACCESS_READ);
   free(name);
   if (e->lock == 0)
      return false;

   e->fib = AllocDosObject(DOS_FIB, NULL);
   if (e->fib == NULL || !Examine(e->lock, e->fib) || e->fib->fib_DirEntryType <= 0) {
      al_close_directory(fs);
      return false;
   }

   return true;
}

static const char *next_name(A5_FS_ENTRY *e, uint32_t *mode)
{
   if (e->lock == 0 || !ExNext(e->lock, e->fib))
      return NULL;

   *mode = fib_mode(e->fib);
   return (const char *)e->fib->fib_FileName;
}

bool al_close_directory(ALLEGRO_FS_ENTRY *fs)
{
   A5_FS_ENTRY *e = ENTRY(fs);

   if (e->fib) {
      FreeDosObject(DOS_FIB, e->fib);
      e->fib = NULL;
   }
   if (e->lock) {
      UnLock(e->lock);
      e->lock = 0;
   }
   return true;
}

#else

bool al_open_directory(ALLEGRO_FS_ENTRY *fs)
{
   A5_FS_ENTRY *e = ENTRY(fs);
   char *name;

   if (e->dir)
      return false;

   name = system_path(e->path);
   if (name == NULL)
      return false;

   e->dir = opendir(name);
   free(name);
   return e->dir != NULL;
}

static const char *next_name(A5_FS_ENTRY *e, uint32_t *mode)
{
   struct dirent *d;

   if (e->dir == NULL)
      return NULL;

   do {
      d = readdir(e->dir);
   } while (d && (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0));

   if (d == NULL)
      return NULL;

   *mode = 0;
   return d->d_name;
}

bool al_close_directory(ALLEGRO_FS_ENTRY *fs)
{
   A5_FS_ENTRY *e = ENTRY(fs);

   if (e->dir) {
      closedir(e->dir);
      e->dir = NULL;
   }
   return true;
}

#endif

ALLEGRO_FS_ENTRY *al_read_directory(ALLEGRO_FS_ENTRY *fs)
{
   A5_FS_ENTRY *e = ENTRY(fs);
   uint32_t mode = 0;
   const char *name = next_name(e, &mode);
   size_t dir_len, name_len;
   char *path;
   bool sep;

   if (name == NULL)
      return NULL;

   dir_len = strlen(e->path);
   name_len = strlen(name);
   sep = (dir_len > 0 && e->path[dir_len - 1] != '/' && e->path[dir_len - 1] != ':');

   path = malloc(dir_len + sep + name_len + 1);
   if (path == NULL)
      return NULL;

   memcpy(path, e->path, dir_len);
   if (sep)
      path[dir_len] = '/';
   memcpy(path + dir_len + sep, name, name_len + 1);

   if (mode == 0)
      mode = path_mode(path);

   return new_entry(path, mode);
}

/* Makes path and any directories above it that are missing, as Allegro
 * does; true if it is a directory afterwards.
 */
bool al_make_directory(const char *path)
{
   char *name = system_path(path);
   char *p;
   bool ok;

   if (name == NULL)
      return false;

   /* Each parent after the volume or root, then the directory itself. */
   p = strchr(name, ':');
   p = p ? p + 1 : name;
   while (*p == '/')
      p++;

   for (; *p; p++) {
      if (*p == '/') {
         *p = '\0';
         make_one_directory(name);
         *p = '/';
      }
   }

   ok = make_one_directory(name);
   free(name);
   return ok;
}
