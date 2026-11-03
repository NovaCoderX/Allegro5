/* Audio addon: allegro_audio.h, on SDL2 audio (AHI on AmigaOS).
 *
 * One voice is one SDL audio device. Its callback, on SDL's audio thread
 * (an AmigaOS process of its own), pulls the attached mixer, which mixes
 * its sample instances and streams in integers: no floating point per
 * sample, since the 68060 build has no FPU code (every float operation is
 * a library call). The mixer works at the voice's rate and channel count
 * (Dune Dynasty asks for 22050 Hz stereo on Amiga, see audio_a5.c) and
 * whatever depth it is created with: it always mixes in 32-bit integers
 * and the voice is 16-bit.
 *
 * What follows Allegro's mixer (addons/audio/kcm_*.c):
 *
 * - The channel matrix and the pan law: a mono source goes to both sides
 *   at 1/sqrt(2); with a pan (not ALLEGRO_AUDIO_PAN_NONE) the left side is
 *   scaled by sqrt((1 - pan) / 2) and the right by sqrt((1 + pan) / 2);
 *   then the gain. Worked out (in float) when the gain, pan or mixer
 *   changes, kept as 2.14 fixed point (gains above 2.0 are cut to 2.0;
 *   Dune Dynasty's never pass 1.0).
 * - Samples play once (ALLEGRO_PLAYMODE_ONCE) and are resampled to the
 *   mixer's rate with linear interpolation, Allegro's default quality.
 * - Streams: the program takes a free fragment with
 *   al_get_audio_stream_fragment(), fills it and queues it with
 *   al_set_audio_stream_fragment(). Each time the mixer finishes a
 *   fragment, or finds none queued, the stream emits one
 *   ALLEGRO_EVENT_AUDIO_STREAM_FRAGMENT per free fragment, as Allegro
 *   does, less its events still waiting in the queue (see
 *   stream_emit_events_locked()); that is what drives Dune Dynasty's
 *   AdLib rendering. A stream at
 *   the mixer's rate is copied; at another rate it is resampled to the
 *   nearest frame. An empty stream plays silence and keeps playing.
 *
 * Not here: al_load_audio_stream() (no codecs; it returns NULL), loop
 * modes, speed, and mixers attached to mixers.
 *
 * Locking: the mixer's lists and the players' state change under
 * SDL_LockAudioDevice() of the open voice, which holds off the callback.
 * The callback takes the events lock (a5_lock()) to emit, so the main
 * thread never holds a5_lock() while it locks the device.
 */

#include <math.h>
#include <string.h>

#include "a5_internal.h"
#include "allegro5/allegro.h"
#include "allegro5/allegro_audio.h"

/* Gains are 2.14 fixed point, at most 2.0, so a 16-bit value times a gain
 * fits in 31 bits; each product is rounded back to 16-bit scale. */
#define A5_GAIN_SHIFT      14
#define A5_GAIN_ONE        (1 << A5_GAIN_SHIFT)
#define A5_MAX_GAIN        (2 * A5_GAIN_ONE)
#define A5_GAIN_ROUND      (1 << (A5_GAIN_SHIFT - 1))
#define APPLY_GAIN(v, g)   (((v) * (g) + A5_GAIN_ROUND) >> A5_GAIN_SHIFT)

/* What a mixer plays: the part shared by sample instances and streams. */
typedef struct A5_PLAYER {
   ALLEGRO_MIXER *mixer;         /* attached to, or NULL */
   bool is_stream;
   bool playing;
   float gain, pan;

   /* The source's format. */
   unsigned int freq;
   ALLEGRO_AUDIO_DEPTH depth;
   ALLEGRO_CHANNEL_CONF chan_conf;
   int channels;

   /* Worked out on attach and on gain or pan changes. */
   Sint32 matrix[2][2];          /* [mixer channel][source channel], 2.14 */
   Uint32 step;                  /* source frames per mixer frame, 16.16 */

   /* Position: whole frames and 16-bit fraction. */
   Uint32 pos;
   Uint32 frac;
} A5_PLAYER;

struct ALLEGRO_SAMPLE {
   void *buf;
   unsigned int len;             /* frames */
   unsigned int freq;
   ALLEGRO_AUDIO_DEPTH depth;
   ALLEGRO_CHANNEL_CONF chan_conf;
   bool free_buf;
};

struct ALLEGRO_SAMPLE_INSTANCE {
   A5_PLAYER p;                  /* first: the mixer keeps A5_PLAYER pointers */
   ALLEGRO_SAMPLE *spl;
};

struct ALLEGRO_AUDIO_STREAM {
   A5_PLAYER p;                  /* first */
   A5_EVENT_SOURCE_ALIGN ALLEGRO_EVENT_SOURCE es;
   unsigned int frag_samples;
   size_t buf_count;
   size_t frame_size;
   void **free_bufs;             /* for the program to fill, oldest first */
   void **pending_bufs;          /* queued to play; [0] is playing */
   char *main_buffer;
   bool want_events;             /* set by the mixer, emitted after mixing */
};

struct ALLEGRO_MIXER {
   unsigned int freq;
   ALLEGRO_AUDIO_DEPTH depth;
   ALLEGRO_CHANNEL_CONF chan_conf;
   int channels;
   ALLEGRO_VOICE *voice;         /* attached to, or NULL */
   A5_PLAYER **players;
   int num_players;
   int max_players;
   Sint32 *mix;                  /* the accumulator: mix_frames * channels */
   int mix_frames;
};

struct ALLEGRO_VOICE {
   SDL_AudioDeviceID dev;
   unsigned int freq;
   int channels;
   ALLEGRO_MIXER *mixer;
};

static bool s_installed = false;

/* The open voice's device: the one lock for everything below. */
static SDL_AudioDeviceID s_device = 0;

static void audio_lock(void)
{
   if (s_device != 0)
      SDL_LockAudioDevice(s_device);
}

static void audio_unlock(void)
{
   if (s_device != 0)
      SDL_UnlockAudioDevice(s_device);
}

/*--------------------------------------------------------------*/

bool al_install_audio(void)
{
   if (s_installed)
      return true;

   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
      return false;

   s_installed = true;
   return true;
}

void al_uninstall_audio(void)
{
   if (!s_installed)
      return;

   /* Dune Dynasty destroys its voice first; this only closes SDL. */
   SDL_QuitSubSystem(SDL_INIT_AUDIO);
   s_device = 0;
   s_installed = false;
}

/* Real implementation, as in Allegro's audio.c. */
size_t al_get_audio_depth_size(ALLEGRO_AUDIO_DEPTH depth)
{
   switch (depth) {
      case ALLEGRO_AUDIO_DEPTH_INT8:
      case ALLEGRO_AUDIO_DEPTH_UINT8:
         return sizeof(int8_t);
      case ALLEGRO_AUDIO_DEPTH_INT16:
      case ALLEGRO_AUDIO_DEPTH_UINT16:
         return sizeof(int16_t);
      case ALLEGRO_AUDIO_DEPTH_INT24:
      case ALLEGRO_AUDIO_DEPTH_UINT24:
         return sizeof(int32_t);
      case ALLEGRO_AUDIO_DEPTH_FLOAT32:
         return sizeof(float);
      default:
         return 0;
   }
}

/* Mono and stereo only: Allegro's (chan_conf >> 4) + (chan_conf & 0xF). */
static int channel_count(ALLEGRO_CHANNEL_CONF chan_conf)
{
   if (chan_conf == ALLEGRO_CHANNEL_CONF_1)
      return 1;
   if (chan_conf == ALLEGRO_CHANNEL_CONF_2)
      return 2;
   return 0;
}

/*--------------------------------------------------------------*/
/* The player's matrix and step, from Allegro's _al_rechannel_matrix(). */

static Sint32 to_gain(float g)
{
   if (!(g > 0.0f))
      return 0;
   if (g >= (float)A5_MAX_GAIN / A5_GAIN_ONE)
      return A5_MAX_GAIN;
   return (Sint32)(g * A5_GAIN_ONE + 0.5f);
}

static void player_update_locked(A5_PLAYER *p)
{
   const ALLEGRO_MIXER *mixer = p->mixer;
   float mat[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
   int dst_chans, i, j;

   if (mixer == NULL)
      return;

   dst_chans = mixer->channels;

   if (p->channels == 1 && dst_chans == 2) {
      mat[0][0] = mat[1][0] = 0.70710678f;   /* centre to front l/r */
   }
   else if (p->channels == 2 && dst_chans == 1) {
      mat[0][0] = mat[0][1] = 0.70710678f;   /* stereo to mono */
   }
   else {
      for (i = 0; i < p->channels && i < dst_chans; i++)
         mat[i][i] = 1.0f;
   }

   if (p->pan != ALLEGRO_AUDIO_PAN_NONE) {
      const float pan = p->pan < -1.0f ? -1.0f : p->pan > 1.0f ? 1.0f : p->pan;
      const float rgain = sqrtf((pan + 1.0f) / 2.0f);
      const float lgain = sqrtf((-pan + 1.0f) / 2.0f);

      for (j = 0; j < p->channels; j++) {
         mat[0][j] *= lgain;
         mat[1][j] *= rgain;
      }
   }

   for (i = 0; i < 2; i++) {
      for (j = 0; j < 2; j++)
         p->matrix[i][j] = to_gain(mat[i][j] * p->gain);
   }

   p->step = (Uint32)(((Uint64)p->freq << 16) / mixer->freq);
   if (p->step == 0)
      p->step = 1;
}

/*--------------------------------------------------------------*/
/* Mixing. Everything here runs in the audio callback. */

/* One source value as 16-bit signed. */
static Sint32 fetch(const void *buf, ALLEGRO_AUDIO_DEPTH depth, Uint32 i)
{
   switch (depth) {
      case ALLEGRO_AUDIO_DEPTH_INT8:
         return (Sint32)((const Sint8 *)buf)[i] * 256;
      case ALLEGRO_AUDIO_DEPTH_UINT8:
         return ((Sint32)((const Uint8 *)buf)[i] - 128) * 256;
      case ALLEGRO_AUDIO_DEPTH_INT16:
         return ((const Sint16 *)buf)[i];
      case ALLEGRO_AUDIO_DEPTH_UINT16:
         return (Sint32)((const Uint16 *)buf)[i] - 32768;
      case ALLEGRO_AUDIO_DEPTH_INT24:
         return ((const Sint32 *)buf)[i] >> 8;
      case ALLEGRO_AUDIO_DEPTH_UINT24:
         return (((const Sint32 *)buf)[i] - 0x800000) >> 8;
      case ALLEGRO_AUDIO_DEPTH_FLOAT32:
         /* Not used by the game: a float per value. */
         return (Sint32)(((const float *)buf)[i] * 32767.0f);
   }
   return 0;
}

static void mix_frame(Sint32 *acc, int dst_chans, const Sint32 m[2][2],
   Sint32 s0, Sint32 s1)
{
   acc[0] += APPLY_GAIN(s0, m[0][0]) + APPLY_GAIN(s1, m[0][1]);
   if (dst_chans == 2)
      acc[1] += APPLY_GAIN(s0, m[1][0]) + APPLY_GAIN(s1, m[1][1]);
}

/* A sample instance: plays once, linear interpolation. */
static void mix_instance(ALLEGRO_SAMPLE_INSTANCE *si, Sint32 *acc,
   int frames, int dst_chans)
{
   A5_PLAYER *p = &si->p;
   const ALLEGRO_SAMPLE *spl = si->spl;
   const Uint32 len = spl->len;
   const Uint32 step = p->step;
   Uint32 pos = p->pos;
   Uint32 frac = p->frac;
   int n;

   if (p->channels == 1 && p->depth == ALLEGRO_AUDIO_DEPTH_UINT8 && dst_chans == 2) {
      /* Dune II's voices and sound effects (.VOC): 8-bit mono. The
       * interpolation is done at 8 bits, where it cannot overflow. */
      const Uint8 *buf = spl->buf;
      const Sint32 gl = p->matrix[0][0];
      const Sint32 gr = p->matrix[1][0];

      for (n = 0; n < frames && pos < len; n++, acc += 2) {
         const Sint32 a = (Sint32)buf[pos] - 128;
         const Sint32 b = (pos + 1 < len) ? (Sint32)buf[pos + 1] - 128 : a;
         const Sint32 v = a * 256 + (((b - a) * (Sint32)frac + 128) >> 8);

         acc[0] += APPLY_GAIN(v, gl);
         acc[1] += APPLY_GAIN(v, gr);

         frac += step & 0xFFFF;
         pos += (step >> 16) + (frac >> 16);
         frac &= 0xFFFF;
      }
   }
   else {
      const int chans = p->channels;

      for (n = 0; n < frames && pos < len; n++, acc += dst_chans) {
         const Uint32 next = (pos + 1 < len) ? pos + 1 : pos;
         Sint32 s[2] = { 0, 0 };
         int c;

         for (c = 0; c < chans; c++) {
            const Sint32 a = fetch(spl->buf, p->depth, pos * chans + c);
            const Sint32 b = fetch(spl->buf, p->depth, next * chans + c);
            s[c] = a + (((b - a) * (Sint32)(frac >> 1) + (1 << 14)) >> 15);
         }

         mix_frame(acc, dst_chans, (const Sint32 (*)[2])p->matrix, s[0], s[1]);

         frac += step & 0xFFFF;
         pos += (step >> 16) + (frac >> 16);
         frac &= 0xFFFF;
      }
   }

   if (pos >= len) {
      /* ALLEGRO_PLAYMODE_ONCE: back to the start, stopped. */
      p->playing = false;
      pos = 0;
      frac = 0;
   }

   p->pos = pos;
   p->frac = frac;
}

/* The fragment at the front of the queue is done: back to the free list. */
static void stream_retire_fragment(ALLEGRO_AUDIO_STREAM *stream)
{
   void *done = stream->pending_bufs[0];
   size_t i;

   for (i = 0; i + 1 < stream->buf_count && stream->pending_bufs[i]; i++)
      stream->pending_bufs[i] = stream->pending_bufs[i + 1];
   stream->pending_bufs[i] = NULL;

   for (i = 0; stream->free_bufs[i]; i++)
      ;
   stream->free_bufs[i] = done;
}

static void mix_stream(ALLEGRO_AUDIO_STREAM *stream, Sint32 *acc,
   int frames, int dst_chans)
{
   A5_PLAYER *p = &stream->p;
   const Uint32 len = stream->frag_samples;
   const int chans = p->channels;
   int n = 0;

   while (n < frames) {
      const void *buf = stream->pending_bufs[0];

      if (buf == NULL) {
         /* Nothing queued: silence, and ask for more. */
         stream->want_events = true;
         return;
      }

      if (p->step == 0x10000 && p->frac == 0) {
         /* At the mixer's rate: copy. */
         int count = (int)(len - p->pos);
         int k;

         if (count > frames - n)
            count = frames - n;

         if (chans == 1 && dst_chans == 2 && p->depth == ALLEGRO_AUDIO_DEPTH_INT16) {
            /* Dune Dynasty's AdLib music: 16-bit mono. */
            const Sint16 *src = (const Sint16 *)buf + p->pos;
            const Sint32 gl = p->matrix[0][0];
            const Sint32 gr = p->matrix[1][0];
            Sint32 *a = acc + n * 2;

            for (k = 0; k < count; k++, a += 2) {
               const Sint32 v = src[k];
               a[0] += APPLY_GAIN(v, gl);
               a[1] += APPLY_GAIN(v, gr);
            }
         }
         else {
            for (k = 0; k < count; k++) {
               const Uint32 f = (p->pos + k) * chans;
               const Sint32 s0 = fetch(buf, p->depth, f);
               const Sint32 s1 = (chans == 2) ? fetch(buf, p->depth, f + 1) : 0;
               mix_frame(acc + (n + k) * dst_chans, dst_chans,
                  (const Sint32 (*)[2])p->matrix, s0, s1);
            }
         }

         n += count;
         p->pos += count;
      }
      else {
         /* Another rate: the nearest frame. */
         for (; n < frames && p->pos < len; n++) {
            const Uint32 f = p->pos * chans;
            const Sint32 s0 = fetch(buf, p->depth, f);
            const Sint32 s1 = (chans == 2) ? fetch(buf, p->depth, f + 1) : 0;

            mix_frame(acc + n * dst_chans, dst_chans,
               (const Sint32 (*)[2])p->matrix, s0, s1);

            p->frac += p->step & 0xFFFF;
            p->pos += (p->step >> 16) + (p->frac >> 16);
            p->frac &= 0xFFFF;
         }
      }

      while (p->pos >= len && stream->pending_bufs[0] != NULL) {
         p->pos -= len;
         stream_retire_fragment(stream);
         stream->want_events = true;
      }
   }
}

static size_t stream_free_fragments(const ALLEGRO_AUDIO_STREAM *stream)
{
   size_t i;

   for (i = 0; i < stream->buf_count && stream->free_bufs[i]; i++)
      ;
   return i;
}

/* One event per free fragment, less those still waiting in the queue:
 * a stream with nothing queued asks again on every callback, and while the
 * program is busy (loading, saving) it would otherwise add a fragment's
 * worth of events every 46 ms and grow the queue from the audio task. The
 * program answers each event by filling one fragment, so this is all it
 * needs, and it holds after a flush or unregister too.
 */
static void stream_emit_events_locked(ALLEGRO_AUDIO_STREAM *stream)
{
   const size_t free_frags = stream_free_fragments(stream);
   const size_t waiting = a5_queued_events_locked(&stream->es);
   size_t count = (free_frags > waiting) ? free_frags - waiting : 0;

   while (count--) {
      ALLEGRO_EVENT event;

      memset(&event, 0, sizeof(event));
      event.user.type = ALLEGRO_EVENT_AUDIO_STREAM_FRAGMENT;
      event.user.timestamp = al_get_time();
      a5_emit_event_locked(&stream->es, &event);
   }
}

/* Mixes frames of the mixer into out, 16-bit at the mixer's channels. */
static void mixer_render(ALLEGRO_MIXER *mixer, Sint16 *out, int frames)
{
   const int chans = mixer->channels;
   bool events = false;

   while (frames > 0) {
      const int chunk = frames < mixer->mix_frames ? frames : mixer->mix_frames;
      Sint32 *acc = mixer->mix;
      int i;

      memset(acc, 0, chunk * chans * sizeof(*acc));

      for (i = 0; i < mixer->num_players; i++) {
         A5_PLAYER *p = mixer->players[i];

         if (!p->playing)
            continue;

         if (p->is_stream) {
            mix_stream((ALLEGRO_AUDIO_STREAM *)p, acc, chunk, chans);
            if (((ALLEGRO_AUDIO_STREAM *)p)->want_events)
               events = true;
         }
         else if (((ALLEGRO_SAMPLE_INSTANCE *)p)->spl != NULL) {
            mix_instance((ALLEGRO_SAMPLE_INSTANCE *)p, acc, chunk, chans);
         }
      }

      for (i = 0; i < chunk * chans; i++) {
         const Sint32 v = acc[i];
         out[i] = v < -32768 ? -32768 : v > 32767 ? 32767 : (Sint16)v;
      }

      out += chunk * chans;
      frames -= chunk;
   }

   if (events) {
      int i;

      a5_lock();
      for (i = 0; i < mixer->num_players; i++) {
         A5_PLAYER *p = mixer->players[i];

         if (p->is_stream && ((ALLEGRO_AUDIO_STREAM *)p)->want_events) {
            ((ALLEGRO_AUDIO_STREAM *)p)->want_events = false;
            stream_emit_events_locked((ALLEGRO_AUDIO_STREAM *)p);
         }
      }
      a5_unlock();
   }
}

static void SDLCALL voice_callback(void *userdata, Uint8 *stream, int len)
{
   ALLEGRO_VOICE *voice = userdata;
   const int frames = len / (int)(voice->channels * sizeof(Sint16));

   if (voice->mixer == NULL) {
      memset(stream, 0, len);
      return;
   }

   mixer_render(voice->mixer, (Sint16 *)stream, frames);
}

/*--------------------------------------------------------------*/
/* Voices and mixers. */

ALLEGRO_VOICE *al_create_voice(unsigned int freq,
   ALLEGRO_AUDIO_DEPTH depth, ALLEGRO_CHANNEL_CONF chan_conf)
{
   ALLEGRO_VOICE *voice;
   SDL_AudioSpec want, have;
   const int channels = channel_count(chan_conf);

   /* One device, 16-bit: what Dune Dynasty asks for. */
   if (!s_installed || s_device != 0 || freq == 0 || channels == 0 ||
         depth != ALLEGRO_AUDIO_DEPTH_INT16)
      return NULL;

   voice = SDL_calloc(1, sizeof(*voice));
   if (voice == NULL)
      return NULL;

   SDL_zero(want);
   want.freq = freq;
   want.format = AUDIO_S16SYS;
   want.channels = channels;
   /* About 46 ms at 22050 Hz. */
   want.samples = (freq <= 24000) ? 1024 : 2048;
   want.callback = voice_callback;
   want.userdata = voice;

   /* No changes allowed: SDL converts to whatever the device takes. */
   voice->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
   if (voice->dev == 0) {
      SDL_free(voice);
      return NULL;
   }

   voice->freq = freq;
   voice->channels = channels;
   s_device = voice->dev;
   return voice;
}

void al_destroy_voice(ALLEGRO_VOICE *voice)
{
   if (voice == NULL)
      return;

   SDL_CloseAudioDevice(voice->dev);
   if (s_device == voice->dev)
      s_device = 0;

   if (voice->mixer != NULL)
      voice->mixer->voice = NULL;

   SDL_free(voice);
}

ALLEGRO_MIXER *al_create_mixer(unsigned int freq,
   ALLEGRO_AUDIO_DEPTH depth, ALLEGRO_CHANNEL_CONF chan_conf)
{
   ALLEGRO_MIXER *mixer;
   const int channels = channel_count(chan_conf);

   if (freq == 0 || channels == 0)
      return NULL;

   mixer = SDL_calloc(1, sizeof(*mixer));
   if (mixer == NULL)
      return NULL;

   mixer->freq = freq;
   mixer->depth = depth;
   mixer->chan_conf = chan_conf;
   mixer->channels = channels;
   mixer->mix_frames = 1024;
   mixer->mix = SDL_malloc(mixer->mix_frames * channels * sizeof(*mixer->mix));
   if (mixer->mix == NULL) {
      SDL_free(mixer);
      return NULL;
   }

   return mixer;
}

void al_destroy_mixer(ALLEGRO_MIXER *mixer)
{
   int i;

   if (mixer == NULL)
      return;

   audio_lock();
   if (mixer->voice != NULL)
      mixer->voice->mixer = NULL;
   for (i = 0; i < mixer->num_players; i++)
      mixer->players[i]->mixer = NULL;
   audio_unlock();

   SDL_free(mixer->players);
   SDL_free(mixer->mix);
   SDL_free(mixer);
}

bool al_attach_mixer_to_voice(ALLEGRO_MIXER *mixer, ALLEGRO_VOICE *voice)
{
   if (mixer == NULL || voice == NULL || voice->mixer != NULL ||
         mixer->voice != NULL)
      return false;

   /* As in Allegro: same rate and channels, no conversion. */
   if (mixer->freq != voice->freq || mixer->channels != voice->channels)
      return false;

   SDL_LockAudioDevice(voice->dev);
   voice->mixer = mixer;
   mixer->voice = voice;
   SDL_UnlockAudioDevice(voice->dev);

   SDL_PauseAudioDevice(voice->dev, 0);
   return true;
}

static bool mixer_add_player_locked(ALLEGRO_MIXER *mixer, A5_PLAYER *p)
{
   if (mixer->num_players == mixer->max_players) {
      const int max = mixer->max_players ? mixer->max_players * 2 : 16;
      A5_PLAYER **players = SDL_realloc(mixer->players, max * sizeof(*players));

      if (players == NULL)
         return false;

      mixer->players = players;
      mixer->max_players = max;
   }

   mixer->players[mixer->num_players++] = p;
   p->mixer = mixer;
   player_update_locked(p);
   return true;
}

static void player_detach_locked(A5_PLAYER *p)
{
   ALLEGRO_MIXER *mixer = p->mixer;
   int i;

   if (mixer == NULL)
      return;

   for (i = 0; i < mixer->num_players; i++) {
      if (mixer->players[i] == p) {
         /* Keep the order: it is the mixing order. */
         memmove(&mixer->players[i], &mixer->players[i + 1],
            (mixer->num_players - i - 1) * sizeof(*mixer->players));
         mixer->num_players--;
         break;
      }
   }

   p->mixer = NULL;
}

/*--------------------------------------------------------------*/
/* Samples. */

ALLEGRO_SAMPLE *al_create_sample(void *buf, unsigned int samples,
   unsigned int freq, ALLEGRO_AUDIO_DEPTH depth,
   ALLEGRO_CHANNEL_CONF chan_conf, bool free_buf)
{
   ALLEGRO_SAMPLE *spl;

   if (buf == NULL || freq == 0 || channel_count(chan_conf) == 0 ||
         al_get_audio_depth_size(depth) == 0)
      return NULL;

   spl = SDL_calloc(1, sizeof(*spl));
   if (spl == NULL)
      return NULL;

   spl->buf = buf;
   spl->len = samples;
   spl->freq = freq;
   spl->depth = depth;
   spl->chan_conf = chan_conf;
   spl->free_buf = free_buf;
   return spl;
}

void al_destroy_sample(ALLEGRO_SAMPLE *spl)
{
   if (spl == NULL)
      return;

   /* Dune Dynasty stops its instances before; Allegro would too. */
   if (spl->free_buf)
      al_free(spl->buf);
   SDL_free(spl);
}

ALLEGRO_SAMPLE_INSTANCE *al_create_sample_instance(ALLEGRO_SAMPLE *data)
{
   ALLEGRO_SAMPLE_INSTANCE *si = SDL_calloc(1, sizeof(*si));

   if (si == NULL)
      return NULL;

   si->p.is_stream = false;
   si->p.gain = 1.0f;
   si->p.pan = 0.0f;              /* as Allegro: centred */
   si->p.freq = 44100;
   si->p.depth = ALLEGRO_AUDIO_DEPTH_INT16;
   si->p.chan_conf = ALLEGRO_CHANNEL_CONF_1;
   si->p.channels = 1;

   if (data != NULL)
      al_set_sample(si, data);

   return si;
}

void al_destroy_sample_instance(ALLEGRO_SAMPLE_INSTANCE *si)
{
   if (si == NULL)
      return;

   audio_lock();
   player_detach_locked(&si->p);
   audio_unlock();

   SDL_free(si);
}

bool al_set_sample(ALLEGRO_SAMPLE_INSTANCE *si, ALLEGRO_SAMPLE *data)
{
   if (si == NULL)
      return false;

   audio_lock();
   si->p.playing = false;
   si->p.pos = 0;
   si->p.frac = 0;
   si->spl = data;
   if (data != NULL) {
      si->p.freq = data->freq;
      si->p.depth = data->depth;
      si->p.chan_conf = data->chan_conf;
      si->p.channels = channel_count(data->chan_conf);
      player_update_locked(&si->p);
   }
   audio_unlock();
   return true;
}

bool al_attach_sample_instance_to_mixer(ALLEGRO_SAMPLE_INSTANCE *si,
   ALLEGRO_MIXER *mixer)
{
   bool ok;

   if (si == NULL || mixer == NULL)
      return false;

   audio_lock();
   player_detach_locked(&si->p);
   ok = mixer_add_player_locked(mixer, &si->p);
   audio_unlock();
   return ok;
}

bool al_play_sample_instance(ALLEGRO_SAMPLE_INSTANCE *si)
{
   if (si == NULL || si->spl == NULL)
      return false;

   audio_lock();
   si->p.playing = true;
   audio_unlock();
   return true;
}

bool al_get_sample_instance_playing(const ALLEGRO_SAMPLE_INSTANCE *si)
{
   return si != NULL && si->p.playing;
}

bool al_set_sample_instance_gain(ALLEGRO_SAMPLE_INSTANCE *si, float val)
{
   if (si == NULL)
      return false;

   audio_lock();
   si->p.gain = val;
   player_update_locked(&si->p);
   audio_unlock();
   return true;
}

bool al_set_sample_instance_pan(ALLEGRO_SAMPLE_INSTANCE *si, float val)
{
   if (si == NULL)
      return false;

   audio_lock();
   si->p.pan = val;
   player_update_locked(&si->p);
   audio_unlock();
   return true;
}

/*--------------------------------------------------------------*/
/* Audio streams. */

ALLEGRO_AUDIO_STREAM *al_create_audio_stream(size_t buffer_count,
   unsigned int samples, unsigned int freq,
   ALLEGRO_AUDIO_DEPTH depth, ALLEGRO_CHANNEL_CONF chan_conf)
{
   ALLEGRO_AUDIO_STREAM *stream;
   const int channels = channel_count(chan_conf);
   const size_t frame_size = channels * al_get_audio_depth_size(depth);
   size_t i;

   if (buffer_count == 0 || samples == 0 || freq == 0 || frame_size == 0)
      return NULL;

   stream = SDL_calloc(1, sizeof(*stream));
   if (stream == NULL)
      return NULL;

   stream->p.is_stream = true;
   stream->p.playing = true;
   stream->p.gain = 1.0f;
   stream->p.pan = 0.0f;
   stream->p.freq = freq;
   stream->p.depth = depth;
   stream->p.chan_conf = chan_conf;
   stream->p.channels = channels;
   stream->frag_samples = samples;
   stream->buf_count = buffer_count;
   stream->frame_size = frame_size;

   stream->free_bufs = SDL_calloc(buffer_count * 2, sizeof(void *));
   stream->main_buffer = SDL_calloc(buffer_count, samples * frame_size);
   if (stream->free_bufs == NULL || stream->main_buffer == NULL) {
      SDL_free(stream->free_bufs);
      SDL_free(stream->main_buffer);
      SDL_free(stream);
      return NULL;
   }

   stream->pending_bufs = stream->free_bufs + buffer_count;
   for (i = 0; i < buffer_count; i++)
      stream->free_bufs[i] = stream->main_buffer + i * samples * frame_size;

   a5_event_source_init(&stream->es);
   return stream;
}

ALLEGRO_AUDIO_STREAM *al_load_audio_stream(const char *filename,
   size_t buffer_count, unsigned int samples)
{
   /* No codecs on Amiga (no FLAC or Ogg Vorbis: too heavy for a 68k). */
   (void)filename, (void)buffer_count, (void)samples;
   return NULL;
}

void al_destroy_audio_stream(ALLEGRO_AUDIO_STREAM *stream)
{
   if (stream == NULL)
      return;

   audio_lock();
   player_detach_locked(&stream->p);
   audio_unlock();

   /* Unregisters it from its queues and drops its queued events. */
   a5_lock();
   a5_event_source_free_locked(&stream->es);
   a5_unlock();

   SDL_free(stream->free_bufs);
   SDL_free(stream->main_buffer);
   SDL_free(stream);
}

bool al_attach_audio_stream_to_mixer(ALLEGRO_AUDIO_STREAM *stream,
   ALLEGRO_MIXER *mixer)
{
   bool ok;

   if (stream == NULL || mixer == NULL)
      return false;

   audio_lock();
   player_detach_locked(&stream->p);
   ok = mixer_add_player_locked(mixer, &stream->p);
   audio_unlock();

   /* As Allegro does when a stream starts playing: ask for its fragments,
    * or none would ever be asked for. */
   if (ok && stream->p.playing) {
      a5_lock();
      stream_emit_events_locked(stream);
      a5_unlock();
   }

   return ok;
}

bool al_detach_audio_stream(ALLEGRO_AUDIO_STREAM *stream)
{
   if (stream == NULL)
      return false;

   audio_lock();
   player_detach_locked(&stream->p);
   audio_unlock();
   return true;
}

ALLEGRO_EVENT_SOURCE *al_get_audio_stream_event_source(
   ALLEGRO_AUDIO_STREAM *stream)
{
   return &stream->es;
}

void *al_get_audio_stream_fragment(const ALLEGRO_AUDIO_STREAM *cstream)
{
   ALLEGRO_AUDIO_STREAM *stream = (ALLEGRO_AUDIO_STREAM *)cstream;
   void *fragment;
   size_t i;

   if (stream == NULL)
      return NULL;

   audio_lock();
   fragment = stream->free_bufs[0];
   if (fragment != NULL) {
      for (i = 0; i + 1 < stream->buf_count && stream->free_bufs[i]; i++)
         stream->free_bufs[i] = stream->free_bufs[i + 1];
      stream->free_bufs[i] = NULL;
   }
   audio_unlock();
   return fragment;
}

bool al_set_audio_stream_fragment(ALLEGRO_AUDIO_STREAM *stream, void *val)
{
   bool ok = false;
   size_t i;

   if (stream == NULL || val == NULL)
      return false;

   audio_lock();
   for (i = 0; i < stream->buf_count && stream->pending_bufs[i]; i++)
      ;
   if (i < stream->buf_count) {
      stream->pending_bufs[i] = val;
      ok = true;
   }
   audio_unlock();
   return ok;
}

bool al_get_audio_stream_playing(const ALLEGRO_AUDIO_STREAM *stream)
{
   return stream != NULL && stream->p.playing;
}

bool al_set_audio_stream_gain(ALLEGRO_AUDIO_STREAM *stream, float val)
{
   if (stream == NULL)
      return false;

   audio_lock();
   stream->p.gain = val;
   player_update_locked(&stream->p);
   audio_unlock();
   return true;
}

bool al_set_audio_stream_pan(ALLEGRO_AUDIO_STREAM *stream, float val)
{
   if (stream == NULL)
      return false;

   audio_lock();
   stream->p.pan = val;
   player_update_locked(&stream->p);
   audio_unlock();
   return true;
}

#ifdef A5_AUDIO_TEST
/* For the wrapper's tests: render a mixer without a device. */
void a5_audio_test_render(ALLEGRO_MIXER *mixer, Sint16 *out, int frames)
{
   mixer_render(mixer, out, frames);
}
#endif
