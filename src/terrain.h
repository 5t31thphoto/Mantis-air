#pragma once
#include <stdint.h>
#include "journey.h"

// The land (Mantis Rally's, without its roads, with airfields, seen from the sky).
//
// Land: a natural height function (biome blended, with mountain massifs for a flight to cross) with the airfields
// cut in. Three scrolling caches hold it baked for the renderer and the physics: near (2.5 m cells, ~240 m: the
// wheels, the trees, the runway you're rolling on), far (8 m cells, ~512 m) and vast (40 m cells, ~5 km: the view from
// altitude). Each cell knows its height, material, normal and a little more (see Cell). The vast cache is swept
// again continuously, so the land you see far off follows the journey's slow changes instead of going stale.

namespace mantis {

// Baked cell
struct Cell {
    int16_t h;        // height, decimetres
    int16_t across;   // runway cells: across the runway (1/50 m). Others: 30000 (nothing man-made)
    uint8_t along;    // runway cells: along it, 1/20 m mod 12.8 m
    uint8_t mat;      // low nibble MAT_*, high nibble SURF_*
    uint8_t nrm;      // normal: high nibble x, low nibble z, (v - 8) / 8
    uint8_t extra;    // water: depth 1/20 m.  runway / lot: the field's slot (low nibble).  ground: vegetation hint
};

// Farm plots (56 m squares): 0 not farmed, else the code a plot's cells keep in Cell::extra (bit 7 farmed, bits 5-6
// 0 corn 1 potatoes 2 grain 3 pasture, bit 4 the rows' heading). Near rock cells with extra 0x80: a waterfall's lip.
constexpr float PLOT_M = 56.f;
uint8_t terrainPlot(int pi, int pj);
uint8_t terrainNearExtra(float x, float z);   // a near ground cell's extra (0 off the window, water or runway)
float terrainRiverDepth(float x, float z);   // how far a big river's valley is carved below the land here (0: none)
float terrainRiverDist(float x, float z);    // distance to the nearest big river's centre line (large: none near)

struct Grid {
    Cell* c; int N; float cell, inv;
    int ox, oz;                     // window origin (cell coords)
    int jobAxis, jobLine, jobPos;   // the row / column being dreamt
    int rebake;                     // >=0: sweeping every cell again
    float maxH;                     // highest cell seen (ray early-out)
    uint8_t* sh;                    // 1 bit a cell (same order as c): out of the sun (a ridge between it and the sun)
    int shCur;                      // the next cell the slow sweep re-checks
};

void terrainInit(uint32_t seed);
// Each frame: scrolls the caches (budgeted by speed), sweeps the vast one, keeps the land's shade.
void terrainUpdate(float x, float z, float yaw, float speed, float dt);
void terrainRebuild();                       // re-dream everything now (start, teleports, previews)
void terrainSetSun(float az, float elev);    // each frame, before terrainUpdate: which cells the land shades from the sun

// --- land ---
float terrainNatural(float x, float z);      // height before the airfields are cut in
float terrainNaturalLod(float x, float z);   // the same without the metre-scale roughness (the vast cache)
float terrainWaterLevel(float x, float z);   // lake / sea surface here
struct Ground {
    float h, nx, ny, nz;       // surface height (a lake's bed) and normal
    uint8_t mat, surf;         // MAT_*, SURF_* (what the wheels feel)
    float water;               // water depth (m) when mat == MAT_WATER
    uint8_t slot;              // the airfield's slot (runway / lot)
    bool cached;               // came from a cache (else computed)
};
void terrainGround(float x, float z, Ground& g);
float terrainHeight(float x, float z);       // the surface: land, or a lake's level

// --- caches (renderer) ---
const Grid& terrainNear();
const Grid& terrainFar();
const Grid& terrainVast();
inline int gridWrap(int i, int N) { int m = i % N; return m < 0 ? m + N : m; }
float terrainMaxH();                         // the highest cell in any cache (ray early-out, altitude)

// --- vegetation: deterministic placement on the near cache's lattice ---
enum : uint8_t { VEG_NONE = 0, VEG_PINE, VEG_BROAD, VEG_REDWOOD, VEG_BUSH, VEG_CACTUS, VEG_SNAG, VEG_BOULDER, VEG_FLOWERS, VEG_N };
constexpr float VEG_CELL = 2.5f;
struct Veg { uint8_t type, var; float x, z, y, size, radius; };
bool vegAt(int ci, int cj, Veg& v);          // false if nothing grows there (or outside the near cache)

}
