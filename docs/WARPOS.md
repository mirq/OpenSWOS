# OpenSWOS for WarpOS (PowerPC) on an RTG screen

The chunky target without SDL: the game the SDL build plays — 8bpp chunky renderer, hosted
libc, `SWOS.ini`, JSON saves, `.SRP` replays — on an Amiga with a WarpOS PowerPC card and a
Picasso96 (or CyberGraphX) graphics card. Offline play only.

**Status (2026-10-22): it builds and links; it has never run.** There is no WarpOS machine or
PPC emulator on the development box, so everything below "Running it" is untested. The first
thing to do on real hardware is in [First run](#first-run).

## Building

```bash
make -f Makefile.warpos -j$(nproc)      # -> bin/SWOS.warpelf
```

Needs the V19 MiniGL SDK's common makefile (`MGL_DIR`, default
`/home/mirek/MiniGL_WOS_V19_mglQ3`) for the toolchain paths and the ET_REL contract —
`ppc-amigaos-gcc` 8.4 (adtools) with the MOS2WOS WarpOS overlay. **No MiniGL is linked**: the
game draws 8bpp chunky, which is an 8-bit RTG screen's own pixel format, so it needs no 3D.

The product is a **relocatable ELF** that `Elf2Exe2` converts **on the Amiga** (it needs
ixemul.library V48; there is no Linux build of it):

```
Elf2Exe2 SWOS.warpelf SWOS
```

`build/warpos/ppc/SWOS.debug.elf` is the same link with its debug info, for `addr2line`
against a crash address. The shipped `.warpelf` has only the debug sections dropped — never
strip a hunk executable's symbols.

Flags that are not negotiable, each in `Makefile.warpos` with its reason:
`-mcpu=603e` (runs on every WarpOS board), `-fsigned-char` (char is UNSIGNED on PPC and signed
where the game was written), `-fno-common` (the ET_REL check refuses COMMON symbols),
`-msdata=none -G0` (no small-data relocations), `-fno-jump-tables` (Elf2Exe2 accepts only
`R_PPC_ADDR32 / ADDR16_LO / ADDR16_HA / REL24`), `-Uamiga -Uamigaos` (the compiler predefines
them and the game has variables called `amiga`). The SDK's SDL 1.2 headers are taken OFF the
include path, so an SDL call that should have been ported cannot quietly compile against them.

## Running it

- AmigaOS 3.x, WarpOS (`powerpc.library`), `cybergraphics.library` v41+ (Picasso96 v2+
  provides it), an 8-bit RTG mode at least **336x272**, about **16 MB** of free fast RAM
  (the BSS alone is 14.6 MB), `lowlevel.library` for joysticks.
- **A big stack.** WarpOS's default (1-4 KB) is nowhere near enough; `startupwos.o` takes the
  stack of the process that starts it. From a Shell: `Stack 200000` first; for an icon, its
  stack field.
- The game data, as for the Amiga drawer: the data setup fills it on a PC or Mac.
- The game's folder is its own drawer (`PROGDIR:`): `SWOS.ini`, `SWOS.log` and `save/` sit
  beside the program.

### The screen

`[video] resolution = WxH` in `SWOS.ini` names the RTG mode to try first. Then the ladder:
336x272 and 352x288 (a mode made in Picasso96Mode shows the game 1:1 and full screen),
400x300, 640x480, 640x512, 800x600 — the picture centred, the border in colour 0 as on the
Amiga. Every mode is checked three ways (a CyberGraphX id, 8 bits, 1 byte a pixel) and for
room; each step down writes a `[video]` line to `SWOS.log`, and the one chosen says the mode
asked for and the mode granted.

### Controls

As on the Amiga: player 1 is the joystick port and the keyboard (cursor keys; CTRL, RETURN or
ENTER fire; SPACE skips outside a match), player 2 the mouse port. CD32 pads: red fires, blue
fires and skips, PLAY pauses. CTRL-C in the Shell quits.

## How it is put together

`include/swos_platform.h` gives the build `SWOS_WARPOS_BUILD` and `SWOS_CHUNKY_BUILD` but not
`SWOS_SDL_BUILD`, so every gate on the renderer, the save format, the replays — the chunky
target — is in, and everything that calls SDL is out.

| file | what it is |
|---|---|
| `src/platform_wos.c` | the screen (mode ladder, LUT8 screen, backdrop window for input, the locked copy with `WriteLUTPixelArray` as the fallback), `LoadRGB32` only when the palette changed, the 50/70 Hz pace on `powerpc.library`'s own clock, the Amiga VBlank interrupt run from `Platform_VBlank` |
| `src/platform_frame.c` | SHARED with SDL: which buffer is the finished frame, its palette (copper list, DOS pitch, fade), the pacing rate — so both show the same frame in the same colours |
| `src/input_wos.c` | IDCMP raw keys straight into `keyState[]`/`keyBuffer` (they are Amiga scancodes already), `ReadJoyPort` into JOY0DAT/JOY1DAT/CIAA |
| `src/audio_wos.c` | **silent placeholder** (below) |
| `src/netplay_offline.c` | the answers a build without online play gives: no match is live, every online option and self-test "not in this build" |

**Single-threaded**, as the WASM build: no timer thread, so the VBlank interrupt runs from
`Platform_VBlank`, which every wait in the chunky game already goes through.

**The cost of the OS.** Every OS call from PPC code is a switch to the 68k side and back, and
the MOS2WOS stubs flush the whole cache both ways (`IF_CACHEFLUSHALL` in
`powerup/ppcinline/*.h`). That makes coherency automatic — a buffer 68k code reads needs no
flush of its own — and every call expensive. A frame makes: lock + unlock, the window's
messages, `SetSignal` for CTRL-C, two `ReadJoyPort`s, and `LoadRGB32` only on a palette
change. The clock and the wait never leave the PPC. Measure before optimising; the fix if it
is too much is one 68k helper doing present + input in a single switch.

## Not done yet

1. **Sound.** `audio_wos.c` answers the music/effects interface silently. The plan: split
   `audio_sdl.c` into the mixer it mostly is (RJP1 music, effect voices, the overlay sample —
   none of it needs SDL) and its output (SDL's audio queue), and add an AHI output beside the
   SDL one. The PC game's samples (`dos_sound.c`) the same way.
2. **The rest of the controls**: the player's own key bindings, the press latch, the CONTROLS
   menu — they live in `input_sdl.c`'s game-facing half, which should be shared the same way
   `platform_frame.c` shares the frame.
3. **The menus' online entries** are still shown (MULTIPLAYER answers nothing); hide them.
4. **Unchanged frames** are copied anyway; a menu that does not move could skip the copy.
5. **The big-endian hosted code** — `replay_v2.c`, `save_format_json.c`, `team_db.c`,
   `pitch_map.c`, `srp_writer.c` and the rest of what only the SDL build compiled — has never
   run on a big-endian machine. The check is `--srp-resim` of a corpus recording on the PPC,
   which must name no frame that parts from it.
6. **Packaging**: a drawer like `packaging/amiga/` with the stack set, and
   `build_dist.sh --only warpos`.

## First run

From a Shell, in the game's drawer, after `Stack 200000`:

```
Elf2Exe2 SWOS.warpelf SWOS
SWOS
```

Then read `SWOS.log`: the first line is the build stamp, then `[video]` says which mode was
asked for and granted and how frames reach the screen. A crash before that log exists: run
with `SWOS_NO_LOG=1` so stderr stays in the Shell.
