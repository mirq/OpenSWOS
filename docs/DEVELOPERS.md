# OpenSWOS for developers

How the source is laid out and the conventions it follows. Building it is
[BUILDING.md](BUILDING.md); the full engineering rules are
[`.claude/CLAUDE.md`](../.claude/CLAUDE.md); [INDEX.md](INDEX.md) lists every other doc.

## Where the code came from

A pure C port of *Sensible World of Soccer* (Amiga, 1994). The original 68k
assembly has been translated to C, and **one source tree builds three targets**:

| Target | Build | Output |
|---|---|---|
| **SDL2** — Windows, Linux, WSL2, macOS, WebAssembly, ARM | `make` | `bin/SWOS*` |
| **Amiga m68k** — A1200/AGA, FS-UAE/WinUAE or real hardware | `make -f Makefile.amiga` | `bin/SWOS2` |
| **Nintendo Switch** — devkit only; needs the licensee SDK + SDL fork | `scripts/build_switch.sh` | `bin/SWOS.nss` |

On SDL the OCS/AGA chipset, Paula audio, and joystick I/O are emulated by thin
SDL2 shims; on Amiga the same C drives the real hardware. `src/` and `include/`
are shared — platform divergence is expressed with `#ifdef SWOS_CHUNKY_BUILD`
(the portable chunky target: no custom chips) and `#ifdef SWOS_SDL_BUILD` (the
SDL library itself; see `include/swos_platform.h`), and
a short list of SDL-only files is excluded from the m68k build by name
(`SDL_ONLY_SRCS` in `Makefile.amiga`).

The C didn't come from nowhere: it's the downstream product of
**[SWOS-RE](https://github.com/benbaker76/SWOS-RE)**, a separate reverse-engineering
project that documents the original Amiga 68k binary — IDA Pro + Ghidra databases,
function/variable/struct naming, sprite and menu formats, and the annotated
`SWOS2.s` disassembly this port was first translated from. If you want to know
*why* a piece of this code looks the way it does, that's where the answer lives.

> These were two separate repos (`SWOS_C_SDL` and `SWOS_C`) kept in step by a
> one-way copy script until they were merged on 2026-08-12. Older notes in
> `docs/` that talk about "back-porting" or `sync_to_amiga_port.sh` describe
> that historical workflow — there is no sync step any more.

## Goals

1. **Keep both targets building.** A change to shared code has to compile for
   m68k as well as SDL. The Amiga build is the stricter of the two (32-bit
   pointers, big-endian, `-nostdlib`), so it catches bitness and endian slips
   the SDL build would let through.
2. **Retire `SWOS2.s`.** It is now reference material (`data/SWOS2.s`) — the
   Amiga build already runs entirely from C.
3. **Untangle the Amiga BSS overlays.** The current code mirrors the Amiga's
   memory layout faithfully (adjacent-globals, `careerFileBuffer` overlays,
   `selTeamsPtr` aliased onto its own data). Once both ports run on C, those
   overlays become proper structs with named fields.
4. **Replace the save format.** The current `.CAR` format is a raw memory
   dump of the career-buffer overlay region — fragile and contains absolute
   pointers. Eventually replace with a structured format (JSON or tagged
   binary) that serialises named fields, not memory addresses.

## Project layout

```
Makefile                     — SDL targets  (make)
Makefile.amiga               — Amiga m68k target (make -f Makefile.amiga),
                               incl. SDL_ONLY_SRCS: the files the m68k
                               build must skip
SWOS2.ld                     — Amiga linker script (CHIP vs FAST placement)
src/                         — port source, SHARED by both targets
    SWOS.c                   — (start of the Amiga C entry tree)
    main.c                   — entry point; #ifdef SWOS_SDL_BUILD splits the
                               SDL2 host from the Amiga -nostdlib/startShim one
    platform_sdl.c           — window, render, vblank pacing        [SDL only]
    platform_amiga_stubs.c   — startShim + Amiga-side platform glue
    audio_sdl.c              — SDL2 audio backend
    rjp1.c                   — RJP1 (Richard Joseph Player) module decoder
    input_sdl.c              — keyboard + gamepad → joy0Dat / joy1Dat / CIAA
    input.c                  — Amiga-side joystick decode (unchanged)
    game_loop.c              — main match loop
    menu_system.c            — packed-menu interpreter
    menus.c, menu_data.c     — every menu, every callback
    transfer_market.c        — buy / sell / offers
    career_*.c               — career mode (init, season, teams, eurocups)
    diy_competition.c        — custom league/cup runner
    pitch.c, scroll_blit.c   — pitch tile rendering and scrolling
    image_cmd.c              — Amiga sprite/image command stream decoder
    swos_globals*.c          — shared backing buffers + `.equ` aliases for
                               adjacent-globals BSS runs that the original
                               Amiga code reads as one struct
bin/                         — runtime dir for BOTH targets; FS-UAE mounts
                               this as DH1: so the Amiga sees the same files
    data/, grafs/, sound/    — original Amiga data (NOT in this repo)
    grafs_aga/               — 8-bitplane AGA graphics
    save/                    — career save files (.CAR/.SEA/.PRE/.DIY/.HIL)
    SWOS / SWOS.exe          — the built SDL binary
    SWOS2                    — the built Amiga binary (+ .elf, .map)
data/                        — IDA/Ghidra reference only, NOT compiled
                               (Data.c, SWOS2.s, SWOS2_IDA.h)
asm/, support/               — the small amount of m68k asm still linked in
uae/                         — FS-UAE bootable-volume scaffolding (dh0/c, dh0/s)
                               and legacy disk images under uae/artifacts/
scripts/amiga/               — gdb/profiling harness (AmigaScope submodule)
.claude/CLAUDE.md            — engineering conventions for contributors
```

## Architecture notes

A few non-obvious conventions ([`.claude/CLAUDE.md`](../.claude/CLAUDE.md) has
the long version):

- **Graphics: 8bpp chunky end-to-end.** The Amiga 4/8-bitplane decode runs
  once at sprite load time, not per blit.
- **Endian helpers (`src/endian_helpers.h`).** Use `BE_READ_U16` /
  `PACK_BE_U16` / `BE32_TO_HOST` etc. for any read of Amiga-authored
  data — they're no-ops on Amiga BE, byte-swap on x86 LE, and grep-able.
- **Adjacent-globals BSS.** The Amiga relies on neighbouring globals being
  contiguous (e.g., `MENU_CMD_SPRITE_COPY` reads three labels as one
  36-byte struct). On SDL the linker scatters them, so we restore the
  layout via shared backing buffers + `.equ` aliases. This is a temporary
  scaffold (step 3 of the roadmap).
- **Don't patch out panics.** `_PanicHaltLoop` / panic-reason codes mean an
  invariant was violated — the previous setup step left state corrupt.
  Trace upward and fix the cause; don't add a `if (!ptr) return;` to make
  the menu draw work.
