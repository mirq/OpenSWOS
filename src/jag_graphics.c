/*
 * jag_graphics.c -- the Atari Jaguar sprite tier (SDL only, --jag-graphics).
 *
 * The 1995 Jaguar release redrew SWOS's gameplay art at 256 colours: the same
 * sprites, in the same bank layout, with every flat colour turned into a little
 * ramp of shades. SwosGfx converts it back into the Amiga banks, and
 * scripts/make_jag_graphics.py lays those out as the planar sheets the game
 * loads (bin/grafs_jag), so the game's own image scripts cut the same sprites
 * from Jaguar pixels. What is left -- and what this file is -- is COLOUR.
 *
 * WHAT THE ART DOES WITH COLOUR
 *
 * Comparing the Jaguar sheets with the 16-colour originals, pixel for pixel,
 * every Amiga colour became either itself or a ramp:
 *
 *      Amiga 1,2,3  9,12,13  4,5,6      kept, 1:1   (greys, hair, skin: they
 *                                                    were already ramps)
 *      Amiga 8      (shadow)  -> 29-31, 220-223     the art's own grass-tinted
 *                                                    shadow shades
 *      Amiga 10     stripes   -> 240-243            \
 *      Amiga 11     shirt     -> 244-247             | the KIT: four slots,
 *      Amiga 14     shorts    -> 248-251             | four shades each
 *      Amiga 15     socks     -> 252-255            /
 *
 * So a Jaguar pixel is "(the colour SWOS means) at (a shade)", and only the kit
 * has to follow the team. Skin and hair need nothing new: the game already
 * varies exactly those indices per player (ConvertColorTableToSkinColor), and
 * the art kept them.
 *
 * IT DOES NOT NEED A PALETTE OF ITS OWN
 *
 * The game's 256 colours came from the DOS palette, which carries dimmed copies
 * of the 16 SWOS colours -- which is exactly what a kit ramp is. Every kit
 * colour's four shades are already in there (measured: the furthest is 3 units
 * of 4096 from the shade we would have made), and so are 64 of the art's own 76
 * colours. So this tier RE-USES entries rather than installing them:
 *
 *   - a kit shade is the nearest existing entry to colour x {16,13,9,6}/16, and
 *     shade 0 is the kit colour ITSELF, so a Jaguar kit is the colour the Amiga
 *     draws with shading under it;
 *   - an art colour takes the nearest entry when one is close enough and free,
 *     and only otherwise costs an entry from a small pool.
 *
 * That matters more than the entries it saves. The palette is NOT a scratchpad:
 * the pitch rewrites nine entries per pitch condition, the replay "R" bank
 * (soccer_s, sprites 0x447..0x486) owns 0x70..0x7F, the stadium and crowd
 * backgrounds are 8bpp art that reaches across it, and the fade walks all of it.
 * Installing this tier's colours across that range is how the spinning R came
 * out in the wrong colours. Those entries are volatile below: never written,
 * never re-used.
 */
#include "jag_graphics.h"

#ifdef SWOS_CHUNKY_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_graphics.h"        /* the palette a DOS-graphics match is drawn with */

extern int16_t   LoadFileEx(uint8_t *filename, uint8_t *dest, uint32_t *outFileSize);
extern uint16_t  _gamePalette[];
extern int       _gDosGraphics;
/* The nine the PITCH owns: WriteCopperPitchPalette rewrites them with the grass
 * and line colours of the pitch being played on. */
extern uint8_t   _paletteIndices[];
#define JAG_PITCH_ENTRIES 9

enum {
    /* The replay "R" (soccer_s) bank's own 16 (display.h). */
    JAG_R_FIRST     = 0x70,
    JAG_R_LAST      = 0x7F,
    /* The few art colours with nothing close enough in the palette live here.
     * Small, and high: measured free through a match, the stadium line-up and
     * the goal celebration. */
    JAG_POOL_FIRST  = 201,
    JAG_POOL_LAST   = 239,
    /* Near enough to re-use an entry rather than spend one, as 12-bit channels
     * squared: 2 is one channel one step out -- invisible, and the difference
     * between 64 of the art's colours costing nothing and costing an entry. */
    JAG_NEAR_ENOUGH = 2,
    JAG_UNASSIGNED  = 0xFF
};

/* THE SHADOW, and why it is not just another of the art's colours.
 *
 * A drop shadow is the pitch in shade, and the Jaguar drew it that way: against
 * its own grass (its pitch CLUT's 317100) these seven shades run from the grass
 * itself down to about a third of it. But OUR pitch changes with the condition --
 * muddy is brown (705000), frozen pale, dry yellow -- so keeping the art's greens
 * puts green patches under the players on a muddy pitch (reported 2026-09-21).
 * The 16-colour game does not have the problem: its shadow is one near-black
 * colour that reads on anything.
 *
 * IT IS THESE THREE AND NO OTHERS. 220..223 look like a deeper shadow ramp and
 * are not: a pixel-for-pixel diff of the sheets says they come from Amiga colour
 * 8, which is the shadow -- but only on CJCTEAMG, where colour 8 ALSO draws the
 * keeper's green jersey, and the Jaguar artists split the two (29..31 the ground
 * shadow, 220..223 the jersey). The outfield sheets contain no 220..223 at all.
 * Recolouring them per pitch turned the keeper brown on a muddy pitch (reported
 * 2026-09-22). The index alone does not say what a colour MEANS -- only the
 * sheet it is drawn on does, which is one of the arguments for docs/PALETTE_V2.md.
 *
 * So these shades are worked out from the grass of the pitch being played on,
 * keeping the artist's ramp: sixteenths of the live grass colour, re-made
 * whenever the pitch palette is (JagGfx_PitchApplied). They are the only entries
 * this tier rewrites after it has placed them. */
static const struct { uint8_t art; uint8_t sixteenths; } k_shadow[] = {
    {  29, 16 }, {  30, 14 }, {  31, 12 },
};
#define JAG_SHADOW_SHADES ((int32_t)(sizeof k_shadow / sizeof k_shadow[0]))
static uint8_t s_shadowEntry[JAG_SHADOW_SHADES];    /* the entries they were given */

/* Which of SWOS's colours each kit slot is, in the order the art's ramps run. */
static const uint8_t k_kitSlotAmiga[JAG_KIT_SLOTS] = { 10, 11, 14, 15 };

/* The shade of a ramp, as sixteenths of the colour: 16, 16-step, 16-2*step,
 * 16-3*step. The Jaguar's own ratios were 16,13,9,6 -- a step of about 3.3,
 * chosen for its own artwork rather than for SWOS kit colours, which are often
 * near-white. At that depth a white sock bottoms out at 37.5% and reads as dark
 * grey (reported 2026-09-22), so the step is a setting and the default is 2. */
static uint8_t k_shade[JAG_KIT_SHADES] = { 16, 14, 12, 10 };

static int32_t s_outfitShading = 1;
static int32_t s_shadowShading = 1;
static int32_t s_hairShading = 1;
static int32_t s_shadeStep = 2;

/* SWOS's own drop shadow: one near-black colour that reads on any pitch, which
 * is what "shadow_shading = false" asks for. */
#define JAG_FLAT_SHADOW_COLOUR 8

void JagGfx_SetShading(int32_t outfit, int32_t shadow, int32_t hair, int32_t step)
{
    int32_t k;
    s_outfitShading = outfit != 0;
    s_shadowShading = shadow != 0;
    s_hairShading = hair != 0;
    if (step < 1) step = 1;
    if (step > 5) step = 5;                 /* 16,11,6,1 is already almost black */
    s_shadeStep = step;
    for (k = 0; k < JAG_KIT_SHADES; k++) {
        int32_t v = 16 - k * step;
        k_shade[k] = (uint8_t)(v < 1 ? 1 : v);
    }
}

int32_t JagGfx_KitShade(int32_t shade)
{
    return shade < 0 || shade >= JAG_KIT_SHADES ? 16 : k_shade[shade];
}

static const char *const k_banks[] = {
    "CJCTEAM1", "CJCTEAM2", "CJCTEAM3", "CJCTEAMG", "CJCBITS", "CJCBENCH"
};

static int32_t  s_enabled;
static int32_t  s_haveArt;                  /* JAG.PAL is loaded */
static int32_t  s_triedArt;
static uint8_t  s_art[256][3];              /* the sheets' own palette, 8 bits a channel */
static uint8_t  s_used[256];                /* ... and which of it the banks draw in */
static uint8_t  s_remap[256];               /* sheet colour -> palette entry */
static uint8_t  s_taken[256];               /* entries this tier has spoken for */
static uint8_t  s_nextPool = JAG_POOL_FIRST;
static int32_t  s_saidFull;

static int32_t ensureArt(void);

/* Cheap and without side effects on purpose: the asset loader asks this while it
 * is opening a file, so loading anything here would be a loader inside a loader. */
int32_t JagGfx_Enabled(void) { return s_enabled; }

void JagGfx_SetEnabled(int32_t on) { s_enabled = on != 0; }

int32_t JagGfx_IsBank(const char *name)
{
    const char *base;
    size_t i;

    if (!s_enabled || !name || !ensureArt())
        return 0;
    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (i = 0; i < sizeof k_banks / sizeof k_banks[0]; i++) {
        size_t n = strlen(k_banks[i]);
        if (strncasecmp(base, k_banks[i], n) == 0 && (base[n] == '.' || base[n] == '\0'))
            return 1;
    }
    return 0;
}

/* THE DONATION MAP — where this tier's spare entries actually come from.
 *
 * The old answer was a hard-coded pool at 201..239, and it was a guess that has
 * now been wrong twice: it ran through the replay R bank (the spinning R came
 * out wrong), and in the grafs_2020 tier every one of those 39 entries is real
 * pitch art (the stadium roof turned yellow). Measured, entries 16..239 that
 * nothing draws are 163 in the AGA tier and ZERO in both DOS tiers, so there is
 * no better range -- there is no free range at all.
 *
 * But the palette is full of colours nobody needs: the DOS palette carries 62
 * entries whose colour is byte-identical to another entry (109, 110, 147, 163,
 * 177, 178, 237, 238 are all #000000, the same as entry 3). An entry like that
 * can be DONATED -- remap the art that draws in it to its twin, which is the
 * same colour, so nothing changes visibly, and the entry is genuinely free.
 *
 * Duplicates are a property of the palette alone, so this can be worked out
 * without knowing which pitch is loaded, which is what makes it order
 * independent: the tier allocates whenever its banks decode, and the pitch
 * remaps its pixels whenever it loads (JagGfx_RemapDonated).
 *
 * docs/PALETTE_V2.md is the eventual answer -- two palette banks, where a sprite
 * cannot be in a pitch's way at all. This is what fits in front of it. */
static int32_t takeSpareEntry(void);
static int32_t volatileEntry(int32_t entry);
static void    liveColour(int32_t entry, uint8_t out[3]);
static void    liveColour8(int32_t entry, uint8_t out[3]);

static uint8_t s_donateTo[256];        /* entry -> its twin, or 0 = not donated */
static uint8_t s_owned[256];           /* entries this tier WROTE (vs merely shares) */
static int32_t s_donateBuilt;

static void buildDonations(void)
{
    int32_t i, j, n = 0;

    if (s_donateBuilt)
        return;
    s_donateBuilt = 1;

    /* Against the LIVE palette, which in DOS-graphics mode is the DOS one and not
     * _gamePalette. Getting this wrong would be the whole bug in miniature: two
     * entries identical in the palette nobody is looking at, so the remap moves
     * pitch pixels to a DIFFERENT colour on screen. */
    for (i = 16; i < JAG_KIT_FIRST; i++) {
        uint8_t ci[3], cj[3];
        if (volatileEntry(i))
            continue;
        liveColour8(i, ci);
        for (j = 1; j < i; j++) {          /* the LOWER twin survives */
            if (volatileEntry(j))
                continue;
            liveColour8(j, cj);
            if (ci[0] == cj[0] && ci[1] == cj[1] && ci[2] == cj[2]) {
                s_donateTo[i] = (uint8_t)j;
                n++;
                break;
            }
        }
    }
    if (getenv("SWOS_JAG_DUMP"))
        fprintf(stderr, "[jag] %d palette entries are exact duplicates and can be "
                        "donated to the sprites\n", n);
}

int32_t JagGfx_RemapDonated(uint8_t *pixels, uint32_t count)
{
    uint32_t i;
    int32_t moved = 0;

    if (!s_enabled || !pixels)
        return 0;
    buildDonations();
    for (i = 0; i < count; i++) {
        uint8_t t = s_donateTo[pixels[i]];
        if (t) { pixels[i] = t; moved++; }
    }
    return moved;
}

/* An entry whose colour is not this tier's to read or to write: the pitch's nine
 * (they follow the pitch condition), the replay R bank's sixteen, and the kit
 * block, which is what a Jaguar sprite's pixels MEAN before they are recoloured. */
static int32_t volatileEntry(int32_t entry)
{
    int32_t i;
    if (entry >= JAG_R_FIRST && entry <= JAG_R_LAST)
        return 1;
    if (entry >= JAG_KIT_FIRST)
        return 1;
    for (i = 0; i < JAG_PITCH_ENTRIES; i++)
        if ((int32_t)_paletteIndices[i] == entry)
            return 1;
    return 0;
}

/* THE PALETTE THE SCREEN IS ACTUALLY DRAWN WITH. With --dos-graphics a match is
 * drawn in the DOS palette, not the Amiga one (platform_sdl.c), so matching the
 * art against _gamePalette there would pick entries by colours nobody displays.
 * It covers this art just as well -- the Amiga palette was converted from it. */
/* The live palette at ITS OWN precision -- 8 bits a channel in DOS mode, and the
 * Amiga's 4 bits widened. Donations compare with this and not with liveColour():
 * at 4 bits two DOS colours that merely look alike collapse into one, and a
 * "duplicate" that is not identical moves pitch pixels to a different colour. */
static void liveColour8(int32_t entry, uint8_t out[3])
{
    if (_gDosGraphics) {
        static uint8_t rgb[768];
        static int32_t have;
        if (!have) { have = 1; DosGfx_BuildPalette(4, rgb); }
        out[0] = rgb[entry * 3 + 0];
        out[1] = rgb[entry * 3 + 1];
        out[2] = rgb[entry * 3 + 2];
        return;
    }
    out[0] = (uint8_t)(((_gamePalette[entry] >> 8) & 15) * 17);
    out[1] = (uint8_t)(((_gamePalette[entry] >> 4) & 15) * 17);
    out[2] = (uint8_t)((_gamePalette[entry] & 15) * 17);
}

static void liveColour(int32_t entry, uint8_t out[3])
{
    if (_gDosGraphics) {
        static uint8_t rgb[768];
        static int32_t have;
        if (!have) {
            have = 1;
            /* Any pitch type: the nine it changes are volatile here anyway. */
            DosGfx_BuildPalette(4, rgb);
        }
        out[0] = (uint8_t)(rgb[entry * 3 + 0] >> 4);
        out[1] = (uint8_t)(rgb[entry * 3 + 1] >> 4);
        out[2] = (uint8_t)(rgb[entry * 3 + 2] >> 4);
        return;
    }
    out[0] = (uint8_t)((_gamePalette[entry] >> 8) & 15);
    out[1] = (uint8_t)((_gamePalette[entry] >> 4) & 15);
    out[2] = (uint8_t)(_gamePalette[entry] & 15);
}

static int32_t distance(int32_t entry, const uint8_t rgb12[3])
{
    uint8_t c[3];
    int32_t dr, dg, db;
    liveColour(entry, c);
    dr = (int32_t)c[0] - rgb12[0];
    dg = (int32_t)c[1] - rgb12[1];
    db = (int32_t)c[2] - rgb12[2];
    return dr * dr + dg * dg + db * db;
}

/* The entry nearest `rgb12`, and how far off it is. `freeOnly` skips entries this
 * tier has already given to another colour, so two shades of the art's own ramps
 * cannot collapse onto one entry and flatten it. */
static int32_t nearestEntry(const uint8_t rgb12[3], int32_t freeOnly, int32_t *outDist)
{
    int32_t best = -1, bestD = 1 << 30, i;
    for (i = 0; i < 256; i++) {
        int32_t d;
        if (volatileEntry(i) || (freeOnly && s_taken[i]))
            continue;
        d = distance(i, rgb12);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (outDist)
        *outDist = bestD;
    return best;
}

static void to12(const uint8_t rgb8[3], uint8_t out[3])
{
    out[0] = (uint8_t)(rgb8[0] >> 4);
    out[1] = (uint8_t)(rgb8[1] >> 4);
    out[2] = (uint8_t)(rgb8[2] >> 4);
}

/* The sheets' palette, read the first time the game asks about a Jaguar bank.
 * NOT at the command line: LoadFileEx stages through the frontbuffer, which the
 * game has not allocated yet. */
static int32_t ensureArt(void)
{
    uint8_t buf[4 + 768 + 256];
    uint32_t size = 0;
    int32_t i;

    if (s_haveArt)
        return 1;
    if (!s_enabled || s_triedArt)
        return 0;
    s_triedArt = 1;
    if (LoadFileEx((uint8_t *)"grafs_jag/JAG.PAL", buf, &size) != 0 || size < sizeof buf ||
        memcmp(buf, "JGP1", 4) != 0) {
        fprintf(stderr, "[jag] bin/grafs_jag/JAG.PAL is missing or not this format: staying with "
                        "the Amiga art (scripts/make_jag_graphics.py)\n");
        s_enabled = 0;
        return 0;
    }
    for (i = 0; i < 256; i++) {
        s_art[i][0] = buf[4 + i * 3];
        s_art[i][1] = buf[4 + i * 3 + 1];
        s_art[i][2] = buf[4 + i * 3 + 2];
        s_used[i]   = buf[4 + 768 + i];
        /* Identity where a sprite's colours already mean what the game means: the
         * low 16 are SWOS's own (the team table recolours them), and the kit block
         * is replaced per team. */
        s_remap[i] = (i < 16 || i >= JAG_KIT_FIRST) ? (uint8_t)i : JAG_UNASSIGNED;
    }
    s_haveArt = 1;
    return 1;
}

/* Where one of the art's own colours goes: the nearest entry when one is close
 * enough and not already spoken for, else an entry of its own from the pool. */
static uint8_t artEntry(int32_t sheetIndex)
{
    uint8_t rgb12[3];
    int32_t d = 0, near;

    to12(s_art[sheetIndex], rgb12);
    /* An entry holding exactly this colour is shared gladly -- the art spends
     * several indices on one colour where its ramps overlap. */
    near = nearestEntry(rgb12, 0, &d);
    if (near >= 0 && d == 0) {
        s_taken[near] = 1;
        s_remap[sheetIndex] = (uint8_t)near;
        return (uint8_t)near;
    }
    /* Otherwise it needs one of its own, or two shades of a ramp would land on
     * one entry and flatten it. */
    near = nearestEntry(rgb12, 1, &d);
    if (near >= 0 && d <= JAG_NEAR_ENOUGH) {
        s_taken[near] = 1;
        s_remap[sheetIndex] = (uint8_t)near;
        return (uint8_t)near;
    }
    {
        int32_t slot32 = takeSpareEntry();
        if (slot32 >= 0) {
        uint8_t slot = (uint8_t)slot32;
        _gamePalette[slot] = (uint16_t)((rgb12[0] << 8) | (rgb12[1] << 4) | rgb12[2]);
        /* And in the palette a DOS-graphics match is drawn with, which is a
         * different one (DosGfx_SetExtraColour). Both, so the tier does not
         * depend on which mode the game is in when a bank is decoded. */
        DosGfx_SetExtraColour(slot, (uint8_t)(s_art[sheetIndex][0]),
                              (uint8_t)(s_art[sheetIndex][1]), (uint8_t)(s_art[sheetIndex][2]));
        s_taken[slot] = 1;
        s_owned[slot] = 1;
        s_remap[sheetIndex] = slot;
        return slot;
        }
    }
    if (!s_saidFull) {
        s_saidFull = 1;
        fprintf(stderr, "[jag] the art wants more colours than the %d spare entries: the rest "
                        "take the nearest one in the palette\n", JAG_POOL_LAST - JAG_POOL_FIRST + 1);
    }
    near = nearestEntry(rgb12, 0, NULL);
    s_remap[sheetIndex] = (uint8_t)(near < 0 ? 0 : near);
    return s_remap[sheetIndex];
}

/* The next entry this tier may keep for itself: a donated duplicate if the
 * palette has one, else the old high pool for tiers that genuinely have room
 * (AGA has 163 free entries; the DOS tiers have none, which is the whole point
 * of donations). */
static int32_t takeSpareEntry(void)
{
    int32_t i;

    buildDonations();
    for (i = JAG_KIT_FIRST - 1; i >= 16; i--)      /* high first: out of the way */
        if (s_donateTo[i] && !s_taken[i] && !volatileEntry(i)) {
            s_taken[i] = 1;
            return i;
        }
    while (s_nextPool <= JAG_POOL_LAST && (volatileEntry(s_nextPool) || s_taken[s_nextPool]))
        s_nextPool++;
    if (s_nextPool <= JAG_POOL_LAST)
        return s_nextPool++;
    return -1;
}

/* An entry of this tier's own, for a colour that will be re-made later (the
 * shadow): never shared with the art, since rewriting it would move something
 * else's colour too. */
static uint8_t ownEntry(int32_t sheetIndex)
{
    int32_t slot32 = takeSpareEntry();
    if (slot32 < 0)
        return artEntry(sheetIndex);          /* no room: as good as it gets */
    {
        uint8_t slot = (uint8_t)slot32;
        uint8_t rgb12[3];
        to12(s_art[sheetIndex], rgb12);
        _gamePalette[slot] = (uint16_t)((rgb12[0] << 8) | (rgb12[1] << 4) | rgb12[2]);
        DosGfx_SetExtraColour(slot, s_art[sheetIndex][0], s_art[sheetIndex][1], s_art[sheetIndex][2]);
        s_taken[slot] = 1;
        s_owned[slot] = 1;
        s_remap[sheetIndex] = slot;
        return slot;
    }
}

const uint8_t *JagGfx_SheetRemap(void)
{
    static int32_t said;
    int32_t i, reused = 0, spent = 0;

    if (!s_enabled || !ensureArt())
        return NULL;
    /* The shadow first, so it has entries of its own before the art takes any. */
    for (i = 0; i < JAG_SHADOW_SHADES; i++) {
        if (s_remap[k_shadow[i].art] != JAG_UNASSIGNED)
            continue;
        if (!s_used[k_shadow[i].art]) {
            s_remap[k_shadow[i].art] = 0;        /* the art never draws in it */
            continue;
        }
        s_shadowEntry[i] = ownEntry(k_shadow[i].art);
        spent++;
    }
    for (i = 16; i < JAG_KIT_FIRST; i++) {
        if (s_remap[i] != JAG_UNASSIGNED)
            continue;
        if (!s_used[i]) {
            s_remap[i] = 0;             /* the art never draws in it */
            continue;
        }
        artEntry(i);
        if (s_remap[i] >= JAG_POOL_FIRST && s_remap[i] <= JAG_POOL_LAST)
            spent++;
        else
            reused++;
    }
    if (!said && (reused || spent)) {
        said = 1;
        fprintf(stderr, "[jag] Jaguar sprites: bin/grafs_jag; %d of the art's colours found in "
                        "the palette, %d given entries of their own\n", reused, spent);
        if (getenv("SWOS_JAG_DUMP")) {
            int32_t e;
            int32_t risky = 0;
            fprintf(stderr, "[jag] entries WRITTEN:");
            for (e = 16; e < JAG_KIT_FIRST; e++) {
                if (!s_owned[e]) continue;
                if (s_donateTo[e]) fprintf(stderr, " %d(dup of %d)", e, s_donateTo[e]);
                else { fprintf(stderr, " %d(POOL!)", e); risky++; }
            }
            fprintf(stderr, "\n");
            if (risky)
                fprintf(stderr, "[jag] %d entries came from the old pool -- those can "
                                "collide with pitch art\n", risky);
        }
    }
    /* The shadow entries hold the art's greens until the pitch says otherwise, and
     * the banks may well decode after the pitch palette was applied. */
    JagGfx_PitchApplied();
    return s_remap;
}

/* The grass this match is played on, as 12-bit channels: whichever palette the
 * match will be drawn with, for the pitch it is being set up for -- worked out
 * the way the two sources do it rather than read back, so it does not matter
 * whether the pitch palette has been applied yet. */
static void grassColour(uint8_t out[3])
{
    extern uint16_t  _pitchNumberAndType;
    extern const uint16_t *_pitchPalettePtrs[];
    uint16_t pitch = (uint16_t)(_pitchNumberAndType & 0xFF);

    if (_gDosGraphics) {
        uint8_t rgb[768];
        /* The pitch platform_sdl.c will draw with. */
        DosGfx_BuildPalette((int)pitch, rgb);
        out[0] = (uint8_t)(rgb[0] >> 4);
        out[1] = (uint8_t)(rgb[1] >> 4);
        out[2] = (uint8_t)(rgb[2] >> 4);
        return;
    }
    {
        /* The first of the nine the pitch owns is the grass (general_data.c's
         * _paletteIndices[0]), and the palettes are in the same order. */
        const uint16_t *pal = _pitchPalettePtrs[pitch];
        uint16_t c = pal ? pal[0] : _gamePalette[0];
        out[0] = (uint8_t)((c >> 8) & 15);
        out[1] = (uint8_t)((c >> 4) & 15);
        out[2] = (uint8_t)(c & 15);
    }
}

void JagGfx_PitchApplied(void)
{
    uint8_t grass[3];
    int32_t i;

    if (!s_enabled || !s_haveArt)
        return;
    if (!s_shadowShading) {
        /* Flat: every shadow shade IS SWOS's colour 8, so the tier spends no
         * entries on a shadow at all and the ground looks like the 16-colour
         * game's. The entries it was given keep whatever they hold; nothing
         * draws in them. */
        for (i = 0; i < JAG_SHADOW_SHADES; i++)
            s_remap[k_shadow[i].art] = JAG_FLAT_SHADOW_COLOUR;
        return;
    }
    for (i = 0; i < JAG_SHADOW_SHADES; i++)
        if (s_shadowEntry[i])
            s_remap[k_shadow[i].art] = s_shadowEntry[i];   /* undo a previous flat */

    grassColour(grass);
    for (i = 0; i < JAG_SHADOW_SHADES; i++) {
        uint8_t e = s_shadowEntry[i];
        uint16_t r, g, b;
        if (!e)
            continue;                    /* the art has not been placed yet */
        r = (uint16_t)((grass[0] * k_shadow[i].sixteenths) >> 4);
        g = (uint16_t)((grass[1] * k_shadow[i].sixteenths) >> 4);
        b = (uint16_t)((grass[2] * k_shadow[i].sixteenths) >> 4);
        _gamePalette[e] = (uint16_t)((r << 8) | (g << 4) | b);
        DosGfx_SetExtraColour(e, (uint8_t)(r * 17), (uint8_t)(g * 17), (uint8_t)(b * 17));
    }
    if (getenv("SWOS_JAG_DUMP")) {
        fprintf(stderr, "[jag] shadow on grass %X%X%X:", grass[0], grass[1], grass[2]);
        for (i = 0; i < JAG_SHADOW_SHADES; i++)
            if (s_shadowEntry[i])
                fprintf(stderr, " %d=%03X", s_shadowEntry[i],
                        _gamePalette[s_shadowEntry[i]] & 0xFFF);
        fprintf(stderr, "\n");
    }
}

void JagGfx_NewMatch(void)
{
    if (s_enabled)
        (void)ensureArt();               /* a safe moment: the game is up */
    JagGfx_PitchApplied();               /* in case the pitch was set first */
    /* SWOS_JAG_DUMP=1: the entries this tier must not have touched -- the replay
     * "R" bank's, whose colours the spinning R is drawn in. They are what the
     * game put there, or this tier has trodden on them again. */
    if (s_enabled && getenv("SWOS_JAG_DUMP")) {
        int32_t i;
        fprintf(stderr, "[jag] R bank (%02X..%02X):", JAG_R_FIRST, JAG_R_LAST);
        for (i = JAG_R_FIRST; i <= JAG_R_LAST; i++)
            fprintf(stderr, " %03X", _gamePalette[i] & 0xFFF);
        fprintf(stderr, "\n");
    }
}

/* How far a collapsed hair colour is lifted for its two lighter shades, in
 * 4-bit channels. Black has nothing darker than it, so the highlight lifts
 * rather than the shadow deepening -- and it has to lift FAR ENOUGH: at 3/1 the
 * mid shade is 111, indistinguishable from the 000 beside it, and the hair reads
 * as the flat block the 16-colour game draws (reported 2026-09-21). At 7/3 it
 * reads as grey hair. 6/3 shades it while the mass stays black -- and that is
 * what shade_step's default of 2 gives: the lift is 3 x step and half of it, so
 * the hair's contrast follows the same setting the kit's does (step 1 = 3/1,
 * step 3 = 9/4). Taste, so SWOS_JAG_HAIR=hi,mid overrides it without a rebuild. */
static int32_t s_lift0 = 6, s_lift1 = 3;

void JagGfx_TeamTable(const uint16_t *amiga16, uint16_t *out256)
{
    {
        const char *t = getenv("SWOS_JAG_HAIR");
        s_lift0 = 3 * s_shadeStep;
        s_lift1 = s_lift0 / 2;
        if (t && *t) { s_lift0 = atoi(t); if (strchr(t, ',')) s_lift1 = atoi(strchr(t, ',') + 1); }
    }
    int32_t i, slot, shade;

    if (!s_haveArt) {
        for (i = 0; i < 256; i++)
            out256[i] = (uint16_t)(i < 16 ? amiga16[i] : i);
        return;
    }
    for (i = 0; i < 256; i++)
        out256[i] = (uint16_t)i;
    /* What the game says about its own colours -- skin and hair among them -- and
     * [0] with it, which is what tells ConvertSpriteColors to leave transparent
     * pixels alone. */
    for (i = 0; i < 16; i++)
        out256[i] = amiga16[i];
    /* HAIR. The art draws it in three shades (13 light, 9 mid, 12 dark), and for a
     * ginger head the game's own table keeps three (F60 B40 620). For a BLACK one it
     * sends all three to colour 3, so the Jaguar's shading is thrown away and the hair
     * is a flat block -- which is what the 16-colour game draws, and looks it. When the
     * three collapse onto one colour, shade that colour instead. A colour with nothing
     * darker than it (black) lifts its highlight rather than deepening its shadow.
     * Any other colour deepens by the kit's own ramp (k_shade: 16, 16-step,
     * 16-2*step), so one shade_step sets the contrast of both. "hair_shading =
     * false" skips all of it: the game's table as it stands, which is the
     * 16-colour game's hair exactly. */
    if (s_hairShading) {
        static const uint8_t hairArt[3] = { 13, 9, 12 };     /* light, mid, dark */
        uint16_t c = (uint16_t)(amiga16[hairArt[0]] & 0xFF);
        if ((uint16_t)(amiga16[hairArt[1]] & 0xFF) == c && (uint16_t)(amiga16[hairArt[2]] & 0xFF) == c) {
            uint8_t ch[3];
            int32_t k;
            liveColour(c, ch);
            for (k = 0; k < 3; k++) {
                uint8_t want[3];
                int32_t j, near;
                for (j = 0; j < 3; j++) {
                    int32_t v;
                    if (ch[0] <= 3 && ch[1] <= 3 && ch[2] <= 3)
                        v = (int32_t)ch[j] + (k == 0 ? s_lift0 : k == 1 ? s_lift1 : 0); /* lift */
                    else
                        v = ((int32_t)ch[j] * k_shade[k]) >> 4;
                    want[j] = (uint8_t)(v > 15 ? 15 : v);
                }
                near = nearestEntry(want, 0, NULL);
                out256[hairArt[k]] = (uint16_t)(near < 0 ? c : near);
            }
            if (getenv("SWOS_JAG_DUMP"))
                fprintf(stderr, "[jag] hair %d collapsed -> %03X %03X %03X\n", c,
                        _gamePalette[out256[hairArt[0]]] & 0xFFF,
                        _gamePalette[out256[hairArt[1]]] & 0xFFF,
                        _gamePalette[out256[hairArt[2]]] & 0xFFF);
        }
    }
    if (getenv("SWOS_JAG_DUMP"))
        fprintf(stderr, "[jag] shades %d/%d/%d/%d sixteenths, outfit=%s shadow=%s hair=%s\n",
                k_shade[0], k_shade[1], k_shade[2], k_shade[3],
                s_outfitShading ? "on" : "off", s_shadowShading ? "on" : "off",
                s_hairShading ? "on" : "off");
    for (slot = 0; slot < JAG_KIT_SLOTS; slot++) {
        uint16_t colour = (uint16_t)(amiga16[k_kitSlotAmiga[slot]] & 0xFF);
        uint8_t base[3];
        liveColour(colour, base);
        for (shade = 0; shade < JAG_KIT_SHADES; shade++) {
            uint16_t entry;
            if (shade == 0 || !s_outfitShading) {
                /* The colour itself, entry and all: a Jaguar kit is the kit the
                 * Amiga draws. (Kit colour 0 is one the pitch rewrites, and the
                 * 16-colour game lets that follow the pitch too.) */
                entry = colour;
            } else {
                uint8_t want[3];
                int32_t near;
                want[0] = (uint8_t)((base[0] * k_shade[shade]) >> 4);
                want[1] = (uint8_t)((base[1] * k_shade[shade]) >> 4);
                want[2] = (uint8_t)((base[2] * k_shade[shade]) >> 4);
                near = nearestEntry(want, 0, NULL);
                entry = (uint16_t)(near < 0 ? colour : near);
            }
            out256[JAG_KIT_FIRST + slot * JAG_KIT_SHADES + shade] = entry;
        }
        if (getenv("SWOS_JAG_DUMP")) {
            static const char *k_slotName[JAG_KIT_SLOTS] =
                { "stripes", "shirt", "shorts", "socks" };
            fprintf(stderr, "[jag]   %-7s colour %2d ->", k_slotName[slot], colour);
            for (shade = 0; shade < JAG_KIT_SHADES; shade++)
                fprintf(stderr, " %03X",
                        _gamePalette[out256[JAG_KIT_FIRST + slot * JAG_KIT_SHADES + shade]] & 0xFFF);
            fprintf(stderr, "\n");
        }
    }
}

#else   /* the Amiga build has no Jaguar tier: 8bpp sprite banks are twice the
         * chip RAM and bandwidth of the 4bpp ones the A1200 draws today. */

int32_t        JagGfx_Enabled(void) { return 0; }
int32_t        JagGfx_KitShade(int32_t shade) { (void)shade; return 16; }
void           JagGfx_SetEnabled(int32_t on) { (void)on; }
int32_t        JagGfx_IsBank(const char *name) { (void)name; return 0; }
const uint8_t *JagGfx_SheetRemap(void) { return 0; }
void           JagGfx_NewMatch(void) {}
void           JagGfx_SetShading(int32_t o, int32_t s, int32_t h, int32_t st) { (void)o; (void)s; (void)h; (void)st; }
int32_t        JagGfx_RemapDonated(uint8_t *p, uint32_t n) { (void)p; (void)n; return 0; }
void           JagGfx_PitchApplied(void) {}
void           JagGfx_TeamTable(const uint16_t *amiga16, uint16_t *out256)
{
    (void)amiga16; (void)out256;
}

#endif /* SWOS_CHUNKY_BUILD */
