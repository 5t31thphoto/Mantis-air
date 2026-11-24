// The land and its airfields (see terrain.h). Dreamt ahead, forgotten behind: coherent, never stored.
#include "terrain.h"
#include "universe.h"
#include "mem.h"
#include "airfield.h"
#include <math.h>
#include <string.h>

namespace mantis {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth01(float t) { t = clampf(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }
static inline float sq(float v) { return v * v; }

static uint32_t s_seed = 1;

// =====================================================================================================
//  Natural land
// =====================================================================================================
float terrainWaterLevel(float x, float z) {
    const Land& L = journeyLand();
    float thresh = 0.20f + 0.32f * L.water;                         // more water: more of the low ground floods
    return journeyBase(x, z) + (thresh - 0.45f) * L.hills * 2.2f - 0.6f;
}

// ---- mountain massifs: a slow noise whose high ground rises (the valleys keep the biome's own land), with ridged
// crests. It changes over kilometres, so it's sampled on a 40 m lattice (remembered) and blended: a baked cell costs a
// few multiplies, not five octaves of noise. Its shape is fixed; the biome sets how tall (Land::macro).
static constexpr float MASSIF_S = 40.f;
struct MassifMemo { int i, j; float v; };
static MassifMemo s_mm[64];
static float massifRaw(float x, float z) {
    float m = fbm2(x * 0.00042f, z * 0.00042f, 3, s_seed + 0x3A5u);
    float r = fbm2(x * 0.0012f, z * 0.0012f, 2, s_seed + 0x3A6u);
    r = 1.f - fabsf(2.f * r - 1.f);
    float k = fmaxf(0.f, m - 0.42f) * 2.7f;
    return k * k * (0.7f + 0.55f * r * r);
}
static inline float massifAt(int i, int j) {
    MassifMemo& m = s_mm[(i & 7) | ((j & 7) << 3)];
    if (m.i != i || m.j != j) { m.i = i; m.j = j; m.v = massifRaw(i * MASSIF_S, j * MASSIF_S); }
    return m.v;
}
static float massif(float x, float z) {
    const float fx = x * (1.f / MASSIF_S), fz = z * (1.f / MASSIF_S);
    const int i = (int)floorf(fx), j = (int)floorf(fz);
    const float u = fx - i, w = fz - j;
    const float a = massifAt(i, j), b = massifAt(i + 1, j), c = massifAt(i, j + 1), d = massifAt(i + 1, j + 1);
    return a + (b - a) * u + (c - a) * w + (a - b - c + d) * u * w;
}

static inline float naturalCore(float x, float z, const Land& L) {
    float h = journeyBase(x, z);
    float n1 = fbm2(x * 0.0035f, z * 0.0035f, 4, s_seed + 0x11);
    h += (n1 - 0.45f) * L.hills * 2.2f;
    if (L.ridge > 0.3f) {                                            // sharp crests, Seadiver style
        float r = fbm2(x * 0.0062f, z * 0.0062f, 3, s_seed + 0x41);
        r = 1.f - fabsf(2.f * r - 1.f);
        h += (r * r * r - 0.3f) * L.ridge * 1.5f;
    }
    if (L.cliff > 0.3f) {                                            // terraces, mesas, rock steps
        float c = fbm2(x * 0.0042f, z * 0.0042f, 2, s_seed + 0xC1) * 4.f;
        float fl = floorf(c);
        float st = fl + smooth01((c - fl - 0.72f) * 5.f);
        h += (st * 0.25f - 0.45f) * L.cliff * 1.6f;
    }
    if (L.macro > 1.f) h += massif(x, z) * L.macro;
    return h;
}
float terrainNatural(float x, float z) {
    const Land& L = journeyLand();
    float h = naturalCore(x, z, L);
    // medium bumps and fine roughness (what the wheels feel on a field)
    h += (vnoise2(x * 0.045f, z * 0.045f, s_seed + 0x70) - 0.5f) * L.rough * 3.2f;
    h += (vnoise2(x * 0.17f, z * 0.17f, s_seed + 0x71) - 0.5f) * L.rough * 0.55f;
    return h;
}
float terrainNaturalLod(float x, float z) { return naturalCore(x, z, journeyLand()); }

// ---- big rivers: winding contour lines of a very slow noise (about one a kilometre), carved below the water table so
// the lakes' own rules fill them: one level, banks above it. A 22 m channel, then the valley: gentle in soft country,
// sheer walls (a gorge) where the land is rocky or desert.
// how sheer the valley's walls are, 0 (soft) to 1 (a gorge): smooth in the land's numbers, so a biome blend eases it
static inline float riverGorge(const Land& L) {
    return clampf(fmaxf(fmaxf((L.cliff - 0.2f) * 2.5f, (L.ridge - 0.4f) * 2.5f), L.ground == MAT_SAND ? 1.f : 0.f), 0.f, 1.f);
}
// ---- slow noise on a lattice: the rivers' and the trails' fields change over hundreds of metres, so they're sampled
// at a lattice's corners (remembered: 16 squares) and blended, with the blend's own gradient. A baked cell costs a few
// multiplies instead of one to three 2-octave noises (that had made baking a quarter dearer: choppy at speed).
struct LatMemo { int i, j; float v[4]; };
static LatMemo s_mRiver[16], s_mTrail[16], s_mBike[16];
static void latReset() { for (int k = 0; k < 16; k++) s_mRiver[k].i = s_mTrail[k].i = s_mBike[k].i = 0x7fffffff; }
static float latField(LatMemo* memo, float x, float z, float S, float f, uint32_t salt, float& gx, float& gz) {
    const float fx = x * (1.f / S), fz = z * (1.f / S);
    const int i = (int)floorf(fx), j = (int)floorf(fz);
    LatMemo& m = memo[(i & 3) | ((j & 3) << 2)];
    if (m.i != i || m.j != j) {
        m.i = i; m.j = j;
        const float x0 = i * S * f, z0 = j * S * f, x1 = (i + 1) * S * f, z1 = (j + 1) * S * f;
        m.v[0] = fbm2(x0, z0, 2, salt); m.v[1] = fbm2(x1, z0, 2, salt); m.v[2] = fbm2(x0, z1, 2, salt); m.v[3] = fbm2(x1, z1, 2, salt);
    }
    const float u = fx - i, w = fz - j, a = m.v[0], b = m.v[1], c = m.v[2], d = m.v[3];
    gx = ((b - a) * (1.f - w) + (d - c) * w) * (1.f / S);
    gz = ((c - a) * (1.f - u) + (d - b) * u) * (1.f / S);
    return a + (b - a) * u + (c - a) * w + (a - b - c + d) * u * w;
}

// The rivers are filled by the lakes' rule (one level for all the water that touches): that holds while the base plane
// lies (nearly) flat. While it leans (a biome blending into one at another height) the water table leans with it, so
// the rivers fade out (shallow dry valleys, then nothing) rather than stand at one level up a slope.
static float riverK() { return clampf(1.f - (journeyTilt() - 0.003f) * (1.f / 0.003f), 0.f, 1.f); }
static float riverSd(float x, float z, float& ux, float& uz) {     // signed distance to the river's centre line (m)
    ux = uz = 0.f;
    if (riverK() <= 0.f) return 1e4f;
    float gx, gz;
    const float n = latField(s_mRiver, x, z, 24.f, 0.0011f, s_seed + 0x41A3u, gx, gz) - 0.5f;
    if (fabsf(n) > 0.3f) return n > 0.f ? 1e4f : -1e4f;           // nowhere near one (100 m and more)
    const float gl = sqrtf(gx * gx + gz * gz);                     // per metre
    if (gl < 1e-6f) return n > 0.f ? 1e4f : -1e4f;
    ux = gx / gl; uz = gz / gl;
    return n / gl;
}
static float riverProfile(float d, float wl, float gorge) {          // the carved land's height d metres from the centre
    const float W = 11.f;
    if (d < W - 3.f) return wl - 2.5f;
    if (d < W) return wl - 2.5f + (d - (W - 3.f)) * (3.3f / 3.f);  // the bank, up to 0.8 m above the water
    return wl + 0.8f + (d - W) * (0.32f + 2.08f * gorge);
}
// the land with the river carved in (n: the natural height at (x, z))
static inline float riverCarve(float n, float sd, float wl, float gorge) {
    const float d = fabsf(sd);
    if (d > 75.f) return n;
    float c = riverProfile(d, wl, gorge);
    if (c >= n) return n;
    return n - (n - c) * (1.f - (1.f - gorge) * smooth01((d - 45.f) * (1.f / 30.f))) * riverK();   // a soft valley eases out into the hills
}
float terrainRiverDepth(float x, float z) {                       // how far the river's valley is carved below the land
    float ux, uz, sd = riverSd(x, z, ux, uz);
    if (fabsf(sd) > 75.f) return 0.f;
    float n = terrainNatural(x, z);
    return n - riverCarve(n, sd, terrainWaterLevel(x, z), riverGorge(journeyLand()));
}
float terrainRiverDist(float x, float z) { float ux, uz; return fabsf(riverSd(x, z, ux, uz)); }

// =====================================================================================================
//  Caches
// =====================================================================================================
static Grid s_near, s_far, s_vast;
static float s_lagYaw = 0.f, s_time = 0.f;
static int s_frame = 0;

const Grid& terrainNear() { return s_near; }
const Grid& terrainFar() { return s_far; }
const Grid& terrainVast() { return s_vast; }
float terrainMaxH() { return fmaxf(s_near.maxH, fmaxf(s_far.maxH, s_vast.maxH)); }

static inline uint8_t encN(float nx, float nz) {
    int a = (int)lroundf(clampf(nx, -1.f, 1.f) * 7.f) + 8, b = (int)lroundf(clampf(nz, -1.f, 1.f) * 7.f) + 8;
    return (uint8_t)((a << 4) | b);
}
static uint8_t pickVeg(float x, float z, int ci, int cj, uint8_t mat) {
    const Land& L = journeyLand();
    if (mat == MAT_RUNWAY || mat == MAT_LOT || mat == MAT_WATER) return VEG_NONE;
    uint32_t h = hash32((uint32_t)ci * 73856093u ^ (uint32_t)cj * 19349663u ^ s_seed);
    float cl = vnoise2(x * 0.011f, z * 0.011f, s_seed + 0x5E);            // groves and clearings
    float grove = smooth01((cl - 0.22f) * 2.2f) * 1.25f;
    float k = 0.17f;
    float r = hashf(h);
    float acc = 0.f;
    acc += L.pine * grove * k;           if (r < acc) return VEG_PINE;
    acc += L.broad * grove * k;          if (r < acc) return VEG_BROAD;
    acc += L.redwood * grove * k * 0.6f; if (r < acc) return VEG_REDWOOD;
    acc += L.snag * k * 0.4f;            if (r < acc) return VEG_SNAG;
    acc += L.cactus * k * 0.35f;         if (r < acc) return VEG_CACTUS;
    acc += L.boulder * k * 0.3f;         if (r < acc) return VEG_BOULDER;
    acc += L.bush * k * 0.6f;            if (r < acc) return mat == MAT_SNOW ? VEG_NONE : VEG_BUSH;
    acc += L.flowers * k * 0.9f;         if (r < acc) return (mat == MAT_GRASS) ? VEG_FLOWERS : VEG_NONE;
    return VEG_NONE;
}

// ---- farm plots: open, gentle grass country is parcelled into 56 m plots; some are planted (corn, potatoes or grain),
// some fenced as pasture (cattle), each with a 3 m grass headland. The code a plot's cells keep in Cell::extra (0: not
// farmed): bit 7 farmed, bits 5-6 the use (0 corn, 1 potatoes, 2 grain, 3 pasture), bit 4 the rows' heading. Its low
// nibble (vegetation) stays 0: nothing grows in it.
uint8_t terrainPlot(int pi, int pj) {
    const Land& L = journeyLand();
    if (L.ground != MAT_GRASS) return 0;
    float farm = clampf((0.6f - (L.pine + L.broad + L.redwood)) * 2.5f, 0.f, 1.f) * clampf((24.f - L.hills) * 0.1f, 0.f, 1.f);
    if (farm <= 0.f) return 0;
    uint32_t hp = hash32((uint32_t)pi * 0x9E3779B1u ^ (uint32_t)pj * 0x85EBCA77u ^ s_seed ^ 0xFA4Du);
    float r = hashf(hp);
    if (r >= 0.65f * farm) return 0;
    uint8_t kind = r < 0.45f * farm ? (uint8_t)((hp >> 8) % 3u) : 3;
    return (uint8_t)(0x80 | (kind << 5) | (((hp >> 12) & 1u) << 4));
}
static uint8_t cropAt(float x, float z) {
    const float P = PLOT_M;
    float fi = floorf(x * (1.f / P)), fj = floorf(z * (1.f / P));
    float u = x - fi * P, v = z - fj * P;
    if (u < 3.f || v < 3.f || u > P - 3.f || v > P - 3.f) return 0;     // the headland
    return terrainPlot((int)fi, (int)fj);
}
uint8_t terrainNearExtra(float x, float z) {
    const Grid& g = s_near;
    if (!g.c) return 0;
    int i = (int)floorf(x * g.inv + 0.5f), j = (int)floorf(z * g.inv + 0.5f);   // the cell whose sample point is nearest
    if (i < g.ox || j < g.oz || i >= g.ox + g.N || j >= g.oz + g.N) return 0;
    const Cell& c = g.c[gridWrap(j, g.N) * g.N + gridWrap(i, g.N)];
    return (c.mat & 15) < MAT_WATER ? c.extra : 0;
}


static bool fallAt(int ci, int cj, const Land& L) {
    if (L.ground != MAT_GRASS || L.water + L.pine + L.redwood + L.broad * 0.6f < 0.7f) return false;
    int bi = ci >> 4, bj = cj >> 4;
    uint32_t hb = hash32((uint32_t)bi * 0x9E3779B1u ^ (uint32_t)bj * 0x7FEB352Du ^ s_seed ^ 0xFA11u);
    if (hashf(hb) > 0.7f) return false;
    return ci - (bi << 4) == (int)((hb >> 8) & 15u) && cj - (bj << 4) == (int)((hb >> 16) & 15u);
}

// ---- lakes: one level per lake --------------------------------------------------------------------------
// A cell is dreamt now if it is inside the window and not a cell of the line (or fill) still being dreamt.
static bool cellReady(const Grid& g, int i, int j) {
    if (!g.c || i < g.ox || j < g.oz || i >= g.ox + g.N || j >= g.oz + g.N) return false;
    int li = i - g.ox, lj = j - g.oz;
    if (g.jobAxis == 2) return lj * g.N + li < g.jobPos;                  // the whole window is being filled, row by row
    if (g.jobPos < g.N) {                                                // a new edge line: its later cells are last lap's
        if (g.jobAxis == 0 && i == g.jobLine && lj >= g.jobPos) return false;
        if (g.jobAxis == 1 && j == g.jobLine && li >= g.jobPos) return false;
    }
    return true;
}
static inline Cell& cellAt(const Grid& g, int i, int j) { return g.c[gridWrap(j, g.N) * g.N + gridWrap(i, g.N)]; }
static inline bool cellWater(const Cell& c) { return (c.mat & 15) == MAT_WATER; }
static inline bool cellRoadish(const Cell& c) { int m = c.mat & 15; return m == MAT_RUNWAY || m == MAT_LOT; }
static void cellDryUp(Grid& g, Cell& o, float level) {                   // water that would stand above a runway: bank
    o.h = (int16_t)clampf((level + 0.05f) * 10.f, -32000.f, 32000.f);
    o.mat = (uint8_t)((SURF_GRASS << 4) | MAT_GRASS); o.extra = 0; o.nrm = encN(0.f, 0.f);
    if (level + 0.05f > g.maxH) g.maxH = level + 0.05f;
}
static const int LAKE_DI[4] = {1, -1, 0, 0}, LAKE_DJ[4] = {0, 0, 1, -1};

// ---- the land's shadows: does a ridge stand between this cell and the sun? Looked for along the sun's heading out
// to 42 cells (105 m near, 336 m far) and kept as one bit a cell; worked out when a cell is dreamt and swept again
// slowly as the sun moves. The renderer leaves the direct sun out of a shaded cell's light (soft edges come from its
// usual blend of four cells).
static float s_sunUx = 1.f, s_sunUz = 0.f, s_sunTan = 1.f;
static bool s_sunUp = false;
void terrainSetSun(float az, float elev) {
    s_sunUx = cosf(az); s_sunUz = sinf(az);
    s_sunUp = elev > 0.01f;
    s_sunTan = tanf(elev < 1.4f ? elev : 1.4f);
}
static void shadowCell(Grid& g, int ci, int cj) {
    if (!g.sh) return;
    const int N = g.N;
    const int k = gridWrap(cj, N) * N + gridWrap(ci, N);
    bool shade = false;
    const int cm = g.c[k].mat & 15;
    (void)cm;
    if (s_sunUp) {
        const float h0 = g.c[k].h * 0.1f + 0.5f;                 // half a metre: no shade from the cell's own grain
        static const uint8_t D[12] = {1, 2, 3, 4, 6, 8, 11, 15, 20, 26, 33, 42};
        for (int s = 0; s < 12; s++) {
            int i = ci + (int)floorf(s_sunUx * D[s] + 0.5f), j = cj + (int)floorf(s_sunUz * D[s] + 0.5f);
            if (i < g.ox || j < g.oz || i >= g.ox + N || j >= g.oz + N) break;     // past the window: open sky
            if (g.c[gridWrap(j, N) * N + gridWrap(i, N)].h * 0.1f > h0 + D[s] * g.cell * s_sunTan) { shade = true; break; }
        }
    }
    const uint8_t m = (uint8_t)(1u << (k & 7));
    if (shade) g.sh[k >> 3] |= m; else g.sh[k >> 3] &= (uint8_t)~m;
}
static void shadowStep(Grid& g, int n) {                         // the slow sweep: n cells a frame, round the window
    if (!g.c || !g.sh || !s_sunUp) return;
    const int N = g.N, total = N * N;
    const int wx = gridWrap(g.ox, N), wz = gridWrap(g.oz, N);
    for (int c = 0; c < n; c++) {
        int k = g.shCur; g.shCur = k + 1 >= total ? 0 : k + 1;
        int sx = k % N, sz = k / N;
        shadowCell(g, g.ox + (sx - wx + N) % N, g.oz + (sz - wz + N) % N);
    }
}


// a field's crop, standing on the soil: corn 1 m, grain 0.5 m (the cell is drawn that much higher; terrainGround
// takes it back off for the wheels)
static inline float cropRaise(const Cell& k) {
    if ((k.mat & 15) != MAT_SOIL || !(k.extra & 0x80)) return 0.f;
    const int kind = (k.extra >> 5) & 3;
    return kind == 0 ? 1.f : (kind == 2 ? 0.5f : 0.f);
}

static void bakeCell(Grid& g, int ci, int cj) {
    const bool vast = &g == &s_vast, nearG = &g == &s_near;
    const float x = ci * g.cell, z = cj * g.cell;
    const Land& L = journeyLand();
    const float e = g.cell * 0.5f;
    float n0, nX, nZ;
    if (vast) { n0 = terrainNaturalLod(x, z); nX = terrainNaturalLod(x + e, z); nZ = terrainNaturalLod(x, z + e); }
    else { n0 = terrainNatural(x, z); nX = terrainNatural(x + e, z); nZ = terrainNatural(x, z + e); }
    const float wl = terrainWaterLevel(x, z);
    {   float ux, uz, sd = riverSd(x, z, ux, uz);                    // the river and its valley
        if (fabsf(sd) < 80.f) {
            const float gz = riverGorge(L);
            n0 = riverCarve(n0, sd, wl, gz); nX = riverCarve(nX, sd + ux * e, wl, gz); nZ = riverCarve(nZ, sd + uz * e, wl, gz);
        }
    }
    float h = n0, hX = nX, hZ = nZ;
    uint8_t surf = SURF_GRASS, mat = L.ground, slot = 0;
    bool field = false;                                           // inside an airfield's flat
    {   uint8_t m2 = 255, s2 = 0, sl = 0, dm = 255, ds = 0, dsl = 0; bool in0 = false, inX = false, inZ = false;
        h = airfieldShape(x, z, h, m2, s2, sl, in0);
        hX = airfieldShape(x + e, z, hX, dm, ds, dsl, inX);
        hZ = airfieldShape(x, z + e, hZ, dm, ds, dsl, inZ);
        if (m2 != 255) { mat = m2; surf = s2; }
        slot = sl; field = in0;
    }
    const bool strip = mat == MAT_RUNWAY || mat == MAT_LOT;
    float depth = 0.f;
    // Lakes are level. A lake keeps the level of the first part of it that was dreamt: a cell beside water already
    // dreamt (this cache or the other) takes that water's level. Where the ground is below that level but isn't
    // lake (or is an airfield), the land rises to just above the water. (The vast cache, seen from far off, takes the
    // water table as it is.)
    const bool canWater = !strip && !field;
    float lev = wl; bool known = false;
    if (!vast) {
        for (int q = 0; q < 4; q++) {
            int ni = ci + LAKE_DI[q], nj = cj + LAKE_DJ[q];
            if (!cellReady(g, ni, nj)) continue;
            const Cell& o = cellAt(g, ni, nj);
            if (cellWater(o)) { float l = o.h * 0.1f; if (!known || l < lev) lev = l; known = true; }
        }
        if (!known) {                                                     // the same lake in the other cache
            const Grid& og = nearG ? s_far : s_near;
            int oi = (int)floorf(x * og.inv), oj = (int)floorf(z * og.inv);
            if (cellReady(og, oi, oj) && cellWater(cellAt(og, oi, oj))) { lev = cellAt(og, oi, oj).h * 0.1f; known = true; }
        }
    }
    if (canWater && h < lev) {
        bool water = h < wl || known;
        for (int q = 0; water && !vast && q < 4; q++) {                   // nothing beside it may sit lower than the water
            int ni = ci + LAKE_DI[q], nj = cj + LAKE_DJ[q];
            if (!cellReady(g, ni, nj)) continue;
            const Cell& o = cellAt(g, ni, nj); float oh = o.h * 0.1f;
            if (cellWater(o)) { if (fabsf(oh - lev) > 0.05f) { lev = fmaxf(lev, oh); water = false; } }   // two lakes meet: a bank between
            else if (cellRoadish(o) && oh < lev + 0.05f) water = false;
        }
        if (water) { depth = lev - h; mat = MAT_WATER; surf = SURF_MUD; }
        else { h = lev + 0.05f; hX = fmaxf(hX, h); hZ = fmaxf(hZ, h); }
    }
    float nx = -(hX - h) / e, nz = -(hZ - h) / e;
    float inv = 1.f / sqrtf(nx * nx + 1.f + nz * nz);
    nx *= inv; nz *= inv; float ny = inv;
    uint8_t crop = 0;
    if (!strip && mat != MAT_WATER) {
        float p = vnoise2(x * 0.06f, z * 0.06f, s_seed + 0xD1);
        float base = journeyBase(x, z);
        if (h - base > L.snowLine + (p - 0.5f) * 14.f + (vast ? 0.f : 0.f)) { mat = MAT_SNOW; }
        else if (ny < 0.72f + 0.08f * p) { mat = MAT_ROCK; }
        else if (L.ground == MAT_SAND) { mat = p > 0.66f ? MAT_SOIL : MAT_SAND; }
        else if (L.ground == MAT_SNOW) { mat = p > 0.8f ? MAT_ROCK : MAT_SNOW; }
        else { mat = p > 0.70f ? (L.water > 0.7f ? MAT_MUD : MAT_SOIL) : MAT_GRASS; }
        if ((mat == MAT_GRASS || mat == MAT_SOIL) && ny > 0.95f && !field) {
            crop = cropAt(x, z);
            if (crop && (crop & 0x60) != 0x60) mat = MAT_SOIL;           // tilled ground (pasture stays grass)
        }
        if (mat == MAT_ROCK && nearG && ny < 0.6f && fallAt(ci, cj, L)) crop = 0x80;   // a waterfall's lip
        if (field) mat = MAT_GRASS;                                      // the airfield's mown grass
        static const uint8_t TYRE[] = {SURF_GRASS, SURF_DIRT, SURF_ROCK, SURF_SNOW, SURF_SAND, SURF_MUD};
        surf = TYRE[mat];
    }
    Cell& c = g.c[gridWrap(cj, g.N) * g.N + gridWrap(ci, g.N)];
    float hs = mat == MAT_WATER ? lev : h;                       // water: the lake's level
    c.h = (int16_t)clampf(hs * 10.f, -32000.f, 32000.f);
    c.across = 30000;
    c.along = 0;
    c.mat = (uint8_t)((surf << 4) | mat);
    c.nrm = mat == MAT_WATER ? encN(0.f, 0.f) : encN(nx, nz);
    if (strip) c.extra = slot;
    else if (mat == MAT_WATER) c.extra = (uint8_t)clampf(depth * 20.f, 0.f, 255.f);
    else if (crop) c.extra = crop & 0x80 ? crop : 0;               // a field or pasture, a waterfall's lip: no trees
    else if (field) c.extra = 0;
    else if (nearG) c.extra = pickVeg(x, z, ci, cj, mat);
    else {                                                        // far and vast: forest canopy height in metres
        float cl = vnoise2(x * 0.011f, z * 0.011f, s_seed + 0x5E);
        float dens = (L.pine + L.broad + L.redwood * 1.5f) * smooth01((cl - 0.22f) * 2.2f);
        c.extra = (dens > 0.35f && mat != MAT_ROCK && mat != MAT_SNOW) ? (uint8_t)clampf(5.f + dens * 6.f + L.redwood * 12.f, 0.f, 22.f) : 0;
    }
    if (!vast) for (int q = 0; q < 4; q++) {
        int ni = ci + LAKE_DI[q], nj = cj + LAKE_DJ[q];
        if (!cellReady(g, ni, nj)) continue;
        Cell& o = cellAt(g, ni, nj); float oh = o.h * 0.1f;
        if (mat == MAT_WATER) {                                           // the land beside the water rises above it
            if (!cellWater(o) && !cellRoadish(o) && oh < hs + 0.05f) {
                o.h = (int16_t)clampf((hs + 0.05f) * 10.f, -32000.f, 32000.f);
                if (hs + 0.05f > g.maxH) g.maxH = hs + 0.05f;
            }
        } else if (cellWater(o) && oh > hs - 0.05f) {
            if (strip) cellDryUp(g, o, oh);                                // never water above a runway
            else { hs = oh + 0.05f; c.h = (int16_t)clampf(hs * 10.f, -32000.f, 32000.f); }   // the shore rises above it
        }
    }
    {   const float cr = cropRaise(c);                             // corn and grain stand above the ground (drawn; the
        if (cr > 0.f) { hs += cr; c.h = (int16_t)clampf(hs * 10.f, -32000.f, 32000.f); }   // wheels go down to the soil)
    }
    if (hs > g.maxH) g.maxH = hs;
    shadowCell(g, ci, cj);
}

static void gridInit(Grid& g, int N, float cell) {
    g.N = N; g.cell = cell; g.inv = 1.f / cell;
    if (!g.c) g.c = (Cell*)(N == 96 ? memFast(sizeof(Cell) * N * N) : memSlow(sizeof(Cell) * N * N));   // near: internal RAM (the hottest memory)
    if (!g.sh) g.sh = (uint8_t*)memFast((size_t)(N * N + 7) / 8);   // 1.1 KB near, 0.5 KB far, 2 KB vast: read by every lit sample
    if (g.sh) memset(g.sh, 0, (size_t)(N * N + 7) / 8);
    g.shCur = 0;
    g.jobAxis = -1; g.jobPos = N; g.rebake = -1; g.maxH = -1e9f;
}

static void gridFill(Grid& g, float cx, float cz) {
    if (!g.c) return;
    g.ox = (int)floorf(cx * g.inv) - g.N / 2;
    g.oz = (int)floorf(cz * g.inv) - g.N / 2;
    g.maxH = -1e9f;
    g.jobAxis = 2;                                                        // filling: cells not yet reached aren't ready
    for (int j = 0; j < g.N; j++) for (int i = 0; i < g.N; i++) { g.jobPos = j * g.N + i; bakeCell(g, g.ox + i, g.oz + j); }
    g.jobAxis = -1; g.jobPos = g.N; g.rebake = -1;
}

static void gridScroll(Grid& g, float cx, float cz, int budget) {
    if (!g.c) return;
    int wx = (int)floorf(cx * g.inv) - g.N / 2;
    int wz = (int)floorf(cz * g.inv) - g.N / 2;
    if (abs(wx - g.ox) > g.N / 2 || abs(wz - g.oz) > g.N / 2) { gridFill(g, cx, cz); return; }
    while (budget > 0) {
        if (g.jobPos >= g.N) {
            if (g.ox < wx) { g.ox++; g.jobAxis = 0; g.jobLine = g.ox + g.N - 1; }
            else if (g.ox > wx) { g.ox--; g.jobAxis = 0; g.jobLine = g.ox; }
            else if (g.oz < wz) { g.oz++; g.jobAxis = 1; g.jobLine = g.oz + g.N - 1; }
            else if (g.oz > wz) { g.oz--; g.jobAxis = 1; g.jobLine = g.oz; }
            else break;
            g.jobPos = 0;
        }
        int n = g.N - g.jobPos < budget ? g.N - g.jobPos : budget;
        for (int k = 0; k < n; k++, g.jobPos++) {
            if (g.jobAxis == 0) bakeCell(g, g.jobLine, g.oz + g.jobPos);
            else bakeCell(g, g.ox + g.jobPos, g.jobLine);
        }
        budget -= n;
    }
    if (budget > 0 && g.rebake >= 0) {
        int total = g.N * g.N;
        while (budget-- > 0 && g.rebake < total) {
            int k = g.rebake++;
            bakeCell(g, g.ox + k % g.N, g.oz + k / g.N);
        }
        if (g.rebake >= total) g.rebake = -1;
    }
}


static void gridMax(Grid& g) {
    if (!g.c) return;
    float m = -1e9f;
    for (int k = 0; k < g.N * g.N; k++) { float h = g.c[k].h * 0.1f + (g.c[k].mat & 15) * 0.f; if (h > m) m = h; }
    g.maxH = m + 40.f;                            // + the tallest tree / canopy
}


// the vast cache is swept again all the time (about every 6 s): the land far off follows the journey's slow changes
static void vastSweep(int n) {
    Grid& g = s_vast;
    if (!g.c || g.jobPos < g.N) return;                       // (a line still being dreamt goes first)
    const int total = g.N * g.N;
    if (g.rebake < 0 || g.rebake >= total) g.rebake = 0;
    while (n-- > 0) {
        int k = g.rebake++;
        bakeCell(g, g.ox + k % g.N, g.oz + k / g.N);
        if (g.rebake >= total) { g.rebake = 0; break; }
    }
}

static void windowCentres(float x, float z, float& nx, float& nz, float& fx, float& fz, float& vx, float& vz) {
    float c = cosf(s_lagYaw), s = sinf(s_lagYaw);
    nx = x + c * 45.f; nz = z + s * 45.f;
    fx = x + c * 150.f; fz = z + s * 150.f;
    vx = x + c * 900.f; vz = z + s * 900.f;
}
static float s_x = 0.f, s_z = 0.f;
void terrainRebuild() {
    float nx, nz, fx, fz, vx, vz;
    windowCentres(s_x, s_z, nx, nz, fx, fz, vx, vz);
    s_far.jobAxis = 2; s_far.jobPos = 0;                  // the far cache is about to be refilled: nothing in it counts yet
    gridFill(s_near, nx, nz);
    gridFill(s_far, fx, fz);
    gridFill(s_vast, vx, vz);
    s_vast.rebake = 0;
    gridMax(s_near); gridMax(s_far); gridMax(s_vast);
}

void terrainInit(uint32_t seed) {
    s_seed = seed ? seed : 1;
    latReset();
    for (int k = 0; k < 64; k++) s_mm[k].i = 0x7fffffff;
    airfieldReset();
    s_frame = 0; s_time = 0.f;
    gridInit(s_near, 96, 2.5f);
    gridInit(s_far, 64, 8.f);
    gridInit(s_vast, 128, 40.f);
    s_lagYaw = 0.f; s_x = s_z = 0.f;
    terrainRebuild();
}

void terrainUpdate(float x, float z, float yaw, float speed, float dt) {
    s_frame++; s_time += dt;
    airfieldTick(s_time);
    const float jump = (x - s_x) * (x - s_x) + (z - s_z) * (z - s_z);
    s_x = x; s_z = z;
    // the cache windows look ahead along a lagged heading (a turn doesn't thrash them)
    float k = clampf(dt * (0.25f + fabsf(speed) * 0.02f), 0.f, 1.f);
    s_lagYaw += wrapPi(yaw - s_lagYaw) * k;
    if (jump > 3000.f * 3000.f) { s_lagYaw = yaw; terrainRebuild(); return; }   // put somewhere new
    float nx, nz, fx, fz, vx, vz;
    windowCentres(x, z, nx, nz, fx, fz, vx, vz);
    const float sp = fabsf(speed);
    gridScroll(s_near, nx, nz, 200 + (int)(sp * 1.6f));
    gridScroll(s_far, fx, fz, 72 + (int)(sp * 0.6f));
    const int vb = 96;
    const int before = s_vast.jobPos;
    gridScroll(s_vast, vx, vz, vb);
    if (s_vast.jobPos >= s_vast.N && before >= s_vast.N) vastSweep(vb);   // nothing to scroll this frame: sweep
    shadowStep(s_near, 160);                              // the whole near window every ~2 s
    shadowStep(s_far, 64);
    shadowStep(s_vast, 96);
    if ((s_frame & 31) == 0) gridMax(s_near);
    if ((s_frame & 31) == 11) gridMax(s_far);
    if ((s_frame & 63) == 22) gridMax(s_vast);
}

// =====================================================================================================
//  Queries
// =====================================================================================================
static bool gridInside(const Grid& g, float gx, float gz) {
    int ix = (int)floorf(gx), iz = (int)floorf(gz);
    return g.c && ix >= g.ox + 1 && iz >= g.oz + 1 && ix + 2 < g.ox + g.N && iz + 2 < g.oz + g.N;
}

void terrainGround(float x, float z, Ground& o) {
    const Grid* gp = nullptr;
    if (gridInside(s_near, x * s_near.inv, z * s_near.inv) && s_near.jobAxis != 2) gp = &s_near;
    else if (gridInside(s_far, x * s_far.inv, z * s_far.inv) && s_far.jobAxis != 2) gp = &s_far;
    else if (gridInside(s_vast, x * s_vast.inv, z * s_vast.inv) && s_vast.jobAxis != 2) gp = &s_vast;
    if (gp) {
        const Grid& g = *gp;
        float gx = x * g.inv, gz = z * g.inv;
        int ix = (int)floorf(gx), iz = (int)floorf(gz);
        float fx = gx - ix, fz = gz - iz;
        int N = g.N;
        int x0 = gridWrap(ix, N), x1 = gridWrap(ix + 1, N), z0 = gridWrap(iz, N) * N, z1 = gridWrap(iz + 1, N) * N;
        const Cell &a = g.c[z0 + x0], &b = g.c[z0 + x1], &c = g.c[z1 + x0], &d = g.c[z1 + x1];
        // the ground under a water cell is its bed: the cell stores the surface (for drawing) and the depth
        auto ground = [](const Cell& k) { return k.h * 0.1f - ((k.mat & 15) == MAT_WATER ? k.extra * 0.05f : 0.f) - cropRaise(k); };
        float ha = ground(a), hb = ground(b), hc = ground(c), hd = ground(d);
        float h0 = ha + (hb - ha) * fx, h1 = hc + (hd - hc) * fx;
        o.h = h0 + (h1 - h0) * fz;
        float dhx = ((hb - ha) * (1.f - fz) + (hd - hc) * fz) * g.inv;
        float dhz = ((hc - ha) * (1.f - fx) + (hd - hb) * fx) * g.inv;
        float inv = 1.f / sqrtf(dhx * dhx + 1.f + dhz * dhz);
        o.nx = -dhx * inv; o.ny = inv; o.nz = -dhz * inv;
        const Cell& n = (fx < 0.5f) ? (fz < 0.5f ? a : c) : (fz < 0.5f ? b : d);
        o.mat = n.mat & 15; o.surf = n.mat >> 4;
        o.slot = (o.mat == MAT_RUNWAY || o.mat == MAT_LOT) ? (n.extra & 15) : 0;
        // the water's depth here: the lake's level (one flat level per lake) over the blended bed
        o.water = o.mat == MAT_WATER ? fmaxf(0.f, n.h * 0.1f - o.h) : 0.f;
        o.cached = true;
        return;
    }
    // outside every cache: compute directly (rare: teleports, tests)
    float n0 = terrainNatural(x, z);
    {   float ux, uz, sd = riverSd(x, z, ux, uz); n0 = riverCarve(n0, sd, terrainWaterLevel(x, z), riverGorge(journeyLand())); }
    uint8_t mat = journeyLand().ground, surf = SURF_GRASS, slot = 0; bool in = false;
    n0 = airfieldShape(x, z, n0, mat, surf, slot, in);
    o.h = n0; o.nx = 0.f; o.ny = 1.f; o.nz = 0.f;
    o.mat = mat; o.surf = surf; o.water = 0.f; o.cached = false; o.slot = slot;
    float wl = terrainWaterLevel(x, z);
    if (mat != MAT_RUNWAY && mat != MAT_LOT && !in && o.h < wl) { o.mat = MAT_WATER; o.water = wl - o.h; }
}

float terrainHeight(float x, float z) { Ground g; terrainGround(x, z, g); return g.mat == MAT_WATER ? g.h + g.water : g.h; }

bool vegAt(int ci, int cj, Veg& v) {
    const Grid& g = s_near;
    if (!g.c || ci < g.ox + 1 || cj < g.oz + 1 || ci >= g.ox + g.N - 1 || cj >= g.oz + g.N - 1) return false;
    const Cell& c = g.c[gridWrap(cj, g.N) * g.N + gridWrap(ci, g.N)];
    uint8_t mat = c.mat & 15;
    if (mat == MAT_RUNWAY || mat == MAT_WATER || mat == MAT_LOT) return false;
    uint8_t t = c.extra & 15;
    if (t == VEG_NONE || t >= VEG_N) return false;
    uint32_t h = hash32((uint32_t)ci * 2654435761u ^ (uint32_t)cj * 40503u ^ s_seed);
    v.type = t; v.var = (uint8_t)(h >> 24);
    v.x = (ci + 0.2f + 0.6f * ((h & 255) / 255.f)) * g.cell;
    v.z = (cj + 0.2f + 0.6f * (((h >> 8) & 255) / 255.f)) * g.cell;
    float s = ((h >> 16) & 255) / 255.f;
    switch (t) {
        case VEG_PINE:    v.size = 8.f + 9.f * s;  v.radius = 0.35f; break;
        case VEG_BROAD:   v.size = 6.f + 5.f * s;  v.radius = 0.4f; break;
        case VEG_REDWOOD: v.size = 26.f + 16.f * s; v.radius = 1.1f; break;
        case VEG_BUSH:    v.size = 0.8f + 1.0f * s; v.radius = 0.f; break;
        case VEG_CACTUS:  v.size = 2.5f + 2.5f * s; v.radius = 0.3f; break;
        case VEG_SNAG:    v.size = 5.f + 5.f * s;  v.radius = 0.3f; break;
        case VEG_BOULDER: v.size = 0.9f + 2.4f * s; v.radius = v.size * 0.45f; break;
        default:          v.size = 0.4f; v.radius = 0.f; break;
    }
    {   // its own spot on the land (the cells' blend there, as drawn), not its cell's corner: on a slope that had
        // floated it (or sunk it) by up to the slope across most of a cell. A hair into the ground.
        const Cell& b = g.c[gridWrap(cj, g.N) * g.N + gridWrap(ci + 1, g.N)];
        const Cell& d = g.c[gridWrap(cj + 1, g.N) * g.N + gridWrap(ci, g.N)];
        const Cell& e = g.c[gridWrap(cj + 1, g.N) * g.N + gridWrap(ci + 1, g.N)];
        const float fx = v.x * g.inv - ci, fz = v.z * g.inv - cj;
        const float h0 = c.h + (b.h - c.h) * fx, h1 = d.h + (e.h - d.h) * fx;
        v.y = (h0 + (h1 - h0) * fz) * 0.1f - 0.12f;
    }
    return true;
}

}
