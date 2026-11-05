# Allegro 5 wrapper for Amiga 68k

A static library, `liballegro5.a`, that provides the part of the Allegro 5
API the Amiga build of Dune Dynasty 1.5.7 links against: 150 functions, plus
`al_uninstall_system`. They are being implemented on SDL2 (SDL2-NovaMesa) for
AmigaOS 3.x with RTG one module at a time; the rest are still stubs, so the
game always compiles and links.

It was developed inside the Dune Dynasty port
([NovaCoderX/DuneDynasty](https://github.com/NovaCoderX/DuneDynasty)),
which expects this repository checked out beside it as `Allegro5`: its
`amiga_build` takes `include/` and `lib/` from `../Allegro5`, and its host
tests of this library (`test/wrapper_audio`, `test/wrapper_threads`)
compile `src/` from there (their `A5` variable).

## Layout

```
include/allegro5/          public headers, Allegro 5.2.11.3, unmodified
  allegro_audio.h, allegro_image.h, allegro_memfile.h, allegro_primitives.h
                           the four addon headers Dune Dynasty includes
  internal/, inline/, platform/
                           only the internal headers the public ones include
  platform/alplatf.h       hand-written Amiga configuration (see below)
src/                       one file per Allegro module
  a5_internal.h            event sources, the shared lock, timer internals
  a5_stub.h                A5_STUB() for functions not implemented yet
makefile                   builds lib/liballegro5.a
```

The headers are from the same release as the `internal/` and `platform/`
headers already in the Allegro5 repository: those files are byte-identical
to Allegro 5.2.11.3. Left out because nothing on the Amiga uses them:
`allegro_opengl.h`, `allegro_direct3d.h`, and the Windows, X11, macOS, iOS
and Android headers.

## Building

```
make                           # lib/liballegro5.a
make install PREFIX=/opt/amiga # copies include/allegro5 and the library
```

Compiler flags match the Dune Dynasty build: `-O2 -m68060 -noixemul`
(libnix), GCC 13.3 from AmigaPorts/m68k-amigaos-gcc.

The sources include `<SDL.h>` (and `bitmap.c` `<SDL_image.h>`), so the compiler needs SDL2's header
directory: the makefile uses `SDL2_CFLAGS`, `-I/opt/amiga/include/SDL2` by
default. Anything linking the library also needs `-lSDL2`, after
`-lallegro5`.

## Implemented on SDL2

### System and time (`system.c`)

`al_install_system()` calls `SDL_Init(0)`, which starts SDL's clock
(timer.device and the EClock on AmigaOS). It deliberately does not use
`SDL_INIT_TIMER`, which would start SDL's timer thread, a separate Amiga
process the wrapper does not need. It registers `al_uninstall_system()`
with the `atexit` function it is given, as Allegro does, so `SDL_Quit()`
also runs when the game calls `exit()`.

`al_get_time()` is seconds since `al_install_system()`, from SDL's
performance counter (the EClock). `al_set_app_name()` does nothing:
Allegro only uses the name for standard paths and default window titles,
and the Amiga build needs neither.

### Event queues (`events.c`) and timers (`timer.c`)

Queues are growable ring buffers. One SDL mutex guards every queue, event
source and timer, since the game's main thread and its music thread both
use them; `al_wait_for_event()` sleeps on a per-queue SDL condition
variable.

Timers have no thread. A timer's ticks are worked out from `al_get_time()`
whenever something looks at it: a queue it feeds being checked or waited
on, or its count being read. Ticks that fell due in the meantime are
generated together, oldest first across all of a queue's timers, the same
way Allegro's timer thread catches up when it runs late. Counts and events
come out as in Allegro, without a task waking up for every tick: a thread
waiting on a queue sleeps until the next tick with a timed condition wait,
which SDL2-NovaMesa implements with timer.device (UNIT_MICROHZ below
100 ms).

Behaviour matches Allegro 5.2 where Dune Dynasty depends on it:

- `al_start_timer()` keeps the count and schedules the first tick one
  period ahead; `al_stop_timer()` freezes the count.
- `al_set_timer_speed()` lengthens or shortens the tick in progress by the
  change in speed.
- One timer can feed several queues (the GUI timer feeds the input queue
  and, during `Timer_Sleep()`, the timer queue).
- `al_unregister_event_source()` and `al_destroy_timer()` drop the
  source's events that are still queued.
- `al_wait_for_event(queue, NULL)` waits without removing the event.

### Keyboard (`keyboard.c`) and mouse (`mouse.c`)

SDL's input has to be pumped by the task that owns the window, so, as in
Allegro's own SDL backend, checking, reading, flushing or waiting on a
queue pumps SDL's events first, but only on the thread that called
`al_install_system()`; the music thread's queue never touches SDL. A wait
on the main thread wakes at least every 10 ms to pump again, which is how
input reaches a queue with no timer running. `SDL_PumpEvents()` runs once
per call, followed by `SDL_PeepEvents()`, since `SDL_PollEvent()` would
pump again for every event.

- Keys map from SDL scancodes as in Allegro's SDL backend: left Alt is
  `ALLEGRO_KEY_ALT`, right Alt `ALLEGRO_KEY_ALTGR`, the Amiga keys
  `ALLEGRO_KEY_LWIN`/`RWIN`; keys Allegro has no code for (such as Help)
  come through as keycode 0.
- `KEY_DOWN` on the first press, `KEY_UP` on release, and `KEY_CHAR` on
  every press and auto-repeat (with `repeat` set), which is where Dune
  Dynasty reads Enter, Alt+Enter and F10 to F12 from. `unichar` is the
  character the key types unshifted; `SDL_TEXTINPUT` is not used, since
  the game never reads `unichar`.
- Mouse buttons are 1 left, 2 right, 3 middle. Motion and the wheel are
  `ALLEGRO_EVENT_MOUSE_AXES` with `x`/`y`, `dx`/`dy`, and `z`/`dz` for the
  wheel.
- `display` in keyboard and mouse events is the display.

Input only arrives once there is a window: SDL2's video driver reads it
from the window's IDCMP port. Warping and cursors: see below.

### File system and paths (`fshook.c`, `path.c`)

Directory listing and `al_make_directory()`, for the saves list, saving
games and the scan for extra campaigns in `campaign/`. On AmigaOS they
call dos.library directly (`Lock()`, `Examine()`, `ExNext()`,
`CreateDir()`), so paths reach AmigaDOS as written; elsewhere they use
POSIX, for testing on the host. An entry's name is its full path, a
directory's entries joining its path and the file name, as in Allegro.
Dune Dynasty builds directory paths ending in `/` (`.../save/`), which is
dropped before AmigaDOS sees it. `al_make_directory()` also makes any
missing parents, as Allegro's does.

Paths are kept as a directory part, ending in `/` or `:` for a volume,
and a file name, so `Work:Games/DuneDynasty/save/_SAVE001.DAT` goes
through unchanged. Only the functions the game uses are there.

Tested with the game's own save-list and campaign-scan code, on the host
and on m68k under vamos (dos.library).

### Configuration files (`config.c`)

Plain C on stdio, reading and writing Allegro's format: `[section]`
headers, `key=value` lines (trimmed), and a global section before the
first header. Comments and blank lines are kept where they are, so the
game saving `dunedynasty.cfg` changes only the values it sets. A key the
program adds goes after the last key of its section, and a new section
goes at the end after a blank line. Line endings are read either way and
written as LF.

Two Amiga extensions (`allegro_amiga.h`) let the game write a section out
again in a set order, comments included: `a5_clear_config_section()`
removes a section's lines but for its trailing blank lines, keeping its
place in the file, and `a5_append_config_line()` adds a key, or a comment,
after the section's last line that is not blank.

### Display, bitmaps and drawing

`display.c`, `bitmap.c`, `bitmap_draw.c`, `primitives.c` and
`transformations.c` are a software renderer, not SDL_Renderer: Dune
Dynasty draws into its atlas textures and later locks them to read the
pixels back, which SDL's textures cannot do.

- **The display** is one SDL window, or with `ALLEGRO_FULLSCREEN` or
  `ALLEGRO_FULLSCREEN_WINDOW` an `SDL_WINDOW_FULLSCREEN` one, never
  `SDL_WINDOW_FULLSCREEN_DESKTOP` (SDL2-NovaMesa would then open the
  screen at the desktop size and scale, which is slow). `ALLEGRO_OPENGL`
  is ignored. The backbuffer is a bitmap over the window surface, which
  SDL2-NovaMesa keeps as ARGB8888 in fast RAM; `al_flip_display()` is
  `SDL_UpdateWindowSurface()`, which copies it to the RTG screen. Window
  close, expose and focus become display events. Windowed and fullscreen
  cannot be swapped while running, so `al_set_display_flag()` fails.
- **Bitmaps** are 32-bit ARGB in host byte order, memory and video
  bitmaps alike. Sub-bitmaps share their root's pixels and may extend past
  it (drawing is clipped to the root). Locks in ARGB_8888 point at the
  pixels; locks in ABGR_8888_LE (bytes R, G, B, A), which the game uses,
  are converted on lock and back on unlock.
- **Transforms and clipping rectangles** belong to the target bitmap, as
  in Allegro: the game keeps one sub-bitmap of the backbuffer per screen
  area, each with its own scale.
- **Drawing** follows Allegro's own software renderer, the one it uses
  for memory bitmaps. A bitmap draw covers the target pixels whose centres
  are inside the transformed region and takes the source pixel under each
  centre (nearest neighbour). With the copy blender, no tint and a
  translation-only transform it copies to the position truncated to whole
  pixels, as Allegro does. Primitives are built from the same triangles as
  Allegro's (and its one-pixel line stepper), so they cover the same
  pixels. All blenders work; copy, alpha, premultiplied alpha and additive
  have their own code. Blending is in integers, rounded; Allegro truncates
  its float result, so colours can be 1 higher than Allegro's (2 where two
  blended draws overlap).
- **Mouse cursors**: SDL2-NovaMesa ignores cursor images, so
  `al_create_mouse_cursor()` returns NULL, `al_show_mouse_cursor()` and
  `al_hide_mouse_cursor()` show or hide the system pointer, and the game
  always draws its own cursor on Amiga. `al_set_mouse_xy()` warps with
  `SDL_WarpMouseInWindow()`, as Allegro's SDL backend does.
- **Image files** go through the owner's 68k SDL2_image (`libSDL2-Image.a`,
  which reads PNG with its own stb_image and writes it with miniz, so it
  needs no libpng or zlib). `al_load_bitmap()` is `IMG_Load()`, converted
  to ARGB8888 into a bitmap of the new bitmap flags, its alpha premultiplied
  as Allegro does unless `ALLEGRO_NO_PREMULTIPLIED_ALPHA` is set;
  `al_save_bitmap()` is `IMG_SavePNG()` (PNG whatever the extension), and a
  bitmap on the display's backbuffer is written without alpha. Dune Dynasty
  loads `gfx/rubblemask.png` (transparent rubble) and saves screenshots
  (F9, or F12) of the backbuffer, at 320x240 on Amiga.
- **Sandworm and sonic-wave ripple**: Dune Dynasty draws it only with an
  OpenGL or Direct3D stencil, rendering the scene again shifted under the
  brush. `a5_draw_displaced()` (an Amiga extension, `allegro_amiga.h`)
  does it in software, as Dune II did: where the brush's alpha is at least
  half, each target pixel takes the pixel a few to its right, read before
  it is changed. Without it sandworms and sonic blasts were invisible.

**Display scale.** `a5_set_new_display_scale()` (an Amiga extension, in
`allegro5/allegro_amiga.h`) makes the next display's window or screen 2 to
4 times the size given to `al_create_display()`. The backbuffer stays at
the given size and each flip scales it up into the window surface, each
pixel written that many times across and each row once, then copied for
the rows below; mouse positions and movements are divided back down (the
remainder of a movement is kept for the next). Dune Dynasty at 320x240
with a scale of 2 looks like 640x480 with 2x screen areas and draws a
quarter of the pixels, as OpenDUNE does with its scalers.

At a scale of 2, `a5_set_new_display_filter()` can make the flip use
Scale2x instead (`scale2x.c`): `A5_FILTER_SCALE2X`, or
`A5_FILTER_SCALE2X_ADV`, which leaves a pixel much brighter than its four
neighbours (by 3 R + 6 G + B, as OpenDUNE's Amiga scaler) as a plain 2x2
block so thin text keeps its strokes. Pixels are compared whole, so
blended ones fall back to nearest. Integers only, with the flat case as the
straight path through the loop, since Emu68 treats a forward branch as not
taken.

**SDL2-NovaMesa details**, as in the owner's V-WARS:

- After opening the display the wrapper checks with
  `SDL_GetWindowWMInfo()` that there is an Intuition window: SDL2-NovaMesa
  can come back from going fullscreen without one when P96 has no mode of
  the size, and the program would seem to quit silently. The size and
  format the display got go to the trace.
- Copies of whole rows (an opaque 1x row, a row repeated from the one
  above, the scaled rows) use `CopyMemQuick()`, exec's fastest copy, and
  clearing the whole backbuffer copies a frame of the colour made ready
  beforehand.
- Alpha blending works out red and blue in one multiply and alpha and
  green in another (each channel's product fits in its own 16 bits),
  rounding each exactly as before.

**No floating point.** Built with `-m68060` and no `-mhard-float`, every
float operation is a library call, so drawing is done in integers and
16.16 fixed point: floats arriving through the API (positions, sizes,
colours, the transform when it is set) are converted with integer
operations on their bits, the scales Dune Dynasty uses (1 and 2) are
shifts, rotation takes sine and cosine from a table, thick diagonal lines
use an integer square root, and `al_map_rgba()` reads a table the
compiler fills. Only `al_build_transform()` (when the screen areas are
set up) and one-pixel lines (drawn once, at load time) still use floats.
The output is byte-identical to the float version on the comparison
scenes.

**Speed.** Nearly every draw goes through one scaled blit. Per draw it
works out the source column under each target column once, grouped into
runs, so a 2x draw reads each source pixel once and writes it twice; rows
step through the source in fixed point. A row that samples the same
source row as the row above is copied from it when the result cannot
depend on what was underneath (copy blender, or alpha blender over a
fully opaque row: the second row of every 2x tile). Untinted alpha draws
copy opaque pixels and skip transparent ones without blending maths;
tinted ones (text, shadows) skip transparent pixels, reuse the tint of a
repeated source pixel and store fully opaque results directly. Clears and
solid spans fill eight pixels per loop pass, and an alpha-blended solid
span (the shade under the game's dialogs) works out the colour's share
once. The column tables are static, not on the stack. On a benchmark of
a typical in-game frame at 640x480 (about 210 tiles, 60 units with
shadows and health bars, the sidebar and menubar, 80 characters of text
and the minimap, all at 2x) this cut the time per frame from 1.27 ms to
0.55 ms on the host and the 68k instructions per frame (timed under
vamos) by about 2.4 times, with identical output.

Tested against upstream Allegro 5.2.12 built on Linux, drawing 19 scenes
into memory bitmaps with both: every blender, fractional positions,
regions partly outside the source, sub-bitmap sources and targets, flips,
scaling, tints, target transforms and clipping, rotation, Dune Dynasty's
rectangles and lines, hairlines, triangle lists, clears and locks. Every
scene covers exactly the same pixels, with colours within the rounding
difference above. The same scenes give byte-identical output on m68k
(under vamos) and on the host.

### Audio (`audio.c`)

One voice is one SDL audio device (AHI on AmigaOS): `al_create_voice()`
opens it at the rate and channels asked for, 16-bit, about 46 ms per
callback at 22050 Hz (1024 frames), and `al_attach_mixer_to_voice()` starts
it. The callback, on SDL's audio task, pulls the mixer, which mixes its
sample instances and streams in 32-bit integers whatever depth it was
created with: there is no floating point per sample, since every float
operation is a library call on the 68060 build. Dune Dynasty opens 22050 Hz
stereo on Amiga (`audio_a5.c`).

What follows Allegro's mixer (`addons/audio/kcm_*.c`):

- The channel matrix and pan law of `_al_rechannel_matrix()`: a mono
  source goes to both sides at 1/sqrt(2); a pan other than
  `ALLEGRO_AUDIO_PAN_NONE` scales left by sqrt((1 - pan) / 2) and right by
  sqrt((1 + pan) / 2); then the gain. Worked out in float when the gain,
  pan or mixer changes, and kept as 2.14 fixed point (at most 2.0; the
  game's gains never pass 1.0), each product rounded.
- Samples play once and are resampled with linear interpolation (Allegro's
  default quality), rounded; 8-bit mono, Dune II's `.VOC` voices and
  effects, has its own loop. `al_set_sample()` stops the instance and
  rewinds it.
- Streams: `al_get_audio_stream_fragment()` hands out a free fragment and
  `al_set_audio_stream_fragment()` queues it. Each time the mixer finishes
  a fragment, or finds none queued, and when the stream is attached, the
  stream emits one `ALLEGRO_EVENT_AUDIO_STREAM_FRAGMENT` per free fragment,
  as Allegro does, less the stream's events still waiting in the queue, so
  a stream left unfilled while the game is busy (loading, saving) does not
  add a fragment's worth of events on every callback and grow the queue
  from the audio task; Dune Dynasty renders its AdLib music on those
  events, one fragment per event.
  A stream at the mixer's rate is copied (16-bit mono, the AdLib music,
  has its own loop); at another rate it takes the nearest frame. An empty
  stream plays silence and keeps playing.
- The sum is clipped to 16 bits.

Not implemented: `al_load_audio_stream()` returns NULL (no codecs; FLAC
and Ogg Vorbis are too heavy for a 68k), loop modes, speed, and mixers on
mixers. The game's state changes under `SDL_LockAudioDevice()`; the
callback takes the events lock only to emit, and the main task never holds
the events lock while it locks the device.

Tested on the host (Dune Dynasty's `test/wrapper_audio`): `mixer_test` renders mixers
directly and checks 1.85 million output values against a float model of
Allegro's mixer (6 sample rates, gains up to 1.4, 6 pans, mono and stereo
streams, fragment cycling and events, clipping): all within 2, clean under
AddressSanitizer and UBSan. `stream_test` plays Dune II's AdLib music
through a stream on SDL's disk driver the way the game does; the device's
output is the player's PCM times the gain to within 1, with no fragment
lost or repeated. `starve_test` leaves a stream unfilled for 2 s, as
during a load: the queue holds at most one event per fragment (4), where
it used to gain 4 on every callback (172 in the 2 s).

### Threads (`threads.c`)

SDL threads, which are AmigaOS processes: as in Allegro, a thread made by
`al_create_thread()` does not run until `al_start_thread()` (the SDL thread
is created then, with a 64 KB stack: Amiga processes default to a few KB,
and Dune Dynasty's MIDI thread keeps its music buffer on the stack).
`al_join_thread()` sets the thread's should-stop flag and waits for it;
`al_destroy_thread()` joins first. The flag is an SDL atomic.

Tested on the host (Dune Dynasty's `test/wrapper_threads`) with the game's own MIDI
player: `mt32mpu.c` on a thread with a 120 Hz Allegro timer, as
`audio_a5.c` runs it, against the same tracks played without threads. The
messages are the same, in the same order, each within a few ms of its
tick, and the thread stops when the music ends or when it is destroyed.

## How the stubs behave

Each stub does nothing and returns a failure value: `false`, `NULL`, `0` or
`-1`, or an empty string for path queries. The stubs left are `file.c`
and the memfile addon; nothing on the game's path dereferences a stub's
NULL.

`ALLEGRO_STUB_TRACE` (on by default in the makefile) reports the first
call to each stub through Allegro's trace output (`debug.c`): to the
handler the program registered with `al_register_trace_handler()`, or to
stderr if there is none. Dune Dynasty registers one that sends the trace
to its log:

```
allegro5 stub: al_set_new_display_flags()
allegro5 stub: al_set_new_display_option()
allegro5 stub: al_create_display()
```

As modules get implemented, this trace shows which call is next on the path.
Remove `-DALLEGRO_STUB_TRACE` from `CPPFLAGS` to silence it.

## Function list by module

| File | Header(s) | Functions | Status |
| --- | --- | --- | --- |
| `system.c` | system.h, altime.h, memory.h | 6 | SDL2 |
| `events.c` | events.h | 8 | SDL2 |
| `timer.c` | timer.h | 7 | SDL2 |
| `display.c` | display.h, drawing.h, blender.h | 18 | SDL2 |
| `bitmap.c` | bitmap.h, bitmap_lock.h, bitmap_io.h | 15 | plain C, SDL2_image |
| `bitmap_draw.c` | bitmap_draw.h | 8 | plain C |
| `color.c` | color.h | 2 | plain C |
| `transformations.c` | transformations.h | 3 | plain C |
| `keyboard.c` | keyboard.h | 2 | SDL2 |
| `mouse.c` | mouse.h, mouse_cursor.h | 8 | SDL2 |
| `threads.c` | threads.h | 6 | SDL2 |
| `path.c` | path.h | 9 | plain C |
| `config.c` | config.h | 7 | plain C |
| `file.c` | file.h | 9 | stubs |
| `fshook.c` | fshook.h | 8 | dos.library |
| `primitives.c` | allegro_primitives.h | 5 | plain C |
| `image.c` | allegro_image.h | 1 | plain C |
| `memfile.c` | allegro_memfile.h | 1 | stubs |
| `audio.c` | allegro_audio.h | 29 | SDL2 |
| `debug.c` | debug.h | 1 | plain C |

`al_is_sub_bitmap` is only called inside `assert()`, so a release build
(`-DNDEBUG`) does not need it; it is stubbed anyway so debug builds link.
`al_get_standard_path` is not needed at all: on the Amiga the game takes
its directory from `g_amiga_path` (see `src/os/amiga_main.c`).

If Dune Dynasty starts calling a new Allegro function, the link fails with
`undefined reference to 'al_...'`. Add the function to the matching
`src/` file, copying the prototype from the header.

## platform/alplatf.h

Upstream generates this from `alplatf.h.cmake`. The Amiga version is
written by hand and differs from the earlier draft (a Linux CMake output)
as follows:

- `ALLEGRO_CFG_OPENGL`, `_OPENGLES`, `_OPENGLES2`,
  `_OPENGL_PROGRAMMABLE_PIPELINE` and `ALLEGRO_CFG_SHADER_GLSL` are off,
  because SDL2-NovaMesa is built without `SDL_VIDEO_OPENGL`. Dune Dynasty's
  `video_a5.c` tests `ALLEGRO_CFG_OPENGL` to decide whether to build its GL
  stencil effects.
- `ALLEGRO_CFG_PTHREADS_TLS` is off: the wrapper does not use Allegro's
  thread-local state code (`tls.c`).
- `ALLEGRO_HAVE_*` entries were checked against the libnix headers and
  libraries in the GCC 13.3 toolchain. Off: `OSAtomic.h` (macOS only),
  `mmap` and `mprotect` (no `sys/mman.h`), and `sched_yield`, which is
  declared but has no implementation. libnix does provide `mkstemp`,
  `fseeko`/`ftello`, `strerror_r`, `sysconf` and `uname`, so those stay on.
  These settings only matter when compiling Allegro's own sources.
- Unchanged: `ALLEGRO_SDL` and `ALLEGRO_BIG_ENDIAN`.

## Next steps

`file.c` and the memfile addon are plain C and could be compiled from the
5.2.11.3 sources (`file.c`, `file_stdio.c`, `memfile.c`) instead of
rewritten; they need the `internal/` headers, which are already in the
Allegro5 repository. The game only needs them for Dune 2000 `.AUD` music
(`al_fopen()`); the AdLib player no longer goes through a memfile.

Allegro 5 is distributed under the zlib licence; see `LICENSE.txt`.
