// Airfields (see airfield.h): placed by the seed, levelled by the land.
#pragma GCC optimize("Os")
#include "airfield.h"
#include "mem.h"
#include "terrain.h"
#include "journey.h"
#include "universe.h"
#include "aircraft.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace mantis {

// ======================= TUNING =======================
constexpr float AF_CHANCE = 0.9f;        // a square with a field
constexpr float AF_FLAT_LON = 60.f;      // flat ground past each runway end (m)
constexpr float AF_FLAT_HANGAR = 110.f;  // flat ground past the runway's edge on the hangar side
constexpr float AF_FLAT_FAR = 45.f;      // ...and on the other side
constexpr float AF_EASE = 170.f;         // metres over which the field eases back into the land
constexpr float AF_APPROACH = 1800.f;    // the obstacle-clear approach beyond each end
// ======================================================

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth01(float t) { t = clampf(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }

static const char* const NAME_A[24] = {"CEDAR", "HOLLOW", "PINE", "EAGLE", "MOSS", "STONE", "BIRCH", "OTTER", "FALCON", "RAVEN",
                                       "WILLOW", "ASPEN", "CRANE", "FOX", "HERON", "LUPINE", "SAGE", "CLOVER", "MANTIS", "THISTLE",
                                       "MAPLE", "SPRUCE", "BRAMBLE", "HAWK"};
static const char* const NAME_B[14] = {"CREEK", "RIDGE", "LAKE", "HOLLOW", "BLUFF", "FLATS", "MEADOW", "SPRINGS", "POINT", "GAP",
                                       "VALLEY", "BEND", "HILL", "COVE"};

static bool airfieldMake(int i, int j, Airfield& a) {
    const uint32_t h = hash32((uint32_t)i * 0x8DA6B343u ^ (uint32_t)j * 0xD8163841u ^ worldSeed() ^ 0xA1F1E1Du);
    a.valid = false;
    if (i == 0 && j == 0) {}                                   // the home square always has one (you start there)
    else if (hashf(h) > AF_CHANCE) return false;
    a.valid = true; a.i = i; a.j = j;
    a.slot = (uint8_t)((i & 3) | ((j & 3) << 2));
    const uint32_t h2 = hash32(h + 0x51u), h3 = hash32(h + 0x77u);
    if (i == 0 && j == 0) { a.x = 0.5f * AF_CELL; a.z = 0.5f * AF_CELL; }   // home: the middle of its square
    else { a.x = (i + 0.25f + 0.5f * hashf(h2)) * AF_CELL; a.z = (j + 0.25f + 0.5f * hashf(h2 >> 3)) * AF_CELL; }
    a.yaw = hashf(h3) * 3.14159265f;                          // a runway works both ways: half a circle is enough
    const float r = hashf(h3 >> 5);
    if (i == 0 && j == 0) a.surf = SURF_GRASS;                 // home: a grass strip
    else a.surf = r < 0.5f ? SURF_TARMAC : (r < 0.75f ? SURF_GRASS : (r < 0.88f ? SURF_DIRT : SURF_GRAVEL));
    const float lr = hashf(h3 >> 9);
    a.military = false;
    if (a.surf == SURF_TARMAC) {
        a.len = 800.f + 600.f * lr; a.hw = a.len > 1150.f ? 15.f : 12.f;
        if (hashf(h3 >> 17) < 0.28f) { a.military = true; a.len = 1500.f + 400.f * lr; a.hw = 20.f; }   // an air base
    }
    else { a.len = 460.f + 380.f * lr; a.hw = 12.f; }
    a.side = (h3 & 0x8000u) ? 1.f : -1.f;
    const float c = cosf(a.yaw), s = sinf(a.yaw);
    const float hlon = -a.len * 0.25f, hlat = (a.hw + 70.f) * a.side;
    a.hx = a.x + c * hlon - s * hlat; a.hz = a.z + s * hlon + c * hlat;
    a.hyaw = a.yaw + (a.side > 0.f ? -1.5707963f : 1.5707963f);   // its doors face the runway
    float hdg = a.yaw * 57.29578f + 90.f;                      // compass: north 0, east 90
    hdg -= 360.f * floorf(hdg / 360.f);
    a.num = (int)lroundf(hdg / 10.f) % 36; if (a.num == 0) a.num = 36;
    const uint32_t hn = hash32(h + 0x4E41u);
    const char* sfx = a.surf == SURF_TARMAC ? (a.len > 1200.f ? "INTL" : ((hn >> 9) & 1 ? "FIELD" : "MUNI")) : ((hn >> 9) & 1 ? "STRIP" : "FARM");
    if (i == 0 && j == 0) snprintf(a.name, sizeof a.name, "MANTIS HOME");
    else if (a.military) snprintf(a.name, sizeof a.name, "%s AIR BASE", NAME_A[hn % 24u]);
    else if ((hn >> 12) & 1) snprintf(a.name, sizeof a.name, "%s %s", NAME_A[hn % 24u], NAME_B[(hn >> 5) % 14u]);
    else snprintf(a.name, sizeof a.name, "%s %s", NAME_A[hn % 24u], sfx);
    a.y = 0.f;
    return true;
}

// the squares asked about lately, remembered (every baked cell asks about its own square)
struct AtMemo { int i, j; bool ok, has; Airfield a; };
static AtMemo* s_at = nullptr;                     // (PSRAM: 32 memos)
static AtMemo* atMemo() { if (!s_at) { s_at = (AtMemo*)memSlow(32 * sizeof(AtMemo)); if (s_at) memset(s_at, 0, 32 * sizeof(AtMemo)); } return s_at; }
bool airfieldAt(int i, int j, Airfield& a) {
    AtMemo& m = atMemo()[((unsigned)i * 7u + (unsigned)j * 13u) & 31u];
    if (!m.ok || m.i != i || m.j != j) { m.i = i; m.j = j; m.ok = true; m.has = airfieldMake(i, j, m.a); }
    if (!m.has) { a.valid = false; return false; }
    a = m.a;
    return true;
}

bool airfieldFits(const Airfield& a, uint8_t id, const char** why) {
    static const float MINLEN[A_COUNT] = {250.f, 350.f, 1000.f, 1100.f};
    const bool paved = a.surf == SURF_TARMAC;
    if (a.military && id != A_FIGHTER) { if (why) *why = "MILITARY FIELD, CIVILIANS KEEP OUT"; return false; }
    if ((id == A_JET || id == A_FIGHTER) && !paved) { if (why) *why = "UNPAVED STRIP, NOT FOR JETS"; return false; }
    if (a.len < MINLEN[id % A_COUNT]) { if (why) *why = "RUNWAY TOO SHORT FOR YOU"; return false; }
    return true;
}

// ---- the level: worked out from the land along the runway, then held while the field is in use ----
struct LevelMemo { int i, j; float y, lastT; bool ok; };
static LevelMemo s_lv[16];
static float s_clock = 0.f;
void airfieldTick(float t) { s_clock = t; }


float airfieldLevel(Airfield& a) {
    LevelMemo& m = s_lv[a.slot & 15];
    if (m.ok && m.i == a.i && m.j == a.j && s_clock - m.lastT < 30.f) { m.lastT = s_clock; a.y = m.y; return a.y; }
    const float c = cosf(a.yaw), s = sinf(a.yaw);
    float sum = 0.f, lo = 1e9f;
    for (int k = -2; k <= 2; k++) {
        const float px = a.x + c * k * a.len * 0.2f, pz = a.z + s * k * a.len * 0.2f;
        const float h = terrainNatural(px, pz);
        sum += h; lo = fminf(lo, h);
    }
    float y = sum * 0.2f * 0.6f + lo * 0.4f;                    // leaning low: fewer embankments than cuttings
    y = fmaxf(y, terrainWaterLevel(a.x, a.z) + 1.2f);          // never under a lake's level
    m.i = a.i; m.j = a.j; m.y = y; m.lastT = s_clock; m.ok = true;
    a.y = y;
    return y;
}

bool airfieldAround(float x, float z, Airfield& a) {
    const int i = (int)floorf(x * (1.f / AF_CELL)), j = (int)floorf(z * (1.f / AF_CELL));
    if (!airfieldAt(i, j, a)) return false;
    airfieldLevel(a);
    return true;
}

bool airfieldBySlot(uint8_t slot, float nx, float nz, Airfield& a) {
    const int ci = (int)floorf(nx * (1.f / AF_CELL)), cj = (int)floorf(nz * (1.f / AF_CELL));
    for (int dj = -2; dj <= 2; dj++) for (int di = -2; di <= 2; di++) {
        const int i = ci + di, j = cj + dj;
        if ((uint8_t)((i & 3) | ((j & 3) << 2)) != slot) continue;
        if (!airfieldAt(i, j, a)) return false;
        airfieldLevel(a);
        return true;
    }
    return false;
}

bool airfieldNearest(float x, float z, Airfield& out, float* dist) {
    const int ci = (int)floorf(x * (1.f / AF_CELL)), cj = (int)floorf(z * (1.f / AF_CELL));
    float best = 1e30f; bool any = false;
    for (int dj = -1; dj <= 1; dj++) for (int di = -1; di <= 1; di++) {
        Airfield a;
        if (!airfieldAt(ci + di, cj + dj, a)) continue;
        const float d2 = (a.x - x) * (a.x - x) + (a.z - z) * (a.z - z);
        if (d2 < best) { best = d2; out = a; any = true; }
    }
    if (any) airfieldLevel(out);
    if (dist) *dist = any ? sqrtf(best) : 1e9f;
    return any;
}

// ---- bush camps ----
static LevelMemo s_cl[8];
bool campAt(int i, int j, Camp& c) {
    const uint32_t h = hash32((uint32_t)i * 0x2C1B3C6Du ^ (uint32_t)j * 0x297A2D39u ^ worldSeed() ^ 0xCA3Bu);
    c.valid = false;
    if (hashf(h) > 0.6f || (i == 0 && j == 0)) return false;
    c.valid = true; c.i = i; c.j = j;
    const uint32_t h2 = hash32(h + 7u);
    c.x = i * AF_CELL + 320.f + 560.f * hashf(h2); c.z = j * AF_CELL + 320.f + 560.f * hashf(h2 >> 7);
    c.yaw = hashf(h2 >> 13) * 3.14159265f;
    snprintf(c.name, sizeof c.name, "%s CAMP", NAME_A[(h2 >> 3) % 24u]);
    LevelMemo& m = s_cl[((unsigned)i * 3u + (unsigned)j * 5u) & 7u];
    if (m.ok && m.i == i && m.j == j && s_clock - m.lastT < 30.f) { m.lastT = s_clock; c.y = m.y; return true; }
    float y = terrainNatural(c.x, c.z);
    y = fmaxf(y, terrainWaterLevel(c.x, c.z) + 1.2f);
    m.i = i; m.j = j; m.y = y; m.lastT = s_clock; m.ok = true;
    c.y = y;
    return true;
}

float airfieldShape(float x, float z, float h, uint8_t& mat, uint8_t& surf, uint8_t& slot, bool& inside) {
    inside = false;
    const int i = (int)floorf(x * (1.f / AF_CELL)), j = (int)floorf(z * (1.f / AF_CELL));
    {   const float lx = x - i * AF_CELL, lz = z - j * AF_CELL;             // a camp near this square's corner?
        if (lx < 1150.f && lz < 1150.f) {
            Camp c;
            if (campAt(i, j, c)) {
                const float cc = cosf(c.yaw), ss = sinf(c.yaw), dx = x - c.x, dz = z - c.z;
                const float lon = fabsf(dx * cc + dz * ss), lat = fabsf(-dx * ss + dz * cc);
                const float edge = fmaxf(lon - 125.f, lat - 30.f);
                if (edge < 110.f) {
                    inside = edge < 10.f;
                    const float t = smooth01(edge * (1.f / 110.f));
                    return c.y + (h - c.y) * t;
                }
            }
        }
    }
    Airfield a;
    if (!airfieldAt(i, j, a)) return h;
    const float dx = x - a.x, dz = z - a.z, reach = a.len * 0.5f + AF_APPROACH + 60.f;
    if (dx * dx + dz * dz > reach * reach) return h;
    const float E = airfieldLevel(a);
    const float c = cosf(a.yaw), s = sinf(a.yaw);
    const float lon = dx * c + dz * s, lat = -dx * s + dz * c, ls = lat * a.side;   // ls: + toward the hangar
    const float lonOut = fabsf(lon) - (a.len * 0.5f + AF_FLAT_LON);
    const float latOut = ls >= 0.f ? ls - (a.hw + AF_FLAT_HANGAR) : -ls - (a.hw + AF_FLAT_FAR);
    const float edge = fmaxf(lonOut, latOut);
    // the approaches: nothing grows within 500 m of either end along the centre line (a clear climb-out and final),
    // and the land beyond each end stays under a 2 degree slope for AF_APPROACH metres: a final you can always fly
    const bool approach = fabsf(lat) < a.hw + 80.f && fabsf(lon) < a.len * 0.5f + 500.f;
    if (lonOut > 0.f && lonOut < AF_APPROACH) {
        const float cap = E + 1.5f + lonOut * 0.035f;
        const float w = 1.f - smooth01((fabsf(lat) - a.hw - 140.f) * (1.f / 160.f));
        const float fade = 1.f - smooth01((lonOut - AF_APPROACH * 0.8f) * (1.f / (AF_APPROACH * 0.2f)));
        if (h > cap) h = h + (cap - h) * w * fade;
    }
    if (edge > AF_EASE) { inside = approach; return h; }
    slot = a.slot;
    inside = edge < 15.f || approach;
    const float t = smooth01(edge * (1.f / AF_EASE));
    const float out = E + (h - E) * t;
    if (fabsf(lon) < a.len * 0.5f + 10.f && fabsf(lat) < a.hw + 9.f) { mat = MAT_RUNWAY; surf = a.surf; }
    else {
        const float hlon = -a.len * 0.25f;
        if (fabsf(lon - hlon) < 52.f && ls > a.hw + 9.f && ls < a.hw + 98.f) { mat = MAT_LOT; surf = a.surf == SURF_TARMAC ? SURF_TARMAC : SURF_GRAVEL; }
    }
    return out;
}

void airfieldReset() { memset(s_lv, 0, sizeof s_lv); if (atMemo()) memset(s_at, 0, 32 * sizeof(AtMemo)); memset(s_cl, 0, sizeof s_cl); }

}
