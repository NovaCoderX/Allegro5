/* Keyboard: keyboard.h, from SDL key events.
 *
 * KEY_DOWN on the first press, KEY_UP on release, and KEY_CHAR on every
 * press and auto-repeat, which is what Dune Dynasty reads Enter and F10 to
 * F12 from.  unichar is the character the key types unshifted; there is no
 * SDL_TEXTINPUT handling, since the game never reads unichar.
 */

#include <SDL.h>
#include "allegro5/allegro.h"
#include "a5_internal.h"

static bool s_installed = false;
static A5_EVENT_SOURCE_ALIGN ALLEGRO_EVENT_SOURCE s_source;

/* SDL scancode to Allegro keycode, as in Allegro's SDL backend. */
#define KEY(sdl, allegro, unichar)  [SDL_SCANCODE_##sdl] = { ALLEGRO_KEY_##allegro, unichar }

static const struct {
   unsigned char keycode;
   char unichar;
} s_keys[SDL_NUM_SCANCODES] = {
   KEY(A, A, 'a'), KEY(B, B, 'b'), KEY(C, C, 'c'), KEY(D, D, 'd'),
   KEY(E, E, 'e'), KEY(F, F, 'f'), KEY(G, G, 'g'), KEY(H, H, 'h'),
   KEY(I, I, 'i'), KEY(J, J, 'j'), KEY(K, K, 'k'), KEY(L, L, 'l'),
   KEY(M, M, 'm'), KEY(N, N, 'n'), KEY(O, O, 'o'), KEY(P, P, 'p'),
   KEY(Q, Q, 'q'), KEY(R, R, 'r'), KEY(S, S, 's'), KEY(T, T, 't'),
   KEY(U, U, 'u'), KEY(V, V, 'v'), KEY(W, W, 'w'), KEY(X, X, 'x'),
   KEY(Y, Y, 'y'), KEY(Z, Z, 'z'),

   KEY(1, 1, '1'), KEY(2, 2, '2'), KEY(3, 3, '3'), KEY(4, 4, '4'),
   KEY(5, 5, '5'), KEY(6, 6, '6'), KEY(7, 7, '7'), KEY(8, 8, '8'),
   KEY(9, 9, '9'), KEY(0, 0, '0'),

   KEY(RETURN, ENTER, 13), KEY(ESCAPE, ESCAPE, 27),
   KEY(BACKSPACE, BACKSPACE, 8), KEY(TAB, TAB, 9), KEY(SPACE, SPACE, ' '),
   KEY(MINUS, MINUS, '-'), KEY(EQUALS, EQUALS, '='),
   KEY(LEFTBRACKET, OPENBRACE, '['), KEY(RIGHTBRACKET, CLOSEBRACE, ']'),
   KEY(BACKSLASH, BACKSLASH, '\\'), KEY(SEMICOLON, SEMICOLON, ';'),
   KEY(APOSTROPHE, QUOTE, '\''), KEY(GRAVE, TILDE, '`'),
   KEY(COMMA, COMMA, ','), KEY(PERIOD, FULLSTOP, '.'), KEY(SLASH, SLASH, '/'),

   KEY(F1, F1, 0), KEY(F2, F2, 0), KEY(F3, F3, 0), KEY(F4, F4, 0),
   KEY(F5, F5, 0), KEY(F6, F6, 0), KEY(F7, F7, 0), KEY(F8, F8, 0),
   KEY(F9, F9, 0), KEY(F10, F10, 0), KEY(F11, F11, 0), KEY(F12, F12, 0),

   KEY(PRINTSCREEN, PRINTSCREEN, 0), KEY(PAUSE, PAUSE, 0),
   KEY(INSERT, INSERT, 0), KEY(DELETE, DELETE, 0),
   KEY(HOME, HOME, 0), KEY(END, END, 0),
   KEY(PAGEUP, PGUP, 0), KEY(PAGEDOWN, PGDN, 0),
   KEY(LEFT, LEFT, 0), KEY(RIGHT, RIGHT, 0), KEY(UP, UP, 0), KEY(DOWN, DOWN, 0),

   KEY(KP_DIVIDE, PAD_SLASH, '/'), KEY(KP_MULTIPLY, PAD_ASTERISK, '*'),
   KEY(KP_MINUS, PAD_MINUS, '-'), KEY(KP_PLUS, PAD_PLUS, '+'),
   KEY(KP_ENTER, PAD_ENTER, 13), KEY(KP_PERIOD, PAD_DELETE, 0),
   KEY(KP_EQUALS, PAD_EQUALS, '='),
   KEY(KP_1, PAD_1, '1'), KEY(KP_2, PAD_2, '2'), KEY(KP_3, PAD_3, '3'),
   KEY(KP_4, PAD_4, '4'), KEY(KP_5, PAD_5, '5'), KEY(KP_6, PAD_6, '6'),
   KEY(KP_7, PAD_7, '7'), KEY(KP_8, PAD_8, '8'), KEY(KP_9, PAD_9, '9'),
   KEY(KP_0, PAD_0, '0'),

   KEY(LSHIFT, LSHIFT, 0), KEY(RSHIFT, RSHIFT, 0),
   KEY(LCTRL, LCTRL, 0), KEY(RCTRL, RCTRL, 0),
   KEY(LALT, ALT, 0), KEY(RALT, ALTGR, 0),
   KEY(LGUI, LWIN, 0), KEY(RGUI, RWIN, 0), KEY(MENU, MENU, 0),
   KEY(SCROLLLOCK, SCROLLLOCK, 0), KEY(NUMLOCKCLEAR, NUMLOCK, 0),
   KEY(CAPSLOCK, CAPSLOCK, 0),
};

#undef KEY

static unsigned int get_modifiers(Uint16 mod)
{
   unsigned int result = 0;

   if (mod & KMOD_SHIFT) result |= ALLEGRO_KEYMOD_SHIFT;
   if (mod & KMOD_CTRL)  result |= ALLEGRO_KEYMOD_CTRL;
   if (mod & KMOD_ALT)   result |= ALLEGRO_KEYMOD_ALT;
   if (mod & KMOD_LGUI)  result |= ALLEGRO_KEYMOD_LWIN;
   if (mod & KMOD_RGUI)  result |= ALLEGRO_KEYMOD_RWIN;
   if (mod & KMOD_NUM)   result |= ALLEGRO_KEYMOD_NUMLOCK;
   if (mod & KMOD_CAPS)  result |= ALLEGRO_KEYMOD_CAPSLOCK;
   if (mod & KMOD_MODE)  result |= ALLEGRO_KEYMOD_ALTGR;

   return result;
}

void a5_keyboard_handle_event(const SDL_Event *e)
{
   const SDL_Scancode scancode = e->key.keysym.scancode;
   ALLEGRO_EVENT event;

   if (!s_installed || scancode >= SDL_NUM_SCANCODES)
      return;

   event.keyboard.timestamp = al_get_time();
   event.keyboard.display = a5_current_display();
   event.keyboard.keycode = s_keys[scancode].keycode;
   event.keyboard.unichar = s_keys[scancode].unichar;
   event.keyboard.modifiers = get_modifiers(e->key.keysym.mod);
   event.keyboard.repeat = false;

   a5_lock();

   if (e->type == SDL_KEYUP) {
      event.keyboard.type = ALLEGRO_EVENT_KEY_UP;
      a5_emit_event_locked(&s_source, &event);
   }
   else {
      if (!e->key.repeat) {
         event.keyboard.type = ALLEGRO_EVENT_KEY_DOWN;
         a5_emit_event_locked(&s_source, &event);
      }

      event.keyboard.type = ALLEGRO_EVENT_KEY_CHAR;
      event.keyboard.repeat = (e->key.repeat != 0);
      a5_emit_event_locked(&s_source, &event);
   }

   a5_unlock();
}

bool al_install_keyboard(void)
{
   if (s_installed)
      return true;

   if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0)
      return false;

   a5_event_source_init(&s_source);
   s_installed = true;
   return true;
}

ALLEGRO_EVENT_SOURCE *al_get_keyboard_event_source(void)
{
   return s_installed ? &s_source : NULL;
}
