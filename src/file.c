/* File I/O: file.h. */

#include "allegro5/allegro.h"
#include "a5_stub.h"

ALLEGRO_FILE *al_fopen(const char *path, const char *mode)
{
   A5_STUB();
   return NULL;
}

bool al_fclose(ALLEGRO_FILE *f)
{
   A5_STUB();
   return false;
}

size_t al_fread(ALLEGRO_FILE *f, void *ptr, size_t size)
{
   A5_STUB();
   return 0;
}

int al_fgetc(ALLEGRO_FILE *f)
{
   A5_STUB();
   return EOF;
}

int16_t al_fread16le(ALLEGRO_FILE *f)
{
   A5_STUB();
   return EOF;
}

int32_t al_fread32le(ALLEGRO_FILE *f)
{
   A5_STUB();
   return EOF;
}

bool al_fseek(ALLEGRO_FILE *f, int64_t offset, int whence)
{
   A5_STUB();
   return false;
}

int64_t al_ftell(ALLEGRO_FILE *f)
{
   A5_STUB();
   return -1;
}

int64_t al_fsize(ALLEGRO_FILE *f)
{
   A5_STUB();
   return -1;
}
