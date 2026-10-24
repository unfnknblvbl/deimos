/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * The Mega Drive game - the colours of each enclosed area (the Mega Drive's view
 * palettes), worked out from the campaign's data block (tools/md_campaign.py).
 *
 * The map is cut into areas: the open cells joined to each other without
 * crossing a door or other line (walls and line cells are the borders).
 * Each area gets three palettes of 15 colours. Every wall texture and
 * sprite of the map gets one of the three: the one that serves its colours
 * best. On the console, each 8x8 tile of the 3D view takes the palette of
 * what is drawn at its centre (src/render.s), and every pixel of the tile
 * is shown in that palette (its nearest colour there).
 *
 * How the palettes are chosen: every view from inside the area (each open
 * cell, each facing) is drawn coarsely (64 columns) to see how much of each
 * texture and sprite shows; each one's colour histogram is weighted by that
 * (its square root, so small things count; sprites four times). Then the
 * textures and sprites are grouped in three (k-means, six starts, four
 * rounds each), each group's colours making one palette (render_pal.s's
 * method: the worst served colour first, then k-means on the colours).
 * Integer arithmetic only, so every compiler gives the same bytes: the
 * The Mega Drive game build (this file as a program) and the editor (compiled in).
 *
 * Output, the areas table (big-endian), after the block in the ROM; the
 * cartridge header's $1D8 holds its address:
 *   0  "MDAR", u16 version (1), u16 number of maps
 *   8  per map: u32 offset of its part (from the table's start)
 *   a map's part: u8 areas, u8 textures, u8 sprites, u8 colours (n),
 *     n CRAM words: the colours its textures and sprites use (ascending),
 *     1024 bytes: each cell's area ($FE a line's cell, $FF a wall)
 *     per area: 3 palettes x 16 colours (CRAM words; colour 0 unused),
 *     a palette (0..2) per texture and per sprite, then for each palette
 *     each of the n colours' nearest slot in it (1..15), padded to even
 *
 * Program: md_areas campaign.bin areas.bin
 *
 * On the console, src/area.c reads this table (area_init, area_select)
 * and builds the remap tables the renderer uses.
 *
 * Terms: a "texture" is a 64x64 wall texture with up to 32 colours, a
 * "sprite" a thing's picture with up to 16; each comes with a palette of
 * CRAM colours and a histogram (how many texels use each colour). An
 * "asset" below is either (index a < n_tex: texture a, else sprite
 * a - n_tex). Colours are handled as 9-bit indices (cidx). Distances
 * between colours are weighted squared differences of the channels'
 * brightness levels (dist).
 *
 * Licence: GPL-2.0-or-later.
 */
#include "md_areas.h"

#include <stdlib.h>
#include <string.h>

#define MAXA 255            /* area numbers must stay below $FD ($FD..$FF are markers) */
#define RESTARTS 6          /* k-means starts per area */
#define ROUNDS 4            /* k-means rounds per start */
#define COLS 64             /* columns of the coarse views (the screen's 256 / 4) */

typedef unsigned long long u64;
typedef long long s64;

/* the brightness (0..255) of each of a CRAM channel's 8 levels (as
   src/backdrop.c) */
static const int LEV[8] = {0, 52, 87, 116, 144, 172, 206, 255};

/* big-endian reads: u16, u32, s16 */
static unsigned be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}
static int sbe16(const unsigned char *p) { return (short)be16(p); }

/* CRAM colour -> 9-bit index (r | g << 3 | b << 6) and back */
static int cidx(unsigned c) { return ((c >> 1) & 7) | (((c >> 5) & 7) << 3) | (((c >> 9) & 7) << 6); }
static unsigned cram(int i) { return ((i & 7) << 1) | (((i >> 3) & 7) << 5) | (((i >> 6) & 7) << 9); }

/* the distance between colour indices a and b: 3 dr^2 + 4 dg^2 + 2 db^2
   on the levels' brightness (green counts most, blue least) */
static int dist(int a, int b)
{
    int dr = LEV[a & 7] - LEV[b & 7], dg = LEV[(a >> 3) & 7] - LEV[(b >> 3) & 7], db = LEV[(a >> 6) & 7] - LEV[(b >> 6) & 7];
    return 3 * dr * dr + 4 * dg * dg + 2 * db * db;
}
static int near_level(u64 sum, u64 w)
{
    /* the level nearest sum / w (exactly: compares 2 sum with (a + b) w), the lower on a tie */
    int i = 0;
    while (i < 7 && 2 * sum > (u64)(LEV[i] + LEV[i + 1]) * w) i++;
    return i;
}
/* the integer square root of v (rounded down), bit by bit */
static u64 isqrt(u64 v)
{
    u64 r = 0, b = 1ULL << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

/* a weighted colour histogram: h[colour index] = weight */
typedef struct {
    u64 h[512];
} Hist;

/* render_pal.s's way: greedy (the colour worst served so far, weight x
   distance), then three rounds of weighted k-means; k <= 15 -> n */
/* Chooses up to k colours (indices into pal) for histogram h; returns how
   many (fewer if h has fewer colours, or all are served exactly). */
static int choose(const Hist *h, int k, int *pal)
{
    int cand[512], nc = 0, n = 0, i, j, r;
    u64 mind[512];
    for (i = 0; i < 512; i++)
        if (h->h[i]) cand[nc++] = i;            /* the colours present */
    for (j = 0; j < nc; j++) mind[j] = ~0ULL;   /* distance to the nearest chosen: none yet */
    /* greedy: first the heaviest colour, then each time the colour with the
       largest weight x distance to its nearest chosen colour */
    while (n < k && n < nc) {
        int best = -1;
        u64 bv = 0;
        for (j = 0; j < nc; j++) {
            u64 v = n ? (mind[j] ? h->h[cand[j]] * mind[j] : 0) : h->h[cand[j]];
            if (best < 0 || v > bv) bv = v, best = j;
        }
        if (n && bv == 0) break;                /* every colour served exactly */
        pal[n++] = cand[best];
        for (j = 0; j < nc; j++) {
            u64 d = (u64)dist(cand[j], cand[best]);
            if (d < mind[j]) mind[j] = d;
        }
    }
    /* k-means: each colour to its nearest palette entry; each entry moved
       to the weighted mean (per channel brightness, back to the nearest
       level) of its colours */
    for (r = 0; r < 3; r++) {
        u64 s[15][4];                           /* per entry: sum r, g, b brightness x weight, weight */
        memset(s, 0, sizeof s);
        for (j = 0; j < nc; j++) {
            int c = cand[j], b = 0, q;
            for (q = 1; q < n; q++)
                if (dist(c, pal[q]) < dist(c, pal[b])) b = q;
            s[b][0] += h->h[c] * LEV[c & 7];
            s[b][1] += h->h[c] * LEV[(c >> 3) & 7];
            s[b][2] += h->h[c] * LEV[(c >> 6) & 7];
            s[b][3] += h->h[c];
        }
        for (j = 0; j < n; j++)
            if (s[j][3])
                pal[j] = near_level(s[j][0], s[j][3]) | (near_level(s[j][1], s[j][3]) << 3) |
                         (near_level(s[j][2], s[j][3]) << 6);
    }
    return n;
}

/* how badly palette pal (n colours) serves histogram h: the sum over its
   colours of weight x distance to the nearest palette colour (a large
   value for an empty palette) */
static u64 err_of(const Hist *h, const int *pal, int n)
{
    u64 e = 0;
    int i, q;
    if (!n) return ~0ULL >> 2;
    for (i = 0; i < 512; i++)
        if (h->h[i]) {
            int b = dist(i, pal[0]);
            for (q = 1; q < n; q++)
                if (dist(i, pal[q]) < b) b = dist(i, pal[q]);
            e += h->h[i] * (u64)b;
        }
    return e;
}

/* ---- one map ---- */
/* the entries of a map pack (as src/pack.c, tools/md_campaign.py ENTRIES) */
enum { P_WALLS, P_TEX_OFF, P_TEX_PAL, P_TEX_HIST, P_SPR_COLS, P_SPR_OFF, P_SPR_COFF, P_PVS_INDEX, P_PVS,
       P_SPR_PAL, P_SPR_HIST, P_SCENERY, P_DEF_SPR, P_VIS_INDEX, P_VIS, P_LEVEL, P_STRINGS };
/* the level blob's tables (tools/gen_cdata.py BLOB_TABLES) */
enum { L_ENT_DEF0 = 4, L_ENT_X0 = 5, L_ENT_Z0 = 6, L_ENT_HIDDEN0 = 7, L_WALL_BITS = 24, L_LINE_CELLS = 27 };

/* one map, as load_map reads it */
typedef struct {
    const unsigned char *blk, *pk, *lvl;    /* the campaign block, the map's pack, its level blob */
    int n_walls, n_tex, n_spr, n_scen, n_ents;
    unsigned char cell[1024];               /* per cell (z * 32 + x): its area, $FE line, $FF wall */
    /* things: world x, z, sprite */
    int n_th;
    int th_x[400], th_z[400], th_s[400];
    unsigned short th_cell[400];            /* (and its cell) */
} Map;

/* map pack entry k (P_*) / level blob table k (L_*): its address */
static const unsigned char *entry(const Map *m, int k) { return m->blk + be32(m->pk + 32 + 8 * k); }
static const unsigned char *ltab(const Map *m, int k) { return m->lvl + be16(m->lvl + 24 + 2 * k); }

/* one cell ahead for facings 0 N, 1 E, 2 S, 3 W (north is -z) */
static const int FX[4] = {0, 1, 0, -1}, FZ[4] = {-1, 0, 1, 0};

/* (rx, rz) relative to the camera -> depth d, lateral l, for facing f
   (l positive to the right) */
static void view_xf(int f, int rx, int rz, int *d, int *l)
{
    switch (f) {
    case 0: *d = -rz, *l = rx; break;
    case 1: *d = rx, *l = rz; break;
    case 2: *d = rz, *l = -rx; break;
    default: *d = -rx, *l = -rz; break;
    }
}

/* the view from cell c, facing f: how many pixels (in 1/4 of a column's
   rows, 64 columns) each texture and sprite gets, added to w */
/* The walls drawn are those the build listed as visible from (c, f) (the
   PVS: r_pvs_index / r_pvs, as the renderer uses them). Each column keeps
   its nearest wall. Things are counted where they are nearer than that
   wall, using each sprite column's opaque length (spr_cols). */
static void count_view(const Map *m, int c, int f, u64 *wtex, u64 *wspr)
{
    int gx = c & 31, gz = c >> 5, x, i, k;
    /* the camera: 16 units behind the cell's centre, as in the original */
    int vx = gx * 64 + 32 - 16 * FX[f], vz = gz * 64 + 32 - 16 * FZ[f];
    s64 colz[COLS];      /* depth x 256 of the nearest wall per column */
    int coltex[COLS];
    const unsigned char *walls = entry(m, P_WALLS), *pvi = entry(m, P_PVS_INDEX), *pvs = entry(m, P_PVS);
    /* the list of (c, f): a word offset into pvs (0: none); there, a count
       then the wall numbers */
    unsigned pv = be16(pvi + 2 * (c * 4 + f)), nw;
    for (x = 0; x < COLS; x++) colz[x] = (s64)1 << 40, coltex[x] = -1;   /* far, nothing */
    nw = pv ? be16(pvs + 2 * pv) : 0;
    for (i = 0; i < (int)nw; i++) {
        int wi = be16(pvs + 2 * (pv + 1 + i));
        /* a wall record (16 bytes): s16 ax, az, bx, bz, u16 texture, ...,
           byte 12 bit 0: two-sided */
        const unsigned char *w = walls + 16 * wi;
        int ax = sbe16(w), az = sbe16(w + 2), bx = sbe16(w + 4), bz = sbe16(w + 6), tex = be16(w + 8);
        int two = w[12] & 1, da, la, db, lb;
        /* one-sided walls are seen only from their front side (the sign of
           the cross product of A - camera and B - A) */
        s64 front = (s64)(ax - vx) * (bz - az) - (s64)(az - vz) * (bx - ax);
        if (front <= 0 && !two) continue;
        view_xf(f, ax - vx, az - vz, &da, &la);
        view_xf(f, bx - vx, bz - vz, &db, &lb);
        if (da < 1 && db < 1) continue;         /* wholly behind the camera */
        for (x = 0; x < COLS; x++) {
            /* the ray of the column's middle: l = k d, k = (2x + 1 - COLS) / COLS */
            s64 kn = 2 * x + 1 - COLS, kd = COLS;
            /* A + t (B - A) on the ray: (la + t dl) kd = kn (da + t dd) */
            s64 dd = db - da, dl = lb - la;
            s64 num = (s64)la * kd - kn * da, den = kn * dd - dl * kd;
            s64 depth256;
            if (den == 0) continue;
            /* t = num / den in [0, 1] */
            if (den > 0 ? (num < 0 || num > den) : (num > 0 || num < den)) continue;
            depth256 = ((s64)da * den + dd * num) * 256 / den;     /* depth at t, x 256 */
            if (depth256 < 256) continue;       /* nearer than 1 unit (or behind) */
            if (depth256 < colz[x]) colz[x] = depth256, coltex[x] = tex;
        }
    }
    for (x = 0; x < COLS; x++)
        if (coltex[x] >= 0 && coltex[x] < m->n_tex) {
            /* the wall's height in rows: 64 x 128 / depth, at most 160; a quarter
               (a 64-unit wall, a focal length of 128 pixels: the 256-pixel
               view spans 90 degrees) */
            s64 h = (s64)64 * 128 * 256 / colz[x];
            if (h > 160) h = 160;
            wtex[coltex[x]] += (u64)(h + 3) / 4;
        }
    /* the things: billboards 16 units in front of their cell's centre (as
       the original draws them), where nearer than the wall */
    {
        const unsigned char *vi = entry(m, P_VIS_INDEX), *vl = entry(m, P_VIS), *cols = entry(m, P_SPR_COLS);
        /* the cells whose things can show from (c, f): a list in vis
           ($FFFF ends it), or $FFFF for every cell */
        unsigned v = be16(vi + 2 * (c * 4 + f));
        unsigned char vis[1024];
        if (v == 0xFFFF)
            memset(vis, 1, sizeof vis);
        else {
            memset(vis, 0, sizeof vis);
            for (k = 0; be16(vl + 2 * (v + k)) != 0xFFFF; k++) vis[be16(vl + 2 * (v + k)) & 1023] = 1;
        }
        for (i = 0; i < m->n_th; i++) {
            int d, l, s = m->th_s[i];
            if (!vis[m->th_cell[i]] || s >= m->n_spr) continue;
            view_xf(f, m->th_x[i] - vx, m->th_z[i] - vz, &d, &l);
            d -= 16;                            /* the billboard's plane */
            if (d < 4) continue;                /* too near (or behind) */
            for (x = 0; x < COLS; x++) {
                /* the sprite column at the column's middle: a = k d - l + 32 */
                s64 kn = 2 * x + 1 - COLS;
                s64 a = (kn * d * 256 / COLS - (s64)l * 256) / 256 + 32;
                int len;
                if (a < 0 || a > 63) continue;          /* off the sprite */
                if ((s64)d * 256 > colz[x]) continue;   /* behind the wall */
                len = cols[s * 128 + a * 2 + 1];        /* (spr_cols: top, length per column) */
                if (!len) continue;
                {
                    s64 rows = (s64)len * 128 / d;
                    if (rows > 160) rows = 160;
                    wspr[s] += (u64)(rows + 3) / 4;
                }
            }
        }
    }
}

/* the histogram of asset a (its colours' texel counts) times w, into h */
static void asset_hist(const Map *m, int a, u64 w, Hist *h)
{
    /* a < n_tex: texture a (32 colours), else sprite a - n_tex (16) */
    const unsigned char *pal, *hist;
    int n, k;
    if (a < m->n_tex) {
        pal = entry(m, P_TEX_PAL) + 64 * a;
        hist = entry(m, P_TEX_HIST) + 64 * a;
        n = 32;
    } else {
        pal = entry(m, P_SPR_PAL) + 32 * (a - m->n_tex);
        hist = entry(m, P_SPR_HIST) + 32 * (a - m->n_tex);
        n = 16;
    }
    memset(h, 0, sizeof *h);
    for (k = 0; k < n; k++) {
        unsigned c = be16(pal + 2 * k), x = be16(hist + 2 * k);
        if (x) h->h[cidx(c)] += (u64)x * w;
    }
}

/* one area: its three palettes (pals[3][16]) and the textures' and
   sprites' palette (lab, n_tex + n_spr) */
static void do_area(const Map *m, int area, unsigned short pals[3][16], unsigned char *lab_out)
{
    int na = m->n_tex + m->n_spr, c, f, i, k, r;
    u64 *w = calloc((size_t)na, sizeof(u64));
    Hist *hs = malloc(sizeof(Hist) * (size_t)(na ? na : 1)), all, *own = malloc(sizeof(Hist));
    int *ord = malloc(sizeof(int) * (size_t)(na ? na : 1)), *lab = calloc((size_t)(na ? na : 1), sizeof(int)),
        *blab = calloc((size_t)(na ? na : 1), sizeof(int)), *used = malloc(sizeof(int) * (size_t)(na ? na : 1));
    int nu = 0, pal[3][15], np[3], bpal[3][15], bnp[3] = {0, 0, 0};
    int (*ownpal)[15] = malloc(sizeof(int[15]) * (size_t)(na ? na : 1));
    int *ownn = malloc(sizeof(int) * (size_t)(na ? na : 1));
    u64 best = ~0ULL;
    /* w: per asset (textures, then sprites), the pixels it gets summed over
       every view from the area. hs: each asset's histogram weighted by
       that; all: their sum; used: the assets seen (nu of them); ownpal /
       ownn: each seen asset's own best palette; lab: the group (0..2) of
       each seen asset in this start, blab / bpal / bnp the best start's */
    for (c = 0; c < 1024; c++)
        if (m->cell[c] == area)
            for (f = 0; f < 4; f++) count_view(m, c, f, w, w + m->n_tex);   /* every view from the area */
    memset(&all, 0, sizeof all);
    for (i = 0; i < na; i++) {
        u64 ww = w[i] ? isqrt(w[i] * 256 * 256) : 0;   /* w^(1/2) x 256 */
        if (i >= m->n_tex) ww *= 4;
        asset_hist(m, i, ww, &hs[i]);
        if (ww) {
            used[nu++] = i;
            for (k = 0; k < 512; k++) all.h[k] += hs[i].h[k];
        }
    }
    if (!nu) {   /* nothing seen (a closet): one palette of everything */
        for (i = 0; i < na; i++) {
            Hist t;
            asset_hist(m, i, 1, &t);
            for (k = 0; k < 512; k++) all.h[k] += t.h[k];
        }
        bnp[0] = bnp[1] = bnp[2] = choose(&all, 15, bpal[0]);
        memcpy(bpal[1], bpal[0], sizeof bpal[0]);
        memcpy(bpal[2], bpal[0], sizeof bpal[0]);
    } else {
        /* the used ones, heaviest first (ties: the lower slot) */
        u64 *tot = malloc(sizeof(u64) * (size_t)nu);
        for (i = 0; i < nu; i++) {
            tot[i] = 0;
            for (k = 0; k < 512; k++) tot[i] += hs[used[i]].h[k];
            ord[i] = i;
        }
        for (i = 1; i < nu; i++) {
            int t = ord[i], j = i;
            while (j > 0 && tot[ord[j - 1]] < tot[t]) ord[j] = ord[j - 1], j--;
            ord[j] = t;
        }
        free(tot);
        for (i = 0; i < nu; i++) ownn[i] = choose(&hs[used[i]], 15, ownpal[i]);
        /* RESTARTS runs of k-means over the assets (k = 3), keeping the
           one with the least total error */
        for (r = 0; r < RESTARTS; r++) {
            int sp[3][15], sn[3], it;
            u64 obj = 0;
            if (r == 0) {
                /* start 0: three one-colour palettes, the three colours
                   that best serve everything */
                int seeds[3], ns = choose(&all, 3, seeds);
                for (k = 0; k < 3; k++) sp[k][0] = seeds[k % ns], sn[k] = 1;
            } else {
                /* farthest first from the (r-1)-th heaviest */
                int ch[3], nch = 1;
                ch[0] = ord[(r - 1) % nu];
                while (nch < 3) {
                    int bi = 0;
                    u64 bv = 0;
                    int first = 1;
                    for (i = 0; i < nu; i++) {
                        u64 mn = ~0ULL;
                        int q;
                        for (q = 0; q < nch; q++) {
                            u64 e = err_of(&hs[used[i]], ownpal[ch[q]], ownn[ch[q]]);
                            if (e < mn) mn = e;
                        }
                        if (first || mn > bv) bv = mn, bi = i, first = 0;
                    }
                    ch[nch++] = bi;
                }
                for (k = 0; k < 3; k++) {
                    memcpy(sp[k], ownpal[ch[k]], sizeof sp[k]);
                    sn[k] = ownn[ch[k]];
                }
            }
            /* each asset to the starting palette that serves it best */
            for (i = 0; i < nu; i++) {
                u64 b = 0;
                for (k = 0; k < 3; k++) {
                    u64 e = err_of(&hs[used[i]], sp[k], sn[k]);
                    if (k == 0 || e < b) b = e, lab[i] = k;
                }
            }
            /* rounds: each group's palette from its assets' summed
               histogram (an empty group from everything), then each asset
               to the palette that serves it best; obj = the total error */
            for (it = 0; it < ROUNDS; it++) {
                for (k = 0; k < 3; k++) {
                    Hist g;
                    int any = 0;
                    memset(&g, 0, sizeof g);
                    for (i = 0; i < nu; i++)
                        if (lab[i] == k) {
                            int q;
                            any = 1;
                            for (q = 0; q < 512; q++) g.h[q] += hs[used[i]].h[q];
                        }
                    np[k] = choose(any ? &g : &all, 15, pal[k]);
                }
                obj = 0;
                for (i = 0; i < nu; i++) {
                    u64 b = 0;
                    for (k = 0; k < 3; k++) {
                        u64 e = err_of(&hs[used[i]], pal[k], np[k]);
                        if (k == 0 || e < b) b = e, lab[i] = k;
                    }
                    obj += b;
                }
            }
            if (obj < best) {
                best = obj;
                memcpy(bpal, pal, sizeof bpal);
                memcpy(bnp, np, sizeof bnp);
                memcpy(blab, lab, sizeof(int) * (size_t)nu);
            }
        }
    }
    /* the palettes as CRAM words in slots 1..15 (slot 0 is transparent;
       unused slots black) */
    for (k = 0; k < 3; k++) {
        pals[k][0] = 0;
        for (i = 0; i < 15; i++) pals[k][1 + i] = i < bnp[k] ? (unsigned short)cram(bpal[k][i]) : 0;
    }
    /* every texture and sprite: the palette that serves it best (the seen
       ones as grouped) */
    for (i = 0; i < na; i++) {
        int j, done = 0;
        for (j = 0; j < nu; j++)
            if (used[j] == i) {
                lab_out[i] = (unsigned char)blab[j];
                done = 1;
            }
        if (!done) {
            u64 b = 0;
            asset_hist(m, i, 1, own);
            lab_out[i] = 0;
            for (k = 0; k < 3; k++) {
                u64 e = err_of(own, bpal[k], bnp[k]);
                if (k == 0 || e < b) b = e, lab_out[i] = (unsigned char)k;
            }
        }
    }
    free(w);
    free(hs);
    free(own);
    free(ord);
    free(lab);
    free(blab);
    free(used);
    free(ownpal);
    free(ownn);
}

/* reads map `map` of campaign block blk (n bytes) into m: its counts,
   its areas (flood fill of the open cells, walls and line cells as
   borders) and its things (the entities not hidden at the start, and the
   scenery). Returns the number of areas + 1, or 0 if the map's pack or
   level blob is not where or what it should be. */
static int load_map(Map *m, const unsigned char *blk, size_t n, int map)
{
    const unsigned char *ed, *ex, *ez, *eh, *wb, *lc, *ds, *sc;
    int i, c, na = 0;
    unsigned long po = be32(blk + 32 + 32 * map);   /* the map's record: its pack's offset */
    if (po + 32 > n) return 0;
    m->blk = blk;
    m->pk = blk + po;
    if (memcmp(m->pk, "MDPK", 4)) return 0;
    m->n_walls = be16(m->pk + 8);
    m->n_tex = be16(m->pk + 10);
    m->n_spr = be16(m->pk + 12);
    m->n_scen = be16(m->pk + 14);
    m->lvl = entry(m, P_LEVEL);
    if (memcmp(m->lvl, "LVB2", 4)) return 0;
    m->n_ents = be16(m->lvl + 6);           /* (the blob header's entity count) */
    wb = ltab(m, L_WALL_BITS);
    lc = ltab(m, L_LINE_CELLS);
    /* the areas: walls $FF, line cells $FE, open cells $FD (not yet given
       an area); then each open cell not yet given one starts a new area,
       spread to its open neighbours (4-way) with a stack */
    for (c = 0; c < 1024; c++)
        m->cell[c] = (wb[c >> 3] >> (c & 7) & 1) ? 0xFF : (lc[c >> 3] >> (c & 7) & 1) ? 0xFE : 0xFD;
    for (c = 0; c < 1024; c++) {
        int st[1024], sp = 0;                   /* cells to visit */
        if (m->cell[c] != 0xFD) continue;
        if (na >= MAXA - 2) {   /* (never: too many areas) the rest shares the last */
            m->cell[c] = (unsigned char)(na - 1);
            continue;
        }
        m->cell[c] = (unsigned char)na;
        st[sp++] = c;
        while (sp) {
            int k = st[--sp], x = k & 31, z = k >> 5, nb[4], q;
            nb[0] = x > 0 ? k - 1 : -1;
            nb[1] = x < 31 ? k + 1 : -1;
            nb[2] = z > 0 ? k - 32 : -1;
            nb[3] = z < 31 ? k + 32 : -1;
            for (q = 0; q < 4; q++)
                if (nb[q] >= 0 && m->cell[nb[q]] == 0xFD) {
                    m->cell[nb[q]] = (unsigned char)na;
                    st[sp++] = nb[q];
                }
        }
        na++;
    }
    /* the things: entities shown at the start, scenery (props and decals) */
    ed = ltab(m, L_ENT_DEF0);
    ex = ltab(m, L_ENT_X0);
    ez = ltab(m, L_ENT_Z0);
    eh = ltab(m, L_ENT_HIDDEN0);
    ds = entry(m, P_DEF_SPR);
    sc = entry(m, P_SCENERY);
    m->n_th = 0;
    for (i = 0; i < m->n_ents && m->n_th < 400; i++) {
        if (eh[i]) continue;
        m->th_x[m->n_th] = ex[i] * 64 + 32;
        m->th_z[m->n_th] = ez[i] * 64 + 32;
        m->th_s[m->n_th] = ds[ed[i]];
        m->th_cell[m->n_th] = 0;
        m->n_th++;
    }
    /* scenery records (10 bytes): s16 x, z, sprite, ... */
    for (i = 0; i < m->n_scen && m->n_th < 400; i++) {
        const unsigned char *s = sc + 10 * i;
        m->th_x[m->n_th] = sbe16(s);
        m->th_z[m->n_th] = sbe16(s + 2);
        m->th_s[m->n_th] = sbe16(s + 4);
        m->n_th++;
    }
    /* each thing's cell (64 units a cell), clamped to the map */
    for (i = 0; i < m->n_th; i++) {
        int x = m->th_x[i] >> 6, z = m->th_z[i] >> 6;
        if (x < 0) x = 0;
        if (x > 31) x = 31;
        if (z < 0) z = 0;
        if (z > 31) z = 31;
        m->th_cell[i] = (unsigned short)(z * 32 + x);
    }
    return na + 1;   /* (areas + 1: 0 is a failure) */
}

/* (md_areas.h) builds the whole table: the header, then per map its
   colours, cells and area records, appended to a growing buffer */
size_t md_areas(const unsigned char *blk, size_t n, unsigned char **out)
{
    int nm, map;
    size_t cap, len;
    unsigned char *o;
    Map *m = malloc(sizeof(Map));
    *out = 0;
    if (!m || n < 32 || memcmp(blk, "MDCP", 4)) {
        free(m);
        return 0;
    }
    nm = (int)be16(blk + 6);                    /* the number of maps */
    cap = 8 + 4 * (size_t)nm + 64;
    o = calloc(cap, 1);
    memcpy(o, "MDAR", 4);
    o[5] = 1;                                   /* version 1 */
    o[6] = (unsigned char)(nm >> 8);
    o[7] = (unsigned char)nm;
    len = 8 + 4 * (size_t)nm;
    for (map = 0; map < nm; map++) {
        int na = load_map(m, blk, n, map) - 1, a, i, nc, cols[255];
        size_t rec, need;
        if (na < 0) {
            free(o);
            free(m);
            return 0;
        }
        /* the map's colours (those its textures and sprites use) */
        {
            const unsigned char *tp = entry(m, P_TEX_PAL), *th = entry(m, P_TEX_HIST);
            const unsigned char *sp = entry(m, P_SPR_PAL), *sh = entry(m, P_SPR_HIST);
            unsigned char seen[512];
            int t, k;
            memset(seen, 0, sizeof seen);
            nc = 0;
            for (t = 0; t < m->n_tex; t++)
                for (k = 0; k < 32; k++)
                    if (be16(th + 64 * t + 2 * k)) seen[cidx(be16(tp + 64 * t + 2 * k))] = 1;
            for (t = 0; t < m->n_spr; t++)
                for (k = 0; k < 16; k++)
                    if (be16(sh + 32 * t + 2 * k)) seen[cidx(be16(sp + 32 * t + 2 * k))] = 1;
            for (k = 0; k < 512 && nc < 255; k++)
                if (seen[k]) cols[nc++] = k;
        }
        /* an area record: 48 colours, a label per asset, 3 x nc nearest slots,
           padded to even */
        rec = 96 + (size_t)m->n_tex + (size_t)m->n_spr + 3 * (size_t)nc;
        rec += rec & 1;
        need = len + 4 + 2 * (size_t)nc + 1024 + rec * (size_t)na;
        if (need > cap) {
            cap = need * 2;
            o = realloc(o, cap);
        }
        /* the map's offset in the header, then its part */
        o[8 + 4 * map] = (unsigned char)(len >> 24);
        o[9 + 4 * map] = (unsigned char)(len >> 16);
        o[10 + 4 * map] = (unsigned char)(len >> 8);
        o[11 + 4 * map] = (unsigned char)len;
        o[len] = (unsigned char)na;
        o[len + 1] = (unsigned char)m->n_tex;
        o[len + 2] = (unsigned char)m->n_spr;
        o[len + 3] = (unsigned char)nc;
        len += 4;
        for (i = 0; i < nc; i++) {
            o[len + 2 * i] = (unsigned char)(cram(cols[i]) >> 8);
            o[len + 2 * i + 1] = (unsigned char)cram(cols[i]);
        }
        len += 2 * (size_t)nc;
        memcpy(o + len, m->cell, 1024);
        for (i = 0; i < 1024; i++)
            if (o[len + i] == 0xFD) o[len + i] = 0xFF;  /* (any cell left without an area) */
        len += 1024;
        for (a = 0; a < na; a++) {
            unsigned short pals[3][16];
            unsigned char *lab = o + len + 96, *near = lab + m->n_tex + m->n_spr;
            int k, c;
            memset(o + len, 0, rec);
            do_area(m, a, pals, lab);           /* the palettes and the labels */
            for (k = 0; k < 48; k++) {
                o[len + 2 * k] = (unsigned char)(pals[k / 16][k % 16] >> 8);
                o[len + 2 * k + 1] = (unsigned char)pals[k / 16][k % 16];
            }
            /* each colour's nearest slot (1..15) in each palette */
            for (k = 0; k < 3; k++)
                for (c = 0; c < nc; c++) {
                    int q, best = 1, bd = -1;
                    for (q = 1; q < 16; q++) {
                        int d = dist(cols[c], cidx(pals[k][q]));
                        if (bd < 0 || d < bd) bd = d, best = q;
                    }
                    near[k * nc + c] = (unsigned char)best;
                }
            len += rec;
        }
    }
    free(m);
    *out = o;
    return len;
}

#ifdef MD_AREAS_MAIN
#include <stdio.h>
/* the build's program (build.sh): reads the campaign block, writes the
   areas table; exit status 0 on success */
int main(int argc, char **argv)
{
    FILE *f;
    long n;
    unsigned char *blk, *out;
    size_t len;
    if (argc != 3) {
        fprintf(stderr, "usage: md_areas campaign.bin areas.bin\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) return perror(argv[1]), 1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    blk = malloc((size_t)n);
    if (fread(blk, 1, (size_t)n, f) != (size_t)n) return perror(argv[1]), 1;
    fclose(f);
    len = md_areas(blk, (size_t)n, &out);       /* the table */
    if (!len) {
        fprintf(stderr, "%s: not a campaign block\n", argv[1]);
        return 1;
    }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, len, f) != len) return perror(argv[2]), 1;
    fclose(f);
    printf("areas: %u maps, %u KB\n", (unsigned)(blk[6] << 8 | blk[7]), (unsigned)(len / 1024));
    return 0;
}
#endif
