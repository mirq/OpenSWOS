# OpenSWOS for WarpOS (PowerPC) on an RTG screen

The chunky target without SDL: the game the SDL build plays — 8bpp chunky renderer, hosted
libc, `SWOS.ini`, JSON saves — on an Amiga with a WarpOS PowerPC card and a Picasso96 (or
CyberGraphX) graphics card. Offline play only.

**Status (2026-10-22): it plays.** Tested on an A4000-class machine with a 68060 and a Sonnet
MPC7410 (WarpOS 5.1, `powerpc.library` 17.15), Picasso96 2.495 and AHI: menus, CPU-vs-CPU
matches and highlights at a full 50 Hz, Amiga music and effects through AHI, keyboard and
joystick.

The same game run by the 68060 itself, no PPC, is `Makefile.rtg060` (docs/RTG060.md): it
shares this build's platform files, so a change to them is built for both.

## Building

```bash
make -f Makefile.warpos -j$(nproc)      # -> bin/SWOS.wos (and bin/SWOS.warpelf)
```

Needs the V19 MiniGL SDK's common makefile (`MGL_DIR`, default
`/home/mirek/MiniGL_WOS_V19_mglQ3`) for the toolchain paths and the ET_REL contract —
`ppc-amigaos-gcc` 8.4 (adtools) with the MOS2WOS WarpOS overlay. **No MiniGL is linked**: the
game draws 8bpp chunky, which is an 8-bit RTG screen's own pixel format, so it needs no 3D.

`bin/SWOS.wos` is the Amiga program, converted here by `tools/elf2warpos/elf2warpos.py` — the
same hunk layout and relocations as `Elf2Exe2` (`simulate_load.py` proved the loaded images
identical), plus a 68k stub that:

- **copies the hunks into `MEMF_PPC` memory** when LoadSeg put them elsewhere. A Sonnet's PPC
  runs code only from its own memory: anything else is an ISI exception (`SRR1 $4000F030`).
- **gives the program a 1 MB stack** with `StackSwap` (`--stack`), so no `Stack` command or
  icon field is needed.

`bin/SWOS.warpelf` is the relocatable ELF, for `Elf2Exe2` on the Amiga if wanted;
`build/warpos/ppc/SWOS.debug.elf` is the same link with its debug info, for `addr2line`.

Flags that are not negotiable, each in `Makefile.warpos` with its reason:
`-mcpu=603e` (runs on every WarpOS board), `-fsigned-char` (char is UNSIGNED on PPC and signed
where the game was written), `-fno-common` (the ET_REL check refuses COMMON symbols),
`-msdata=none -G0` (no small-data relocations), `-fno-jump-tables`, `-Uamiga -Uamigaos` (the
compiler predefines them and the game has variables called `amiga`). The SDK's SDL 1.2
headers are taken OFF the include path, so an SDL call that should have been ported cannot
quietly compile against them.

## Running it

- AmigaOS 3.x, WarpOS (`powerpc.library`), `cybergraphics.library` v41+ (Picasso96 v2+
  provides it), an 8-bit RTG mode at least **336x272**, about **16 MB** of free fast RAM,
  `lowlevel.library` for joysticks, `ahi.device` for sound.
- The game data, as for the Amiga drawer (`data grafs grafs_aga sound`), plus `data_json`.
  The game's folder is its own drawer (`PROGDIR:`): `SWOS.ini`, `SWOS.log` and `save/` sit
  beside the program.
- Start it from a Shell or with `Run >NIL: SWOS`. CTRL-C quits.

### The screen

The mode, first match wins:

1. `SWOS_ASL=1` (`SetEnv`): the ASL screen-mode requester, 8-bit modes only. The answer is
   saved in `ENVARC:SWOS_SCREENMODE`, so it is asked once.
2. `SWOS_SCREENMODE=0x...`: that mode id.
3. `[video] resolution = WxH` in `SWOS.ini`.
4. The ladder: 336x272, 352x288, 400x300, 640x480, 640x512, 800x600.

Every mode is checked (a CyberGraphX id, 8 bits, 1 byte a pixel, room for the picture); the
picture is centred, the border colour 0. **A 50 Hz or 100 Hz mode looks smoothest**: the game
runs at 50.

**Pacing.** At start the game times 100 flips between two screen buffers (two seconds of a
black screen): `ChangeScreenBuffer` returns at the board's vertical blank — `WaitTOF` and
`WaitBOVP` follow the native chipset, not the RTG display (`SWOS_VSYNC_PROBE=1` logs all
three). A line fitted through the returns gives the refresh to a fraction of a microsecond and
the moment of a blank. When the refresh is within 4% of 50 Hz the frames and the VBlank work
follow it, each copy starting just after a blank (`SWOS_VSYNC_PHASE_US` moves it,
`SWOS_NO_VSYNC=1` paces at exactly 50 instead): a 50.7 Hz display paced at 50.0 showed a frame
twice every 1.4 s. The wait inside a frame spins on the PPC timebase — `WaitTime` overslept
13 ms — which costs the 68k nothing.

**Double buffering** is used only when a flip is cheap. On the Sonnet + Picasso96 here every
flip cost ~18.5 ms wherever the beam was, both buffers in card memory, so a frame is copied
into the one buffer on screen (~3 ms for a full 336x272). `SWOS_DBUF=1` forces it,
`SWOS_NO_DBUF=1` skips the test.

`SWOS_PRESENT_STATS=1` logs every 250 frames where a frame's time went (work, events, audio,
the copy, the wait), how many were late, the slowest, and the flips.

### Sound

AHI in library mode, as in quake3amiga's `amiga_snd.c`: one looping 16-bit stereo sample is a
ring the game's mixer (`audio_sdl.c`, shared, its SDL output split off behind
`include/audio_out.h`) fills a little ahead of the play position, which an
`AHIET_CHANNELINFO` hook reports. 22050 Hz, `SWOS_AHI_RATE` to change it.

### Controls

As on the Amiga: player 1 is the joystick port and the keyboard (cursor keys; CTRL, RETURN or
ENTER fire; SPACE skips outside a match), player 2 the mouse port. CD32 pads: red fires, blue
fires and skips, PLAY pauses. A port is read every frame only while a stick answers in it; an
empty one is looked at again once a second in the menus, never in a match.

### Testing without anyone at the controls

`SWOS_AUTO_MATCH=<n>` (shared, `src/auto_match.c`) skips the intro, the title screens and the
front end and plays `n` CPU-vs-CPU friendlies (0: until CTRL-C) between random national teams.

## How it is put together

`include/swos_platform.h` gives the build `SWOS_WARPOS_BUILD` and `SWOS_CHUNKY_BUILD` but not
`SWOS_SDL_BUILD`, so every gate on the renderer, the save format, the replays — the chunky
target — is in, and everything that calls SDL is out.

| file | what it is |
|---|---|
| `src/platform_wos.c` | the screen (mode choice, LUT8 screen, backdrop window for input, the locked copy of the rows that changed with `WriteLUTPixelArray` as the fallback), `LoadRGB32` only when the palette changed, the pacing, the VBlank interrupt run from `Platform_VBlank`, the log |
| `src/platform_frame.c` | SHARED with SDL: which buffer is the finished frame, its palette, the pacing rate |
| `src/input_wos.c` | IDCMP raw keys straight into `keyState[]`/`keyBuffer` (they are Amiga scancodes already), `ReadJoyPort` into JOY0DAT/JOY1DAT/CIAA |
| `src/audio_wos.c` | the AHI output |
| `src/netplay_offline.c` | the answers a build without online play gives |

**Single-threaded**, as the WASM build: no timer thread, so the VBlank interrupt runs from
`Platform_VBlank`, which every wait in the chunky game already goes through.

**The cost of the OS.** Every OS call from PPC code is a switch to the 68k side and back, and
the MOS2WOS stubs flush the whole cache both ways. A frame makes few: the lock, one
`SetSignal` for the window and CTRL-C (its port drained only when signalled), `ReadJoyPort`
for a live port. Anything that keeps the 68k busy — a network transfer through a bridge — makes
those calls wait: 40-60 ms frames that are not the game's.

**The log** collects in memory and goes to `SWOS.log` every 2 s and at exit: a line per menu
move written straight out cost 60 ms a frame. `SWOS_LOG_BRIDGE=1` also copies it to an
`AMIGABRIDGE` port.

**Paths.** The WarpOS newlib does not take `./`-prefixed paths, so names are joined with
`swos_path_join`; `PROGDIR:` is entered with `Lock` + `CurrentDir` (newlib's `chdir` unlocked
the Shell's own lock: `AN_MemoryInsane`).

## Not done yet

1. **The rest of the controls**: the player's own key bindings and the CONTROLS menu live in
   `input_sdl.c`'s game-facing half, which should be shared the way `platform_frame.c` is.
2. **The PC game's sound** (`[audio] style = pc`) is not in.
3. **The big-endian hosted code** — `replay_v2.c`, `save_format_json.c`, `team_db.c` and the
   rest of what only the SDL build compiled — has run on the PPC only as far as the menus,
   matches and highlights go; saves are not checked yet.
4. **Packaging**: a drawer with an icon, and `build_dist.sh --only warpos`.
