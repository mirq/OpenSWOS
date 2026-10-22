/*
 * input_wos.c -- the WarpOS build's controls (docs/WARPOS.md, "Controls").
 *
 * The game reads the Amiga's own input registers -- keyState[] and the keyBuffer
 * FIFO in raw Amiga scancodes, JOY0DAT/JOY1DAT, the fire bits of CIAA port A --
 * and on the SDL build input_sdl.c fills them from SDL's events. Here AmigaOS
 * hands over raw Amiga scancodes as they are (IDCMP_RAWKEY, from the window
 * platform_wos.c opens), and lowlevel.library reads the two joystick ports.
 *
 * Who plays: as on the Amiga, player 1 is the joystick port (port 1) and
 * player 2 the mouse port (port 0); the keyboard is player 1 too. CD32 pads:
 * red fires, blue fires and skips (as B does on a pad under SDL), PLAY pauses.
 *
 * The keyboard's rules are input_sdl.c's, which says why each one exists: the
 * arrow pressed last wins over its opposite and a released one hands back to
 * an opposite still held; fire is any of CTRL / RETURN / ENTER, held while any
 * of them is; SPACE skips outside a match. Not here yet: the player's own key
 * bindings, the press latch, the stick deadzone (a stick here is digital), and
 * the CONTROLS menu -- SDL-only until input_sdl.c's game-facing half is shared.
 *
 * The SDL build compiles input_sdl.c, so this one is empty there.
 */

#if defined(SWOS_CHUNKY_BUILD) && !defined(SWOS_SDL_BUILD)

#include <stdio.h>
#include <string.h>
#include "swos_types.h"

#pragma pack(push, 2)
#include <exec/types.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/lowlevel.h>
#include <powerpc/powerpc.h>
#include <powerpc/powerpc_protos.h>
#pragma pack(pop)

struct Library *LowLevelBase;

/* ---- Game-side globals (swos_globals.c, input_sdl.c on SDL) ---- */
extern uint8_t  _keyState[128];
extern uint16_t _joy0Dat;
extern uint16_t _joy1Dat;
extern uint8_t  _ciaa_pra;
extern uint16_t _gameState;
extern uint16_t _seasonUpdateInProgress;
extern volatile uint8_t  _keyBuffer[12];
extern volatile uint16_t _keyCount;

/* Defined by input_sdl.c on the SDL build, which this file stands in for. */
volatile uint16_t _pauseSkipRequested;

/* ---- Amiga raw scancodes ---- */
#define AKEY_SPACE    0x40
#define AKEY_ENTER    0x44
#define AKEY_KP_ENTER 0x43
#define AKEY_UP       0x4C
#define AKEY_DOWN     0x4D
#define AKEY_RIGHT    0x4E
#define AKEY_LEFT     0x4F
#define AKEY_P        0x19
#define AKEY_LSHIFT   0x60
#define AKEY_CTRL     0x63
#define AKEY_LALT     0x64
#define AKEY_RALT     0x65
#define AKEY_LAMIGA   0x66
#define AKEY_RAMIGA   0x67
#define AKEY_UP_PREFIX 0x80     /* IECODE_UP_PREFIX: the key was released */

/* ---- The keyboard as player 1's stick and fire button ---- */
static int s_kbd_up, s_kbd_down, s_kbd_left, s_kbd_right, s_kbd_skip;
static int s_kbd_held_up, s_kbd_held_down, s_kbd_held_left, s_kbd_held_right;
#define KBD_FIRE_CTRL     0x1
#define KBD_FIRE_RETURN   0x2
#define KBD_FIRE_KP_ENTER 0x4
static int s_kbd_fire_keys;
static int s_pad_play_prev[2];

static void kbd_arrow(int akey, int down)
{
    switch (akey) {
    case AKEY_UP:    s_kbd_held_up    = down; break;
    case AKEY_DOWN:  s_kbd_held_down  = down; break;
    case AKEY_LEFT:  s_kbd_held_left  = down; break;
    case AKEY_RIGHT: s_kbd_held_right = down; break;
    default: return;
    }
    if (down) {
        /* The press wins over its opposite. */
        if (akey == AKEY_UP)    { s_kbd_up = 1;    s_kbd_down  = 0; }
        if (akey == AKEY_DOWN)  { s_kbd_down = 1;  s_kbd_up    = 0; }
        if (akey == AKEY_LEFT)  { s_kbd_left = 1;  s_kbd_right = 0; }
        if (akey == AKEY_RIGHT) { s_kbd_right = 1; s_kbd_left  = 0; }
    } else {
        /* The release goes, and an opposite still held takes over. */
        if (akey == AKEY_UP)    { s_kbd_up = 0;    s_kbd_down  = s_kbd_held_down; }
        if (akey == AKEY_DOWN)  { s_kbd_down = 0;  s_kbd_up    = s_kbd_held_up; }
        if (akey == AKEY_LEFT)  { s_kbd_left = 0;  s_kbd_right = s_kbd_held_right; }
        if (akey == AKEY_RIGHT) { s_kbd_right = 0; s_kbd_left  = s_kbd_held_left; }
    }
}

static void push_key(uint8_t akey)
{
    if (_keyCount < (sizeof(_keyBuffer) - 3)) {
        _keyBuffer[_keyCount] = akey;
        _keyCount++;
    }
}

static int fire_bit(int akey)
{
    switch (akey) {
    case AKEY_CTRL:     return KBD_FIRE_CTRL;
    case AKEY_ENTER:    return KBD_FIRE_RETURN;
    case AKEY_KP_ENTER: return KBD_FIRE_KP_ENTER;
    default:            return 0;
    }
}

/* One IDCMP_RAWKEY code, as platform_wos.c's window received it. `repeat`: the
 * Intuition key repeat (IEQUALIFIER_REPEAT), which the FIFO ignores, as input_sdl.c
 * ignores SDL's -- the Amiga ISR had its own repeat logic, not emulated. */
void InputWos_Key(uint16_t code, int repeat)
{
    int down = !(code & AKEY_UP_PREFIX);
    int akey = code & 0x7F;
    int arrow = akey >= AKEY_UP && akey <= AKEY_LEFT;

    if (akey == AKEY_LALT || akey == AKEY_RALT || akey == AKEY_LAMIGA || akey == AKEY_RAMIGA)
        return;                 /* modifiers only, as Alt is on the SDL build */

    if (down) {
        _keyState[akey] = 1;
        /* Outside a match the arrows go into the FIFO too, on the press AND on each
         * of the keyboard's own repeats: the Amiga's keyboard interrupt put every
         * key there, and MenuCheckControls takes an arrow from it whenever the
         * stick logic (12 ticks, then one every 4) moved nothing that frame -- so
         * holding an arrow runs the cursor at the keyboard's repeat rate, as on the
         * original. In a match an arrow steers and nothing else (input_sdl.c keeps
         * them out of the FIFO altogether). */
        extern volatile uint16_t _playGame;       /* set while a match is played */
        int in_match = _playGame != 0;
        if (arrow ? !in_match : !repeat)
            push_key((uint8_t)akey);
        if (arrow && !repeat)
            kbd_arrow(akey, 1);
        s_kbd_fire_keys |= fire_bit(akey);
        /* SPACE: skip outside a match, where it is the highlight key (input_sdl.c). */
        if (akey == AKEY_SPACE &&
            (_seasonUpdateInProgress != 0 || !(_gameState >= 21 && _gameState <= 31)))
            s_kbd_skip = 1;
    } else {
        _keyState[akey] = 0;
        if (arrow)
            kbd_arrow(akey, 0);
        s_kbd_fire_keys &= ~fire_bit(akey);
        if (akey == AKEY_SPACE)
            s_kbd_skip = 0;
    }
}

void InputWos_ClearKeyState(void)
{
    memset(_keyState, 0, sizeof(_keyState));
    memset((void *)_keyBuffer, 0, sizeof(_keyBuffer));
    _keyCount = 0;
    s_kbd_up = s_kbd_down = s_kbd_left = s_kbd_right = 0;
    s_kbd_held_up = s_kbd_held_down = s_kbd_held_left = s_kbd_held_right = 0;
    s_kbd_fire_keys = 0;
    s_kbd_skip = 0;
    _pauseSkipRequested = 0;
}

static uint16_t encode_joydat(int up, int down, int left, int right)
{
    uint16_t v = 0;
    if (right)                  v |= (1u << 1);
    if (right ^ (down ? 1 : 0)) v |= (1u << 0);
    if (left)                   v |= (1u << 9);
    if (left  ^ (up   ? 1 : 0)) v |= (1u << 8);
    return v;
}

/* lowlevel.library's port `port`, OR'd into the directions and buttons given.
 * A mouse, or nothing, in the port contributes nothing. */
/* Each ReadJoyPort is a trip to the 68k side: a port is read every frame only
 * while a joystick or pad answers in it, and an empty one looked at again once a
 * second -- but not during a match, where asking an empty port cost a 7-14 ms
 * frame once a second (a stutter, 2026-10-22). A stick plugged in mid-match
 * answers from the next menu on. */
static int s_port_live[2];
static int s_port_probe[2];

static void read_port(ULONG port, int slot, int *up, int *down, int *left, int *right,
                      int *fire, int *skip)
{
    ULONG s, type;
    if (!LowLevelBase)
        return;
    if (!s_port_live[slot]) {
        extern volatile uint16_t _playGame;
        if (_playGame != 0 || s_port_probe[slot]-- > 0)
            return;
    }
    s_port_probe[slot] = 50;
    {
        struct timeval t0, t1;
        int32_t us;
        GetSysTimePPC(&t0);
        s = ReadJoyPort(port);
        GetSysTimePPC(&t1);
        us = (int32_t)(t1.tv_secs - t0.tv_secs) * 1000000 + (int32_t)t1.tv_micro - (int32_t)t0.tv_micro;
        if (us > 3000)
            fprintf(stderr, "[joy] ReadJoyPort(%lu) took %ld us (type $%08lx)\n",
                    (unsigned long)port, (long)us, (unsigned long)(s & JP_TYPE_MASK));
    }
    type = s & JP_TYPE_MASK;
    s_port_live[slot] = (type == JP_TYPE_JOYSTK || type == JP_TYPE_GAMECTLR);
    if (!s_port_live[slot])
        return;
    if (s & JPF_JOY_UP)    *up = 1;
    if (s & JPF_JOY_DOWN)  *down = 1;
    if (s & JPF_JOY_LEFT)  *left = 1;
    if (s & JPF_JOY_RIGHT) *right = 1;
    if (s & JPF_BUTTON_RED)  *fire = 1;
    if (s & JPF_BUTTON_BLUE) { *fire = 1; *skip = 1; }
    if (type == JP_TYPE_GAMECTLR) {
        int play = (s & JPF_BUTTON_PLAY) != 0;
        if (play && !s_pad_play_prev[slot])
            push_key(AKEY_P);           /* PLAY pauses, as START does on a pad */
        s_pad_play_prev[slot] = play;
    }
}

/* Once a frame, after the window's messages: the registers the game reads. */
void InputWos_PostFrame(void)
{
    /* The replay rig writes them itself, once per game loop iteration. */
    { extern int ReplayTest_Running(void); if (ReplayTest_Running()) return; }

    int p1_up = s_kbd_up, p1_down = s_kbd_down, p1_left = s_kbd_left, p1_right = s_kbd_right;
    int p1_fire = s_kbd_fire_keys != 0;
    int p2_up = 0, p2_down = 0, p2_left = 0, p2_right = 0, p2_fire = 0;
    int skip = s_kbd_skip;

    read_port(1, 0, &p1_up, &p1_down, &p1_left, &p1_right, &p1_fire, &skip);
    read_port(0, 1, &p2_up, &p2_down, &p2_left, &p2_right, &p2_fire, &skip);

    _pauseSkipRequested = (uint16_t)(skip ? 1 : 0);

    /* SWOS reads P1 from JOY1DAT (port 1) and P2 from JOY0DAT (port 0). */
    _joy1Dat = encode_joydat(p1_up, p1_down, p1_left, p1_right);
    _joy0Dat = encode_joydat(p2_up, p2_down, p2_left, p2_right);

    /* CIAA port A fire bits, active low: bit 7 player 1, bit 6 player 2. */
    if (p1_fire) _ciaa_pra &= (uint8_t)~(1u << 7);
    else         _ciaa_pra |= (uint8_t)(1u << 7);
    if (p2_fire) _ciaa_pra &= (uint8_t)~(1u << 6);
    else         _ciaa_pra |= (uint8_t)(1u << 6);
}

void InputWos_Init(void)
{
    _joy0Dat = 0;
    _joy1Dat = 0;
    _ciaa_pra = 0xFF;                   /* both fire bits set (idle) */
    InputWos_ClearKeyState();
    LowLevelBase = OpenLibrary((CONST_STRPTR)"lowlevel.library", 40);
    if (!LowLevelBase)
        fprintf(stderr, "[input] no lowlevel.library: the keyboard only, no joysticks\n");
}

void InputWos_Shutdown(void)
{
    if (LowLevelBase) {
        CloseLibrary(LowLevelBase);
        LowLevelBase = NULL;
    }
}

/* ---- What the shared code asks of input_sdl.c ---- */
void InputSDL_ClearKeyState(void)            { InputWos_ClearKeyState(); }
/* SDL claims the first pad pressed in the intro as player 1's; the ports here are fixed. */
void InputSDL_SetIntroClaim(int on)          { (void)on; }
/* Not here yet (see the top of the file). */
void InputSDL_SetStickDeadzone(int percent)  { (void)percent; }
void InputSDL_SetPressLatch(int on)          { (void)on; }
int  InputSDL_SetKeyBinding(int which, const char *name)
{
    (void)which;
    return (name && name[0]) ? 1 : 0;   /* 1: a name this build does not know */
}
int  InputSDL_SelfTest(void)
{
    fprintf(stderr, "--input-selftest: SDL's input only\n");
    return 1;
}
/* The CONTROLS menu assigns SDL scancodes. */
void ControlsMenu_Show(void)                 { }

#endif
