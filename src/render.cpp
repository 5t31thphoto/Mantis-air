// Mantis Flight renderer (Mantis Rally's, taken up into the sky). 160x120 palette indices; the palette is rebuilt
// every frame from the time of day, weather, biome and lights (MantisPlanet's model: artist ramps lit by ambient + key,
// fog as palette work). The land is three caches deep (near, far, vast: about 8 km from altitude), rays skip what lies
// below the picture, and depth runs to the horizon.
// Order: terrain (front to back, per column) -> panorama -> sky / clouds / night -> the cloud deck -> props
// (z-buffered: trees, airfields) -> reflections (water, wet runways) -> weather -> the aircraft or the cockpit ->
// glass -> instruments.
#include "render.h"
#ifdef HOST
#include <chrono>
#endif
#include "art_cards.h"
#include "art_mesh_cub.h"
#include "art_mesh_beaver.h"
#include "art_mesh_jet.h"
#include "art_mesh_fighter.h"
#include "sprites.h"
#include "terrain.h"
#include "journey.h"
#include "scene.h"
#include "field.h"
#include "aircraft.h"
#include "airfield.h"
#include "universe.h"
#include "mem.h"
#include "display.h"
#include "cockpit.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#ifndef HOST
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif
#if !defined(HOST) && !defined(MR_NO_IRAM)
#include <esp_attr.h>
// inner loops in instruction RAM (no flash-cache stalls), as MantisPlanet does. noinline: a static function called
// once is otherwise inlined into its caller and follows the caller's section (ESP-IDF: "when a function is inlined, it
// will follow its caller's segment, and the attribute will not take effect") -- drawTerrain was landing in flash.
#define MR_HOT IRAM_ATTR __attribute__((noinline))
#define MR_FAST DRAM_ATTR
#else
#define MR_HOT
#define MR_FAST
#endif

namespace mantis {

ViewInfo g_view;
static uint8_t s_npcCol[16][2];            // the traffic/bogey livery: no entries of its own, matched into the palette

static constexpr int W = FB_W, H = FB_H;
static uint8_t fb[W * H];
static uint8_t* zb = nullptr;
// The palette as the panel takes it (RGB565, byte-swapped), four times: one per position in a 2x2 block. Every
// picture pixel is shown as a 2x2 block, so an ordered dither between the two nearest 565 colours there costs
// nothing and gives about two more bits of colour (smooth gradients, true dark tones: no bias from truncation).
static uint16_t pal565q[4 * 256];
// Fog the palette way (as MantisPlanet): every colour has fogged twins, the nearest palette colours (in its own ramp or
// the haze ramp) to it mixed toward the fog at four levels. Fog steps a pixel to its twin: no new colours, no dots.
static uint8_t s_fogLut[4][256];
static float s_fogRGB[3] = {128.f, 128.f, 128.f};
static int s_fogLvl = 0;
static const float FOG_MIX[4] = {0.22f, 0.45f, 0.68f, 0.88f};
static void buildFogLevel(const uint8_t* rgb, int lv) {
    float m = FOG_MIX[lv];
    for (int i = 0; i < 256; i++) {
        float tr = rgb[i * 3] + (s_fogRGB[0] - rgb[i * 3]) * m, tg = rgb[i * 3 + 1] + (s_fogRGB[1] - rgb[i * 3 + 1]) * m, tb = rgb[i * 3 + 2] + (s_fogRGB[2] - rgb[i * 3 + 2]) * m;
        int best = i; float bd = 1e30f;
        for (int pass = 0; pass < 2; pass++) {
            int base = pass == 0 ? (i & ~15) : P_HAZE;
            for (int k = 0; k < 16; k++) {
                int j = base + k;
                float dr = rgb[j * 3] - tr, dg = rgb[j * 3 + 1] - tg, db = rgb[j * 3 + 2] - tb;
                float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
                if (d < bd) { bd = d; best = j; }
            }
        }
        s_fogLut[lv][i] = (uint8_t)best;
    }
}
#ifdef HOST
int renderFogTwinCount(int lv) { int n = 0; for (int i = 0; i < 256; i++) if (s_fogLut[lv][i] != i) n++; return n; }
#endif
// a fog amount (0..1) -> a twin: four clean bands with a thin seam on the surface grain (Planet's banding)
static inline uint8_t fogTwin(uint8_t c, float amt, float grain) {
    float f = amt * 4.f; int lv = (int)f; float fr = f - lv;
    if (fr > 0.86f || (fr > 0.62f && grain < 0.5f)) lv++;
    if (lv <= 0) return c;
    return s_fogLut[lv > 4 ? 3 : lv - 1][c];
}
static uint8_t palRGB[256 * 3];
static uint8_t s_refl[256];                // a palette entry's reflection (darker, same ramp)
static uint8_t s_bright[256];              // one step brighter in its ramp (light shafts)
static float s_light[256];                 // normal byte -> sunlight 0..1
static float s_lightSh[256];               // the same, out of the sun (the land shades it): sky light only
static float s_reliefK = 0.f;              // the ground's grain lit from the sun's side: strength, and the texel
static int s_rdu = 0, s_rdv = 0;           // offset toward the sun
static float s_fog[1024];                  // fog by distance (8 m steps, to 8 km)
static inline float fogAt(float d) { int i = (int)(d * 0.125f); return s_fog[i < 0 ? 0 : (i > 1023 ? 1023 : i)]; }
static uint8_t s_veh = 0;
static uint8_t s_bottom[W];                // rows at and below this are the cockpit's (not raycast)
static uint8_t s_bottomCock[W];
static bool s_capWater = false;            // photo capture: mark the water pixels
static uint8_t* s_wmask = nullptr;
static float s_bob = 0.f;

static MR_FAST uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// As MantisPlanet: every dither keyed to world columns (x + pan, the heading in whole pixels), so the same pattern
// travels with the scenery as you turn instead of sitting on the screen. Pattern and densities are unchanged.
static int g_pan = 0;
static inline float dith(int x, int y) { return (BAYER[((y & 3) << 2) | ((x + g_pan) & 3)] + 0.5f) * (1.f / 16.f); }
// the aurora's pattern: anchored to the sky (view direction), twinkling (half ordered, half a hash that changes over time)
static inline float sdith(int su, int ry, float t, float twinkle) {
    float b = (BAYER[((ry & 3) << 2) | (su & 3)] + 0.5f) * (1.f / 16.f);
    uint32_t h = (uint32_t)su * 73856093u ^ (uint32_t)ry * 19349663u ^ (uint32_t)(t * 6.f) * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return b * (1.f - twinkle) + (h & 1023u) * (1.f / 1024.f) * twinkle;
}
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
// depth code: Rally's square-root scale out to 300 m (fine where the props stand), a log scale beyond (the view from
// altitude, to ~9 km). Even codes; the low bit is the reflection flag. 254 the panorama, 255 the sky.
static inline uint8_t zcode(float d) {
    float c = d < 300.f ? sqrtf(d > 0.f ? d : 0.f) * 11.f : 190.53f + 18.1f * logf(d * (1.f / 300.f));
    return (uint8_t)((int)(c > 252.f ? 252.f : c) & ~1);
}
static float s_zInv[256];                  // the depth where each code begins (built once)
static inline float zdepth(int c) { return s_zInv[c < 0 ? 0 : (c > 255 ? 255 : c)]; }
static inline int band(float f, int x, int y) {              // Planet's pixel-art banding
    int i = (int)f; float fr = f - i;
    if (fr > 0.86f) return i + 1;
    if (fr > 0.62f && ((x ^ y) & 1)) return i + 1;
    return i;
}
static inline int ifloor(float v) { int i = (int)v; return v < (float)i ? i - 1 : i; }
static inline int iceil(float v) { int i = (int)v; return v > (float)i ? i + 1 : i; }
static inline float fastAtan2(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y);
    float a = fminf(ax, ay) / (fmaxf(ax, ay) + 1e-9f);
    float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

// ================================================================================== palette
struct C3 { float r, g, b; };
static inline C3 mix(C3 a, C3 b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
static inline C3 scale(C3 a, float s) { return {a.r * s, a.g * s, a.b * s}; }
static inline C3 add(C3 a, C3 b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
static inline C3 mul(C3 a, C3 b) { return {a.r * b.r / 255.f, a.g * b.g / 255.f, a.b * b.b / 255.f}; }
static inline C3 c3(const uint8_t* p) { return {(float)p[0], (float)p[1], (float)p[2]}; }

struct Key { float t; C3 zen, hor, glow, cDark, cLit, sun, amb, key, fog; float elev, night, mist; };
#define NIGHT_KEY(T, E, M) {T, {6,5,20},{24,18,52},{40,24,70},{18,12,40},{95,55,150},{215,220,255},{70,75,120},{60,70,150},{26,22,55}, E, 1.0f, M}
static const Key KEYS[] = {             // MantisPlanet's day
    NIGHT_KEY(0.00f, -0.40f, 0.25f),
    {0.19f, {18,14,50},{75,42,105},{160,75,115},{40,28,75},{190,105,150},{255,200,160},{105,92,140},{190,105,125},{85,58,115}, -0.06f, 0.55f, 0.55f},
    {0.26f, {58,48,128},{238,118,92},{255,188,112},{105,58,118},{255,168,110},{255,240,190},{160,130,140},{255,150,80},{195,118,128}, 0.07f, 0.0f, 0.60f},
    {0.36f, {70,120,210},{188,208,235},{255,240,200},{138,150,190},{255,250,240},{255,255,230},{220,220,230},{255,230,180},{168,192,222}, 0.42f, 0.0f, 0.25f},
    {0.50f, {40,100,215},{168,208,245},{240,248,255},{150,165,200},{255,255,255},{255,255,240},{255,255,255},{255,245,220},{160,195,235}, 0.80f, 0.0f, 0.10f},
    {0.63f, {62,90,190},{218,190,172},{255,220,150},{150,128,170},{255,230,190},{255,245,200},{230,215,205},{255,210,140},{190,170,182}, 0.36f, 0.0f, 0.15f},
    {0.71f, {82,50,142},{255,118,70},{255,200,90},{120,50,110},{255,148,90},{255,240,170},{170,115,128},{255,128,50},{190,90,120}, 0.06f, 0.0f, 0.35f},
    {0.79f, {35,22,75},{178,50,100},{255,110,70},{60,30,80},{220,90,125},{255,180,120},{105,78,115},{200,70,80},{108,45,95}, -0.08f, 0.35f, 0.45f},
    NIGHT_KEY(0.89f, -0.35f, 0.30f),
    NIGHT_KEY(1.00f, -0.40f, 0.25f),
};
static constexpr int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);
static Key S;
static Key sampleKey(float tod) {
    int k = 0;
    while (k < NKEYS - 2 && tod >= KEYS[k + 1].t) k++;
    const Key &A = KEYS[k], &B = KEYS[k + 1];
    float t = clampf((tod - A.t) / (B.t - A.t), 0.f, 1.f); t = t * t * (3.f - 2.f * t);
    Key o; o.t = tod;
    o.zen = mix(A.zen, B.zen, t); o.hor = mix(A.hor, B.hor, t); o.glow = mix(A.glow, B.glow, t);
    o.cDark = mix(A.cDark, B.cDark, t); o.cLit = mix(A.cLit, B.cLit, t); o.sun = mix(A.sun, B.sun, t);
    o.amb = mix(A.amb, B.amb, t); o.key = mix(A.key, B.key, t); o.fog = mix(A.fog, B.fog, t);
    o.elev = A.elev + (B.elev - A.elev) * t; o.night = A.night + (B.night - A.night) * t; o.mist = A.mist + (B.mist - A.mist) * t;
    return o;
}
struct Ramp4 { C3 k[4]; };
static C3 rampAt(const Ramp4& r, float s) {
    s = clampf(s, 0.f, 1.f) * 3.f; int i = (int)s;
    return i >= 3 ? r.k[3] : mix(r.k[i], r.k[i + 1], s - i);
}
// an artist ramp from a biome's dark / lit pair: shadows lean cool, highlights warm (Planet's rule)
static Ramp4 artist(const uint8_t* d, const uint8_t* l) {
    C3 D = c3(d), Lc = c3(l);
    Ramp4 r;
    r.k[0] = mix(scale(D, 0.55f), C3{20, 30, 60}, 0.35f);
    r.k[1] = D;
    r.k[2] = mix(D, Lc, 0.6f);
    r.k[3] = mix(Lc, C3{255, 236, 170}, 0.18f);
    return r;
}
static Ramp4 blendR(const Ramp4& a, const Ramp4& b, float t) { Ramp4 r; for (int i = 0; i < 4; i++) r.k[i] = mix(a.k[i], b.k[i], t); return r; }

static float s_night = 0.f, s_lights = 0.f, s_sunK = 1.f;
static constexpr float HEAD_K = 1.15f;            // the headlights' strength on the ground and on what they light (1: as before)
static float s_artLit[3] = {1.f, 1.f, 1.f};      // the scene's light on the hero art (photo mode)
static CockpitLight s_cockLight = {{1.f, 1.f, 1.f}, 0.f};   // the cabin's light on the cockpit's art
static float s_sunDx = 0.f, s_sunDy = 1.f, s_sunDz = 0.f;

static void fogTick() {                                 // the fogged twins follow the palette: one level every 8 frames
    static uint8_t tick = 0;
    if (!(tick++ & 7)) { buildFogLevel(palRGB, s_fogLvl); s_fogLvl = (s_fogLvl + 1) & 3; }
}
static void buildPalette() {
    const Scene& sc = g_scene;
    {   // The palette is made from the time of day, the weather, the biomes, the landing light and (toward a low sun) the
        // heading: it was rebuilt every frame, but they move a visible step only now and then. Rebuilt when one has (and
        // every frame of a lightning flash, and at least every 2 s).
        const float yawNow = g_view.photo ? g_view.pYaw : g_view.yaw;
        const float lowSun = clampf(1.f - sc.sunElev / 0.45f, 0.f, 1.f) * clampf((sc.sunElev + 0.08f) * 12.f, 0.f, 1.f);
        const float lightsNow = g_view.photo ? 0.f : sc.lights * clampf(1.f - (g_view.agl - 60.f) * (1.f / 140.f), 0.f, 1.f);
        const int32_t sig[15] = {(int32_t)(sc.tod * 4000.f), (int32_t)(sc.blend * 256.f), (int32_t)(sc.cloud * 64.f), (int32_t)(sc.rain * 64.f),
            (int32_t)(sc.snow * 64.f), (int32_t)(sc.storm * 64.f), (int32_t)(sc.wet * 64.f), (int32_t)(sc.moon * 64.f), (int32_t)(lightsNow * 64.f),
            lowSun > 0.f ? (int32_t)(cosf(sc.sunAz - yawNow) * 48.f) + 1000 : 0, sc.biomeA, sc.biomeB, g_view.photo ? 1 : 0, g_view.view, s_veh};
        static int32_t last[15]; static bool have = false; static uint8_t age = 0;
        if (have && sc.flash <= 0.f && age < 60 && !memcmp(sig, last, sizeof sig)) { age++; fogTick(); return; }
        memcpy(last, sig, sizeof sig); have = true; age = 0;
    }
    const Biome& A = biome(sc.biomeA); const Biome& B = biome(sc.biomeB);
    float bl = sc.blend;
    S = sampleKey(sc.tod);
    // weather washes colour out (Planet's weatherKey)
    float grey = clampf(sc.cloud * 0.6f - 0.15f + sc.rain * 0.45f + sc.storm * 0.25f + sc.snow * 0.2f, 0.f, 0.9f);
    float dark = 1.f - 0.28f * sc.rain - 0.25f * sc.storm;
    auto desat = [](C3 c, float k, float d) -> C3 { float l = c.r * 0.3f + c.g * 0.55f + c.b * 0.15f; return scale(mix(c, C3{l * 0.92f, l * 0.96f, l * 1.05f}, k), d); };
    S.zen = desat(S.zen, grey, dark); S.hor = desat(S.hor, grey * 0.8f, dark); S.glow = desat(S.glow, grey * 0.9f, dark);
    S.cDark = desat(S.cDark, grey, dark * 0.85f); S.cLit = desat(S.cLit, grey * 0.8f, dark); S.fog = desat(S.fog, grey * 0.7f, dark);
    S.amb = scale(S.amb, 1.f - 0.25f * grey); 
    {   // looking toward a low sun the haze glows, lit from behind: a warm veil toward the sun, none away from it
        const float yaw = g_view.photo ? g_view.pYaw : g_view.yaw;
        const float face = cosf(sc.sunAz - yaw);
        const float low = clampf(1.f - sc.sunElev / 0.45f, 0.f, 1.f) * clampf((sc.sunElev + 0.08f) * 12.f, 0.f, 1.f);
        if (face > 0.f && low > 0.f) S.fog = mix(S.fog, S.glow, 0.3f * face * face * low * (1.f - grey));
    }
    s_sunK = (1.f - 0.75f * grey) * clampf((S.elev + 0.05f) * 4.f, 0.f, 1.f);
    s_night = S.night;
    s_lights = g_view.photo ? 0.f : sc.lights * clampf(1.f - (g_view.agl - 60.f) * (1.f / 140.f), 0.f, 1.f);   // the landing light reaches ~200 m
    // the key light: the sun by day; the headlights by night
    C3 key = scale(S.key, 1.f - 0.75f * grey);
    C3 skyKey = key;                                 // daylight / moonlight only (the art in a photo isn't in its own beams)
    C3 HEAD = {255, 242, 215};
    key = mix(key, HEAD, clampf(s_lights * (0.25f + S.night * 0.9f), 0.f, 1.f));
    C3 amb = S.amb;
    if (S.night > 0.3f) amb = add(amb, scale(C3{14, 18, 34}, sc.moon * S.night));
    C3 P[256];
    memset(P, 0, sizeof(P));
    auto litR = [&](const Ramp4& r, float s) -> C3 {
        C3 b = rampAt(r, s);
        return add(scale(mul(b, amb), 0.55f + 0.45f * s), scale(mul(b, key), s * s * 0.55f));
    };
    Ramp4 grass = blendR(artist(A.grassD, A.grassL), artist(B.grassD, B.grassL), bl);
    Ramp4 soil = blendR(artist(A.soilD, A.soilL), artist(B.soilD, B.soilL), bl);
    Ramp4 rock = blendR(artist(A.rockD, A.rockL), artist(B.rockD, B.rockL), bl);
    Ramp4 leaf = blendR(artist(A.leafD, A.leafL), artist(B.leafD, B.leafL), bl);
    C3 acc = mix(c3(A.accent), c3(B.accent), bl);
    C3 wat = mix(c3(A.waterC), c3(B.waterC), bl);
    C3 mtn = mix(c3(A.mtn), c3(B.mtn), bl);
    uint8_t surf = bl < 0.5f ? A.surface : B.surface;
    if (sc.wet > 0.2f) { for (int i = 0; i < 4; i++) { soil.k[i] = scale(soil.k[i], 1.f - 0.25f * sc.wet); grass.k[i] = mix(grass.k[i], C3{16, 50, 52}, 0.2f * sc.wet); } }
    Ramp4 asph;
    if (surf == SURF_SNOW) asph = {{{120, 130, 160}, {170, 180, 205}, {215, 222, 238}, {245, 248, 255}}};
    else asph = {{{22, 22, 34}, {44, 44, 56}, {78, 76, 84}, {128, 124, 122}}};
    if (sc.wet > 0.2f) for (int i = 0; i < 4; i++) asph.k[i] = scale(asph.k[i], 1.f - 0.3f * sc.wet);
    static const Ramp4 SNOWR = {{{120, 140, 190}, {180, 196, 228}, {226, 234, 248}, {255, 255, 255}}};
    static const Ramp4 BARKR = {{{40, 24, 32}, {88, 52, 36}, {150, 92, 52}, {210, 150, 90}}};
    Ramp4 leaf2 = {{scale(acc, 0.3f), scale(acc, 0.6f), acc, mix(acc, C3{255, 250, 220}, 0.3f)}};
    Ramp4 haze = {{scale(S.fog, 0.55f), scale(S.fog, 0.8f), S.fog, mix(S.fog, S.hor, 0.6f)}};
    Ramp4 mt = {{mix(scale(mtn, 0.5f), C3{30, 30, 60}, 0.3f), mtn, mix(mtn, C3{200, 190, 180}, 0.4f), C3{235, 238, 250}}};
    for (int i = 0; i < 16; i++) {
        float t = i / 15.f;
        P[P_SKY + i] = i < 10 ? mix(S.zen, S.hor, powf(i / 9.f, 1.15f)) : mix(S.hor, S.glow, (i - 9) / 6.f);
        C3 c = mix(S.cDark, S.cLit, t);
        P[P_CLOUD + i] = mix(c, S.hor, 0.22f * sinf(t * 3.14159f));
        // mountains: 0..7 far (hazed), 8..15 near
        float s = (i & 7) / 7.f;
        C3 m = rampAt(mt, s * 0.92f + (s > 0.85f ? 0.08f : 0.f));
        C3 ml = add(scale(mul(m, amb), 0.55f + 0.5f * s), scale(mul(m, key), s * s * 0.45f));
        P[P_MTN + i] = mix(ml, S.fog, i < 8 ? 0.5f : 0.25f);
        P[P_HAZE + i] = rampAt(haze, t);
        P[P_GRASS + i] = litR(grass, t);
        P[P_SOIL + i] = litR(soil, t);
        P[P_ROCK + i] = litR(rock, t);
        P[P_ROAD + i] = litR(asph, t);
        P[P_LEAF + i] = litR(leaf, t);
    }
    for (int i = 0; i < 8; i++) {
        float t = i / 7.f;
        P[P_SUN + i] = i == 7 ? mix(S.sun, C3{255, 255, 255}, 0.4f) : mix(S.glow, S.sun, t);
        P[P_LEAF2 + i] = litR(leaf2, t);
        P[P_SNOW + i] = litR(SNOWR, t);
        P[P_BARK + i] = litR(BARKR, t);
        P[P_WATER + i] = add(scale(mul(wat, amb), 0.5f + 0.5f * t), scale(mul(wat, key), t * 0.3f));
    }
    // special: rainbow by day, aurora by night
    static const C3 BOW[8] = {{150, 90, 230}, {80, 120, 255}, {70, 200, 235}, {90, 225, 110}, {250, 235, 90}, {255, 160, 60}, {240, 70, 60}, {255, 255, 255}};
    static const C3 AUR[8] = {{8, 30, 44}, {20, 90, 80}, {40, 170, 110}, {60, 225, 140}, {120, 245, 170}, {120, 140, 230}, {170, 100, 240}, {220, 160, 255}};
    for (int i = 0; i < 8; i++) P[P_SPECIAL + i] = S.night > 0.5f ? AUR[i] : mix(BOW[i], P[P_SKY + 9], 0.25f);
    // flowers: lit, saturated
    static const C3 FL[8] = {{250, 250, 240}, {255, 220, 60}, {200, 120, 230}, {255, 130, 170}, {230, 60, 60}, {110, 150, 255}, {255, 160, 60}, {245, 245, 255}};
    for (int i = 0; i < 8; i++) P[P_FLOWER + i] = mix(add(scale(mul(FL[i], amb), 0.8f), scale(mul(FL[i], key), 0.45f)), mul(FL[i], amb), 0.3f);
    if (A.accent[0] > 0) P[P_FLOWER + 2] = mix(P[P_FLOWER + 2], litR(leaf2, 0.8f), 0.6f);
    for (int k = 0; k < 3; k++) { float a = (&amb.r)[k], kk = (&skyKey.r)[k] * (1.f - S.night); s_artLit[k] = clampf((a * 0.62f + kk * 0.48f) / 255.f, 0.08f, 1.15f); }
    // the aircraft's paint (until its sprite sheet arrives): body, trim and accent ramps of four, glass, dark metal,
    // the prop's blur. P_VEH + 0..3 body, 4..7 trim, 8..11 accent, 12..13 glass, 14 dark, 15 prop.
    {   const AircraftSpec& A = aircraftSpec(s_veh);
        (void)A;
        // the Mantis livery from the sprite sheet (15 colours): lit in P_VEH, shaded in P_COCK (whose cockpit
        // colours are only wanted when the cockpit is on screen, when the aircraft's outside isn't)
        for (int b = 1; b < 16; b++) {
            const C3 v = {LIVERY16_MANTIS[b][0] * 1.25f, LIVERY16_MANTIS[b][1] * 1.25f, LIVERY16_MANTIS[b][2] * 1.25f};
            P[P_VEH + b] = add(scale(mul(v, amb), 0.8f), scale(mul(v, skyKey), 0.55f));
        }
        const bool cockpitShown = g_view.view == VIEW_COCKPIT && !g_view.photo;
        if (!cockpitShown) for (int b = 1; b < 16; b++) {          // (no cockpit on screen: P_COCK is the shaded livery)
            const C3 v = {LIVERY16_MANTIS[b][0] * 1.25f, LIVERY16_MANTIS[b][1] * 1.25f, LIVERY16_MANTIS[b][2] * 1.25f};
            P[P_COCK + b] = add(scale(mul(v, amb), 0.75f), scale(mul(v, skyKey), 0.3f));   // (the art has its own shading: gentle)
        }
        // the panel: lit by the cabin's light; the gauges glow at night
        C3 cab = add(scale(amb, 0.55f), scale(skyKey, 0.25f * (1.f - S.night)));
        const float glow = S.night;
        for (int k = 0; k < 3; k++) s_cockLight.k[k] = clampf((&cab.r)[k] / 255.f * 1.5f, 0.16f, 1.1f);   // (midday: the art as drawn)
        s_cockLight.night = glow;
        auto pan = [&](C3 c) { return add(scale(mul(c, cab), 0.9f), scale(C3{30, 22, 14}, glow * 0.3f)); };
        if (cockpitShown) {
        P[P_COCK + 0] = pan(C3{34, 36, 42}); P[P_COCK + 1] = pan(C3{52, 54, 62}); P[P_COCK + 2] = pan(C3{72, 74, 84}); P[P_COCK + 3] = pan(C3{98, 100, 110});
        P[P_COCK + 4] = pan(C3{150, 150, 158});                                  // bezels
        P[P_COCK + 5] = C3{12, 13, 16};                                          // gauge faces
        P[P_COCK + 6] = mix(pan(C3{235, 235, 228}), C3{200, 240, 200}, glow * 0.6f);   // markings
        P[P_COCK + 7] = mix(pan(C3{255, 250, 235}), C3{255, 220, 140}, glow * 0.5f);   // needles
        P[P_COCK + 8] = mix(C3{60, 130, 220}, C3{30, 60, 120}, glow * 0.5f);   // the attitude ball's sky
        P[P_COCK + 9] = mix(C3{150, 96, 50}, C3{80, 50, 26}, glow * 0.5f);     // ...and its earth
        P[P_COCK + 10] = C3{60, 200, 80}; P[P_COCK + 11] = C3{240, 200, 40}; P[P_COCK + 12] = C3{230, 50, 40};   // arcs
        C3 paint = c3(A.paint[0]);
        P[P_COCK + 13] = add(scale(mul(paint, amb), 0.5f), scale(mul(paint, skyKey), 0.25f));   // the cowling
        P[P_COCK + 14] = add(scale(mul(paint, amb), 0.7f), scale(mul(paint, skyKey), 0.55f));
        P[P_COCK + 15] = pan(C3{16, 16, 20});                                    // the glare shield
        }
    }
    // emissive
    P[P_HUD_W] = {242, 246, 250}; P[P_HUD_A] = {255, 190, 50}; P[P_HUD_R] = {255, 70, 60}; P[P_HUD_G] = {120, 255, 120};
    P[P_HUD_K] = {12, 14, 20}; P[P_FIREFLY] = {225, 255, 130}; P[P_WINDOW] = mix(C3{60, 70, 80}, C3{255, 200, 110}, S.night);
    P[P_BEAM] = mix(S.fog, C3{255, 240, 200}, 0.35f);
    // paint is retro-reflective: it shines in the headlights
    for (int i = 0; i < 4; i++) {
        float t = i / 3.f;
        P[P_LINE_W + i] = add(scale(mul(C3{235, 235, 228}, amb), 0.5f + 0.4f * t), scale(mul(C3{235, 235, 228}, key), t * 0.6f));
        P[P_LINE_Y + i] = add(scale(mul(C3{240, 196, 40}, amb), 0.5f + 0.4f * t), scale(mul(C3{240, 196, 40}, key), t * 0.6f));
    }
    P[P_SIGN_G] = add(scale(mul(C3{20, 110, 60}, amb), 0.8f), scale(mul(C3{20, 110, 60}, key), 0.5f));
    P[P_SIGN_W] = add(scale(mul(C3{240, 240, 240}, amb), 0.7f), scale(mul(C3{240, 240, 240}, key), 0.6f + s_lights * S.night * 0.4f));
    P[P_TAIL_D] = {120, 16, 24}; P[P_TAIL] = {255, 40, 40}; P[P_AMBER] = {255, 170, 40}; P[P_BLACK] = {10, 10, 14};
    P[P_BOLT] = {235, 235, 255}; P[P_MUD] = mul(C3{70, 48, 30}, add(scale(amb, 0.7f), scale(key, 0.3f)));
    // Lit colours can pass 255 (sun plus sky); clipped channel by channel they lose their hue and the top of each ramp
    // goes flat. A shoulder instead: above 235 the brightest channel rolls off toward 255 and the others scale with it.
    {   auto shoulder = [](C3& c) {
            float m = fmaxf(c.r, fmaxf(c.g, c.b));
            if (m > 235.f) { float e = m - 235.f, k = (235.f + 20.f * e / (e + 20.f)) / m; c.r *= k; c.g *= k; c.b *= k; }
        };
        for (int i = P_MTN; i < P_MTN + 16; i++) shoulder(P[i]);
        for (int i = P_GRASS; i < P_WATER + 8; i++) shoulder(P[i]);
        for (int i = P_VEH + 1; i < P_FLOWER + 8; i++) shoulder(P[i]);          // vehicle, cockpit, flowers
        for (int i = P_LINE_W; i <= P_SIGN_W; i++) shoulder(P[i]);             // paint and signs
        shoulder(P[P_MUD]);
    }
    // the other liveries (traffic, bogeys) have no entries of their own: each colour takes the nearest lit one
    {   static uint8_t cand[80]; static int nc = 0;
        if (!nc) {
            for (int i = 0; i < 16; i++) { cand[nc++] = (uint8_t)(P_ROCK + i); cand[nc++] = (uint8_t)(P_ROAD + i); cand[nc++] = (uint8_t)(P_SOIL + i); }
            for (int i = 0; i < 4; i++) { cand[nc++] = (uint8_t)(P_LINE_W + i); cand[nc++] = (uint8_t)(P_LINE_Y + i); }
            for (int i = 0; i < 8; i++) cand[nc++] = (uint8_t)(P_BARK + i);
            cand[nc++] = P_TAIL_D; cand[nc++] = P_TAIL; cand[nc++] = P_AMBER; cand[nc++] = P_BLACK; cand[nc++] = P_MUD;
        }
        for (int b = 1; b < 16; b++) for (int sh = 0; sh < 2; sh++) {
            const C3 v = {LIVERY16_NPC[b][0] * 1.25f, LIVERY16_NPC[b][1] * 1.25f, LIVERY16_NPC[b][2] * 1.25f};
            const C3 t = sh ? add(scale(mul(v, amb), 0.8f), scale(mul(v, skyKey), 0.55f)) : add(scale(mul(v, amb), 0.75f), scale(mul(v, skyKey), 0.3f));
            float best = 1e30f; uint8_t bi = P_BLACK;
            for (int q = 0; q < nc; q++) {
                const C3& c = P[cand[q]];
                const float dr = c.r - t.r, dg = c.g - t.g, db = c.b - t.b, d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
                if (d < best) { best = d; bi = cand[q]; }
            }
            s_npcCol[b][sh] = bi;
        }
    }
    // lightning lifts everything toward a cold white
    if (sc.flash > 0.01f) for (int i = 0; i < 232; i++) P[i] = mix(P[i], C3{225, 230, 255}, sc.flash * (i < P_GRASS ? 0.6f : 0.3f));
    static const float DQ[4] = {0.125f, 0.625f, 0.875f, 0.375f};   // 2x2 ordered offsets: (0,0) (1,0) (0,1) (1,1)
    for (int i = 0; i < 256; i++) {
        const float cr = clampf(P[i].r, 0.f, 255.f), cg = clampf(P[i].g, 0.f, 255.f), cb = clampf(P[i].b, 0.f, 255.f);
        palRGB[i * 3] = (uint8_t)(cr + 0.5f); palRGB[i * 3 + 1] = (uint8_t)(cg + 0.5f); palRGB[i * 3 + 2] = (uint8_t)(cb + 0.5f);
        const float r5 = cr * (31.f / 255.f), g6 = cg * (63.f / 255.f), b5 = cb * (31.f / 255.f);
        for (int q = 0; q < 4; q++) {
            int r = (int)(r5 + DQ[q]), g = (int)(g6 + DQ[q]), b = (int)(b5 + DQ[q]);
            if (r > 31) r = 31;
            if (g > 63) g = 63;
            if (b > 31) b = 31;
            const uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
            pal565q[q * 256 + i] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
    s_fogRGB[0] = clampf(S.fog.r, 0.f, 255.f); s_fogRGB[1] = clampf(S.fog.g, 0.f, 255.f); s_fogRGB[2] = clampf(S.fog.b, 0.f, 255.f);
    static bool fogMade = false;
    if (!fogMade) { for (int l = 0; l < 4; l++) buildFogLevel(palRGB, l); fogMade = true; }
    else fogTick();
}

// ramp tables for reflections and light shafts (static: built once)
static void buildLUTs() {
    for (int i = 0; i < 256; i++) { s_refl[i] = (uint8_t)i; s_bright[i] = (uint8_t)i; }
    auto r16 = [](int base) { for (int k = 0; k < 16; k++) { s_refl[base + k] = (uint8_t)(base + (k > 2 ? k - 2 : 0)); s_bright[base + k] = (uint8_t)(base + (k < 14 ? k + 2 : 15)); } };
    auto r8 = [](int base) { for (int k = 0; k < 8; k++) { s_refl[base + k] = (uint8_t)(base + (k > 1 ? k - 1 : 0)); s_bright[base + k] = (uint8_t)(base + (k < 7 ? k + 1 : 7)); } };
    r16(P_SKY); r16(P_CLOUD); r16(P_MTN); r16(P_HAZE); r16(P_GRASS); r16(P_SOIL); r16(P_ROCK); r16(P_ROAD); r16(P_LEAF);
    r8(P_SUN); r8(P_LEAF2); r8(P_SNOW); r8(P_BARK); r8(P_WATER);
    for (int k = 0; k < 8; k++) s_refl[P_SUN + k] = (uint8_t)(P_SUN + k);     // the sun glitters, undimmed
}

// ================================================================================== camera
static float cx, cy, cz, cyaw, F, fx, fz, rx, rz, hor0, rollT;
static float horX[W], kxC[W], dxC[W], dzC[W], azC[W], beamC[W], sideC[W];
static uint8_t ymin[W];
static float s_camYaw = 0.f, s_camY = 0.f;
static bool s_camInit = false;
static uint8_t s_camView = 255; static float s_camX = 0.f, s_camZ = 0.f;   // last frame's view and car (to re-seat)
static float s_t = 0.f;

bool renderProject(float wx, float wy, float wz, float& sx, float& sy, float& depth) {
    float dx = wx - cx, dz = wz - cz;
    depth = dx * fx + dz * fz;
    if (depth < 0.3f) return false;
    float lat = dx * rx + dz * rz;
    sx = W * 0.5f + lat / depth * F;
    sy = hor0 + (sx - W * 0.5f) * rollT - (wy - cy) * F / depth;
    return true;
}

static void drawDropView();
static bool s_flip = false;                       // a first-person view upside down: drawn upright, then turned over
static float s_camPitch = 0.f;
static float s_viewBase = H * 0.47f;
static void setupCamera() {
    const ViewInfo& v = g_view;
    const AircraftSpec& A = aircraftSpec(v.aircraft);
    float pitch, roll;
    s_flip = false;
    // the chase camera starts behind the aircraft whenever you come into that view or it is put somewhere new
    if (v.view != s_camView || (v.x - s_camX) * (v.x - s_camX) + (v.z - s_camZ) * (v.z - s_camZ) > 250.f * 250.f) s_camInit = false;
    s_camView = v.view; s_camX = v.x; s_camZ = v.z;
    float shake = v.shake * (hashf((uint32_t)(s_t * 997.f)) - 0.5f) * 3.f;
    if (v.photo) {
        cx = v.pX; cy = v.pY; cz = v.pZ; cyaw = v.pYaw;
        F = 96.f; pitch = v.pPitch; roll = 0.f;
        s_viewBase = H * 0.5f;
        hor0 = s_viewBase + tanf(pitch) * F;
    } else if (v.view == VIEW_CHASE) {
        // behind and a little above, following the heading and half the climb smoothly; the horizon stays level
        if (!s_camInit) { s_camYaw = v.yaw; s_camPitch = v.pitch * 0.5f; s_camInit = true; }
        const float k = clampf(g_scene.dt * 2.6f, 0.f, 1.f);
        s_camYaw += wrapPi(v.yaw - s_camYaw) * k;
        s_camPitch += (clampf(v.pitch, -1.f, 1.f) * 0.5f - s_camPitch) * k;
        const float back = A.chaseBack;
        cyaw = s_camYaw;
        const float cp = cosf(s_camPitch), sp = sinf(s_camPitch);
        cx = v.x - cosf(cyaw) * cp * back; cz = v.z - sinf(cyaw) * cp * back;
        cy = v.y - sp * back + back * 0.2f;
        const float gh = terrainHeight(cx, cz);
        cy = fmaxf(cy, gh + 1.6f);
        F = 120.f; pitch = s_camPitch - 0.16f; roll = 0.f;
        s_viewBase = H * 0.5f;
        hor0 = s_viewBase + tanf(pitch) * F + shake * 0.5f;
    } else {
        cyaw = v.yaw;
        cx = v.x + v.fx * A.eyeFwd + v.ux * A.eyeUp; cy = v.y + v.fy * A.eyeFwd + v.uy * A.eyeUp; cz = v.z + v.fz * A.eyeFwd + v.uz * A.eyeUp;
        float r = wrapPi(v.roll);
        if (fabsf(r) > 1.5708f) { s_flip = true; r = wrapPi(r - 3.14159265f); }
        roll = clampf(r, -1.15f, 1.15f);
        pitch = clampf(v.pitch, -1.2f, 1.2f);
        // speed perspective: the view opens up a little with speed
        {   static float s_fovK = 1.f;
            const float tgt = 1.f - 0.1f * clampf((v.ias - A.vCruise * 0.8f) / (A.vNe - A.vCruise * 0.8f + 1.f), 0.f, 1.f);
            s_fovK += (tgt - s_fovK) * clampf(g_scene.dt * 2.f, 0.f, 1.f);
            F = (v.view == VIEW_COCKPIT ? 88.f : 96.f) * s_fovK;
        }
        s_viewBase = v.view == VIEW_COCKPIT ? 38.f : H * 0.47f;
        hor0 = (s_flip ? (H - 1.f - s_viewBase) : s_viewBase) + tanf(pitch) * F + shake;
    }
    fx = cosf(cyaw); fz = sinf(cyaw); rx = -fz; rz = fx;
    g_pan = (int)lroundf(cyaw * F);
    rollT = -tanf(roll);                 // camera rolled right (right side down): the horizon's right end rises
    for (int x = 0; x < W; x++) {
        float kx = (x + 0.5f - W * 0.5f) / F;
        kxC[x] = kx; dxC[x] = fx + rx * kx; dzC[x] = fz + rz * kx;
        horX[x] = hor0 + (x + 0.5f - W * 0.5f) * rollT;
        azC[x] = cyaw + atanf(kx);
        float b = 1.f - (kx * kx) / 0.17f;
        beamC[x] = b > 0.f ? b : 0.f;
        sideC[x] = sinf(wrapPi(g_scene.sunAz - azC[x]));
    }
}

// ================================================================================== terrain
struct Smp { float h, lt; uint8_t mat, surf, extra; };   // (Rally's road distances are gone: Air has no roads)
static uint8_t* s_tex = nullptr;            // 64x64 value noise, 0..255
static inline int tex(int u, int v) { return s_tex[((v & 63) << 6) | (u & 63)]; }
// The dither for the ground, water and road: the same world-position texture lookup the ground's own detail uses
// (so it sits on the surface and moves with it), with the texture's values equalized so every effect keeps the
// same coverage the old screen pattern gave it.
static uint8_t s_texRank[256];
static bool s_rankMade = false;
static void makeTexRank() {
    int hist[256] = {0};
    for (int i = 0; i < 64 * 64; i++) hist[s_tex[i]]++;
    int acc = 0;
    for (int v = 0; v < 256; v++) { s_texRank[v] = (uint8_t)((acc + hist[v] / 2) * 255 / (64 * 64)); acc += hist[v]; }
    s_rankMade = true;
}
// The same fine grain as the old screen pattern (uniform, pixel-sized), but each grain belongs to a spot on the
// surface: a fixed random value per tiny patch of world, sized 1-2 pixels at its distance. Coverage is unchanged.
static inline float surfDith(float wx, float wz, float z) {
    int zi = (int)z;
    int L = zi > 1 ? 31 - __builtin_clz((unsigned)zi) : 0;          // halvings of the distance until 1 (as the old loop)
    if (L > 12) L = 12;
    uint32_t pb = (uint32_t)(127 - L) << 23; float inv2; memcpy(&inv2, &pb, 4);   // 2^-L exactly
    float s = 48.f * inv2;                                           // = 48 / 2^L, bit for bit
    uint32_t ix = (uint32_t)(int32_t)ifloor(wx * s), iz = (uint32_t)(int32_t)ifloor(wz * s);
    uint32_t h = ix * 0x8DA6B343u ^ iz * 0xD8163841u ^ (uint32_t)L * 0xCB1AB31Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return ((h >> 8) & 0xFFFF) * (1.f / 65536.f);
}
static uint8_t* s_gpat = nullptr;
static bool s_mistOn = false;                     // the mist band will read the grain under it this frame                 // per pixel: the surface's dither value (for the reflection pass)
// A ray sample, in two halves: its height (every sample needs it), and the rest (only for samples that draw: about a
// third of them are behind nearer land). Same cells, same weights, same arithmetic as the old sampleAt / sampleLite:
// near samples (lite = false) blend road distance and light; distant ones (lite = true) take them from the nearest cell.
struct Probe { const Cell *a, *b, *c, *d; const Grid* g; float tx, tz, h; bool lite; };
// a cell's light: the sun's, or the sky's only where the land shades it from the sun
// Cloud shadows: a soft blob map (32 x 32, 70 m a cell, wrapping both ways: no seams) drifting with the wind. Only on
// part-cloudy sunny days, and gently: at most ~16% off the light.
static uint8_t s_cloudMap[32 * 32];
static float s_cloudOx = 0.f, s_cloudOz = 0.f, s_cloudThr = 999.f, s_cloudAmt = 0.f;
static inline __attribute__((always_inline)) float cloudK(float x, float z) {
    const float u = (x + s_cloudOx) * (1.f / 70.f), v = (z + s_cloudOz) * (1.f / 70.f);
    const int iu = ifloor(u), iv = ifloor(v);
    float tu = u - iu, tv = v - iv;
    tu = tu * tu * (3.f - 2.f * tu); tv = tv * tv * (3.f - 2.f * tv);
    const int x0 = iu & 31, x1 = (iu + 1) & 31, z0 = (iv & 31) << 5, z1 = ((iv + 1) & 31) << 5;
    const float a = s_cloudMap[z0 | x0], b = s_cloudMap[z0 | x1], c = s_cloudMap[z1 | x0], d = s_cloudMap[z1 | x1];
    const float ab = a + (b - a) * tu, cd = c + (d - c) * tu;
    const float sh = clampf((ab + (cd - ab) * tv - s_cloudThr) * (1.f / 40.f), 0.f, 1.f);
    return 1.f - s_cloudAmt * sh;
}
static inline __attribute__((always_inline)) float cellLight(const Grid& g, const Cell& k) {
    const int i = (int)(&k - g.c);
    return (g.sh && ((g.sh[i >> 3] >> (i & 7)) & 1) ? s_lightSh : s_light)[k.nrm];
}
// check: test the window's bounds (false: the caller knows this spot is well inside them)
static inline __attribute__((always_inline)) bool probeAt(const Grid& g, int wrap, float wx, float wz, bool lite, Probe& p, bool check = true) {
    float gx = wx * g.inv, gz = wz * g.inv;
    int ix, iz;
    if (lite) { ix = (int)(gx + 4096.f) - 4096; iz = (int)(gz + 4096.f) - 4096; }
    else { ix = ifloor(gx); iz = ifloor(gz); }
    if (check && (ix < g.ox + 1 || iz < g.oz + 1 || ix + 2 >= g.ox + g.N || iz + 2 >= g.oz + g.N)) return false;
    p.tx = gx - ix; p.tz = gz - iz; p.lite = lite; p.g = &g;
    const int N = g.N;
    int x0 = ix - g.ox + (wrap & 0xFFFF); if (x0 >= N) x0 -= N;        // = ix mod N (the window's origin, wrapped once a frame)
    int z0 = iz - g.oz + (wrap >> 16); if (z0 >= N) z0 -= N;
    int x1 = x0 + 1 == N ? 0 : x0 + 1, z1 = z0 + 1 == N ? 0 : z0 + 1;
    p.a = &g.c[z0 * N + x0]; p.b = &g.c[z0 * N + x1]; p.c = &g.c[z1 * N + x0]; p.d = &g.c[z1 * N + x1];
    float h0 = p.a->h + (p.b->h - p.a->h) * p.tx, h1 = p.c->h + (p.d->h - p.c->h) * p.tx;
    p.h = (h0 + (h1 - h0) * p.tz) * 0.1f;
    return true;
}
static inline __attribute__((always_inline)) const Cell& probeNearest(const Probe& p) { return p.tx < 0.5f ? (p.tz < 0.5f ? *p.a : *p.c) : (p.tz < 0.5f ? *p.b : *p.d); }
static inline __attribute__((always_inline)) void probeAttr(const Probe& p, Smp& s) {
    const Cell &a = *p.a, &b = *p.b, &c = *p.c, &d = *p.d;
    const Cell& n = probeNearest(p);
    const float tx = p.tx, tz = p.tz;
    s.h = p.h;
    if (p.lite) s.lt = cellLight(*p.g, n);
    else {
        const float la = cellLight(*p.g, a), lb = cellLight(*p.g, b), lc = cellLight(*p.g, c), ld = cellLight(*p.g, d);
        float l0 = la + (lb - la) * tx, l1 = lc + (ld - lc) * tx;
        s.lt = l0 + (l1 - l0) * tz;
    }
    s.mat = n.mat & 15; s.surf = n.mat >> 4; s.extra = n.extra;
}
static float s_lwNear = 0.f;                // z * 0.55 below this: the paint is at its minimum width (no divide needed)

static float s_flowers = 0.f, s_wet = 0.f, s_paved = 1.f;

// a field's pixel (Cell::extra bit 7): rows of corn, potato mounds or grain, the rows fading to their average colour
// once they're too fine to draw. Flash: only field pixels call it.
__attribute__((noinline)) static uint8_t cropPix(int x, int y, float wx, float wz, float s, float z, uint8_t extra) {
    const int kind = (extra >> 5) & 3;
    const float sp = kind == 2 ? 0.35f : 0.9f;                       // metres between rows
    float f = ((extra & 0x10) ? wx : wz) * (1.f / sp); f -= floorf(f);
    const float row = fabsf(f - 0.5f) * 2.f;                         // 0 on the row, 1 in the furrow
    const float vis = clampf((sp * F / (z + 0.1f) - 1.5f) * (1.f / 1.5f), 0.f, 1.f);   // rows at least ~3 px apart: drawn
    const uint32_t hp = hash32((uint32_t)(int)floorf(wx * 3.f) * 0x9E3779B1u ^ (uint32_t)(int)floorf(wz * 3.f) * 0x85EBCA77u);   // a third of a metre
    if (kind == 0) {                                                 // corn: tall bright rows, narrow dark furrows, gold tassels
        if (row > 1.f - 0.3f * vis) return (uint8_t)(P_SOIL + clampi(band(s - 3.f, x, y), 0, 15));
        if (vis > 0.3f && row < 0.5f && (hp & 15) == 0) return (uint8_t)(P_SOIL + clampi(band(s + 4.f, x, y), 0, 15));
        return (uint8_t)(P_GRASS + clampi(band(s + 0.6f - (1.f - vis) * 0.5f - row * 0.8f * vis, x, y), 0, 15));
    }
    if (kind == 1) {                                                 // potatoes: low, dark leafy mounds on wide brown ridges
        if (row > 1.f - 0.55f * vis) return (uint8_t)(P_SOIL + clampi(band(s - 0.5f - (row - 0.45f) * 2.f, x, y), 0, 15));
        return (uint8_t)(P_LEAF + clampi(band(s - 1.2f - (1.f - vis) * 0.5f - row * 1.6f * vis + ((hp & 3) == 0 ? 1.f : 0.f), x, y), 0, 15));
    }
    // grain: ripe gold, fine rows, stalk speckle
    if (vis > 0.2f && (hp & 31) == 0) return (uint8_t)(P_GRASS + clampi(band(s + 1.5f, x, y), 0, 15));   // a green stalk here and there
    return (uint8_t)(P_SOIL + clampi(band(s + 5.f - (row > 0.7f ? 2.6f * vis : 0.f) - ((hp & 7) == 0 ? 1.5f : 0.f), x, y), 0, 15));
}

// ---- airfields: the runways' markings, worked out per pixel from the field's own numbers (airfield.h), so they're
// crisp at any distance. A slot table, refreshed each frame, names the fields near the camera.
struct Rwy { bool ok; float x, z, c, s, len, hw, y; uint8_t surf; int num; };
static Rwy s_rwy[16];
// 3x5 digits (rows top to bottom, 3 bits each, bit 2 the left column)
static const uint8_t DIG[10][5] = {{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,3,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,2,2},{7,5,7,5,7},{7,5,7,1,7}};
__attribute__((noinline)) static uint8_t runwayPix(int x, int y, float wx, float wz, float s, float z, uint8_t surf, uint8_t extra, bool& refl) {
    const Rwy& r = s_rwy[extra & 15];
    if (!r.ok) return (uint8_t)(P_GRASS + clampi(band(s + 0.5f, x, y), 0, 15));
    const float dx = wx - r.x, dz = wz - r.z;
    const float lon = dx * r.c + dz * r.s, lat = -dx * r.s + dz * r.c;
    const float alat = fabsf(lat), alon = fabsf(lon), half = r.len * 0.5f;
    const float foot = z / F;                                        // a pixel's size on the ground here (m)
    const float d = half - alon;                                     // distance in from the nearer end
    if (alat > r.hw || d < 0.f) {                                    // the verges: mown grass, stripes along the strip
        const float stripe = ((int)floorf((lon + 4000.f) * (1.f / 14.f)) & 1) ? 0.6f : 0.f;
        return (uint8_t)(P_GRASS + clampi(band(s + 0.8f + stripe * clampf(2.f - foot, 0.f, 1.f), x, y), 0, 15));
    }
    auto paint = [&](float cover) -> uint8_t {                       // white paint, or asphalt brightened by its share of the pixel
        if (cover > 0.55f) return (uint8_t)(P_LINE_W + clampi((int)(s * 0.25f + 0.2f), 0, 3));
        return (uint8_t)(P_ROAD + clampi(band(s * 0.9f + 1.f + cover * 8.f, x, y), 0, 15));
    };
    if (surf == SURF_TARMAC) {
        int lv = clampi(band(s * 0.9f + 1.f, x, y), 0, 15);
        // rubber in the touchdown zones: darker along the centre third
        if (d > 120.f && d < 600.f && alat < r.hw * 0.4f && foot < 3.f) lv = clampi(lv - (tex((int)(lon * 0.6f), (int)(lat * 3.f)) > 140 ? 2 : 1), 0, 15);
        uint8_t c = (uint8_t)(P_ROAD + lv);
        if (s_wet > 0.25f) refl = true;
        const float lw = fmaxf(0.45f, foot * 0.5f);
        if (alat > r.hw - 0.9f - foot * 0.3f && alat < r.hw - 0.25f) return paint(clampf(0.65f / fmaxf(foot, 0.65f), 0.f, 1.f));   // edge lines
        if (d > 6.f && d < 36.f && alat < r.hw - 2.5f) {                 // threshold keys
            float k = fmodf(alat, 3.4f);
            return k < 1.7f ? paint(clampf(1.7f / fmaxf(foot, 1.7f), 0.f, 1.f)) : c;
        }
        if (d > 46.f && d < 70.f) {                                       // the runway number, read from the approach
            const bool lowEnd = lon < 0.f;
            const int num = lowEnd ? r.num : (r.num + 18 - 1) % 36 + 1;
            const float rowF = (d - 46.f) * (1.f / 4.8f);                // 0..5 from the threshold
            const int row = 4 - (int)rowF;
            const float lx = (lowEnd ? lat : -lat) + 9.1f;               // from the pilot's left
            const int col = (int)floorf(lx * (1.f / 2.6f));
            if (row >= 0 && row < 5 && col >= 0 && col < 7 && col != 3) {
                const int dg = col < 3 ? num / 10 : num % 10, cc = col < 3 ? col : col - 4;
                if ((num >= 10 || col > 3) && ((DIG[dg][row] >> (2 - cc)) & 1)) return paint(clampf(2.6f / fmaxf(foot, 2.6f), 0.f, 1.f));
            }
            return c;
        }
        if (r.len > 900.f && d > 300.f && d < 345.f && alat > 4.f && alat < 9.f) return paint(1.f);   // aiming points
        if (d > 80.f && alat < lw && fmodf(lon + 4000.f, 50.f) < 30.f) return paint(clampf(0.9f / fmaxf(foot, 0.9f), 0.f, 1.f));   // centre line
        return c;
    }
    if (surf == SURF_GRASS) {                                            // a grass strip: mown in bands, white markers
        const float stripe = ((int)floorf((lat + 100.f) * (1.f / 6.f)) & 1) ? 0.9f : 0.f;
        if (d < 4.f && fmodf(alat, 6.f) < 2.5f) return paint(1.f);
        if (alat > r.hw - 1.2f && fmodf(lon + 4000.f, 60.f) < 2.f) return paint(clampf(1.5f / fmaxf(foot, 1.5f), 0.f, 1.f));
        return (uint8_t)(P_GRASS + clampi(band(s + 1.2f + stripe * clampf(2.f - foot * 0.5f, 0.f, 1.f), x, y), 0, 15));
    }
    // dirt and gravel: packed and pale down the middle, wheel tracks, white markers at the edges
    float sh = s + 1.2f + (surf == SURF_GRAVEL ? 0.8f : 0.f);
    if (fabsf(alat - 3.f) < 0.8f) sh -= 0.8f;
    if (d < 4.f && fmodf(alat, 6.f) < 2.5f) return paint(1.f);
    if (alat > r.hw - 1.2f && fmodf(lon + 4000.f, 60.f) < 2.f) return paint(clampf(1.5f / fmaxf(foot, 1.5f), 0.f, 1.f));
    return (uint8_t)(P_SOIL + clampi(band(sh + (tex((int)(wx * 2.f), (int)(wz * 2.f)) - 128) * (1.f / 90.f), x, y), 0, 15));
}

// one ground pixel. Returns the palette index; sets *refl for water / wet surfaces.
static inline uint8_t groundPix(int x, int y, float wx, float wz, float lt, float z, float hgt,
                                uint8_t mat, uint8_t surf, bool farSmp, uint8_t extra, bool& refl) {
    int scl = z < 12.f ? 5 : (z < 35.f ? 2 : 1);
    const int tu = (int)(wx * scl), tv = (int)(wz * scl);
    int n = tex(tu, tv);
    float nf = n * (1.f / 255.f);
    // more of the ramp for the light: shade lands where it always did (step 4), full sun about a step higher
    float s = lt * 13.5f - 0.48f + (nf - 0.5f) * 3.f;
    // grain facing the sun: lit. Never across the texture tile's edge: the tile repeats seamlessly in u but not in v, and
    // the difference across that seam drew a stripe every tile (12.8 m near, 32 m, 64 m far)
    if (s_reliefK > 0.f && !(((tv + s_rdv) ^ tv) & ~63) && !(((tu + s_rdu) ^ tu) & ~63))
        s += (float)(n - tex(tu + s_rdu, tv + s_rdv)) * s_reliefK;
    if (s_lights > 0.02f && z < 75.f) s += s_lights * beamC[x] * (10.f * HEAD_K) * (z + 3.f <= 11.f ? 1.f : fminf(1.f, 11.f / (z + 3.f))) * (0.35f + 0.65f * s_night);
    uint8_t c;
    {   // (Rally's road paint, ruts and shoulders were here: Air has no roads)
        switch (mat) {
            case MAT_WATER: {
                int r = (int)((nf - 0.5f) * 4.f + 3.f + (z < 20.f ? 1.f : 0.f));
                refl = true;
                return (uint8_t)(P_WATER + clampi(r, 0, 7));
            }
            case MAT_ROCK: {
                float strata = sinf(hgt * 2.4f + nf * 2.f) * 1.3f;
                c = (uint8_t)(P_ROCK + clampi(band(s + strata + 0.5f, x, y), 0, 15));
                break;
            }
            case MAT_SNOW: c = (uint8_t)(P_SNOW + clampi((int)(s * 0.55f + 1.f + dith(x, y)), 0, 7)); break;
            case MAT_SAND: c = (uint8_t)(P_SOIL + clampi(band(s + 2.f + sinf(wx * 0.9f + wz * 0.5f) * 0.9f, x, y), 0, 15)); break;
            case MAT_SOIL: c = (extra & 0x80) ? cropPix(x, y, wx, wz, s, z, extra) : (uint8_t)(P_SOIL + clampi(band(s, x, y), 0, 15)); break;
            case MAT_MUD:
                c = (uint8_t)(P_SOIL + clampi(band(s * 0.6f - 1.f, x, y), 0, 15));
                if (nf > 0.6f) { refl = true; }
                break;
            case MAT_LOT:                                    // the apron: tarmac or gravel
                if (surf == SURF_TARMAC) { c = (uint8_t)(P_ROAD + clampi(band(s * 0.9f + 1.8f, x, y), 0, 15)); if (s_wet > 0.25f) refl = true; }
                else c = (uint8_t)(P_SOIL + clampi(band(s + 1.4f, x, y), 0, 15));
                break;
            case MAT_RUNWAY: return runwayPix(x, y, wx, wz, s, z, surf, extra, refl);
            default: {
                if (farSmp && extra > 0 && extra < 0x80 && mat < MAT_WATER) {                      // distant forest canopy
                    int lv = (int)(s * 0.8f - 1.f + nf * 3.f);
                    return (uint8_t)(P_LEAF + clampi(lv, 0, 15));
                }
                float blades = z < 14.f ? (tex((int)(wx * 9.f), (int)(wz * 3.f)) - 128) * (1.f / 60.f) : 0.f;
                c = (uint8_t)(P_GRASS + clampi(band(s + blades, x, y), 0, 15));
                if (s_flowers > 0.05f && z < 40.f) {
                    int fl = tex((int)(wx * 3.f) * 7 + 13, (int)(wz * 3.f) * 5 + 29);
                    if (fl > 250 - (int)(s_flowers * 30.f)) c = (uint8_t)(P_FLOWER + ((int)(wx * 3.f + wz * 7.f) & 7));
                }
            }
        }
    }
    return c;
}

// How far a column's ray (direction ddx, ddz from the camera) certainly stays inside the near cache's usable cells.
// Once a column: flash (not inlined into IRAM).
__attribute__((noinline)) static float nearSafeZ(const Grid& gN, float ddx, float ddz) {
    const float ax = cx * gN.inv, bx = ddx * gN.inv, az = cz * gN.inv, bz = ddz * gN.inv;
    const float lox = gN.ox + 1.25f, hix = gN.ox + gN.N - 3.25f, loz = gN.oz + 1.25f, hiz = gN.oz + gN.N - 3.25f;
    if (!(ax > lox && ax < hix && az > loz && az < hiz)) return 0.f;
    float sx = bx > 0.f ? (hix - ax) / bx : (bx < 0.f ? (lox - ax) / bx : 1e30f);
    float sz = bz > 0.f ? (hiz - az) / bz : (bz < 0.f ? (loz - az) / bz : 1e30f);
    return sx < sz ? sx : sz;
}
// The terrain, a column at a time: columns are independent (each writes only its own pixels and ymin), so both
// cores draw them, taking blocks of columns from one counter: the second core (a task below the sound and the screen)
// takes what its spare time allows and this one the rest. The pixels are exactly those of one core drawing them all.
static float s_landReach = 1e9f;                    // how far ahead the land goes this frame (m)
static volatile bool s_anyRefl = false;             // this frame's land has water or a wet surface in view (else no reflection passes)
static int s_tNext = 0;                             // the next block of columns
static float s_tZFar = 0.f;
static constexpr int TCOLS = 4;
MR_HOT static void terrainColumns() {
    const float zFar = s_tZFar;
    const Grid& gN = terrainNear();
    const Grid& gF = terrainFar();
    const Grid& gV = terrainVast();
    float maxH = terrainMaxH();
    const int wrapN = gridWrap(gN.ox, gN.N) | (gridWrap(gN.oz, gN.N) << 16);   // the windows' origins, wrapped (once)
    const int wrapF = gridWrap(gF.ox, gF.N) | (gridWrap(gF.oz, gF.N) << 16);
    const int wrapV = gV.c ? (gridWrap(gV.ox, gV.N) | (gridWrap(gV.oz, gV.N) << 16)) : 0;
    const float above = cy - maxH;                       // the camera's height over the highest land anywhere
    const float topK = (maxH - cy) * F;                  // the stop test, multiplied out (no divide per sample)
    Smp s = Smp(), q = Smp();
    Probe pr = Probe(), pp = Probe();
    for (;;) {
        const int xb = __atomic_fetch_add(&s_tNext, TCOLS, __ATOMIC_RELAXED);
        if (xb >= W) break;
        const int xe = xb + TCOLS < W ? xb + TCOLS : W;
        for (int x = xb; x < xe; x++) {
            int y = s_bottom[x];
            const float ddx = dxC[x], ddz = dzC[x], hx = horX[x];
            const int mist0 = (int)hx - 10, mist1 = (int)hx + 18;   // the mist band's rows
            float z = 0.55f;
            // from altitude, everything nearer than where the lowest row's ray meets the highest land lies below the
            // picture: start there (no land can rise into the picture nearer than that)
            if (above > 0.f && (float)y - 1.f > hx + 0.5f) { const float zs = above * F / ((float)y - 1.f - hx) * 0.97f; if (zs > z) z = zs; }
            // previous sample (for per-pixel interpolation)
            float pY = (float)H + 40.f, pz = z, pwx = cx + ddx * z, pwz = cz + ddz * z, plt = 0.5f, ph = cy - 2.f;
            uint8_t pmat = 0, psurf = 0, pextra = 0; bool pfar = false;
            bool first = true;
            int lvl = 0;                     // which cache the ray is in: 0 near, 1 far, 2 vast (it only ever moves out)
            bool pHave = false;              // the previous sample's road distance, light and material are worked out
            bool pYok = true;                // ... and its screen row (a hidden one's is worked out only if needed)
            uint8_t zcC = 0; float zLo = 1e30f, zHi = -1.f;   // the depth code: worked out again only where it can change
            // How far this column's ray certainly stays inside the near cache's usable cells: no bounds test per sample
            // before that. The margins (a quarter cell, and a whole one more at the far side) are far beyond the rounding
            // of the sample's own cell (whichever way it is rounded), so the test would pass at every sample skipped.
            const float zSafe = nearSafeZ(gN, ddx, ddz);
            while (z < zFar && y > 0) {
                float wx = cx + ddx * z, wz = cz + ddz * z;
                if (lvl == 0 && !probeAt(gN, wrapN, wx, wz, !(z < 45.f), pr, z >= zSafe)) lvl = 1;
                if (lvl == 1 && !probeAt(gF, wrapF, wx, wz, true, pr)) lvl = 2;
                if (lvl == 2 && (!gV.c || !probeAt(gV, wrapV, wx, wz, true, pr))) break;
                const bool far = lvl > 0;
                if (far) {                                  // forest canopy stands on the far and vast land
                    const Cell& n = probeNearest(pr);
                    if (n.extra && n.extra < 0x80 && (n.mat & 15) < MAT_WATER) pr.h += n.extra * (0.35f + 0.65f * tex((int)(wx * 0.6f), (int)(wz * 0.6f)) * (1.f / 255.f));
                }
                const float A = (pr.h - cy) * F;
                float ys = 0.f;
                bool vis;
                if (first) {
                    ys = hx - A / z;
                    pY = fmaxf(ys, (float)H + 1.f); first = false; pp = pr; pHave = false; pYok = true; ph = pr.h; pfar = far;
                    vis = ys < y;
                } else {
                    // hidden behind what's drawn, for certain (margin far above rounding): no divide, no row
                    const float B = (hx - (float)y) * z;
                    if (B - A > 0.02f * z + (fabsf(A) + fabsf(B)) * 1e-6f) vis = false;
                    else { ys = hx - A / z; vis = ys < y; }
                }
                int yTop = vis ? iceil(ys) : 0;
                if (yTop < 0) yTop = 0;
                if (vis && yTop >= y) {
                    // above the drawn ground, but not by a whole row: nothing to draw. Its road distance, light and material
                    // are worked out only if the next sample draws (exactly the values they would have had now)
                    pY = ys; pYok = true; pHave = false;
                } else if (vis) {
                    if (!pYok) { pY = hx - (ph - cy) * F / pz; pYok = true; }      // exactly the row it would have had
                    for (int k = pHave ? 1 : 0; k < 2; k++) probeAttr(k ? pr : pp, k ? s : q);
                    if (s_cloudAmt > 0.f) { s.lt *= cloudK(wx, wz); if (!pHave) q.lt *= cloudK(pwx, pwz); }   // cloud shadows
                    if (!pHave) { plt = q.lt; pmat = q.mat; psurf = q.surf; pextra = q.extra; }
                    float span = pY - ys;
                    float inv = span > 0.001f ? 1.f / span : 0.f;
                    float fogNear = fogAt(pz), fogFar = fogAt(z);
                    for (int yy = y - 1; yy >= yTop; yy--) {
                        float t = clampf((pY - yy) * inv, 0.f, 1.f);
                        float zz = pz + (z - pz) * t;
                        float wxx = pwx + (wx - pwx) * t, wzz = pwz + (wz - pwz) * t;
                        float lt = plt + (s.lt - plt) * t;
                        float hg = ph + (s.h - ph) * t;
                        // this spot of ground's own grain: only worked out where it matters (an edge between materials,
                        // fog, a reflection, mist); everywhere else it would change nothing
                        float sd = -1.f;
                        bool edge = s.mat != pmat || s.surf != psurf;
                        if (edge) sd = surfDith(wxx, wzz, zz);
                        bool useCur = edge ? t > sd : t > 0.5f;
                        uint8_t mat = useCur ? s.mat : pmat, surf = useCur ? s.surf : psurf;
                        bool refl = false;
                        uint8_t c = groundPix(x, yy, wxx, wzz, lt, zz, hg, mat, surf, useCur ? far : pfar, useCur ? s.extra : pextra, refl);
                        float fg = fogNear + (fogFar - fogNear) * t;
                        {                                                   // fog: the palette way, stepping to fogged twins
                            float fa = clampf((fg - 0.3f) * (1.f / 0.42f), 0.f, 1.f);
                            if (fa > 0.f) {
                                float f4 = fa * 4.f; int lv = (int)f4; float fr = f4 - lv;
                                if (fr > 0.86f) lv++;
                                else if (fr > 0.62f) { if (sd < 0.f) sd = surfDith(wxx, wzz, zz); if (sd < 0.5f) lv++; }   // the grain only in the seam
                                if (lv > 0) c = s_fogLut[lv > 4 ? 3 : lv - 1][c];
                                if (fa > 0.5f) refl = false;
                            }
                        }
                        int i = yy * W + x;
                        fb[i] = c;
                        if (s_capWater && mat == MAT_WATER) s_wmask[i] = 1;
                        if (!(zz >= zLo && zz < zHi)) {             // exactly zcode(zz): up a column depth only grows, and the
                            zcC = zcode(zz); zLo = zz;              // code only steps at known depths (s_zInv)
                            zHi = zcC >= 252 ? 1e30f : zdepth(zcC + 2) * 0.999f;
                        }
                        zb[i] = (uint8_t)(zcC | (refl ? 1 : 0));
                        if (refl) s_anyRefl = true;
                        bool mistRow = s_mistOn && yy >= mist0 && yy < mist1;   // only the mist band's rows
                        if (s_gpat && (refl || mistRow)) { if (sd < 0.f) sd = surfDith(wxx, wzz, zz); s_gpat[i] = (uint8_t)(sd * 255.f); }
                    }
                    y = yTop;
                    pY = ys; pYok = true;
                    plt = s.lt; pmat = s.mat; psurf = s.surf; pextra = s.extra; pHave = true;
                } else {
                    pYok = false; pHave = false;            // its row and the rest only if the next sample draws
                }
                pz = z; pwx = wx; pwz = wz; ph = pr.h; pfar = far; pp = pr;
                // nothing beyond can rise above what is drawn: done (y <= hx - topK / z, multiplied out)
                if ((float)y <= hx && (hx - (float)y) * z >= topK) break;   // a level or rising ray above the highest land: done
                z += lvl == 0 ? 0.12f + z * 0.038f : (lvl == 1 ? 0.6f + z * 0.045f : 3.f + z * 0.03f);
            }
            ymin[x] = (uint8_t)clampi(y, 0, H);
        }
    }
}
#ifndef HOST
static TaskHandle_t s_tWorker = nullptr;
static SemaphoreHandle_t s_tDone = nullptr;
static void terrainWorker(void*) {
    for (;;) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); terrainColumns(); xSemaphoreGive(s_tDone); }
}
#endif
static void drawTerrain(float zFar) {
    s_tZFar = zFar;
    s_anyRefl = false;
    s_lwNear = 0.0999f * F;
    __atomic_store_n(&s_tNext, 0, __ATOMIC_RELEASE);
#ifndef HOST
    const bool help = s_tWorker != nullptr;
    if (help) xTaskNotifyGive(s_tWorker);               // (the notify and the take below are full barriers)
#endif
    terrainColumns();
#ifndef HOST
    if (help) xSemaphoreTake(s_tDone, portMAX_DELAY);  // its last block is done
#endif
}

// ================================================================================== panorama and sky
static float hFar[W + 2], hNear[W + 2];
static float s_skyPow[80], s_skyTT[80];
static float s_sunX, s_sunY, s_sunVis, s_lowSun;

// The ranges live in a ring around the whole horizon (1024 steps), refreshed a slice a frame and looked up by
// heading: turning costs nothing, and the land only morphs slowly with distance.
static constexpr int PAN_N = 1024;
static float* s_panFar = nullptr;
static float* s_panNear = nullptr;
static int s_panCur = 0;
static bool s_panFull = true;
static float s_panPeaks = -1.f; static bool s_panMesa = false; static float s_panMorph = 0.f;
__attribute__((noinline)) static void panoramaRefresh(int count) {   // once a frame: flash (not inlined into IRAM)
    const Scene& sc = g_scene;
    const Biome& A = biome(sc.biomeA); const Biome& B = biome(sc.biomeB);
    float peaks = A.peaks + (B.peaks - A.peaks) * sc.blend;
    bool mesa = (sc.blend < 0.5f ? sc.biomeA : sc.biomeB) == B_CANYON;
    float morph = sc.distance * 0.00015f;
    for (int n = 0; n < count; n++) {
        int k = s_panCur; s_panCur = (s_panCur + 1) & (PAN_N - 1);
        float u = k * (12.f / PAN_N);
        float mass = fbmP(u, morph, 12, 3, worldSeed() + 0x51);                   // broad massifs
        float r = 1.f - fabsf(2.f * fbmP(u * 3.f, morph + 3.f, 36, 2, worldSeed() + 0x52) - 1.f);   // ridged detail
        float hf = (mass - 0.38f) * 1.6f + r * r * 0.35f * mass;
        hf = fmaxf(0.f, hf) * peaks * 62.f;
        if (mesa) hf = floorf(hf / 9.f) * 9.f + 4.f * clampf(fmodf(hf, 9.f) - 6.f, 0.f, 1.f);
        float f2 = fbmP(u * 2.f, morph * 1.6f, 24, 3, worldSeed() + 0x53);
        s_panFar[k] = hf;
        s_panNear[k] = (f2 - 0.42f) * (10.f + peaks * 26.f);
    }
}
__attribute__((noinline)) static void computePanorama() {            // once a frame: flash (not inlined into IRAM)
    if (!s_panFar) { s_panFar = (float*)memSlow(PAN_N * sizeof(float)); s_panNear = (float*)memSlow(PAN_N * sizeof(float)); s_panFull = true; }   // read ~160x a frame: PSRAM is fine
    {   const Scene& sc = g_scene;
        const Biome& A = biome(sc.biomeA); const Biome& B = biome(sc.biomeB);
        float peaks = A.peaks + (B.peaks - A.peaks) * sc.blend;
        bool mesa = (sc.blend < 0.5f ? sc.biomeA : sc.biomeB) == B_CANYON;
        float morph = sc.distance * 0.00015f;
        if (fabsf(peaks - s_panPeaks) > 0.01f || mesa != s_panMesa || fabsf(morph - s_panMorph) > 0.02f) s_panFull = true;
        s_panPeaks = peaks; s_panMesa = mesa; s_panMorph = morph;
    }
    panoramaRefresh(s_panFull ? PAN_N : PAN_N / 8);
    s_panFull = false;
    float lift = (cy - journeyBase(cx, cz)) * F / 1800.f;
    const float toIdx = PAN_N / 6.2831853f;
    for (int x = -1; x <= W; x++) {
        int xi = clampi(x, 0, W - 1);
        float a = azC[xi] + (x - xi) / F;
        float f = a * toIdx;
        f -= PAN_N * floorf(f / PAN_N);
        int i0 = (int)f; float t = f - i0;
        int i1 = (i0 + 1) & (PAN_N - 1); i0 &= PAN_N - 1;
        float hf = s_panFar[i0] + (s_panFar[i1] - s_panFar[i0]) * t;
        float hn = s_panNear[i0] + (s_panNear[i1] - s_panNear[i0]) * t;
        hFar[x + 1] = fmaxf(0.f, hf * F / 96.f - lift);
        hNear[x + 1] = fmaxf(0.f, hn * F / 96.f - lift * 2.f);
    }
}

// once a frame, internal RAM only (no PSRAM to evict it from the cache): flash, paying for the terrain's shade bits in IRAM
__attribute__((noinline)) static void drawRidge(const float* hh, int far, float snowLine) {
    for (int x = 0; x < W; x++) {
        float h = hh[x + 1];
        int baseY = (int)horX[x] + (far ? 1 : 3);
        if (h <= 0.f) continue;
        float top = horX[x] - h;
        float slope = (hh[x + 2] - hh[x]) * 0.5f;
        float face = -slope * sideC[x];
        float lit = 0.5f + 0.3f * face / (1.f + fabsf(face));
        float u = angU(azC[x], 240);
        int y0 = clampi((int)ceilf(top), 0, H);
        int y1 = baseY < ymin[x] ? baseY : ymin[x];
        float snowDepth = h > snowLine ? (h - snowLine) * 0.55f : -1.f;
        for (int y = y0; y < y1; y++) {
            float d = y - top, depth = d / h;
            float g = tex((int)(u * 0.5f + d * 0.25f * (sideC[x] >= 0.f ? 1.f : -1.f)) + (far ? 17 : 0), (int)(d * 0.4f)) * (1.f / 255.f);
            float s = lit + (g - 0.5f) * 0.38f * (0.4f + 0.6f * (1.f - depth)) - depth * 0.3f;
            if (snowDepth > 0.f && d < snowDepth + g * 3.f) s = 0.95f;
            int lv = clampi((int)(s * 7.99f), 0, 7);
            if (d < 1.f && face > 0.15f) lv = clampi(lv + 2, 0, 7);
            fb[y * W + x] = (uint8_t)(P_MTN + (far ? 0 : 8) + lv);
            zb[y * W + x] = 254;
        }
    }
}

MR_HOT static void drawSkyAndPanorama() {
    const Scene& sc = g_scene;
    computePanorama();
    // fill between the far terrain and the horizon with distant hills
    // (from altitude: the land beyond the caches, in the haze: the ground's fogged twins, the thickest at the horizon)
    {   const uint8_t g2 = s_fogLut[2][P_GRASS + 6], g3 = s_fogLut[3][P_GRASS + 6];
        for (int x = 0; x < W; x++) {
            const int y0 = (int)horX[x] + 1, y1 = ymin[x];
            const float inv = y1 > y0 ? 1.f / (y1 - y0) : 0.f;
            for (int y = y0 < 0 ? 0 : y0; y < y1; y++) {
                const float f = (y - y0) * inv;                     // 0 at the horizon, 1 where the land stopped
                fb[y * W + x] = f > 0.55f + 0.3f * dith(x, y) ? g2 : g3; zb[y * W + x] = 254;
            }
        }
    }
    float snowF = 26.f * F / 96.f;
    drawRidge(hNear, 0, 999.f);
    // the far range sits behind the near one: only where the sky still shows
    static uint8_t top[W];
    for (int x = 0; x < W; x++) {
        float hn = hNear[x + 1];
        int t = hn > 0.f ? (int)ceilf(horX[x] - hn) : (int)horX[x] + 3;
        if (t > ymin[x]) t = ymin[x];
        top[x] = (uint8_t)clampi(t, 0, H);
    }
    {
        for (int x = 0; x < W; x++) {
            float h = hFar[x + 1];
            if (h <= 0.f) continue;
            float tp = horX[x] - h;
            float slope = (hFar[x + 2] - hFar[x]) * 0.5f;
            float face = -slope * sideC[x];
            float lit = 0.5f + 0.3f * face / (1.f + fabsf(face));
            for (int y = clampi((int)ceilf(tp), 0, H); y < top[x]; y++) {
                float d = y - tp;
                float s = lit - d / h * 0.25f;
                if (h > snowF && d < (h - snowF) * 0.5f + 1.f) s = 0.92f;
                fb[y * W + x] = (uint8_t)(P_MTN + clampi((int)(s * 7.99f), 0, 7));
                zb[y * W + x] = 254;
            }
            int t2 = (int)ceilf(tp);
            if (t2 < top[x]) top[x] = (uint8_t)clampi(t2, 0, H);
        }
    }
    // sky (Planet's gradient + horizon glow + sun halo)
    float sunRel = wrapPi(sc.sunAz - cyaw);
    s_sunVis = (sc.sunElev > -0.12f ? 1.f : 0.f) * clampf(1.f - sc.cloud * 0.85f, 0.f, 1.f);
    s_lowSun = clampf(1.f - sc.sunElev / 0.5f, 0.f, 1.f) * (sc.sunElev > -0.15f ? 1.f : 0.f);
    s_sunX = W * 0.5f + tanf(clampf(sunRel, -1.4f, 1.4f)) * F;
    s_sunY = hor0 - tanf(sc.sunElev) * F + (s_sunX - W * 0.5f) * rollT;
    if (fabsf(sunRel) > 1.4f) s_sunX = sunRel > 0 ? 9999.f : -9999.f;
    for (int x = 0; x < W; x++) {
        float rel = wrapPi(sc.sunAz - azC[x]);
        float hglow = s_sunVis * s_lowSun / (1.f + rel * rel * 3.f);
        float skyH = horX[x] + 2.f;
        for (int y = 0; y < top[x]; y++) {
            int k = clampi((int)(y + 0.5f - (skyH - 78.f)), 0, 78);
            float base = s_skyPow[k], tt = s_skyTT[k];
            float halo = 0.f;
            if (s_sunVis > 0.f) { float dx = x - s_sunX, dy = y - s_sunY; halo = s_sunVis * 5.5f / (1.f + (dx * dx + dy * dy) * (1.f / 260.f)); }
            fb[y * W + x] = (uint8_t)(P_SKY + clampi(band(base + hglow * tt * 6.5f + halo, x, y), 0, 15));
            zb[y * W + x] = 255;
        }
    }
    // rainbow: a 42 degree circle round the point opposite the sun
    if (sc.rainbow > 0.05f && sc.night < 0.5f) {
        float antiAz = sc.sunAz + 3.14159f, antiEl = -sc.sunElev;
        // the band is 0.69..0.75 rad from the antisolar point: rows outside +-0.76 rad of its elevation can't be in it
        const float elLo = antiEl - 0.76f, elHi = antiEl + 0.76f;
        const float tHi = elHi < 1.55f ? tanf(elHi) : 1e6f, tLo = elLo > -1.55f ? tanf(elLo) : -1e6f;
        for (int x = 0; x < W; x++) {
            float da = wrapPi(azC[x] - antiAz);
            if (fabsf(da) > 0.76f) continue;
            int yA = (int)(horX[x] - tHi * F) - 2, yB = (int)(horX[x] - tLo * F) + 3;   // el <= elHi .. el >= elLo, with room
            if (yA < 0) yA = 0;
            for (int y = yA; y < ymin[x] && y < H && y < yB; y++) {
                if (zb[y * W + x] < 254) continue;
                float el = atanf((horX[x] - y) / F);
                float r = sqrtf(da * da + (el - antiEl) * (el - antiEl));
                if (r < 0.69f || r > 0.75f) continue;
                if (sc.rainbow * 0.75f < dith(x, y)) continue;
                fb[y * W + x] = (uint8_t)(P_SPECIAL + clampi((int)((r - 0.69f) / 0.06f * 7.f), 0, 6));
            }
        }
    }
}

static constexpr int CL_N = 512, CL_R = 26;
static uint8_t* s_cl = nullptr;                // cloud density around the horizon [angle][row]
static int s_clCur = 0;
static bool s_clFull = true;
__attribute__((noinline)) static void cloudRefresh(int count) {      // once a frame: flash (not inlined into IRAM)
    const Scene& sc = g_scene;
    for (int n = 0; n < count; n++) {
        int k = s_clCur; s_clCur = (s_clCur + 1) % CL_N;
        float u = k * (36.f / CL_N);
        for (int r = 0; r < CL_R; r++) {
            float v = r * 0.15f + sc.distance * 0.001f + s_t * 0.004f;
            float nn = (tex((int)(u * 3.556f), (int)(v * 8.f)) * 0.62f + tex((int)(u * 7.11f) + 21, (int)(v * 16.f) + 9) * 0.38f) * (1.f / 255.f);
            nn = (nn - 0.5f) * 1.5f + 0.5f;
            s_cl[k * CL_R + r] = (uint8_t)clampi((int)(nn * 255.f), 0, 255);
        }
    }
}
MR_HOT static void drawClouds() {                                 // Planet's banded pixel clouds
    const Scene& sc = g_scene;
    if (sc.cloud < 0.04f) return;
    const int CR = 26;
    float thresh = 0.92f - 0.62f * sc.cloud + 0.12f * s_night * (1.f - sc.cloud);
    float drift = s_t * 0.006f * (1.f + 2.f * sc.wind);
    float centre = CR * (0.42f + 0.2f * sc.storm);
    if (!s_cl) { s_cl = (uint8_t*)memSlow(CL_N * CL_R); s_clFull = true; }   // read ~2,000x a frame: PSRAM is fine
    if (!s_cl) return;
    cloudRefresh(s_clFull ? CL_N : CL_N / 8);
    s_clFull = false;
    const float toIdx = CL_N / 6.2831853f;
    for (int x = 0; x < W; x += 2) {
        float a = azC[x] + drift;
        float fi = a * toIdx; fi -= CL_N * floorf(fi / CL_N);
        const uint8_t* col = s_cl + ((int)fi % CL_N) * CL_R;
        float rel = wrapPi(sc.sunAz - a);
        float prox = s_sunVis / (1.f + rel * rel * 6.f);
        float prevD = -1.f;
        float n = 0.f;
        for (int y = 0; y < CR * 2; y++) {
            int yy = y + (int)(horX[x] - 70.f);
            if (yy < 0 || yy >= H) continue;
            float cyf = y * 0.5f;
            n = col[y >> 1] * (1.f / 255.f);
            float prof = clampf(1.f - fabsf(cyf - centre) / (CR * 0.6f), 0.f, 1.f);
            float d = n * 0.8f + prof * 0.35f - thresh;
            float dn = d;
            if (d <= 0.f) { prevD = d; continue; }
            float alpha = clampf(d * 11.f, 0.f, 1.f);
            float top = clampf((d - prevD) * 6.f, 0.f, 1.f);
            prevD = dn;
            for (int k = 0; k < 2; k++) {
                int xx = x + k;
                if (xx >= W) break;
                int i = yy * W + xx;
                if (zb[i] < 255) continue;
                if (alpha < 0.45f || (alpha < 0.7f && ((xx ^ yy) & 1))) continue;
                float light = 0.3f + top * 0.45f * (1.f - s_lowSun) + prox * 0.35f + (1.f - alpha) * 0.2f + s_lowSun * 0.25f * (1.f - cyf / CR);
                light *= 1.f - 0.4f * sc.rain - 0.3f * sc.storm;
                int lvl = clampi(band(light * 4.f, xx, yy), 0, 4) * 3 + 2;
                fb[i] = (uint8_t)(P_CLOUD + lvl);
            }
        }
    }
}

static constexpr int STAR_N = 1000;
static void drawNight() {
    const Scene& sc = g_scene;
    float n = s_night;
    if (n < 0.08f) return;
    uint32_t tw = (uint32_t)(s_t * 5.f);
    static float* starAz = nullptr; static float* starEl = nullptr; static uint8_t* starB = nullptr; static bool made = false;
    if (!starAz) {                                       // (PSRAM: read once a frame at night, not worth internal RAM)
        starAz = (float*)memSlow(STAR_N * sizeof(float)); starEl = (float*)memSlow(STAR_N * sizeof(float)); starB = (uint8_t*)memSlow(STAR_N);
        if (!starAz || !starEl || !starB) return;
    }
    if (!made) {
        for (int k = 0; k < STAR_N; k++) {
            uint32_t h = hash32((uint32_t)k * 2654435761u ^ 0x57A2u);
            starAz[k] = (h & 0xFFFF) * (6.2831853f / 65536.f);
            float e = ((h >> 16) & 0x3FF) / 1023.f;
            starEl[k] = atanf(0.02f + e * 0.8f);              // evenly over the rows of sky, like the old per-pixel scan
            starB[k] = (uint8_t)(3 + (hash32(h) % 5));
        }
        made = true;
    }
    float dens = n * (1.f - sc.cloud * 0.8f) * (sc.biomeA == B_TUNDRA ? 1.f : 0.4f);
    int count = (int)(STAR_N * clampf(dens, 0.f, 1.f));
    for (int k = 0; k < count; k++) {
        float rel = wrapPi(starAz[k] - cyaw);
        if (fabsf(rel) > 0.8f) continue;
        float sx = W * 0.5f + tanf(rel) * F;
        int x = (int)sx;
        if ((unsigned)x >= (unsigned)W) continue;
        int y = (int)(horX[x] - tanf(starEl[k]) * F);
        if ((unsigned)y >= (unsigned)H) continue;
        int i = y * W + x;
        if (zb[i] != 255 || fb[i] >= P_CLOUD) continue;
        int b = starB[k];
        if ((hash32((uint32_t)k ^ tw) & 7u) == 0) b -= 3;
        fb[i] = (uint8_t)(P_SUN + clampi(b, 0, 7));
    }
    // moon
    float mrel = wrapPi(sc.sunAz + 3.14159f - cyaw);
    if (n > 0.3f && fabsf(mrel) < 1.2f) {
        float mx = W * 0.5f + tanf(mrel) * F, my = horX[0] - 40.f;
        for (int dy = -5; dy <= 5; dy++) for (int dx = -5; dx <= 5; dx++) {
            if (dx * dx + dy * dy > 22) continue;
            int x = (int)mx + dx, y = (int)my + dy;
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || zb[y * W + x] != 255) continue;
            int b = (dx * 0.8f + dy * 0.4f > 2.6f) ? 5 : 7;
            if ((dx + 1) * (dx + 1) + (dy - 1) * (dy - 1) < 3) b = 5;
            fb[y * W + x] = (uint8_t)(P_SUN + b);
        }
    }
    // aurora: curtains folding over the northern sky
    if (sc.aurora > 0.05f) {
        for (int x = 0; x < W; x++) {
            float u = angU(azC[x], 96);
            float curtain = fbmP(u * 0.5f, s_t * 0.03f, 48, 3, 0xA0A0u);
            curtain = clampf((curtain - 0.42f) * 3.f, 0.f, 1.f) * sc.aurora;
            if (curtain <= 0.f) continue;
            float base = horX[x] - 18.f - 10.f * vnoiseP(u * 0.3f, s_t * 0.02f, 29, 0xA1u);
            float streak = vnoiseP(u * 4.f, s_t * 0.2f, 384, 0xA2u);
            for (int y = (int)(base - 40.f); y < (int)base; y++) {
                if (y < 0 || y >= H) continue;
                int i = y * W + x;
                if (zb[i] != 255) continue;
                float v = (y - (base - 40.f)) / 40.f;
                float k = curtain * v * v * (0.5f + streak);
                if (k < sdith((int)floorf(azC[x] * F), (int)horX[x] - y, s_t, 0.6f) * 0.9f) continue;
                fb[i] = (uint8_t)(P_SPECIAL + clampi((int)(k * 7.f + (v < 0.5f ? 4.f : 0.f)), 1, 7));
            }
        }
    }
}

static void drawSun() {
    if (s_sunVis <= 0.f || s_sunX < -12 || s_sunX > W + 12 || s_sunY < -12) return;
    int sx = (int)lroundf(s_sunX), sy = (int)lroundf(s_sunY);
    for (int dy = -8; dy <= 8; dy++) for (int dx = -8; dx <= 8; dx++) {
        float d = sqrtf((float)(dx * dx + dy * dy));
        int x = sx + dx, y = sy + dy;
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
        int i = y * W + x;
        if (zb[i] != 255) continue;
        if (d < 4.2f) fb[i] = P_SUN + 7;
        else if (d < 7.5f && (1.f - (d - 4.2f) / 3.3f) > dith(x, y)) fb[i] = P_SUN + 5;
    }
}

// ================================================================================== props
static inline void putZ(int x, int y, uint8_t c, uint8_t zc) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    int i = y * W + x;
    if (zb[i] <= zc) return;
    fb[i] = c; zb[i] = zc;
}

// A template pixel's colour depends only on its byte, the headlight step (lightAdd) and the autumn leaf (alt): worked
// out the first time each byte turns up for that lighting, then looked up. (0 = not yet: no sprite colour is 0.)
static uint8_t s_tplCol[256];
static int s_tplKey = -1000;
__attribute__((noinline)) static uint8_t tplColour(uint8_t v, int lightAdd, bool alt) {   // flash: only on a table miss
    int m = v >> 4, sh = (v & 15) + lightAdd;
    uint8_t c;
    switch (m) {
        case TM_LEAF:  c = alt ? (uint8_t)(P_LEAF2 + clampi(sh / 2, 0, 7)) : (uint8_t)(P_LEAF + clampi(sh, 0, 15)); break;
        case TM_LEAF2: c = (uint8_t)(P_LEAF2 + clampi(sh / 2, 0, 7)); break;
        case TM_BARK:  c = (uint8_t)(P_BARK + clampi(sh / 2, 0, 7)); break;
        case TM_ROCK:  c = (uint8_t)(P_ROCK + clampi(sh, 0, 15)); break;
        case TM_SNOW:  c = (uint8_t)(P_SNOW + clampi(sh / 2, 0, 7)); break;
        case TM_FLOWER: c = (uint8_t)(P_FLOWER + (v & 7)); break;
        case TM_WALL:  c = (uint8_t)(P_ROCK + clampi(sh + 2, 0, 15)); break;
        case TM_ROOF:  c = (uint8_t)(P_ROCK + clampi(sh - 4, 0, 15)); break;
        case TM_GLASS: c = P_WINDOW; break;
        case TM_SIGN:  c = (v & 15) > 12 ? P_SIGN_W : P_SIGN_G; break;
        case TM_SIGNTXT: c = P_SIGN_W; break;
        case TM_SIGNY: c = (uint8_t)(P_LINE_Y + clampi(sh / 4, 0, 3)); break;   // lit like the road paint: it shines in the headlights
        case TM_INK:   c = (uint8_t)(P_ROAD + 1); break;
        default:       c = (uint8_t)(P_SNOW + 7); break;
    }
    return c;
}
// a lit template, scaled. base: screen row of its foot at column sx.
MR_HOT static void blitTpl(uint8_t id, float sx, float baseY, float pxH, float depth, bool mirror, bool alt, float headK) {
    const Tpl& t = tpl(id);
    if (!t.px || pxH < 1.5f) return;
    float sc = pxH / t.h;
    float pxW = t.w * sc;
    int x0 = (int)floorf(sx - t.ax * sc), x1 = (int)ceilf(x0 + pxW);
    if (x1 < 0 || x0 >= W) return;
    uint8_t zc = zcode(depth);
    float fog = fogAt(depth);
    float inv = 1.f / sc;
    int lightAdd = (int)(headK * 6.f) - (s_night > 0.5f ? 3 : 0);
    // fogTwin(c, amt, grain) with amt fixed for the whole sprite: two possible levels, the seam picks by grain < 0.5,
    // which for the 4x4 pattern is BAYER <= 7. The same twins, worked out once instead of per pixel.
    const uint8_t* fogA = nullptr; const uint8_t* fogB = nullptr; int fogMode = 0;   // 0 none, 1 always B, 2 B in the seam
    if (fog > 0.3f) {
        float f = clampf((fog - 0.3f) * 2.f, 0.f, 1.f) * 4.f; int lv = (int)f; float fr = f - lv;
        int lB = lv + 1;
        fogA = lv <= 0 ? nullptr : s_fogLut[lv > 4 ? 3 : lv - 1];
        fogB = lB <= 0 ? nullptr : s_fogLut[lB > 4 ? 3 : lB - 1];
        fogMode = fr > 0.86f ? 1 : (fr > 0.62f ? 2 : 3);
    }
    const int key = lightAdd * 2 + (alt ? 1 : 0);
    if (key != s_tplKey) { memset(s_tplCol, 0, sizeof s_tplCol); s_tplKey = key; }
    // The template row for each screen row: the same in every column whose top row (y0) is the same, so worked out
    // again only when y0 changes (once a sprite unless the picture is rolled). A column draws rows below
    // y0 + (int)pxH + 2 only (by < y0 + 1 + pxH), so that is as far as it's needed.
    int16_t rowTy[H];
    int tabY0 = -(1 << 30);
    const int rowsSpan = pxH < (float)H ? (int)pxH + 2 : (1 << 20);       // (a sprite taller than the picture: every row)
    for (int x = x0 > 0 ? x0 : 0; x < x1 && x < W; x++) {
        int tx = (int)((x - x0) * inv);
        if (tx >= t.w) tx = t.w - 1;
        if (mirror) tx = t.w - 1 - tx;
        float by = baseY + (x - sx) * rollT;
        int y0 = (int)floorf(by - pxH);
        if (y0 != tabY0) {
            tabY0 = y0;
            int e = y0 > H ? H : y0 + rowsSpan; if (e > H) e = H;
            for (int y = y0 > 0 ? y0 : 0; y < e; y++) { int ty = (int)((y - y0) * inv); rowTy[y] = (int16_t)(ty >= t.h ? t.h - 1 : ty); }
        }
        for (int y = y0 > 0 ? y0 : 0; y < (int)by && y < H; y++) {
            int i = y * W + x;
            if (zb[i] <= zc) continue;                       // hidden: don't fetch the sprite's pixel at all
            int ty = rowTy[y];
            uint8_t v = t.px[ty * t.w + tx];
            if (!v) continue;
            uint8_t c = s_tplCol[v];
            if (!c) c = s_tplCol[v] = tplColour(v, lightAdd, alt);
            if (fogMode) {                                      // the palette way: the sprite steps to its fogged twins
                const uint8_t* L = fogMode == 1 ? fogB : (fogMode == 2 && BAYER[((ty & 3) << 2) | (tx & 3)] <= 7 ? fogB : fogA);
                if (L) c = L[c];                                // (its seam keyed to the sprite itself)
            }
            fb[i] = c; zb[i] = zc;
        }
    }
}

// A prop's shadow on the ground under it: a flattened ellipse, offset away from the sun, that steps the ground one
// shade darker (the reflection table: same ramp, darker). Only ground at about the prop's own depth (nothing nearer,
// nothing far behind), drawn before the prop itself. Soft edge: the outer part dithered.
static bool s_propSh = false;
static float s_propShX = 0.f, s_propShY = 0.f;
__attribute__((noinline)) static void propShadow(uint8_t id, float sx, float sy, float pxH, float dep) {
    const Tpl& t = tpl(id);
    if (!t.px) return;
    const float rx = t.w * (pxH / t.h) * 0.42f;
    if (rx < 1.5f) return;
    const float ry = rx * 0.28f + 0.5f;
    const float ex = sx + s_propShX * rx, ey = sy - 0.5f + s_propShY * ry;
    const int zc = zcode(dep);
    const int x0 = clampi((int)floorf(ex - rx), 0, W), x1 = clampi((int)ceilf(ex + rx), 0, W);
    const int y0 = clampi((int)floorf(ey - ry), 0, H), y1 = clampi((int)ceilf(ey + ry), 0, H);
    const float irx = 1.f / rx, iry = 1.f / ry;
    for (int y = y0; y < y1; y++) {
        const float v = (y + 0.5f - ey) * iry;
        for (int x = x0; x < x1; x++) {
            const float u = (x + 0.5f - ex) * irx, r2 = u * u + v * v;
            if (r2 >= 1.f) continue;
            const int i = y * W + x;
            const int zp = zb[i] & ~1;
            if (zp >= 254 || zp > zc + 8 || zp < zc - 8) continue;
            if (r2 > 0.6f && (r2 - 0.6f) * 2.5f > dith(x, y)) continue;
            fb[i] = s_refl[fb[i]];
        }
    }
}

static float headAt(float lat, float depth) {               // headlight on a prop
    if (s_lights < 0.02f || depth > 70.f) return 0.f;
    float k = lat / depth;
    float c = 1.f - k * k / 0.17f;
    return c > 0.f ? s_lights * c * HEAD_K * fminf(1.f, 14.f / (depth + 4.f)) * s_night : 0.f;
}

static inline float sq(float v) { return v * v; }
extern float g_fallsNear;                           // the waterfall roar's level (audio)
static inline uint8_t fogged(uint8_t c, float dep, int x, int y);
static void line3(float ax, float ay, float az, float bx, float by, float bz, uint8_t col, int thick);
static inline bool roughlyInView(float x, float z, float maxD);
// ---- waterfalls: a lip cell (terrain marks it) and the lowest ground within 15 m down the face below it: a sheet of
// white water streaming down between them, foam at its foot. Drawn a little in front of the face it runs down.
static float s_fallX[4], s_fallZ[4];
static int s_fallN = 0;
static void drawFall(const Grid& g, int i, int j) {
    const float x0 = i * g.cell, z0 = j * g.cell;
    for (int k = 0; k < s_fallN; k++) if (fabsf(s_fallX[k] - x0) < 12.f && fabsf(s_fallZ[k] - z0) < 12.f) return;   // one a cliff
    const Cell& c = g.c[gridWrap(j, g.N) * g.N + gridWrap(i, g.N)];
    float nx = ((c.nrm >> 4) - 8) * 0.125f, nz = ((c.nrm & 15) - 8) * 0.125f;        // the normal leans downhill
    float nl = sqrtf(nx * nx + nz * nz);
    if (nl < 0.2f) return;
    nx /= nl; nz /= nl;
    // the lip may be anywhere on the face: up the face to its top edge (where the climb flattens out)
    float top = c.h * 0.1f, tx = x0, tz = z0;
    for (int k = 1; k <= 8; k++) {
        int ii = i - (int)floorf(nx * k + 0.5f), jj = j - (int)floorf(nz * k + 0.5f);
        if (ii < g.ox || jj < g.oz || ii >= g.ox + g.N || jj >= g.oz + g.N) break;
        const Cell& o = g.c[gridWrap(jj, g.N) * g.N + gridWrap(ii, g.N)];
        float oh = o.h * 0.1f;
        if (oh < top + 0.6f) break;                                   // the top: the climb stops
        top = oh; tx = ii * g.cell; tz = jj * g.cell;
    }
    float bot = c.h * 0.1f, bx = x0, bz = z0;
    for (int k = 1; k <= 6; k++) {
        int ii = i + (int)floorf(nx * k + 0.5f), jj = j + (int)floorf(nz * k + 0.5f);
        if (ii < g.ox || jj < g.oz || ii >= g.ox + g.N || jj >= g.oz + g.N) break;
        const Cell& o = g.c[gridWrap(jj, g.N) * g.N + gridWrap(ii, g.N)];
        if ((o.mat & 15) == MAT_ROAD || (o.mat & 15) == MAT_SHOULDER) break;
        float oh = o.h * 0.1f;
        if (oh < bot) { bot = oh; bx = ii * g.cell; bz = jj * g.cell; }
    }
    if (top - bot < 5.f) return;
    if (s_fallN < 4) { s_fallX[s_fallN] = x0; s_fallZ[s_fallN] = z0; s_fallN++; }
    float sxT, syT, dT, sxB, syB, dB;
    if (!renderProject(tx, top + 0.3f, tz, sxT, syT, dT) || !renderProject(bx, bot, bz, sxB, syB, dB)) return;
    float dist = sqrtf(sq(x0 - cx) + sq(z0 - cz));
    g_fallsNear = fmaxf(g_fallsNear, clampf(1.f - dist * (1.f / 160.f), 0.f, 1.f));
    int y0 = (int)ceilf(syT), y1 = (int)syB;
    if (y1 <= y0 || y1 - y0 > 2 * H) return;
    const float tt = g_scene.t;
    const float inv = 1.f / (float)(y1 - y0);
    const float drop = top - bot;
    int ya = y0 < 0 ? 0 : y0, yb = y1 + 3 > H ? H : y1 + 3;
    for (int y = ya; y < yb; y++) {
        float v = (y - y0) * inv;                                     // 0 at the lip, 1 at the foot (a little past: foam)
        float vc = v > 1.f ? 1.f : v;
        float sx = sxT + (sxB - sxT) * vc, dep = dT + (dB - dT) * vc;
        if (dep < 1.f) continue;
        float hw = (1.3f + 0.9f * vc + (v > 0.85f ? (v - 0.85f) * 9.f : 0.f)) * F / dep;   // it spreads, and foams at the foot
        uint8_t zc = zcode(fmaxf(fminf(dep - 2.5f, dep * 0.8f), 0.6f));   // in front of the face it runs down (a face bulges)
        int xa = (int)(sx - hw), xb = (int)(sx + hw);
        if (xa < 0) xa = 0;
        if (xb > W - 1) xb = W - 1;
        float m = vc * drop * 2.2f - tt * 7.f;                        // streaks running down, about 7 m/s
        for (int x = xa; x <= xb; x++) {
            int i2 = y * W + x;
            if (y >= s_bottom[x] || zb[i2] <= zc) continue;
            float ux = (x - sx) / (hw + 0.5f);                        // -1..1 across
            uint32_t hh = hash32((uint32_t)(int)((ux + 1.f) * 5.f) * 0x9E3779B1u ^ (uint32_t)(int)floorf(m + ux * 0.7f) * 0x85EBCA77u);
            int lv = 5 + (int)(hh & 3) - (fabsf(ux) > 0.8f ? 2 : 0);
            uint8_t col = v > 0.85f || (hh & 12) == 12 ? (uint8_t)(P_SNOW + clampi(lv + 1, 0, 7)) : (uint8_t)(P_WATER + clampi(lv, 0, 7));
            fb[i2] = fogged(col, dep, x, y); zb[i2] = zc;
        }
    }
}

// ---- farms: pastures (terrainPlot use 3) get a post-and-rail fence round their edge and a few cattle. Posts stand
// only where the pasture is (flat, off the road): the fence follows it.
static void drawFarm() {
    const float R = 130.f;
    const int i0 = (int)floorf((cx - R) * (1.f / PLOT_M)), i1 = (int)floorf((cx + R) * (1.f / PLOT_M));
    const int j0 = (int)floorf((cz - R) * (1.f / PLOT_M)), j1 = (int)floorf((cz + R) * (1.f / PLOT_M));
    const uint8_t col = (uint8_t)(P_BARK + clampi((int)(s_light[0x88] * 6.f), 1, 5));
    const uint8_t rc = (uint8_t)(P_BARK + clampi((int)(s_light[0x88] * 6.f) + 1, 1, 6));
    bool any = false;
    for (int pj = j0; pj <= j1; pj++) for (int pi = i0; pi <= i1; pi++) {
        if ((terrainPlot(pi, pj) & 0xE0) != 0xE0) continue;      // pastures only
        any = true;
        if (!roughlyInView(pi * PLOT_M + PLOT_M * 0.5f, pj * PLOT_M + PLOT_M * 0.5f, R + 45.f)) continue;
        for (int side = 0; side < 4; side++) {                   // the fence (field.cpp: where it stands, what's burst)
            float px = 0.f, pz = 0.f, py = 0.f; bool prev = false;
            for (int k = 0; k <= FENCE_POSTS; k++) {
                float x, z;
                bool on = fieldFencePost(pi, pj, side, k, x, z) && sq(x - cx) + sq(z - cz) < R * R;
                if (!on) { prev = false; continue; }
                const bool burstL = k > 0 && fieldFenceBroken(pi, pj, side, k - 1), burstR = k < FENCE_POSTS && fieldFenceBroken(pi, pj, side, k);
                float y = terrainHeight(x, z);
                float sx, sy, d;
                if (renderProject(x, y, z, sx, sy, d)) {
                    if (!(burstL && burstR)) line3(x, y - 0.1f, z, x, y + 1.25f, z, col, d < 18.f ? 2 : 1);   // (a post with both spans gone went too)
                    if (prev && !burstL) {
                        line3(px, py + 0.55f, pz, x, y + 0.55f, z, rc, 1);
                        line3(px, py + 1.05f, pz, x, y + 1.05f, z, rc, 1);
                    }
                }
                px = x; pz = z; py = y; prev = true;
            }
        }
    }
    if (!any) return;
    Cow cows[24];
    const int n = fieldCows(cx, cz, 150.f, g_scene.t, cows, 24);  // the herds
    for (int k = 0; k < n; k++) {
        const Cow& c = cows[k];
        float sx, sy, d;
        if (!renderProject(c.x, terrainHeight(c.x, c.z), c.z, sx, sy, d) || d < 1.f) continue;
        float pxH = 1.5f * F / d;
        if (pxH < 2.f) continue;
        blitTpl(c.pose ? T_COW1 : T_COW0, sx, sy + 1.f, pxH, d, sinf(c.heading - cyaw) < 0.f, false, headAt((c.x - cx) * rx + (c.z - cz) * rz, d));
    }
}

static void drawVegetation() {
    s_fallN = 0; g_fallsNear = 0.f;
    const Grid& g = terrainNear();
    if (!g.c) return;
    bool snowy = g_scene.biomeA == B_TUNDRA || g_scene.biomeA == B_ALPINE || g_scene.snow > 0.3f;
    bool autumn = (g_scene.blend < 0.5f ? g_scene.biomeA : g_scene.biomeB) == B_AUTUMN;
    float sunRel = g_scene.sunAz;
    float R = 125.f;
    float lx = cx + (fx - rx) * R, lz = cz + (fz - rz) * R, qx = cx + (fx + rx) * R, qz = cz + (fz + rz) * R;
    int i0 = (int)floorf(fminf(cx, fminf(lx, qx)) * g.inv) - 1, i1 = (int)ceilf(fmaxf(cx, fmaxf(lx, qx)) * g.inv) + 1;
    int j0 = (int)floorf(fminf(cz, fminf(lz, qz)) * g.inv) - 1, j1 = (int)ceilf(fmaxf(cz, fmaxf(lz, qz)) * g.inv) + 1;
    if (i0 < g.ox + 1) i0 = g.ox + 1;
    if (j0 < g.oz + 1) j0 = g.oz + 1;
    if (i1 > g.ox + g.N - 1) i1 = g.ox + g.N - 1;
    if (j1 > g.oz + g.N - 1) j1 = g.oz + g.N - 1;
    for (int j = j0; j < j1; j++) {
        for (int i = i0; i < i1; i++) {
            const Cell& c = g.c[gridWrap(j, g.N) * g.N + gridWrap(i, g.N)];
            uint8_t t = c.extra & 15;
            uint8_t m = c.mat & 15;
            if (c.extra == 0x80 && m == MAT_ROCK) { drawFall(g, i, j); continue; }   // a waterfall's lip
            if (!t || t >= VEG_N || m == MAT_ROAD || m == MAT_SHOULDER || m == MAT_WATER || m == MAT_LOT) continue;
            float wx0 = (i + 0.5f) * g.cell - cx, wz0 = (j + 0.5f) * g.cell - cz;
            float d0 = wx0 * fx + wz0 * fz;
            if (d0 < 0.5f || d0 > 125.f) continue;
            float l0 = wx0 * rx + wz0 * rz;
            if (fabsf(l0) > d0 * 0.95f + 6.f) continue;
            Veg v;
            if (!vegAt(i, j, v)) continue;
            float sx, sy, dep;
            if (!renderProject(v.x, v.y, v.z, sx, sy, dep)) continue;
            float pxH = v.size * F / dep;
            if (pxH < 2.5f) continue;
            {   int bx = (int)sx, by = (int)sy - 1, ty = (int)(sy - pxH * 0.7f);   // hidden behind nearer land?
                uint8_t zc = zcode(dep);
                if ((unsigned)bx < (unsigned)W && (unsigned)ty < (unsigned)H && (unsigned)by < (unsigned)H && zb[by * W + bx] < zc && zb[ty * W + bx] < zc) continue; }
            uint8_t id;
            switch (v.type) {
                case VEG_PINE: id = snowy ? T_PINESNOW : (uint8_t)(T_PINE0 + v.var % 3); break;
                case VEG_BROAD: id = (uint8_t)(T_BROAD0 + (v.var & 1)); break;
                case VEG_REDWOOD: id = T_REDWOOD; break;
                case VEG_BUSH: id = T_BUSH; break;
                case VEG_CACTUS: id = T_CACTUS; break;
                case VEG_SNAG: id = T_SNAG; break;
                case VEG_BOULDER: id = T_BOULDER; pxH *= 0.65f; break;
                default: id = T_FLOWERS; if (dep > 30.f) continue; break;
            }
            if (s_propSh && v.type != VEG_FLOWERS) propShadow(id, sx, sy + 1.f, pxH, dep);
            float az = cyaw + atanf((sx - W * 0.5f) / F);
            bool mirror = sinf(wrapPi(sunRel - az)) < 0.f;
            blitTpl(id, sx, sy + 1.f, pxH, dep, mirror, autumn && (v.var & 2), headAt(l0, d0));
        }
    }
}

// ================================================================================== 3D: triangles, lines (airfields, the aircraft)
static inline float rsqrtF(float v) {                 // 1/sqrt(v): two Newton steps (decks: depth codes and planks)
    uint32_t i; memcpy(&i, &v, 4); i = 0x5f3759dfu - (i >> 1);
    float r; memcpy(&r, &i, 4);
    r = r * (1.5f - 0.5f * v * r * r);
    return r * (1.5f - 0.5f * v * r * r);
}
struct CV { float lat, up, dep, u; };                // a point in camera space, and a texture coordinate
struct SV { float x, y, w, uw; };                    // on screen: 1/depth and u/depth interpolate linearly
static constexpr float NEAR_D = 0.4f;
static inline CV camPt(float wx, float wy, float wz, float u) {
    float dx = wx - cx, dz = wz - cz;
    CV v; v.lat = dx * rx + dz * rz; v.up = wy - cy; v.dep = dx * fx + dz * fz; v.u = u;
    return v;
}
static inline CV lerpCV(const CV& a, const CV& b, float t) {
    CV v; v.lat = a.lat + (b.lat - a.lat) * t; v.up = a.up + (b.up - a.up) * t; v.dep = a.dep + (b.dep - a.dep) * t; v.u = a.u + (b.u - a.u) * t;
    return v;
}
static inline SV projCV(const CV& v) {
    float iw = 1.f / v.dep;
    SV o; o.x = W * 0.5f + v.lat * iw * F; o.y = hor0 + (o.x - W * 0.5f) * rollT - v.up * iw * F; o.w = iw; o.uw = v.u * iw;
    return o;
}
static inline uint8_t zcodeW(float w, float& dep) {   // the depth code from 1/depth (as zcode)
    dep = 1.f / w;
    return zcode(dep);
}
static inline uint8_t fogged(uint8_t c, float dep, int x, int y) {   // the palette way, as the sprites
    float f = fogAt(dep);
    return f > 0.3f ? fogTwin(c, clampf((f - 0.3f) * 2.f, 0.f, 1.f), dith(x, y)) : c;
}
// kind 0: deck planks (shade + stripes along u), 1: plain timber. The deck wins a tie with the ground it sits on (at the
// abutments the road's own cells are at the deck's height): two depth codes nearer.
static void fillTri(const SV& a, const SV& b, const SV& c, int kind, int shade) {
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (fabsf(area) < 1e-3f) return;
    int x0 = clampi(ifloor(fminf(a.x, fminf(b.x, c.x))), 0, W - 1), x1 = clampi(iceil(fmaxf(a.x, fmaxf(b.x, c.x))), 0, W - 1);
    int y0 = clampi(ifloor(fminf(a.y, fminf(b.y, c.y))), 0, H - 1), y1 = clampi(iceil(fmaxf(a.y, fmaxf(b.y, c.y))), 0, H - 1);
    if (x0 > x1 || y0 > y1) return;
    const float ia = 1.f / area;
    const float dA = -(c.y - b.y) * ia, dB = -(a.y - c.y) * ia, dC = -(b.y - a.y) * ia;   // weights' step per pixel in x
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f, px = x0 + 0.5f;
        float lA = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * ia;
        float lB = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * ia;
        float lC = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * ia;
        for (int x = x0; x <= x1; x++, lA += dA, lB += dB, lC += dC) {
            if (lA < 0.f || lB < 0.f || lC < 0.f || y >= s_bottom[x]) continue;
            float w = lA * a.w + lB * b.w + lC * c.w;
            if (w <= 0.f) continue;
            float dep; uint8_t zc = zcodeW(w, dep);
            zc = zc > 2 ? (uint8_t)(zc - 2) : 0;
            int i = y * W + x;
            if (zb[i] <= zc) continue;
            int lv = shade;
            uint8_t col;
            if (kind == 0) {
                // weathered deck planks, 0.32 m wide across the road: sun-silvered grey boards, a brown one here and
                // there (newer, or oiled), each its own shade, dark gaps between
                float pu = (lA * a.uw + lB * b.uw + lC * c.uw) * dep * 3.125f;
                int pi = ifloor(pu); float frc = pu - pi;
                uint32_t ph = (uint32_t)pi * 0x9E3779B1u; ph ^= ph >> 15;
                int l2 = lv * 2 + (int)(ph & 3) - 1;
                if (frc < 0.14f) l2 -= 5;                                          // the gap between two planks
                col = ((ph >> 4) % 5u == 0u) ? (uint8_t)(P_BARK + clampi(l2 / 2 - 1, 0, 5)) : (uint8_t)(P_ROAD + clampi(l2, 0, 13));
            } else col = kind == 4 ? (uint8_t)lv                                                    // a given colour
                       : kind == 2 ? (uint8_t)(P_ROCK + clampi(lv + (((x >> 1) ^ (y * 3 >> 2)) & 1), 0, 15))   // rock: a grain
                       : kind == 3 ? (uint8_t)(P_ROAD + clampi(lv, 0, 15))                                    // concrete
                                   : (uint8_t)(P_BARK + clampi(lv - 1, 0, 5));                                // timber: dark, oiled
            fb[i] = fogged(col, dep, x, y);
            zb[i] = zc;
        }
    }
}
// a convex polygon (camera space), cut at the near plane, as a fan
static void fillPoly(const CV* v, int n, int kind, int shade) {
    CV o[8]; int m = 0;
    for (int k = 0; k < n && m < 7; k++) {
        const CV& p = v[k]; const CV& q = v[(k + 1) % n];
        bool pin = p.dep >= NEAR_D, qin = q.dep >= NEAR_D;
        if (pin) o[m++] = p;
        if (pin != qin) o[m++] = lerpCV(p, q, (NEAR_D - p.dep) / (q.dep - p.dep));
    }
    if (m < 3) return;
    SV sv[8];
    for (int k = 0; k < m; k++) sv[k] = projCV(o[k]);
    for (int k = 1; k + 1 < m; k++) fillTri(sv[0], sv[k], sv[k + 1], kind, shade);
}
static void line3(float ax, float ay, float az, float bx, float by, float bz, uint8_t col, int thick) {
    CV p = camPt(ax, ay, az, 0.f), q = camPt(bx, by, bz, 0.f);
    if (p.dep < NEAR_D && q.dep < NEAR_D) return;
    if (p.dep < NEAR_D) p = lerpCV(p, q, (NEAR_D - p.dep) / (q.dep - p.dep));
    else if (q.dep < NEAR_D) q = lerpCV(q, p, (NEAR_D - q.dep) / (p.dep - q.dep));
    SV a = projCV(p), b = projCV(q);
    float dx = b.x - a.x, dy = b.y - a.y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy)) + 1;
    if (n > 480) n = 480;
    float inv = 1.f / n;
    for (int k = 0; k <= n; k++) {
        float t = k * inv;
        int x = ifloor(a.x + dx * t), y = ifloor(a.y + dy * t);
        if ((unsigned)y >= (unsigned)H) continue;
        float w = a.w + (b.w - a.w) * t;
        if (w <= 0.f) continue;
        float dep; uint8_t zc = zcodeW(w, dep);
        for (int j = 0; j < thick; j++) {
            int xx = x - thick / 2 + j;
            if ((unsigned)xx >= (unsigned)W || y >= s_bottom[xx]) continue;
            int i = y * W + xx;
            if (zb[i] <= zc) continue;
            fb[i] = fogged(col, dep, xx, y); zb[i] = zc;
        }
    }
}
static inline bool roughlyInView(float x, float z, float maxD) {
    float dx = x - cx, dz = z - cz, d = dx * fx + dz * fz, l = dx * rx + dz * rz;
    return d > -12.f && d < maxD && fabsf(l) < fmaxf(d, 0.f) * 1.1f + 20.f;
}
// The light a structure gets where it stands, as a share of full sun (1): the land's shadow when it stands within a few
// metres of the land that's shaded (a deck high over a shaded gorge stays in the sun), and the clouds' shadows. Set
// before drawing each piece; the timber, deck and rock shades take it.
static float s_structK = 1.f;
static void structLightAt(float x, float y, float z) {
    float k = 1.f;
    const Grid* gp = nullptr;
    const Grid& gN = terrainNear(); const Grid& gF = terrainFar();
    for (int q = 0; q < 2 && !gp; q++) {
        const Grid& g = q ? gF : gN;
        if (!g.c) continue;
        int i = (int)floorf(x * g.inv + 0.5f), j = (int)floorf(z * g.inv + 0.5f);
        if (i < g.ox || j < g.oz || i >= g.ox + g.N || j >= g.oz + g.N) continue;
        gp = &g;
        const int idx = gridWrap(j, g.N) * g.N + gridWrap(i, g.N);
        if (g.sh && ((g.sh[idx >> 3] >> (idx & 7)) & 1)) {             // the land here is out of the sun
            const float above = y - g.c[idx].h * 0.1f;
            const float sh = s_lightSh[0x88] / fmaxf(s_light[0x88], 0.05f);
            k = sh + (1.f - sh) * clampf((above - 3.f) * (1.f / 6.f), 0.f, 1.f);
        }
    }
    if (s_cloudAmt > 0.f) k *= cloudK(x, z);
    s_structK = k;
}
static inline int timberShade(float nx, float nz) {  // a vertical timber face lit by the sun (the deck's top: flat light)
    float lt = 0.22f + 0.62f * fmaxf(0.f, nx * s_sunDx + nz * s_sunDz) * s_sunK * s_structK;
    return clampi((int)(lt * 8.f) - 1, 0, 5);
}
// ---- geysers (field.cpp): a pale sinter pad, and when one blows, a column of white water up to 20 m; between
// eruptions a little steam
static void drawGeysers() {
    int n; const Geyser* G = fieldGeysers(n);
    const float tt = g_scene.t;
    for (int k = 0; k < n; k++) {
        const Geyser& g = G[k];
        if (!roughlyInView(g.x, g.z, 280.f)) continue;
        CV pad[8];
        for (int i = 0; i < 8; i++) {
            float an = i * 0.785398f;
            pad[i] = camPt(g.x + cosf(an) * 4.f, g.y + 0.08f, g.z + sinf(an) * 3.f, 0.f);
        }
        fillPoly(pad, 8, 2, clampi((int)(s_light[0x88] * 15.f), 2, 15));
        float sx, sy, d;
        if (!renderProject(g.x, g.y, g.z, sx, sy, d) || d < 1.f) continue;
        const float H0 = g.h > 0.5f ? g.h : 1.5f + 0.5f * sinf(tt * 1.3f + g.x);   // the column, or the steam
        const float top = sy - H0 * F / d;
        const uint8_t zc = zcode(fmaxf(d - 1.f, 0.6f));
        int y0 = (int)top, y1 = (int)sy;
        if (y0 < 0) y0 = 0;
        if (y1 > H) y1 = H;
        g_fallsNear = fmaxf(g_fallsNear, g.h > 3.f ? clampf(1.f - d * (1.f / 160.f), 0.f, 1.f) : 0.f);
        for (int y = y0; y < y1; y++) {
            float v = (sy - y) / (sy - top + 0.5f);                    // 0 at the ground, 1 at the top
            float hw = (g.h > 0.5f ? 1.1f + 2.6f * v * v + (v > 0.82f ? (v - 0.82f) * 14.f : 0.f) : 0.6f + 1.2f * v) * F / d;   // a plume, billowing at the top
            int xa = (int)(sx - hw), xb = (int)(sx + hw);
            if (xa < 0) xa = 0;
            if (xb > W - 1) xb = W - 1;
            for (int x = xa; x <= xb; x++) {
                int i2 = y * W + x;
                if (y >= s_bottom[x] || zb[i2] <= zc) continue;
                uint32_t hh = hash32((uint32_t)((x - (int)sx) / 2 + 64) * 0x9E3779B1u ^ (uint32_t)((y + (int)(tt * 30.f)) / 2) * 0x85EBCA77u);
                if (g.h <= 0.5f && (hh & 3) != 0) continue;              // steam: thin, broken
                float ux = fabsf((x - sx) / (hw + 0.5f));
                if (ux > 0.7f && (hh & 1)) continue;                     // a ragged edge
                int lv = 4 + (int)((hh >> 4) & 3) - (ux > 0.6f ? 1 : 0);
                fb[i2] = fogged((uint8_t)(P_SNOW + clampi(lv, 0, 7)), d, x, y); zb[i2] = zc;
            }
        }
    }
}

extern float g_riverNear;                            // the river's sound level (audio)

static void drawCritters() {
    const Critter* c = fieldCritters();
    for (int k = 0; k < CRIT_N; k++) {
        if (!c[k].on) continue;
        float sx, sy, d;
        if (!renderProject(c[k].x, c[k].y, c[k].z, sx, sy, d) || d > 160.f) continue;
        float dirScreen = sinf(c[k].yaw - cyaw);
        const bool big = c[k].kind == 1;                                    // Bigfoot: 2.4 m tall
        const uint8_t id = big ? (uint8_t)(T_BIGFOOT0 + c[k].pose % 3) : (uint8_t)(T_DEER0 + (c[k].pose & 3));
        blitTpl(id, sx, sy + 1.f, (big ? 2.4f : 1.5f) * F / d, d, dirScreen < 0.f, false, headAt((c[k].x - cx) * rx + (c[k].z - cz) * rz, d));
    }
}

static void drawParticles() {
    const Particle* p = fieldParticles();
    if (!p) return;
    for (int k = 0; k < PART_N; k++) {
        if (p[k].life <= 0.f) continue;
        float sx, sy, d;
        if (!renderProject(p[k].x, p[k].y, p[k].z, sx, sy, d) || d < 0.8f) continue;
        float r = p[k].size * F / d;
        float fade = 1.f - p[k].age / p[k].life;
        uint8_t zc = zcode(d);
        uint8_t c;
        switch (p[k].kind) {
            case PK_MUD: c = P_MUD; break;
            case PK_FUR: c = (uint8_t)(P_BARK + 5 + (k & 1)); break;
            case PK_MIST: c = (uint8_t)(P_FLOWER + 3); break;
            case PK_SPRAY: c = P_HAZE + 12; break;
            case PK_SNOW: c = P_SNOW + 6; break;
            case PK_SMOKE: c = P_HAZE + 13; break;
            case PK_TRAIL: c = (uint8_t)(P_CLOUD + 14); break;
            case PK_FIRE: c = (k & 1) ? P_AMBER : P_TAIL; break;
            case PK_TRACER: c = P_AMBER; break;
            default: c = (uint8_t)(P_SOIL + 10); break;
        }
        bool puff = p[k].kind == PK_DUST || p[k].kind == PK_SMOKE || p[k].kind == PK_MIST || p[k].kind == PK_TRAIL || p[k].kind == PK_FIRE;
        if (p[k].kind == PK_TRACER) {                              // a tracer: a short bright streak along its flight
            float sx2, sy2, d2;
            if (renderProject(p[k].x - p[k].vx * 0.02f, p[k].y - p[k].vy * 0.02f, p[k].z - p[k].vz * 0.02f, sx2, sy2, d2)) {
                const int nn = (int)fmaxf(fabsf(sx2 - sx), fabsf(sy2 - sy)) + 1;
                for (int q = 0; q <= nn && q < 12; q++) putZ((int)(sx + (sx2 - sx) * q / nn), (int)(sy + (sy2 - sy) * q / nn), q < 2 ? P_HUD_W : P_AMBER, zc);
            }
            continue;
        }
        // puffs thin out as they grow and age, and near the lens they are mostly air
        float dens = puff ? fade * 0.45f * clampf(d / 6.f, 0.15f, 1.f) : 1.f;
        int R = (int)(r + 0.5f);
        int cap = puff ? (p[k].kind == PK_TRAIL || p[k].kind == PK_FIRE ? 10 : 7) : 2;
        if (R > cap) R = cap;
        if (!puff && d < 2.5f) continue;
        for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
            if (dx * dx + dy * dy > R * R + 1) continue;
            int x = (int)sx + dx, y = (int)sy + dy;
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            if (dens < 1.f && dith(x + k, y) > dens * (1.f - (dx * dx + dy * dy) / (float)(R * R + 1) * 0.6f)) continue;
            putZ(x, y, c, zc);
        }
    }
}

// ================================================================================== reflections, weather
// Wet tarmac: the reflection is seen through the water film, in the road's own grey (its brightness, as a level of the
// road's ramp), and how much of it comes through follows Fresnel: a little looking down at the road, a lot toward the
// horizon where the view skims the water. The curve is cheated a little (a cube, not Schlick's fifth power: it comes
// up sooner) and stops short of a full mirror.
static constexpr float WET_SHEEN = 0.30f;     // how much comes through looking straight down
static constexpr float WET_MIRROR = 0.85f;    // ... and skimming the surface (the top of the curve)
static uint8_t s_lumLv[256];                  // each palette entry's brightness as a level of the road's ramp
static float s_fresK[256];                    // the Fresnel curve by rows below the horizon (this frame's focal length)
static void buildWetTables() {
    static uint8_t tick = 0;
    if (!(tick++ & 7)) {                                     // the palette drifts slowly: every 8th frame is plenty
        float rl[16];
        for (int k = 0; k < 16; k++) { const uint8_t* p = palRGB + (P_ROAD + k) * 3; rl[k] = 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]; }
        for (int i = 0; i < 256; i++) {
            const uint8_t* p = palRGB + i * 3;
            const float l = 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
            int b = 0; float bd = 1e9f;
            for (int k = 0; k < 16; k++) { float d = fabsf(l - rl[k]); if (d < bd) { bd = d; b = k; } }
            s_lumLv[i] = (uint8_t)b;
        }
    }
    for (int dy = 0; dy < 256; dy++) {
        const float c = dy / sqrtf(F * F + (float)(dy * dy));     // the cosine of the angle of incidence
        const float m = 1.f - c;
        s_fresK[dy] = WET_SHEEN + (WET_MIRROR - WET_SHEEN) * m * m * m;
    }
}

MR_HOT static void reflectPass() {
    const float t = s_t;
    for (int x = 0; x < W; x++) {
        for (int y = (int)horX[x] + 1; y < s_bottom[x]; y++) {       // (not under the cockpit's panel)
            if (y < 0) continue;
            int i = y * W + x;
            if (!(zb[i] & 1)) continue;
            uint8_t cur = fb[i];
            bool water = cur >= P_WATER && cur < P_WATER + 8;
            int ym = (int)(2.f * horX[x] - y + 1.f);
            int xs = x + (((y + (int)(t * 6.f)) & 7) < 2 ? 1 : 0);
            if (xs >= W) xs = W - 1;
            uint8_t src = ym >= 0 ? fb[ym * W + xs] : (uint8_t)P_SKY;
            uint8_t r = s_refl[src];
            float fres = clampf(1.f - (y - horX[x]) / 55.f, 0.25f, 1.f);
            // lakes keep the screen dither they were approved with; only wet road and mud use the surface's own grain
            float gp = (water || !s_gpat) ? dith(x, y) : s_gpat[i] * (1.f / 255.f);
            if (water) fb[i] = (fres > gp) ? r : cur;
            else if (cur >= P_ROAD && cur < P_ROAD + 16) {           // wet tarmac (above)
                int dy = (int)(y - horX[x]); dy = dy < 0 ? 0 : (dy > 255 ? 255 : dy);
                const float a = s_wet * s_fresK[dy];
                if ((a - 0.6f * s_wet) * 4.f > gp) fb[i] = r;       // skimming (the top of the curve): a mirror, in its own colours
                else {
                    const float lv = (float)(cur - P_ROAD) + ((float)s_lumLv[r] - (float)(cur - P_ROAD)) * a;
                    fb[i] = (uint8_t)(P_ROAD + clampi((int)(lv + gp), 0, 15));
                }
            }
            else if (s_wet * 0.55f * fres > gp) fb[i] = r;
        }
    }
}

// The sun's glint: a column of sparkles on the water (and wet road) under the sun, widening toward you; it twinkles
// 12 times a second whatever the frame rate. A narrow band once a frame: flash.
__attribute__((noinline)) static void drawGlint() {
    if (!(s_sunVis > 0.f && g_scene.sunElev > -0.02f && s_sunX > -13.f && s_sunX < W + 13.f)) return;
    const float k0 = s_sunVis * (0.45f + 0.55f * s_lowSun) * 0.55f;
    const uint32_t tw = (uint32_t)(s_t * 12.f) * 0x9E3779B1u;
    const int xa = clampi((int)floorf(s_sunX - 13.f), 0, W), xb = clampi((int)ceilf(s_sunX + 13.f), 0, W);
    for (int x = xa; x < xb; x++) {
        const float dx = fabsf(x - s_sunX);
        for (int y = clampi((int)horX[x] + 1, 0, H); y < H; y++) {
            const int i = y * W + x;
            if (!(zb[i] & 1)) continue;
            const float gw = 0.8f + (y - horX[x]) * 0.1f;
            if (dx >= gw) continue;
            uint32_t h = ((uint32_t)i * 0x85EBCA6Bu) ^ tw; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
            if ((h & 1023u) * (1.f / 1024.f) < k0 * (1.f - dx / gw)) fb[i] = (uint8_t)(P_SUN + 6 + (h >> 10 & 1));
        }
    }
}

static void drawRain() {
    const Scene& sc = g_scene;
    float amt = sc.rain;
    if (amt < 0.05f) return;
    int n = (int)(amt * 70.f);
    float slant = clampf(-sc.speed * 0.012f + sinf(sc.windAz - cyaw) * sc.wind * 0.6f, -1.2f, 1.2f);
    uint32_t seed = (uint32_t)(s_t * 30.f) * 2654435761u;
    for (int k = 0; k < n; k++) {
        uint32_t h = hash32(seed + k * 7919u);
        int x = h % W, y = (h >> 9) % H;
        int len = 3 + ((h >> 17) & 3);
        for (int j = 0; j < len; j++) {
            int xx = x + (int)(j * slant * 0.5f), yy = y + j;
            if ((unsigned)xx < (unsigned)W && (unsigned)yy < (unsigned)H) fb[yy * W + xx] = s_bright[fb[yy * W + xx]] == fb[yy * W + xx] ? (uint8_t)(P_HAZE + 13) : s_bright[s_bright[fb[yy * W + xx]]];
        }
    }
}

struct Flake { float x, y, z; };
static Flake s_flk[60];
static void drawSnow() {
    const Scene& sc = g_scene;
    if (sc.snow < 0.05f) return;
    int n = (int)(sc.snow * 60.f);
    float dt = sc.dt;
    for (int k = 0; k < n; k++) {
        Flake& f = s_flk[k];
        f.z -= sc.speed * dt;
        f.y -= 1.2f * dt;
        f.x += sinf(s_t * 0.7f + k) * 0.4f * dt + sc.wind * dt;
        if (f.z < 0.6f || f.y < -2.f) {
            uint32_t h = hash32((uint32_t)k * 31u + (uint32_t)(s_t * 10.f));
            f.z = 2.f + (h & 255) / 255.f * 18.f; f.x = ((h >> 8 & 255) / 255.f - 0.5f) * f.z * 1.6f; f.y = ((h >> 16 & 255) / 255.f) * 6.f - 1.f;
        }
        int x = (int)(W * 0.5f + f.x / f.z * F), y = (int)(hor0 - f.y / f.z * F);
        if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) {
            fb[y * W + x] = P_SNOW + 7;
            if (f.z < 4.f && x + 1 < W) fb[y * W + x + 1] = P_SNOW + 6;
        }
    }
}

static void drawFireflies() {
    const Scene& sc = g_scene;
    if (sc.fireflies < 0.05f) return;
    int n = (int)(sc.fireflies * 26.f);
    for (int k = 0; k < n; k++) {
        uint32_t h = hash32((uint32_t)k * 7u + worldSeed());
        float ph = s_t * (0.2f + (h & 15) * 0.02f) + k;
        float wx = cx + ((h & 255) / 255.f - 0.5f) * 50.f + sinf(ph) * 2.f;
        float wz = cz + ((h >> 8 & 255) / 255.f - 0.5f) * 50.f + cosf(ph * 0.7f) * 2.f;
        float blink = sinf(s_t * (1.f + (h >> 16 & 7) * 0.3f) + k * 2.f);
        if (blink < 0.3f) continue;
        float sx, sy, d;
        float gy = terrainHeight(wx, wz) + 0.6f + sinf(ph * 1.3f) * 0.4f;
        if (!renderProject(wx, gy, wz, sx, sy, d) || d > 40.f) continue;
        putZ((int)sx, (int)sy, P_FIREFLY, zcode(d));
        if (d < 10.f) { putZ((int)sx + 1, (int)sy, P_FIREFLY, zcode(d)); }
    }
}

static void drawLeaves() {
    const Scene& sc = g_scene;
    if (sc.leaves < 0.05f) return;
    int n = (int)(sc.leaves * 14.f);
    for (int k = 0; k < n; k++) {
        uint32_t h = hash32((uint32_t)k * 13u + 99u);
        float sp = 18.f + (h & 31);
        float x = fmodf((h >> 5 & 255) * 0.7f + s_t * (sc.wind * 30.f + 6.f) + sinf(s_t * 2.f + k) * 6.f, (float)W + 20.f) - 10.f;
        float y = fmodf((h >> 13 & 255) * 0.5f + s_t * sp, (float)H + 10.f) - 5.f;
        uint8_t c = (h & 1) ? (uint8_t)(P_LEAF2 + 5) : (uint8_t)(P_LEAF + 12);
        int xi = (int)x, yi = (int)y;
        if ((unsigned)xi < (unsigned)W && (unsigned)yi < (unsigned)H) { fb[yi * W + xi] = c; if (((int)(s_t * 6.f + k) & 1) && xi + 1 < W) fb[yi * W + xi + 1] = c; }
    }
}

static void drawBolt() {
    const Scene& sc = g_scene;
    if (sc.boltAge > 0.3f) return;
    float rel = wrapPi(sc.boltAz - cyaw);
    if (fabsf(rel) > 0.9f) return;
    float x = W * 0.5f + tanf(rel) * F;
    uint32_t h = sc.boltSeed;
    int y0 = 0, y1 = (int)horX[clampi((int)x, 0, W - 1)];
    if (y1 > H) y1 = H;                                  // the horizon below the picture: never past the frame
    for (int y = y0; y < y1; y++) {
        h = hash32(h + (uint32_t)y);
        if ((y & 3) == 0) x += ((h & 255) / 255.f - 0.5f) * 5.f;
        int xi = (int)x;
        if ((unsigned)xi >= (unsigned)W) continue;
        int i = y * W + xi;
        if (zb[i] < 254) continue;
        fb[i] = P_BOLT;
        if (xi + 1 < W && zb[i + 1] >= 254) fb[i + 1] = P_SUN + 6;
    }
}

static void drawMist() {
    float mist = S.mist * 0.4f + g_scene.mist + g_scene.fog * 0.35f;
    if (mist < 0.04f) return;
    float rowK[28];                                          // 1 - |y - hy - 3| / 15 for the band's 28 rows
    for (int r = 0; r < 28; r++) rowK[r] = 1.f - fabsf((float)(r - 13)) / 15.f;
    for (int x = 0; x < W; x++) {
        int hy = (int)horX[x];
        for (int y = hy - 10; y < hy + 18; y++) {
            if (y < 0 || y >= H) continue;
            int i = y * W + x;
            if (zb[i] <= 60) continue;
            float k = mist * rowK[y - (hy - 10)] * (0.7f + 0.3f * tex((int)((x + cyaw * F) * 0.1f), (int)(y * 0.4f + s_t * 0.3f)) * (1.f / 255.f));
            float gp = s_gpat ? s_gpat[i] * (1.f / 255.f) : (((x ^ y) & 1) ? 0.f : 1.f);   // the grain of the land under the mist
            float ma = clampf((k - 0.12f) * 2.2f, 0.f, 1.f);         // mist: the land under it steps to its fogged twins
            if (ma > 0.f) fb[i] = fogTwin(fb[i], ma, gp);
        }
    }
}

static uint8_t* s_shaft = nullptr;            // 80x60 brighten chance, refreshed every other frame
static float s_shaftSX = 0.f, s_shaftSY = 0.f;   // where the sun was when it was made
static int s_frameN = 0;
static void drawShafts() {
    const Scene& sc = g_scene;
    if (sc.beams < 0.12f || s_night > 0.6f || s_sunX < -400.f || s_sunX > W + 400.f) return;
    if (!s_shaft) { s_shaft = (uint8_t*)memFast(80 * 60); if (!s_shaft) return; memset(s_shaft, 0, 80 * 60); }
    if (!(s_frameN & 1)) {
        s_shaftSX = s_sunX; s_shaftSY = s_sunY;
        float str = sc.beams * 0.45f;
        for (int y = 0; y < H; y += 2) for (int x = 0; x < W; x += 2) {
            float dx = x - s_sunX, dy = y - s_sunY;
            uint8_t k8 = 0;
            if (dy >= -6.f) {
                float st = tex((int)(fastAtan2(dy, dx) * 20.f + 64.f), (int)(s_t * 0.3f)) * (1.f / 255.f);
                if (st >= 0.56f) {
                    float dist = sqrtf(dx * dx + dy * dy);
                    float k = (st - 0.56f) * 2.3f * str * clampf(1.f - dist / 260.f, 0.f, 1.f);
                    k8 = (uint8_t)clampi((int)(k * 255.f), 0, 255);
                }
            }
            s_shaft[(y >> 1) * 80 + (x >> 1)] = k8;
        }
    }
    int ox = (int)lroundf(s_sunX - s_shaftSX), oy = (int)lroundf(s_sunY - s_shaftSY);   // the shafts move with the sun
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int mx = (x - ox) >> 1, my = (y - oy) >> 1;
        if ((unsigned)mx >= 80u || (unsigned)my >= 60u) continue;
        uint8_t k8 = s_shaft[my * 80 + mx];
        if (k8 && k8 * (1.f / 255.f) > dith(x, y)) { int i = y * W + x; fb[i] = s_bright[fb[i]]; }
    }
}

static void drawBirds() {
    const Bird* b = fieldBirds();
    if (!b->on) return;
    for (int k = 0; k < b->n; k++) {
        float az = b->az + (k - b->n / 2) * 0.018f - abs(k - b->n / 2) * 0.0f;
        float el = b->el - abs(k - b->n / 2) * 0.012f;
        float rel = wrapPi(az - cyaw);
        if (fabsf(rel) > 1.f) continue;
        int x = (int)(W * 0.5f + tanf(rel) * F), y = (int)(hor0 - tanf(el) * F);
        bool flap = ((int)(s_t * 6.f + k) & 1);
        uint8_t c = P_CLOUD + 1;
        if ((unsigned)x < (unsigned)(W - 2) && (unsigned)y < (unsigned)(H - 1) && y > 0) {
            fb[y * W + x] = c;
            fb[(flap ? y - 1 : y) * W + x - 1 + (x > 0 ? 0 : 1)] = c; fb[(flap ? y - 1 : y) * W + x + 1] = c;
        }
    }
}

// ================================================================================== the aircraft (chase view, photos)
// The aircraft from the sprite sheets (drawPlaneArt: cards or textured meshes), its shadow on the ground, nav lights.
Marker g_marks[MARK_N];
int g_markN = 0;

// ---- the aircraft: the sprite sheets' views as cards in the aircraft's frame (art_cards.h). Every card the camera
// doesn't see nearly edge-on is drawn, whole, and they cut through each other in depth: straight on it is the sprite
// itself, at 45 degrees the wing card and the fuselage card make the shape together. Drawn through texTri:
// perspective-correct, depth-tested against the world (its 8-bit codes) and the aircraft itself (its own 16-bit
// depth), and then the silhouette gets the pixel art's dark outline.
static const PlaneArt* const CARDS[8] = {&PLANE_CUB, &PLANE_BEAVER, &PLANE_JET, &PLANE_FIGHTER,
                                         &PLANE_NPC_CUB, &PLANE_NPC_BEAVER, &PLANE_NPC_JET, &PLANE_NPC_FIGHTER};
static uint16_t* s_acZ = nullptr;          // the aircraft's own depth (valid where s_acSt == s_acId)
static uint8_t* s_acSt = nullptr;
static uint8_t s_acId = 0;
static int s_acX0, s_acY0, s_acX1, s_acY1;  // what this aircraft drew (for the outline)
static uint8_t s_acZcN, s_acZcF;           // the world depth codes it sits between (outside: no per-pixel test)
// an 8x8 ordered-dither threshold (0..255): the cross-fade between cards
static const uint8_t s_bayer8[64] = {
      0, 128,  32, 160,   8, 136,  40, 168, 192,  64, 224,  96, 200,  72, 232, 104,
     48, 176,  16, 144,  56, 184,  24, 152, 240, 112, 208,  80, 248, 120, 216,  88,
     12, 140,  44, 172,   4, 132,  36, 164, 204,  76, 236, 108, 196,  68, 228, 100,
     60, 188,  28, 156,  52, 180,  20, 148, 252, 124, 220,  92, 244, 116, 212,  84};

struct TexRef { const uint8_t* px; int w, h, stride; };    // 4 bits a texel, 0 transparent
struct TV { float x, y, iw, uw, vw; };                       // on screen; 1/depth; u/depth, v/depth (texels)
struct TriStyle { const uint8_t* lut; uint8_t dLo, dHi; const uint8_t (*skip)[4]; int nSkip; uint8_t fill; };   // colours; dither band; gear boxes; (models) the colour where the texture's empty, 0: see-through
static inline uint8_t texel(const TexRef& t, int u, int v) {
    const uint8_t b = t.px[v * t.stride + (u >> 1)];
    return (u & 1) ? (b & 15) : (b >> 4);
}
static bool acBegin() {
    if (!s_acZ) { s_acZ = (uint16_t*)memSlow(W * H * 2); s_acSt = (uint8_t*)memSlow(W * H); if (s_acSt) memset(s_acSt, 0, W * H); }
    if (!s_acZ || !s_acSt) return false;
    if (++s_acId == 0) { memset(s_acSt, 0, W * H); s_acId = 1; }
    s_acX0 = W; s_acY0 = H; s_acX1 = -1; s_acY1 = -1;
    return true;
}
// one textured triangle (screen-space, perspective-correct)
static void texTri(const TV& A, const TV& B, const TV& C, const TexRef& t, const TriStyle& st) {
    const float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
    if (fabsf(area) < 0.05f) return;
    const float ia = 1.f / area;
    int x0 = (int)floorf(fminf(A.x, fminf(B.x, C.x))), x1 = (int)ceilf(fmaxf(A.x, fmaxf(B.x, C.x)));
    int y0 = (int)floorf(fminf(A.y, fminf(B.y, C.y))), y1 = (int)ceilf(fmaxf(A.y, fmaxf(B.y, C.y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) {
        const float py = y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            const float px = x + 0.5f;
            float w0 = ((B.x - px) * (C.y - py) - (B.y - py) * (C.x - px)) * ia;
            float w1 = ((C.x - px) * (A.y - py) - (C.y - py) * (A.x - px)) * ia;
            const float w2 = 1.f - w0 - w1;
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
            if (y >= s_bottom[x]) continue;                         // (the panel)
            if (st.dHi != 255 || st.dLo != 0) { const uint8_t d = s_bayer8[(y & 7) * 8 + (x & 7)]; if (d < st.dLo || d >= st.dHi) continue; }
            const float iw = A.iw * w0 + B.iw * w1 + C.iw * w2;
            const float dep = 1.f / iw;
            const int u = (int)((A.uw * w0 + B.uw * w1 + C.uw * w2) * dep), v = (int)((A.vw * w0 + B.vw * w1 + C.vw * w2) * dep);
            uint8_t col;
            if ((unsigned)u >= (unsigned)t.w || (unsigned)v >= (unsigned)t.h) { if (!st.fill) continue; col = st.fill; }
            else {
                bool hid = false;
                for (int g = 0; g < st.nSkip; g++) if (u >= st.skip[g][0] && v >= st.skip[g][1] && u < st.skip[g][2] && v < st.skip[g][3]) { hid = true; break; }
                if (hid) continue;
                const uint8_t c = texel(t, u, v);
                if (c) col = st.lut[c];
                else if (st.fill) col = st.fill;
                else continue;
            }
            const int pi = y * W + x;
            const uint16_t dz = (uint16_t)fminf(dep * 64.f, 65535.f);
            if (s_acSt[pi] == s_acId) { if (s_acZ[pi] <= dz) continue; }
            else {
                const uint8_t wz = zb[pi] & ~1;
                if (wz <= s_acZcN) continue;                        // the world in front of all of it
                if (wz < s_acZcF && wz <= zcode(dep)) continue;
            }
            fb[pi] = col; s_acZ[pi] = dz; s_acSt[pi] = s_acId;
            if (x < s_acX0) s_acX0 = x;
            if (x > s_acX1) s_acX1 = x;
            if (y < s_acY0) s_acY0 = y;
            if (y > s_acY1) s_acY1 = y;
        }
    }
}
// the end of an aircraft: its pixels into the world's depth, and the dark outline round its silhouette
static void acEnd(float dc, uint8_t ink) {
    if (s_acX1 < 0) return;
    const uint8_t zcm = zcode(fmaxf(dc, 0.3f));
    for (int y = s_acY0; y <= s_acY1; y++) for (int x = s_acX0; x <= s_acX1; x++) { const int pi = y * W + x; if (s_acSt[pi] == s_acId) zb[pi] = (uint8_t)((zb[pi] & 1) | zcm); }
    const int ox0 = s_acX0 > 0 ? s_acX0 - 1 : 0, oy0 = s_acY0 > 0 ? s_acY0 - 1 : 0, ox1 = s_acX1 < W - 1 ? s_acX1 + 1 : W - 1, oy1 = s_acY1 < H - 1 ? s_acY1 + 1 : H - 1;
    for (int y = oy0; y <= oy1; y++) for (int x = ox0; x <= ox1; x++) {
        const int pi = y * W + x;
        if (s_acSt[pi] == s_acId || (zb[pi] & ~1) <= zcm || y >= s_bottom[x]) continue;
        if ((x > 0 && s_acSt[pi - 1] == s_acId) || (x < W - 1 && s_acSt[pi + 1] == s_acId) || (y > 0 && s_acSt[pi - W] == s_acId) || (y < H - 1 && s_acSt[pi + W] == s_acId)) fb[pi] = ink;
    }
}
// a body-frame point (forward, up, right; metres) to the screen: false if behind the camera
struct ACPose { float x, y, z, f[3], u[3], r[3]; };
static inline bool acProject(const ACPose& P, float bf, float bu, float br, TV& o) {
    const float wx = P.x + P.f[0] * bf + P.u[0] * bu + P.r[0] * br, wy = P.y + P.f[1] * bf + P.u[1] * bu + P.r[1] * br, wz = P.z + P.f[2] * bf + P.u[2] * bu + P.r[2] * br;
    const float dx = wx - cx, dz = wz - cz, dep = dx * fx + dz * fz;
    if (dep < 0.3f) return false;
    const float iw = 1.f / dep;
    o.x = W * 0.5f + (dx * rx + dz * rz) * iw * F;
    o.y = hor0 + (o.x - W * 0.5f) * rollT - (wy - cy) * iw * F;
    o.iw = iw;
    return true;
}
// the palette for one livery: lit or shaded (shade: P_COCK's entries, free whenever the cockpit isn't on screen)
static uint8_t s_lutLit[16], s_lutShade[16], s_lutNpcLit[16], s_lutNpcShade[16];
static void acLuts() {
    for (int i = 0; i < 16; i++) {
        s_lutLit[i] = (uint8_t)(P_VEH + i); s_lutShade[i] = (uint8_t)(P_COCK + i);
        s_lutNpcLit[i] = s_npcCol[i][1]; s_lutNpcShade[i] = s_npcCol[i][0];
    }
}
// the afterburners (the parts column's flame and its glow, in colours that glow): open past 70% throttle, the
// flame as two crossed cards trailing from each nozzle (it reads from the side and from above), the glow facing aft
// (what you see from behind); flickering. L, S, Hh: the cards' length, span, height; bf, bu, br: the view (body frame);
// tailF: where the nozzles are along the flight line
static void drawFlames(const PlaneArt& A, const ACPose& P, float L, float S, float upBase, float Hh, float bf, float bu, float br, float pxPerM, float rpm01, float tailF) {
    if (A.nNozzle && A.flame && A.glow && rpm01 > 0.7f) {
        static const uint8_t FL[16] = FLAME_LUT_INIT;
        const float k = (rpm01 - 0.7f) / 0.3f;
        for (int n = 0; n < A.nNozzle; n++) {
            const float nf = tailF, nr = A.nozzle[n][0] * S, nu = upBase + A.nozzle[n][1] * Hh;
            const float flick = 0.85f + 0.3f * hashf((uint32_t)(s_t * 30.f) * 7u + n);
            // seen end-on from behind, a trailing card would come at the camera as a slab: there the glow carries it
            // and the flame shortens away (fully gone looking straight up the pipe)
            const float endOn = fminf(1.f, fmaxf(0.f, (0.92f - bf) / 0.3f));
            const float len = A.nozzle[n][2] * L * (0.35f + 0.65f * k) * flick * (bf > 0.f ? endOn : 1.f);
            const float hh = A.nozzle[n][2] * L * (0.35f + 0.65f * k) * flick * A.flame->h[0] / A.flame->w[0] * 0.5f;
            TriStyle st = {FL, 0, 255, nullptr, 0};
            for (int c = 0; c < 3; c++) {
                if (c == 0 && (fabsf(br) < 0.22f || len < 0.3f)) continue;     // edge-on, or too short to show
                if (c == 1 && (fabsf(bu) < 0.22f || len < 0.3f)) continue;
                if (c == 2 && bf < 0.15f) continue;                              // the glow faces aft
                const Card& cd = c < 2 ? *A.flame : *A.glow;
                int mip = 0;
                const float sz = c < 2 ? len : hh * 2.4f;
                while (mip < 2 && cd.w[mip + 1] >= sz * pxPerM) mip++;
                TexRef t = {cd.px + cd.off[mip], cd.w[mip], cd.h[mip], (cd.w[mip] + 1) >> 1};
                // corners (forward, up, right): c 0 side card (forward-up), 1 top card (forward-right), 2 the glow
                float cf[4], cu2[4], cr2[4];
                if (c == 0) { const float f_[4] = {nf, nf - len, nf - len, nf}, u_[4] = {nu + hh, nu + hh, nu - hh, nu - hh};
                              for (int e = 0; e < 4; e++) { cf[e] = f_[e]; cu2[e] = u_[e]; cr2[e] = nr; } }
                else if (c == 1) { const float f_[4] = {nf, nf - len, nf - len, nf}, r_[4] = {nr - hh, nr - hh, nr + hh, nr + hh};
                              for (int e = 0; e < 4; e++) { cf[e] = f_[e]; cu2[e] = nu; cr2[e] = r_[e]; } }
                else {  // the glow sits on the nozzle as the rear card draws it: that card stands at the body's middle, so
                        // straight from behind the glow goes there, sliding back to the tail as the view turns side-on
                       const float gf = tailF * fminf(1.f, fmaxf(0.f, (0.95f - bf) / 0.6f)) - 0.05f;
                       const float g = hh * 0.6f, u_[4] = {nu + g, nu + g, nu - g, nu - g}, r_[4] = {nr - g, nr + g, nr + g, nr - g};
                       for (int e = 0; e < 4; e++) { cf[e] = gf; cu2[e] = u_[e]; cr2[e] = r_[e]; } }
                TV v[4]; bool ok = true;
                const float uu[4] = {0, 1, 1, 0}, vv[4] = {0, 0, 1, 1};
                for (int e = 0; e < 4 && ok; e++) {
                    ok = acProject(P, cf[e], cu2[e], cr2[e], v[e]);
                    v[e].uw = uu[e] * t.w * v[e].iw; v[e].vw = vv[e] * t.h * v[e].iw;
                }
                if (!ok) continue;
                texTri(v[0], v[1], v[2], t, st); texTri(v[0], v[2], v[3], t, st);
            }
        }
}
}
// a prop: the parts column's sprite, face on, a quad at its hub across the flight line, turning. Its blades at idle;
// spun up, its blur disc at half density (see-through). In depth with the rest of the aircraft, so a wing hides it.
static void drawProp(const PlaneArt& A, const ACPose& P, float hf, float hu, float hr, float R, int p, float pxPerM, bool npc, float rpm01) {
    const bool blur = rpm01 > 0.35f;
    const Card& pc = blur ? *A.disc : *A.blade;
    const float spin = blur ? s_t * 2.5f : s_t * (1.f + rpm01 * 30.f);
    const float a = spin + p * 1.1f, ca = cosf(a), sa = sinf(a);
    // the quad's corners in the body frame (in the right-up plane), rotated by the spin
    const float e1r = ca * R, e1u = sa * R, e2r = -sa * R, e2u = ca * R;
    const float cr[4] = {-e1r + e2r, e1r + e2r, e1r - e2r, -e1r - e2r}, cu[4] = {-e1u + e2u, e1u + e2u, e1u - e2u, -e1u - e2u};
    int mip = 0;
    while (mip < 2 && pc.w[mip + 1] >= 2.f * R * pxPerM) mip++;
    TexRef t = {pc.px + pc.off[mip], pc.w[mip], pc.h[mip], (pc.w[mip] + 1) >> 1};
    TV v[4];
    const float uu[4] = {0, 1, 1, 0}, vv[4] = {0, 0, 1, 1};
    for (int e = 0; e < 4; e++) {
        if (!acProject(P, hf, hu + cu[e], hr + cr[e], v[e])) return;
        v[e].uw = uu[e] * t.w * v[e].iw; v[e].vw = vv[e] * t.h * v[e].iw;
    }
    TriStyle st = {npc ? s_lutNpcLit : s_lutLit, 0, (uint8_t)(blur ? 128 : 255), nullptr, 0};
    if (!blur) st.dHi = 255;
    texTri(v[0], v[1], v[2], t, st); texTri(v[0], v[2], v[3], t, st);
}
// the cards: the one(s) the camera faces, as quads in the aircraft's frame
static void drawCards(const PlaneArt& A, const ACPose& P, float span, float upBase, bool gearDown, bool npc, float rpm01) {
    const float L = A.len * span, Hh = A.hgt * span, S = span;
    const float mid = upBase + Hh * 0.5f;                        // (the cards' middle: half way up from the wheels)
    // where the camera looks from, in the aircraft's frame
    const float mx = P.x + P.u[0] * mid, my = P.y + P.u[1] * mid, mz = P.z + P.u[2] * mid;
    const float vx = mx - cx, vy = my - cy, vz = mz - cz, vl = sqrtf(vx * vx + vy * vy + vz * vz) + 1e-3f;
    const float bf = (vx * P.f[0] + vy * P.f[1] + vz * P.f[2]) / vl, bu = (vx * P.u[0] + vy * P.u[1] + vz * P.u[2]) / vl, br = (vx * P.r[0] + vy * P.r[1] + vz * P.r[2]) / vl;
    // every card not seen nearly edge-on is drawn, whole, and they cut through each other in depth (a cross): at
    // 45 degrees the wing card and the fuselage card make the shape together instead of a dithered blend
    const float wT = fabsf(bu) > 0.22f ? 1.f : 0.f, wS = fabsf(br) > 0.22f ? 1.f : 0.f, wF = fabsf(bf) > 0.22f ? 1.f : 0.f;
    const float dc = (mx - cx) * fx + (mz - cz) * fz;
    const float rad = fmaxf(L, S) * 0.6f;
    if (dc < rad * 0.35f || dc > 7000.f) return;
    s_acZcN = zcode(fmaxf(dc - rad, 0.3f)); s_acZcF = zcode(dc + rad);
    const float pxPerM = F / dc;
    // each card: its corners (body frame), its texture, its light
    struct Q { const Card* c; float o[3], ax[3], ay[3]; float w; uint8_t lo, hi; bool lit; };
    Q q[3]; int nq = 0;
    auto litOf = [&](float nf, float nu, float nr) {
        const float n0 = P.f[0] * nf + P.u[0] * nu + P.r[0] * nr, n1 = P.f[1] * nf + P.u[1] * nu + P.r[1] * nr, n2 = P.f[2] * nf + P.u[2] * nu + P.r[2] * nr;
        return (n0 * s_sunDx + n1 * s_sunDy + n2 * s_sunDz) * s_sunK + n1 * 0.3f > 0.05f;
    };
    // top: on the wing plane, nose at the top row, the right wing on the right
    if (wT > 0.004f) { Q& c = q[nq++]; c.c = &A.top; c.o[0] = L * 0.5f; c.o[1] = mid; c.o[2] = -S * 0.5f;
        c.ax[0] = 0; c.ax[1] = 0; c.ax[2] = S; c.ay[0] = -L; c.ay[1] = 0; c.ay[2] = 0; c.w = S; c.lo = 0; c.hi = 255; c.lit = bu < 0.f && litOf(0, 1, 0); }
    // side: on the centre plane, nose at the left, rows down
    if (wS > 0.004f) { Q& c = q[nq++]; c.c = &A.side; c.o[0] = L * 0.5f; c.o[1] = upBase + Hh; c.o[2] = 0.f;
        c.ax[0] = -L; c.ax[1] = 0; c.ax[2] = 0; c.ay[0] = 0; c.ay[1] = -Hh; c.ay[2] = 0; c.w = L; c.lo = 0; c.hi = 255; c.lit = litOf(0, 0, br < 0.f ? 1.f : -1.f); }
    // front (from ahead: the left wing on the right) or rear (from behind: the right wing on the right)
    if (wF > 0.004f) { Q& c = q[nq++];
        // from behind its rear view, from ahead its front; where one isn't drawn, the other from its back (mirrored)
        const bool rear = bf > 0.f ? A.rear.w[0] != 0 : A.front.w[0] == 0;
        c.c = rear ? &A.rear : &A.front;
        c.o[0] = 0.f; c.o[1] = upBase + Hh; c.o[2] = rear ? -S * 0.5f : S * 0.5f;
        c.ax[0] = 0; c.ax[1] = 0; c.ax[2] = rear ? S : -S; c.ay[0] = 0; c.ay[1] = -Hh; c.ay[2] = 0; c.w = S; c.lo = 0; c.hi = 255;
        c.lit = litOf(bf < 0.f ? 1.f : -1.f, 0, 0); }
    for (int k = 0; k < nq; k++) {
        const Q& c = q[k];
        // the mip: about a texel a pixel
        const float px = c.w * pxPerM;
        int mip = 0;
        while (mip < 2 && c.c->w[mip + 1] >= px) mip++;
        TexRef t = {c.c->px + c.c->off[mip], c.c->w[mip], c.c->h[mip], (c.c->w[mip] + 1) >> 1};
        uint8_t skipBox[3][4]; int nSkip = 0;
        if (!gearDown) for (int g = 0; g < c.c->nGear; g++, nSkip++) {      // (gear boxes: mip-0 texels -> this mip's)
            skipBox[nSkip][0] = (uint8_t)(c.c->gear[g][0] * t.w / c.c->w[0]); skipBox[nSkip][2] = (uint8_t)((c.c->gear[g][2] * t.w + c.c->w[0] - 1) / c.c->w[0]);
            skipBox[nSkip][1] = (uint8_t)(c.c->gear[g][1] * t.h / c.c->h[0]); skipBox[nSkip][3] = (uint8_t)((c.c->gear[g][3] * t.h + c.c->h[0] - 1) / c.c->h[0]);
        }
        TriStyle st = {c.lit ? (npc ? s_lutNpcLit : s_lutLit) : (npc ? s_lutNpcShade : s_lutShade), c.lo, (uint8_t)c.hi, skipBox, nSkip};
        if (c.hi == 255) st.dHi = 255;
        TV v[4]; bool ok = true;
        const float cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
        for (int e = 0; e < 4 && ok; e++) {
            ok = acProject(P, c.o[0] + c.ax[0] * cu[e] + c.ay[0] * cv[e], c.o[1] + c.ax[1] * cu[e] + c.ay[1] * cv[e], c.o[2] + c.ax[2] * cu[e] + c.ay[2] * cv[e], v[e]);
            v[e].uw = cu[e] * t.w * v[e].iw; v[e].vw = cv[e] * t.h * v[e].iw;
        }
        if (!ok) continue;
        texTri(v[0], v[1], v[2], t, st); texTri(v[0], v[2], v[3], t, st);
    }
    // the props (the parts column's sprites, face on): a quad at each hub across the flight line, turning
    if (A.nProp && A.blade && A.disc)
        for (int p = 0; p < A.nProp; p++) drawProp(A, P, A.prop[p][0] * L, upBase + A.prop[p][2] * Hh, A.prop[p][1] * S, A.prop[p][3] * S, p, pxPerM, npc, rpm01);
    drawFlames(A, P, L, S, upBase, Hh, bf, bu, br, pxPerM, rpm01, -L * 0.5f);
}
// ---- the models (art_mesh.h): the aircraft as a solid, textured from its own cards (the same texels), each face lit
// or shaded as the cards are (the sun on its normal), back faces skipped. Its silhouette gets the same outline.
static const MeshArt* const MESHES[8] = {&MESH_CUB, &MESH_BEAVER, &MESH_JET, &MESH_FIGHTER, nullptr, nullptr, nullptr, nullptr};
static bool drawMesh(const MeshArt& M, const PlaneArt& A, const ACPose& P, float span, float upBase, bool gearDown, bool npc, float rpm01) {
    // (the projected corners: in PSRAM, made once, for the biggest model)
    static TV* pv = nullptr; static float (*pb)[3] = nullptr; static uint8_t* pok = nullptr;
    enum { MAXV = 1024 };
    if (!pv) { pv = (TV*)memSlow(sizeof(TV) * MAXV); pb = (float (*)[3])memSlow(sizeof(float) * 3 * MAXV); pok = (uint8_t*)memSlow(MAXV); }
    if (!pv || !pb || !pok || M.nV > MAXV) return false;
    const float s = span / M.span, du = upBase + M.gearH * s;     // (the model's wheels where the pose puts them)
    const float dc = (P.x - cx) * fx + (P.z - cz) * fz, rad = span * 0.6f;
    if (dc < rad * 0.35f || dc > 7000.f) return true;
    s_acZcN = zcode(fmaxf(dc - rad, 0.3f)); s_acZcF = zcode(dc + rad);
    const float pxPerM = F / dc;
    // the cards' texels at about a texel a pixel (the top card spans the wings)
    const Card* C[4] = {&A.top, &A.side, &A.front, &A.rear};
    int mip = 0;
    while (mip < 2 && A.top.w[mip + 1] >= span * pxPerM) mip++;
    TexRef tex[4]; float su[4], sv[4];
    for (int k = 0; k < 4; k++) {
        const Card& c = *C[k];
        if (!c.w[0]) { tex[k].px = nullptr; continue; }
        tex[k] = {c.px + c.off[mip], c.w[mip], c.h[mip], (c.w[mip] + 1) >> 1};
        su[k] = (float)c.w[mip] / (16.f * c.w[0]); sv[k] = (float)c.h[mip] / (16.f * c.h[0]);
    }
    static const uint8_t SOLID[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    // the camera in the body frame (for the back faces)
    const float dx = cx - P.x, dy = cy - P.y, dz = cz - P.z;
    const float cf = dx * P.f[0] + dy * P.f[1] + dz * P.f[2], cu = dx * P.u[0] + dy * P.u[1] + dz * P.u[2], cr = dx * P.r[0] + dy * P.r[1] + dz * P.r[2];
    for (int i = 0; i < M.nV; i++) {
        pb[i][0] = M.v[i * 3] * s; pb[i][1] = M.v[i * 3 + 1] * s + du; pb[i][2] = M.v[i * 3 + 2] * s;
        pok[i] = acProject(P, pb[i][0], pb[i][1], pb[i][2], pv[i]);
    }
    const uint8_t* lutL = npc ? s_lutNpcLit : s_lutLit;
    const uint8_t* lutS = npc ? s_lutNpcShade : s_lutShade;
    for (int k = 0; k < M.nT; k++) {
        const MeshTri& T = M.t[k];
        if ((T.card & 0x80) && !gearDown) continue;                  // (retracting gear: stowed)
        const int card = T.card & 0x7F;
        const int a = T.i[0], b = T.i[1], c = T.i[2];
        if (!pok[a] || !pok[b] || !pok[c]) continue;
        const float nf = T.n[0] * (1.f / 127.f), nu = T.n[1] * (1.f / 127.f), nr = T.n[2] * (1.f / 127.f);
        if (nf * (pb[a][0] - cf) + nu * (pb[a][1] - cu) + nr * (pb[a][2] - cr) >= 0.f) continue;     // facing away
        const float n0 = P.f[0] * nf + P.u[0] * nu + P.r[0] * nr, n1 = P.f[1] * nf + P.u[1] * nu + P.r[1] * nr, n2 = P.f[2] * nf + P.u[2] * nu + P.r[2] * nr;
        const bool lit = (n0 * s_sunDx + n1 * s_sunDy + n2 * s_sunDz) * s_sunK + n1 * 0.3f > 0.05f;
        const uint8_t* lut = lit ? lutL : lutS;
        TriStyle st = {lut, 0, 255, nullptr, 0, 0};
        TV A0 = pv[a], B0 = pv[b], C0 = pv[c];
        TexRef t;
        if (card >= 4 || !tex[card].px) {                            // a solid colour (or a view this livery hasn't)
            t = {&SOLID[T.col & 15], 2, 1, 1};
            A0.uw = A0.vw = B0.uw = B0.vw = C0.uw = C0.vw = 0.f;
            if (card < 4 && !T.col) continue;
        } else {
            t = tex[card];
            if (T.col) st.fill = lut[T.col & 15];                    // (where the view leaves this face empty)
            const float fu = su[card], fv = sv[card];
            A0.uw = T.uv[0][0] * fu * A0.iw; A0.vw = T.uv[0][1] * fv * A0.iw;
            B0.uw = T.uv[1][0] * fu * B0.iw; B0.vw = T.uv[1][1] * fv * B0.iw;
            C0.uw = T.uv[2][0] * fu * C0.iw; C0.vw = T.uv[2][1] * fv * C0.iw;
        }
        texTri(A0, B0, C0, t, st);
    }
    {   // the afterburners: where the cards put them (the cards' frame: wheels at upBase, the middle half way up)
        const float L = A.len * span, Hh = A.hgt * span, mid = upBase + Hh * 0.5f;
        const float mx = P.x + P.u[0] * mid - cx, my = P.y + P.u[1] * mid - cy, mz = P.z + P.u[2] * mid - cz, vl = sqrtf(mx * mx + my * my + mz * mz) + 1e-3f;
        const float bf = (mx * P.f[0] + my * P.f[1] + mz * P.f[2]) / vl, bu = (mx * P.u[0] + my * P.u[1] + mz * P.u[2]) / vl, br = (mx * P.r[0] + my * P.r[1] + mz * P.r[2]) / vl;
        float tail = 0.f;                                             // (the model's own tail: its aft-most point)
        if (A.nNozzle) for (int i = 0; i < M.nV; i++) tail = fminf(tail, M.v[i * 3] * s);
        drawFlames(A, P, L, span, upBase, Hh, bf, bu, br, pxPerM, rpm01, tail);
    }
    if (A.nProp && A.blade && A.disc)
        for (int p = 0; p < M.nProp && p < 2; p++) drawProp(A, P, M.prop[p][0] * s, M.prop[p][1] * s + du, M.prop[p][2] * s, M.prop[p][3] * s, p, pxPerM, npc, rpm01);
    return true;
}
// one aircraft
#ifdef HOST
double g_planeArtUs = 0.0; int g_planeArtN = 0;          // (host: time spent drawing aircraft, for comparing methods)
#endif
static void drawPlaneArtImpl(int art, const ACPose& P, float span, float upBase, bool gearDown, float rpm01);
static void drawPlaneArt(int art, const ACPose& P, float span, float upBase, bool gearDown, float rpm01 = 1.f) {
#ifdef HOST
    auto t0 = std::chrono::high_resolution_clock::now();
    drawPlaneArtImpl(art, P, span, upBase, gearDown, rpm01);
    g_planeArtUs += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count(); g_planeArtN++;
#else
    drawPlaneArtImpl(art, P, span, upBase, gearDown, rpm01);
#endif
}
static void drawPlaneArtImpl(int art, const ACPose& P, float span, float upBase, bool gearDown, float rpm01) {
    const bool npc = art >= 4;
    if (!acBegin()) return;
    acLuts();
    if (!MESHES[art & 7] || !drawMesh(*MESHES[art & 7], *CARDS[art & 7], P, span, upBase, gearDown, npc, rpm01))
        drawCards(*CARDS[art & 7], P, span, upBase, gearDown, npc, rpm01);
    acEnd((P.x - cx) * fx + (P.z - cz) * fz, npc ? s_npcCol[1][0] : (uint8_t)(P_VEH + 1));
}
#ifdef HOST
// (host only, for judging the aircraft: one on a blank screen from an exact camera, by either method; a PPM)
void renderDebugPlane(int art, int mode, float yawDeg, float pitchDeg, float rollDeg, float camAz, float camEl, float dist, const char* path) {
    static const float SPAN[4] = {10.7f, 14.6f, 12.f, 10.f};
    const float ya = yawDeg * 0.0174533f, pa = pitchDeg * 0.0174533f, ra = rollDeg * 0.0174533f;
    float f[3] = {cosf(ya) * cosf(pa), sinf(pa), sinf(ya) * cosf(pa)};
    float r0[3] = {-sinf(ya), 0.f, cosf(ya)}, u0[3] = {-cosf(ya) * sinf(pa), cosf(pa), -sinf(ya) * sinf(pa)};
    const float az = camAz * 0.0174533f, el = camEl * 0.0174533f;
    cx = cosf(az) * cosf(el) * dist; cz = sinf(az) * cosf(el) * dist; cy = 100.f + sinf(el) * dist;
    cyaw = atan2f(-cz, -cx); fx = cosf(cyaw); fz = sinf(cyaw); rx = -fz; rz = fx;
    F = W * 0.5f / tanf(0.5f * 1.05f); rollT = 0.f;
    hor0 = H * 0.5f - tanf(el) * F;
    if (!zb) zb = (uint8_t*)memFast(W * H);
    memset(fb, P_SIGN_W, W * H); memset(zb, 254, W * H);
    for (int x = 0; x < W; x++) s_bottom[x] = H;
    const uint8_t (*liv)[3] = art >= 4 ? LIVERY16_NPC : LIVERY16_MANTIS;
    for (int b = 0; b < 16; b++) for (int c = 0; c < 3; c++) {
        palRGB[(P_VEH + b) * 3 + c] = (uint8_t)fminf(255.f, liv[b][c] * 1.2f);
        palRGB[(P_COCK + b) * 3 + c] = (uint8_t)(liv[b][c] * 0.8f);
    }
    for (int b = 0; b < 16; b++) { s_npcCol[b][1] = (uint8_t)(P_VEH + b); s_npcCol[b][0] = (uint8_t)(P_COCK + b); }
    palRGB[P_SIGN_W * 3] = 225; palRGB[P_SIGN_W * 3 + 1] = 230; palRGB[P_SIGN_W * 3 + 2] = 236;   // (the backdrop)
    {   // the glowing entries, as the game's palette has them
        static const struct { int i; uint8_t c[3]; } E[] = {{P_HUD_W, {242, 246, 250}}, {P_FIREFLY, {225, 255, 130}}, {P_HUD_G, {120, 255, 120}},
            {P_HUD_A, {255, 190, 50}}, {P_AMBER, {255, 170, 40}}, {P_HUD_R, {255, 70, 60}}, {P_TAIL, {255, 40, 40}}, {P_TAIL_D, {120, 16, 24}}};
        for (const auto& e : E) for (int c = 0; c < 3; c++) palRGB[e.i * 3 + c] = e.c[c];
    }
    s_sunDx = -0.5f; s_sunDy = 0.75f; s_sunDz = -0.43f; s_sunK = 1.f;
    ACPose P; P.x = 0.f; P.y = 100.f; P.z = 0.f;
    for (int a = 0; a < 3; a++) { P.f[a] = f[a]; P.u[a] = u0[a] * cosf(ra) + r0[a] * sinf(ra); P.r[a] = r0[a] * cosf(ra) - u0[a] * sinf(ra); }
    const float span = SPAN[art & 3];
    drawPlaneArt(art, P, span, -CARDS[art & 7]->hgt * span * 0.5f, true, mode / 100.f);   // (mode: the engine's rpm, percent)
    if (!path) return;
    FILE* fp = fopen(path, "wb"); if (!fp) return;
    fprintf(fp, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) fwrite(&palRGB[fb[i] * 3], 1, 3, fp);
    fclose(fp);
}
#endif
// nav lights and the like: the wingtips and the fin's top, from the cards' size
static float s_tipX = 0.f, s_tipY = 0.f, s_tipZ = 0.f, s_tailX = 0.f, s_tailY = 0.f;
static void planeFit(uint8_t id) {
    const PlaneArt& A = *CARDS[id % A_COUNT];
    const AircraftSpec& S = aircraftSpec(id);
    s_tipX = 0.f; s_tipY = -S.gearH + A.hgt * S.span * 0.5f; s_tipZ = S.span * 0.5f;
    s_tailX = -A.len * S.span * 0.5f; s_tailY = -S.gearH + A.hgt * S.span;
}
static inline void bodyToWorld(const ViewInfo& v, const float* p, float& x, float& y, float& z) {
    x = v.x + v.fx * p[0] + v.ux * p[1] + v.rx * p[2];
    y = v.y + v.fy * p[0] + v.uy * p[1] + v.ry * p[2];
    z = v.z + v.fz * p[0] + v.uz * p[1] + v.rz * p[2];
}
static void drawShadowAt(float px, float pz, float gy, float radius) {   // the aircraft's shadow on the ground
    float sx, sy, d;
    if (!renderProject(px, gy, pz, sx, sy, d)) return;
    const float rx = radius * F / d;
    if (rx < 1.f) return;
    const float ry = fmaxf(0.8f, rx * clampf(fabsf(cy - gy) / d, 0.12f, 1.f));
    const int zc = zcode(d);
    const int x0 = clampi((int)(sx - rx), 0, W - 1), x1 = clampi((int)(sx + rx), 0, W - 1);
    const int y0 = clampi((int)(sy - ry), 0, H - 1), y1 = clampi((int)(sy + ry), 0, H - 1);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const float u = (x + 0.5f - sx) / rx, w = (y + 0.5f - sy) / ry;
        const float r2 = u * u * (1.f + 0.6f * fabsf(w)) + w * w * 2.2f;   // a cross: wings and fuselage
        if (r2 > 1.f) continue;
        const int i = y * W + x, zp = zb[i] & ~1;
        if (zp >= 254 || zp > zc + 10 || zp < zc - 10 || y >= s_bottom[x]) continue;
        if (r2 > 0.55f && dith(x, y) < 0.5f) continue;
        fb[i] = s_refl[s_refl[fb[i]]];
    }
}
static void drawAircraft() {
    const ViewInfo& v = g_view;
    const AircraftSpec& A = aircraftSpec(v.aircraft);
    // its shadow first (on the ground under it, cast away from the sun)
    if (s_sunK > 0.15f && v.agl < 250.f && g_scene.sunElev > 0.08f) {
        const float off = v.agl / tanf(fmaxf(g_scene.sunElev, 0.1f));
        const float sxw = v.x - cosf(g_scene.sunAz) * off, szw = v.z - sinf(g_scene.sunAz) * off;
        drawShadowAt(sxw, szw, terrainHeight(sxw, szw) + 0.1f, aircraftSpec(v.aircraft).span * 0.45f);
    }
    {   const float ex = v.x - cx, ey = v.y - cy, ez = v.z - cz;  // the camera in the seat (the pilot's photo): no outside
        if (ex * ex + ey * ey + ez * ez < A.span * A.span * 0.16f) return;
    }
    ACPose P;
    P.x = v.x; P.y = v.y; P.z = v.z;
    P.f[0] = v.fx; P.f[1] = v.fy; P.f[2] = v.fz; P.u[0] = v.ux; P.u[1] = v.uy; P.u[2] = v.uz; P.r[0] = v.rx; P.r[1] = v.ry; P.r[2] = v.rz;
    drawPlaneArt(v.aircraft % A_COUNT, P, A.span, -A.gearH, v.gearDown || !A.retract, v.rpm01);
    // nav lights: red on the left tip, green on the right, white tail; a strobe that flashes
    {   const float L[3] = {s_tipX, s_tipY, -s_tipZ}, R[3] = {s_tipX, s_tipY, s_tipZ}, T[3] = {s_tailX, s_tailY, 0.f};
        const bool strobe = fmodf(s_t, 1.3f) < 0.06f;
        const float* pts[3] = {L, R, T};
        const uint8_t cols[3] = {P_TAIL, P_HUD_G, P_HUD_W};
        for (int k = 0; k < 3; k++) {
            float wx, wy, wz, sx, sy, d;
            bodyToWorld(v, pts[k], wx, wy, wz);
            if (!renderProject(wx, wy, wz, sx, sy, d)) continue;
            const uint8_t zc = zcode(d);
            putZ((int)sx, (int)sy, strobe ? P_HUD_W : cols[k], zc > 2 ? zc - 2 : 0);
            if (strobe || s_night > 0.5f) { putZ((int)sx + 1, (int)sy, strobe ? P_HUD_W : cols[k], zc > 2 ? zc - 2 : 0); putZ((int)sx, (int)sy - 1, strobe ? P_HUD_W : cols[k], zc > 2 ? zc - 2 : 0); }
        }
    }
}

// ---- the gig's markers: race gates (rings) and the target's beacon ----
static void drawMarks() {
    for (int k = 0; k < g_markN; k++) {
        const Marker& m = g_marks[k];
        const float dx = m.x - cx, dz = m.z - cz;
        if (dx * dx + dz * dz > 6000.f * 6000.f) continue;
        if (m.kind == MK_ZONE) {                                // a ring of orange marker panels on the ground
            float px0 = m.x + m.r, pz0 = m.z, py0 = terrainHeight(px0, pz0) + 0.4f;
            for (int q = 1; q <= 24; q++) {
                const float a = q * (6.2831853f / 24.f);
                const float qx = m.x + cosf(a) * m.r, qz = m.z + sinf(a) * m.r, qy = terrainHeight(qx, qz) + 0.4f;
                line3(px0, py0, pz0, qx, qy, qz, (q & 1) ? P_AMBER : P_HUD_W, 1);
                px0 = qx; py0 = qy; pz0 = qz;
            }
            continue;
        }
        if (m.kind == MK_GTARGET || m.kind == MK_GTARGET_LOCK) {   // a radar, an AA gun, a depot's tanks
            const int kind = (int)m.r;
            const float c = cosf(m.yaw), s2 = sinf(m.yaw);
            auto box = [&](float ox, float oz, float hx, float hz, float h, int shade) {
                const float bx = m.x + ox, bz = m.z + oz, by = m.y;
                CV top[4] = {camPt(bx - hx, by + h, bz - hz, 0.f), camPt(bx + hx, by + h, bz - hz, 0.f), camPt(bx + hx, by + h, bz + hz, 0.f), camPt(bx - hx, by + h, bz + hz, 0.f)};
                CV s1[4] = {camPt(bx - hx, by, bz - hz, 0.f), camPt(bx + hx, by, bz - hz, 0.f), camPt(bx + hx, by + h, bz - hz, 0.f), camPt(bx - hx, by + h, bz - hz, 0.f)};
                CV s3[4] = {camPt(bx - hx, by, bz + hz, 0.f), camPt(bx + hx, by, bz + hz, 0.f), camPt(bx + hx, by + h, bz + hz, 0.f), camPt(bx - hx, by + h, bz + hz, 0.f)};
                CV s4[4] = {camPt(bx - hx, by, bz - hz, 0.f), camPt(bx - hx, by, bz + hz, 0.f), camPt(bx - hx, by + h, bz + hz, 0.f), camPt(bx - hx, by + h, bz - hz, 0.f)};
                CV s5[4] = {camPt(bx + hx, by, bz - hz, 0.f), camPt(bx + hx, by, bz + hz, 0.f), camPt(bx + hx, by + h, bz + hz, 0.f), camPt(bx + hx, by + h, bz - hz, 0.f)};
                fillPoly(s1, 4, 3, shade); fillPoly(s3, 4, 3, shade - 2); fillPoly(s4, 4, 3, shade - 1); fillPoly(s5, 4, 3, shade + 1); fillPoly(top, 4, 3, shade + 3);
            };
            if (kind == 0) { box(0.f, 0.f, 4.f, 4.f, 3.f, 7); line3(m.x, m.y + 3.f, m.z, m.x + c * 3.f, m.y + 8.f, m.z + s2 * 3.f, P_HUD_W, 2); }
            else if (kind == 1) { box(0.f, 0.f, 2.5f, 2.5f, 1.8f, 5); line3(m.x, m.y + 1.8f, m.z, m.x + c * 5.f, m.y + 4.5f, m.z + s2 * 5.f, (uint8_t)(P_ROAD + 2), 2); }
            else { box(-6.f, 0.f, 3.f, 3.f, 4.f, 8); box(3.f, 4.f, 3.f, 3.f, 4.f, 8); box(3.f, -5.f, 3.f, 3.f, 4.f, 8); }
            float sx, sy, d;
            if (renderProject(m.x, m.y + 2.f, m.z, sx, sy, d) && d < 6000.f) {    // the HUD's diamond
                const uint8_t col = m.kind == MK_GTARGET_LOCK ? (((int)(s_t * 8.f) & 1) ? P_HUD_R : P_AMBER) : P_AMBER;
                for (int k = 0; k <= 4; k++) { putZ((int)sx - 4 + k, (int)sy - k, col, 0); putZ((int)sx + 4 - k, (int)sy - k, col, 0); putZ((int)sx - 4 + k, (int)sy + k, col, 0); putZ((int)sx + 4 - k, (int)sy + k, col, 0); }
            }
            continue;
        }
        if (m.kind == MK_BOMB || m.kind == MK_CRATE) {
            float sx, sy, d;
            if (!renderProject(m.x, m.y, m.z, sx, sy, d)) continue;
            const uint8_t zc = zcode(d);
            if (m.kind == MK_BOMB) { putZ((int)sx, (int)sy, (uint8_t)(P_ROAD + 1), zc); putZ((int)sx, (int)sy + 1, (uint8_t)(P_ROAD + 1), zc); continue; }
            const int r = (int)fmaxf(1.f, 1.2f * F / d);
            for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) putZ((int)sx + x, (int)sy + y, (uint8_t)(P_SOIL + 6), zc);
            if (m.pitch > 0.f) {                                // the chute: a dome above it
                const int cr = (int)fmaxf(2.f, 3.5f * F / d);
                for (int x = -cr; x <= cr; x++) { const int h = (int)(sqrtf((float)(cr * cr - x * x)) * 0.6f); putZ((int)sx + x, (int)sy - (int)(4.f * F / d) - h, (x & 1) ? P_HUD_W : P_HUD_G, zc); }
            }
            continue;
        }
        if (m.kind == MK_PIP) continue;                          // (the drop view's own)
        if (m.kind == MK_MISSILE) {
            float sx, sy, d;
            if (renderProject(m.x, m.y, m.z, sx, sy, d)) { putZ((int)sx, (int)sy, P_HUD_W, zcode(d)); putZ((int)sx + 1, (int)sy, P_AMBER, zcode(d)); }
            continue;
        }
        if (m.kind == MK_BOGEY || m.kind == MK_BOGEY_LOCK) {    // an enemy jet: a dark dart, and the HUD's box round it
            const float cy0 = cosf(m.yaw), sy0 = sinf(m.yaw), cp = cosf(m.pitch), sp = sinf(m.pitch), cr = cosf(m.roll), sr = sinf(m.roll);
            const float f[3] = {cy0 * cp, sp, sy0 * cp};
            const float r0[3] = {-sy0, 0.f, cy0}, u0[3] = {-cy0 * sp, cp, -sy0 * sp};
            const float rr[3] = {r0[0] * cr - u0[0] * sr, r0[1] * cr - u0[1] * sr, r0[2] * cr - u0[2] * sr};
            // the bogey: the red fighter off the other sheet, in its own attitude
            {   const float uu[3] = {rr[1] * f[2] - rr[2] * f[1], rr[2] * f[0] - rr[0] * f[2], rr[0] * f[1] - rr[1] * f[0]};
                ACPose Q; Q.x = m.x; Q.y = m.y; Q.z = m.z;
                for (int a = 0; a < 3; a++) { Q.f[a] = f[a]; Q.r[a] = rr[a]; Q.u[a] = uu[a]; }
                drawPlaneArt(7, Q, 11.f, -2.f, false, 0.9f);     // the bogey: the red fighter off the other sheet, burners lit
            }
            float sx, sy, d;
            if (renderProject(m.x, m.y, m.z, sx, sy, d) && d < 5000.f) {
                const int bx = (int)sx, by = (int)sy, r = m.kind == MK_BOGEY_LOCK ? 5 : 4;
                const uint8_t c = m.kind == MK_BOGEY_LOCK ? (((int)(s_t * 8.f) & 1) ? P_HUD_R : P_AMBER) : P_HUD_R;
                for (int k = -r; k <= r; k++) {
                    if ((unsigned)(bx + k) < (unsigned)W) { if ((unsigned)(by - r) < (unsigned)H) fb[(by - r) * W + bx + k] = c; if ((unsigned)(by + r) < (unsigned)H) fb[(by + r) * W + bx + k] = c; }
                    if ((unsigned)(by + k) < (unsigned)H) { if ((unsigned)(bx - r) < (unsigned)W) fb[(by + k) * W + bx - r] = c; if ((unsigned)(bx + r) < (unsigned)W) fb[(by + k) * W + bx + r] = c; }
                }
            }
            continue;
        }
        if (m.kind == MK_BEACON) {                              // a tall light column, seen from afar
            float sx, sy, d;
            if (!renderProject(m.x, m.y, m.z, sx, sy, d)) continue;
            const int top = (int)(sy - 400.f * F / d);
            const uint8_t zc = zcode(d);
            const bool blink = fmodf(s_t, 1.f) < 0.7f;
            for (int y = clampi(top, 0, H - 1); y <= clampi((int)sy, 0, H - 1); y++)
                if (blink || ((y + (int)(s_t * 20.f)) & 3) == 0) putZ((int)sx, y, P_AMBER, zc);
            continue;
        }
        const float c = cosf(m.yaw), s = sinf(m.yaw);          // the ring stands across the course
        const uint8_t col = m.kind == MK_GATE_NEXT ? ((int)(s_t * 4.f) & 1 ? P_AMBER : P_TAIL) : (uint8_t)(P_LINE_Y + 2);
        float px = m.x - s * m.r, py = m.y, pz = m.z + c * m.r;
        for (int q = 1; q <= 20; q++) {
            const float a = q * (6.2831853f / 20.f);
            const float qx = m.x - s * m.r * cosf(a), qy = m.y + m.r * sinf(a), qz = m.z + c * m.r * cosf(a);
            line3(px, py, pz, qx, qy, qz, col, m.kind == MK_GATE_NEXT ? 2 : 1);
            px = qx; py = qy; pz = qz;
        }
    }
}

// ---- airfields: the hangar, the windsock, the approach lights (PAPI), the runway lights at night ----
static void refreshRunways() {
    for (int k = 0; k < 16; k++) s_rwy[k].ok = false;
    const int ci = (int)floorf(cx * (1.f / AF_CELL)), cj = (int)floorf(cz * (1.f / AF_CELL));
    for (int dj = -1; dj <= 1; dj++) for (int di = -1; di <= 1; di++) {
        Airfield a;
        if (!airfieldAt(ci + di, cj + dj, a)) continue;
        airfieldLevel(a);
        Rwy& r = s_rwy[a.slot];
        r.ok = true; r.x = a.x; r.z = a.z; r.c = cosf(a.yaw); r.s = sinf(a.yaw); r.len = a.len; r.hw = a.hw; r.y = a.y; r.surf = a.surf; r.num = a.num;
    }
}
static void drawHangar(const Airfield& a) {
    const float c = cosf(a.hyaw), s = sinf(a.hyaw);             // forward = out of the doors
    const float hw = 15.f, hd = 12.f, hh = 9.f, y = a.y;
    auto P = [&](float f, float r, float up) { return camPt(a.hx + c * f - s * r, y + up, a.hz + s * f + c * r, 0.f); };
    structLightAt(a.hx, y + 4.f, a.hz);
    const float ls = s_structK;
    auto shadeN = [&](float nx, float nz) { return clampi((int)((0.3f + 0.6f * fmaxf(0.f, nx * s_sunDx + nz * s_sunDz) * s_sunK * ls) * 12.f) + 2, 2, 13); };
    // walls
    CV fr[4] = {P(hd, -hw, 0.f), P(hd, hw, 0.f), P(hd, hw, hh), P(hd, -hw, hh)};
    CV bk[4] = {P(-hd, hw, 0.f), P(-hd, -hw, 0.f), P(-hd, -hw, hh), P(-hd, hw, hh)};
    CV lf[4] = {P(-hd, -hw, 0.f), P(hd, -hw, 0.f), P(hd, -hw, hh), P(-hd, -hw, hh)};
    CV rt[4] = {P(hd, hw, 0.f), P(-hd, hw, 0.f), P(-hd, hw, hh), P(hd, hw, hh)};
    fillPoly(bk, 4, 3, shadeN(-c, -s)); fillPoly(lf, 4, 3, shadeN(s, -c)); fillPoly(rt, 4, 3, shadeN(-s, c));
    fillPoly(fr, 4, 3, shadeN(c, s));
    // the doors (dark, a lit gap at night) and the barrel roof
    CV dr[4] = {P(hd + 0.05f, -hw * 0.8f, 0.f), P(hd + 0.05f, hw * 0.8f, 0.f), P(hd + 0.05f, hw * 0.8f, hh * 0.78f), P(hd + 0.05f, -hw * 0.8f, hh * 0.78f)};
    fillPoly(dr, 4, 4, s_night > 0.4f ? P_WINDOW : (uint8_t)(P_ROAD + 2));
    for (int k = 0; k < 4; k++) {
        const float a0 = k * 0.785398f, a1 = (k + 1) * 0.785398f;
        const float r0 = -hw * cosf(a0), u0 = hh + 4.f * sinf(a0), r1 = -hw * cosf(a1), u1 = hh + 4.f * sinf(a1);
        CV rf[4] = {P(hd, r0, u0), P(hd, r1, u1), P(-hd, r1, u1), P(-hd, r0, u0)};
        fillPoly(rf, 4, 3, clampi(shadeN(0.f, 0.f) + (k < 2 ? 2 : -1), 1, 14));
    }
    // MANTIS AERO: a green band over the doors
    CV sg[4] = {P(hd + 0.1f, -hw * 0.6f, hh * 0.84f), P(hd + 0.1f, hw * 0.6f, hh * 0.84f), P(hd + 0.1f, hw * 0.6f, hh * 0.98f), P(hd + 0.1f, -hw * 0.6f, hh * 0.98f)};
    fillPoly(sg, 4, 4, P_SIGN_G);
}
static void drawAirfields() {
    refreshRunways();
    const float lightsOn = clampf((s_night - 0.25f) * 3.f, 0.f, 1.f) + clampf(g_scene.fog * 2.f - 0.4f, 0.f, 1.f);
    const int ci = (int)floorf(cx * (1.f / AF_CELL)), cj = (int)floorf(cz * (1.f / AF_CELL));
    for (int dj = -1; dj <= 1; dj++) for (int di = -1; di <= 1; di++) {
        Airfield a;
        if (!airfieldAt(ci + di, cj + dj, a)) continue;
        airfieldLevel(a);
        const float dx = a.x - cx, dz = a.z - cz, dist = sqrtf(dx * dx + dz * dz);
        if (dist > 7000.f) continue;
        const float c = cosf(a.yaw), s = sinf(a.yaw);
        if (roughlyInView(a.hx, a.hz, 2500.f)) drawHangar(a);
        // the windsock, by the hangar: a pole and an orange sock streaming downwind
        if (dist < 1500.f) {
            const float wx = a.hx + cosf(a.hyaw) * 20.f - sinf(a.hyaw) * 22.f, wz = a.hz + sinf(a.hyaw) * 20.f + cosf(a.hyaw) * 22.f;
            line3(wx, a.y, wz, wx, a.y + 6.f, wz, (uint8_t)(P_ROAD + 12), 1);
            const float wind = clampf(g_scene.wind, 0.15f, 1.f), dax = cosf(g_scene.windAz + 3.14159f), daz = sinf(g_scene.windAz + 3.14159f);
            const float droop = (1.f - wind) * 1.6f;
            line3(wx, a.y + 6.f, wz, wx + dax * 1.4f * wind, a.y + 6.f - droop * 0.5f, wz + daz * 1.4f * wind, P_AMBER, 2);
            line3(wx + dax * 1.4f * wind, a.y + 6.f - droop * 0.5f, wz + daz * 1.4f * wind, wx + dax * 2.8f * wind, a.y + 6.f - droop, wz + daz * 2.8f * wind, P_HUD_W, 1);
        }
        // PAPI: four lights left of each end, 300 m in: white above the 3 degree path, red below
        for (int e = -1; e <= 1; e += 2) {
            const float tl = e * (a.len * 0.5f - 300.f);                   // along the runway (the end at -lon lands toward +lon)
            const float px0 = a.x + c * tl, pz0 = a.z + s * tl;
            const float ax = -e * c, az = -e * s;                          // out along the approach
            const float rel = (cx - px0) * ax + (cz - pz0) * az;
            if (rel < 200.f || rel > 9000.f) continue;
            const float lat = -(cx - px0) * (-az) + (cz - pz0) * (-ax) * 0.f;   // (only in front of it)
            (void)lat;
            const float ang = atan2f(cy - a.y, rel) * 57.29578f;
            for (int k = 0; k < 4; k++) {
                const float side = (a.hw + 14.f + k * 9.f) * (e > 0 ? 1.f : -1.f);   // the landing pilot's left
                const float lx = px0 - s * side, lz = pz0 + c * side;
                float sx, sy, d;
                if (!renderProject(lx, a.y + 0.6f, lz, sx, sy, d)) continue;
                static const float TH[4] = {3.5f, 3.17f, 2.83f, 2.5f};
                const uint8_t col = ang > TH[k] ? P_HUD_W : P_TAIL;
                const uint8_t zc = zcode(d);
                putZ((int)sx, (int)sy, col, zc);
                if (d < 1500.f) putZ((int)sx, (int)sy - 1, col, zc);
            }
        }
        // runway edge lights at dusk, night and in fog: white along the sides, green at the ends
        if (lightsOn > 0.05f && dist < 6500.f) {
            const int n = (int)(a.len / 60.f);
            for (int k = 0; k <= n; k++) {
                const float lon = -a.len * 0.5f + k * (a.len / n);
                for (int sd = -1; sd <= 1; sd += 2) {
                    const float lat = sd * (a.hw + 1.5f);
                    float sx, sy, d;
                    if (!renderProject(a.x + c * lon - s * lat, a.y + 0.4f, a.z + s * lon + c * lat, sx, sy, d)) continue;
                    putZ((int)sx, (int)sy, (k == 0 || k == n) ? P_HUD_G : P_HUD_W, zcode(d));
                }
            }
        }
    }
}

// ---- the cloud deck: a sheet at the cloud base you can fly under, through and above (the same blobs that shade the
// land). Seen from above in the sun it's white and bright; from below, grey.
static float s_deckY = 1e9f, s_deckThr = 999.f, s_deckAmt = 0.f, s_inCloud = 0.f;
static inline float deckDens(float x, float z) {
    float u = (x + s_cloudOx) * (1.f / 230.f), v = (z + s_cloudOz) * (1.f / 230.f);
    int iu = ifloor(u), iv = ifloor(v); float tu = u - iu, tv = v - iv;
    const int x0 = iu & 31, x1 = (iu + 1) & 31, z0 = (iv & 31) << 5, z1 = ((iv + 1) & 31) << 5;
    float a = s_cloudMap[z0 | x0], b = s_cloudMap[z0 | x1], c2 = s_cloudMap[z1 | x0], d = s_cloudMap[z1 | x1];
    float ab = a + (b - a) * tu, cd = c2 + (d - c2) * tu, big = ab + (cd - ab) * tv;
    u *= 3.3f; v *= 3.3f; iu = ifloor(u); iv = ifloor(v);
    const int x2 = iu & 31, z2 = (iv & 31) << 5;
    const float fine = s_cloudMap[z2 | x2];
    return big * 0.75f + fine * 0.25f;
}
static void setupDeck() {
    const Scene& sc = g_scene;
    s_deckAmt = clampf((sc.cloud - 0.12f) * 2.f, 0.f, 1.f);
    s_deckY = journeyBase(cx, cz) + 950.f - 420.f * sc.storm - 250.f * sc.rain + 380.f * (1.f - sc.cloud);
    s_deckThr = 250.f - sc.cloud * 175.f;
    s_inCloud = 0.f;
    if (s_deckAmt > 0.f && fabsf(cy - s_deckY) < 45.f) {
        const float d = deckDens(cx, cz) - s_deckThr;
        s_inCloud = clampf(d * 0.06f, 0.f, 1.f) * (1.f - fabsf(cy - s_deckY) / 45.f) * s_deckAmt;
    }
}
static void drawCloudDeck() {
    if (s_deckAmt <= 0.f) return;
    const bool above = cy > s_deckY;
    const float hgt = s_deckY - cy;
    const Scene& sc = g_scene;
    const float topLit = clampf(0.55f + sc.sunElev * 1.2f, 0.f, 1.f) * (1.f - s_night * 0.8f);
    for (int x = 0; x < W; x++) {
        const float hx = horX[x];
        const int bot = s_bottom[x];
        int y0, y1;                                            // the rows whose rays meet the sheet within 9 km
        if (!above) { y0 = 0; y1 = clampi((int)(hx - hgt * F / 9000.f), 0, bot); }
        else { y0 = clampi((int)(hx - hgt * F / 9000.f) + 1, 0, bot); y1 = bot; }
        for (int y = y0; y < y1; y++) {
            const float up = (hx - (y + 0.5f)) / F;
            if (fabsf(up) < 1e-4f) continue;
            const float t = hgt / up;
            if (t <= 0.f || t > 9000.f) continue;
            const int i = y * W + x;
            const uint8_t zc = zcode(t);
            if ((zb[i] & ~1) < zc) continue;                   // the land is nearer
            const float d = deckDens(cx + dxC[x] * t, cz + dzC[x] * t) - s_deckThr;
            if (d <= 0.f) continue;
            float a = clampf(d * 0.07f, 0.f, 1.f) * s_deckAmt * (1.f - fogAt(t) * 0.75f);
            if (a < dith(x, y)) continue;
            float lv = above ? 9.f + topLit * 5.f + clampf(d * 0.02f, 0.f, 1.f) : 7.f - clampf(d * 0.03f, 0.f, 4.f) - sc.storm * 2.f;
            fb[i] = (uint8_t)(P_CLOUD + clampi(band(lv, x, y), 0, 15));
            zb[i] = zc;
        }
    }
}
static void drawInCloud() {                                     // inside it: white all round
    if (s_inCloud <= 0.02f) return;
    const uint8_t c = (uint8_t)(P_CLOUD + (cy > s_deckY ? 12 : 8));
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (y < s_bottom[x] && dith(x * 3 + 1, y) < s_inCloud) fb[y * W + x] = c;
}

// ---- photo capture: which pixels are water, and what kind (the jig file's water regions) ----
void renderCaptureWater(bool on) { s_capWater = on; if (on && !s_wmask) s_wmask = (uint8_t*)memSlow(W * H); if (on && s_wmask) memset(s_wmask, 0, W * H); }
const uint8_t* renderWaterMask() { return s_wmask; }
float renderPixelDepth(int x, int y) { const int z = zb[y * W + x] & ~1; return z >= 254 ? 1e9f : zdepth(z); }
int renderHorizonRow() { return (int)hor0; }
static void classifyWater() {                                  // still, river or ocean, from where each water pixel is
    if (!s_wmask) return;
    const bool coast = g_scene.biomeA == B_COAST || g_scene.biomeB == B_COAST;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t& m = s_wmask[y * W + x];
        if (!m) continue;
        const float d = renderPixelDepth(x, y);
        const float wx = cx + dxC[x] * d, wz = cz + dzC[x] * d;
        m = terrainRiverDist(wx, wz) < 30.f ? 2 : (coast ? 3 : 1);
    }
}

// ---- the glass: mud and drops
struct Splat { float x, y, r, life; uint8_t kind; };
static Splat s_spl[14];
void renderSplat(float strength) {
    for (Splat& s : s_spl) if (s.life <= 0.f) {
        uint32_t h = hash32((uint32_t)(s_t * 1000.f) + (uint32_t)(strength * 99.f));
        bool left = h & 1;
        s.x = left ? (h >> 4) % 40 : W - 1 - (h >> 4) % 40; s.y = 10 + (h >> 12) % 90; s.r = 2.f + strength * 4.f + (h >> 20) % 3;
        s.life = 2.5f + strength * 3.f; s.kind = 0; return;
    }
}
void renderWater(float strength) {
    for (Splat& s : s_spl) if (s.life <= 0.f) {
        uint32_t h = hash32((uint32_t)(s_t * 997.f) + 7u);
        s.x = (float)(h % W); s.y = (float)((h >> 8) % 90); s.r = 1.f + strength * 1.5f; s.life = 1.5f; s.kind = 1; return;
    }
}
static void drawGlass() {
    float dt = g_scene.dt;
    if (g_view.view == VIEW_COCKPIT && g_scene.rain > 0.1f && hashf((uint32_t)(s_t * 60.f)) < g_scene.rain * 0.6f) renderWater(0.3f);
    for (Splat& s : s_spl) {
        if (s.life <= 0.f) continue;
        s.life -= dt;
        if (s.kind == 1) s.y += dt * (6.f + g_view.ias * 0.7f);
        int R = (int)s.r;
        for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
            if (dx * dx + dy * dy > R * R) continue;
            int x = (int)s.x + dx, y = (int)s.y + dy;
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            if (s.kind == 0) { if (s.life > 1.f || dith(x, y) < s.life) fb[y * W + x] = (dx * dx + dy * dy < R * R / 3) ? P_MUD : (((x ^ y) & 1) ? P_MUD : fb[y * W + x]); }
            else if (s.kind == 2) { if ((dx + dy) & 1 || R < 2) fb[y * W + x] = (uint8_t)(P_FLOWER + 3 + ((x * 7 + y) & 1)); }
            else fb[y * W + x] = s_bright[fb[y * W + x]];
        }
    }
}

// ================================================================================== the panel, the HUD
static void hudText(int x, int y, const char* s, uint8_t c, int sc = 1) { font3(fb, W, H, x, y, s, c, P_HUD_K, sc); }
static inline void px(int x, int y, uint8_t c) { if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) fb[y * W + x] = c; }
static void line2(float x0, float y0, float x1, float y1, uint8_t c) {
    const int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int k = 0; k <= n; k++) { const float t = (float)k / n; px((int)lroundf(x0 + (x1 - x0) * t), (int)lroundf(y0 + (y1 - y0) * t), c); }
}
static float s_wayRel = 0.f;          // the waypoint's bearing relative to the nose (rad); nav on/off below
static bool s_wayOn = false;
static void drawThrottle() {                                   // the slider on the right edge (drag it)
    const int x0 = W - 7, y0 = 7, y1 = 80;
    const int ty = y1 - (int)(g_view.throttle * (y1 - y0) + 0.5f);
    for (int y = y0; y <= y1; y++) { px(x0 + 3, y, (uint8_t)(P_HUD_K)); if ((y - y0) % 18 == 0) { px(x0 + 1, y, P_HUD_W); px(x0 + 5, y, P_HUD_W); } }
    const uint8_t kc = g_view.throttle > 0.95f ? P_HUD_A : P_HUD_G;
    for (int y = ty - 2; y <= ty + 2; y++) for (int x = x0; x <= x0 + 6; x++) px(x, y, (y == ty - 2 || y == ty + 2 || x == x0 || x == x0 + 6) ? P_HUD_K : kc);
}
// the cockpit's world-side part: the Cub's prop turning in front of the windscreen (the panel itself is the overlay's:
// cockpit.cpp)
static void drawPropDisc() {
    const ViewInfo& v = g_view;
    if (v.aircraft != A_CUB) return;                           // (the Beaver's are out on its wings; the jets have none)
    const float bl = v.rpm01 > 0.35f ? 0.10f : 0.4f;           // idling: you see the blades; at power, a haze
    const float spin = s_t * (4.f + v.rpm01 * 60.f);
    const float hy = s_bottomCock[W / 2] + 6.f;                // the hub: just below the glare shield
    for (int y = 0; y < H; y++) for (int x = 20; x < 140; x++) {
        if (y >= s_bottomCock[x]) continue;
        const float dx = x - 80.f, dy = y - hy, r = sqrtf(dx * dx + dy * dy);
        if (r > 62.f || r < 6.f) continue;
        const float ang = atan2f(dy, dx) - spin;
        const float blade = fabsf(sinf(ang));
        if (blade < 0.08f && dith(x, y) < bl * 2.f) fb[y * W + x] = s_refl[s_refl[fb[y * W + x]]];
        else if (v.rpm01 > 0.35f && dith(x + 7, y * 3) < 0.035f) fb[y * W + x] = s_refl[fb[y * W + x]];
    }
}
static void drawHud() {
    const ViewInfo& v = g_view;
    char t[24];
    const bool cock = v.view == VIEW_COCKPIT;
    // the waypoint: where it is from the nose
    s_wayOn = v.navOk;
    if (v.navOk) s_wayRel = wrapPi(atan2f(v.navZ - v.z, v.navX - v.x) - v.yaw);
    if (cock) drawPropDisc();
    else {
        // speed and altitude, out of the way; the heading and the waypoint across the top
        snprintf(t, sizeof t, "%d", (int)(v.ias * 2.237f + 0.5f));
        hudText(4, H - 14, t, P_HUD_G, 2); hudText(4, H - 21, "MPH", P_HUD_G);
        snprintf(t, sizeof t, "%d", (int)(v.y * 3.2808f + 0.5f));
        hudText(W - 12 - font3Width(t, 2), H - 14, t, P_HUD_G, 2); hudText(W - 12 - font3Width("FT"), H - 21, "FT", P_HUD_G);
        float hdg = (v.yaw + 1.5707963f) * 57.29578f; hdg -= 360.f * floorf(hdg / 360.f);
        snprintf(t, sizeof t, "%03d", (int)(hdg + 0.5f) % 360);
        hudText(W / 2 - font3Width(t) / 2, 3, t, P_HUD_G);
        for (int k = -6; k <= 6; k++) px(W / 2 + k * 6 - (int)(fmodf(hdg, 10.f) * 0.6f), 10, P_HUD_G);
        if (v.navOk) {                                         // the waypoint arrow
            const float a = s_wayRel;
            const float ax = W / 2.f, ay = 22.f;
            line2(ax - sinf(a) * 4.f, ay + cosf(a) * 4.f, ax + sinf(a) * 7.f, ay - cosf(a) * 7.f, P_HUD_G);
            const float hx = ax + sinf(a) * 7.f, hy = ay - cosf(a) * 7.f;
            line2(hx, hy, hx - sinf(a + 0.6f) * 4.f, hy + cosf(a + 0.6f) * 4.f, P_HUD_G);
            line2(hx, hy, hx - sinf(a - 0.6f) * 4.f, hy + cosf(a - 0.6f) * 4.f, P_HUD_G);
            snprintf(t, sizeof t, "%.1f MI", v.navDist / 1609.f);
            hudText(W / 2 + 10, 19, t, P_HUD_G);
            // the caret on the heading strip
            const int cxp = W / 2 + (int)clampf(s_wayRel * 57.3f * 0.6f, -40.f, 40.f);
            px(cxp, 12, P_HUD_A); px(cxp - 1, 13, P_HUD_A); px(cxp + 1, 13, P_HUD_A);
        }
        if (v.retract && v.gearDown && !v.onGround) hudText(4, 10, "GEAR", P_HUD_G);
    }
    if (!cock) drawThrottle();                                 // (in the cockpit: its lever, on the frame)
    // the radio: who's talking and what they say, across the top (in the cockpit: along its roof)
    if (!cock && v.radioText && v.radioA > 0.05f) {
        for (int y = 0; y < 8; y++) for (int x = 0; x < W - 9; x++) fb[y * W + x] = s_refl[s_refl[fb[y * W + x]]];
        char line[80]; snprintf(line, sizeof line, "%s: %s", v.radioWho ? v.radioWho : "", v.radioText);
        int w = font3Width(line);
        int x0 = w < W - 12 ? 2 : 2 - (int)(fmodf(s_t * 18.f, (float)(w - (W - 14) + 40)));   // a long call scrolls
        if (x0 > 2) x0 = 2;
        font3(fb, W, H, x0, 1, line, P_HUD_G, P_HUD_K, 1);
    }
    if (cock) ;                                                  // (the cockpit's TOWER button)
    else if (v.towerBtn) {                                       // CONTACT TOWER (a chip to touch)
        const int x0 = 2, y0 = 15, x1 = 49, y1 = 25;
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px(x, y, (y == y0 || y == y1 || x == x0 || x == x1) ? P_HUD_G : P_HUD_K);
        hudText(x0 + 3, y0 + 3, "CONTACT TWR", P_HUD_G);
    } else if (v.cleared) hudText(3, 17, "CLEARED", P_HUD_G);
    if (v.apMode && !(cock && v.aircraft == A_JET)) hudText(cock ? 6 : 4, cock ? 30 : 26, v.apMode, P_HUD_G);
    if (v.view == VIEW_DROP) hudText(3, 3, "DROP VIEW", P_HUD_G);
    if (!(cock && v.aircraft == A_FIGHTER)) {                          // the fighter's stores, the crates left (its cockpit: a screen)
        char st[32]; int y = cock ? 36 : 34;
        if (v.aam >= 0) { snprintf(st, sizeof st, "AAM %d", v.aam); hudText(3, y, st, P_HUD_G); y += 7; }
        if (v.agm >= 0) { snprintf(st, sizeof st, "AGM %d", v.agm); hudText(3, y, st, P_HUD_G); y += 7; }
        if (v.bombs >= 0) { snprintf(st, sizeof st, "BOMB %d", v.bombs); hudText(3, y, st, P_HUD_G); y += 7; }
        if (v.crates > 0) { snprintf(st, sizeof st, "CRATES %d", v.crates); hudText(3, y, st, P_AMBER); y += 7; }
        if (v.hull >= 0.f && v.hull < 0.99f) { snprintf(st, sizeof st, "HULL %d%%", (int)(v.hull * 100.f)); hudText(3, y, st, v.hull < 0.35f ? P_HUD_R : P_AMBER); }
    }
    if (v.stall > 0.5f && ((int)(s_t * 6.f) & 1)) hudText(W / 2 - font3Width("STALL", 2) / 2, cock ? 52 : 40, "STALL", P_HUD_R, 2);
    if (v.hangarA > 0.05f) hudText(W / 2 - font3Width("MANTIS AERO") / 2, cock ? 46 : 34, "MANTIS AERO", P_HUD_A);
    if (v.msg && v.msgA > 0.05f) {
        hudText(W / 2 - font3Width(v.msg) / 2, 30, v.msg, P_HUD_W);
        if (v.sub) hudText(W / 2 - font3Width(v.sub) / 2, 38, v.sub, P_HUD_A);
    }
    if (cock) cockpitDraw(v, s_cockLight, s_t, s_wayRel, s_wayOn);   // the panel, its instruments, the frame's hardware
    if (v.showFps) { snprintf(t, sizeof t, "%d", v.fps); hudText(2, cock ? 40 : H - 30, t, P_HUD_G); if (g_memNote) hudText(2, cock ? 47 : H - 37, g_memNote, g_memNote[4] == 'O' ? P_HUD_G : P_HUD_R); }
}


// ================================================================================== frame
void renderSetAircraft(uint8_t a) {
    s_veh = a % A_COUNT;
    planeFit(s_veh);
    const uint8_t* cb = cockpitBottom(s_veh);                 // the cockpit's art covers the rest: no raycast there
    for (int x = 0; x < W; x++) s_bottomCock[x] = cb[x];
}


void renderInit() {
    if (!zb) zb = (uint8_t*)memFast(W * H);
#ifndef HOST
    if (!s_tWorker) {   // the terrain's second core: priority 3, below the sound (4), the speaker driver (5) and the screen
        // (6): the sound takes core 0 the moment it has work, the terrain only what is left. (At the sound's own priority
        // the two shared the core a tick at a time and the sound fell behind every frame: choppy audio.)
        s_tDone = xSemaphoreCreateBinary();
        if (!s_tDone || xTaskCreatePinnedToCore(terrainWorker, "terrain", 3072, nullptr, 3, &s_tWorker, 0) != pdPASS) s_tWorker = nullptr;
    }
#endif
    if (!s_gpat) s_gpat = (uint8_t*)memSlow(W * H);
    for (int i = 0; i < 32 * 32; i++) s_cloudMap[i] = (uint8_t)(hash32(0xC10D5u + (uint32_t)i * 0x9E3779B1u) >> 24);
    if (!s_tex) {
        s_tex = (uint8_t*)memFast(64 * 64);
        for (int v = 0; v < 64; v++) for (int u = 0; u < 64; u++) {
            float n = fbmP((float)u / 8.f, (float)v / 8.f, 8, 3, 0x7E57u);
            s_tex[v * 64 + u] = (uint8_t)clampi((int)(n * 255.f), 0, 255);
        }
    }
    spritesInit();
    buildLUTs();
    for (int k = 0; k < 80; k++) { float ty = clampf(k / 78.f, 0.f, 1.f); s_skyPow[k] = 9.f * powf(ty, 1.2f); s_skyTT[k] = ty * ty; }
    for (int c = 0; c < 256; c++) s_zInv[c] = c < 190.53f ? (c / 11.f) * (c / 11.f) : 300.f * expf((c - 190.53f) / 18.1f);
    renderSetAircraft(0);
    memset(fb, 0, sizeof(fb));
}

void (*g_renderTick)() = nullptr;
static inline void renderTick() { if (g_renderTick) g_renderTick(); }
void renderFrame() {
    s_mistOn = (S.mist * 0.4f + g_scene.mist + g_scene.fog * 0.35f) >= 0.04f;
    if (!s_rankMade && s_tex) makeTexRank();
    const Scene& sc = g_scene;
    s_t = sc.t;
    if (g_view.aircraft != s_veh) renderSetAircraft(g_view.aircraft);
    buildPalette();
    setupCamera();
    // sunlight per baked normal, and fog by distance, once a frame
    float se = fmaxf(sc.sunElev, 0.f);
    s_sunDx = cosf(sc.sunAz) * cosf(se); s_sunDz = sinf(sc.sunAz) * cosf(se); s_sunDy = sinf(se) + 0.15f;
    for (int b = 0; b < 256; b++) {
        float nx = ((b >> 4) - 8) / 8.f, nz = ((b & 15) - 8) / 8.f;
        float ny = sqrtf(fmaxf(0.05f, 1.f - nx * nx - nz * nz));
        float d = fmaxf(0.f, nx * s_sunDx + ny * s_sunDy + nz * s_sunDz);
        s_light[b] = 0.22f + 0.1f * ny + 0.62f * d * s_sunK;
        s_lightSh[b] = 0.22f + 0.1f * ny;
    }
    {   // the ground's grain lit from the sun's side, and the props' shadows on the ground (sunny days only)
        const float hx = cosf(sc.sunAz), hz = sinf(sc.sunAz);
        s_rdu = hx > 0.38f ? 1 : (hx < -0.38f ? -1 : 0);
        s_rdv = hz > 0.38f ? 1 : (hz < -0.38f ? -1 : 0);
        s_reliefK = s_sunK * (1.f / 45.f);
        // cloud shadows: more cover, more of the map in shade; none under a clear sky or a full overcast
        const float cover = g_scene.cloud;
        s_cloudAmt = s_sunK > 0.2f ? 0.16f * clampf(cover * 3.f, 0.f, 1.f) * clampf((0.9f - cover) * 4.f, 0.f, 1.f) : 0.f;
        s_cloudThr = 255.f * (1.f - cover * 0.8f) - 20.f;
        const float drift = (2.f + g_scene.wind * 6.f) * g_scene.dt;      // the shadows move with the wind
        s_cloudOx -= cosf(g_scene.windAz) * drift; s_cloudOz -= sinf(g_scene.windAz) * drift;
        if (fabsf(s_cloudOx) > 2240.f * 64.f) s_cloudOx = fmodf(s_cloudOx, 2240.f);   // (the map repeats every 2240 m)
        if (fabsf(s_cloudOz) > 2240.f * 64.f) s_cloudOz = fmodf(s_cloudOz, 2240.f);
        s_propSh = s_sunK > 0.25f;
        const float rel = wrapPi(sc.sunAz - cyaw), low = clampf(1.f - sc.sunElev / 0.6f, 0.f, 1.f);
        s_propShX = -sinf(rel) * (0.5f + 0.9f * low);       // away from the sun: left when it's to the right
        s_propShY = -cosf(rel) * 0.6f;                      // toward you when it's ahead (lower on screen)
    }
    // visibility: kilometres on a clear day (a little more from altitude), hundreds of metres in fog
    // fog and dust lie in the valleys and thin out above them: from altitude you see down through the layer (rain
    // and snow fall from the cloud base, so they stay, a little thinner)
    const float layer = 1.f / (1.f + fmaxf(0.f, g_view.agl - 60.f) * (1.f / 120.f));
    float vis = 8000.f - sc.fog * 7000.f * layer - sc.rain * 4200.f * (0.5f + 0.5f * layer) - sc.snow * 5500.f * (0.6f + 0.4f * layer)
              - sc.dust * 3500.f * layer - sc.storm * 1500.f;
    vis = fmaxf(vis, 260.f) * (1.f + clampf(g_view.agl * (1.f / 3000.f), 0.f, 0.4f));
    {   // from altitude the land's far edge (the vast cache's) is in view: the haze closes in to meet it there, so the
        // land fades into the horizon instead of stopping (the gap above it is filled with the same haze)
        const Grid& gV = terrainVast();
        if (gV.c) {
            const float cs = 1.f / gV.inv, x0 = (gV.ox + 2) * cs, x1 = (gV.ox + gV.N - 2) * cs, z0 = (gV.oz + 2) * cs, z1 = (gV.oz + gV.N - 2) * cs;
            float reach = 1e9f;
            const int cols[3] = {0, W / 2, W - 1};
            for (int k = 0; k < 3; k++) {
                float dx = dxC[cols[k]], dz = dzC[cols[k]]; const float l = sqrtf(dx * dx + dz * dz); dx /= l; dz /= l;
                const float tx = dx > 1e-4f ? (x1 - cx) / dx : (dx < -1e-4f ? (x0 - cx) / dx : 1e9f);
                const float tz = dz > 1e-4f ? (z1 - cz) / dz : (dz < -1e-4f ? (z0 - cz) / dz : 1e9f);
                reach = fminf(reach, fminf(tx, tz));
            }
            s_landReach = fmaxf(reach, 1200.f);
            const float k = clampf((g_view.agl - 40.f) * (1.f / 260.f), 0.f, 1.f);   // low down the land hides its own edge
            vis = fminf(vis, vis + (s_landReach / 1.25f - vis) * k);
        }
    }
    for (int k = 0; k < 1024; k++) s_fog[k] = clampf((k * 8.f - vis * 0.3f) / (vis * 1.0f), 0.f, 1.f);
    const Land& L = journeyLand();
    s_flowers = L.flowers; s_wet = sc.wet; s_paved = 1.f;
    const bool cockpit = g_view.view == VIEW_COCKPIT && !g_view.photo;
    if (!cockpit) cockpitOff();                                 // (hands the overlay back: photo mode, the other views)
    if (!cockpit || s_flip) memset(s_bottom, H, sizeof(s_bottom));   // upside down: the whole picture (the panel goes on after)
    else memcpy(s_bottom, s_bottomCock, sizeof(s_bottom));
    memset(zb, 255, W * H);
    if (g_view.view == VIEW_DROP && !g_view.photo) {                // straight down: the drop view
        memset(s_bottom, H, sizeof(s_bottom));
        drawDropView();
        renderTick();
        drawHud();
        s_frameN++;
        return;
    }
    setupDeck();
    drawTerrain(8200.f);
    renderTick();
    drawSkyAndPanorama();
    drawNight();
    drawSun();
    if (cy < s_deckY || s_deckAmt <= 0.f) drawClouds();      // the horizon's clouds: from under the deck
    drawCloudDeck();
    drawBolt();
    drawBirds();
    g_riverNear = clampf(1.f - terrainRiverDist(cx, cz) * (1.f / 160.f), 0.f, 1.f) * clampf(1.f - g_view.agl / 300.f, 0.f, 1.f);
    if (g_view.agl < 600.f || g_view.photo) drawVegetation();
    renderTick();
    const bool low = g_view.agl < 120.f || g_view.photo;      // (cows, fences and deer are a pixel or less from higher up)
    if (low) drawFarm();
    drawGeysers();
    drawAirfields();
    drawMarks();
    if (low) drawCritters();
    drawParticles();
    renderTick();
    if (s_anyRefl) {                                 // (no water or wet ground in view: nothing to mirror or glint)
        if (s_wet > 0.f) buildWetTables();
        reflectPass();
        drawGlint();
    }
    if (g_view.agl < 60.f || g_view.photo) drawFireflies();   // (around the aircraft on the ground: none up there)
    drawMist();
    drawShafts();
    drawRain();
    drawSnow();
    if (g_view.agl < 60.f || g_view.photo) drawLeaves();
    drawInCloud();
    if (g_view.view == VIEW_CHASE || g_view.photo) drawAircraft();
    if (s_capWater) { classifyWater(); s_capWater = false; }
    if (g_view.photo) { s_frameN++; return; }        // the viewfinder: the world only
    if (s_flip) {                                    // upside down: the picture turned over (sky below)
        for (int i = 0, j = W * H - 1; i < j; i++, j--) { uint8_t t = fb[i]; fb[i] = fb[j]; fb[j] = t; t = zb[i]; zb[i] = zb[j]; zb[j] = t; }
    }
    drawGlass();
    renderTick();
    drawHud();
    s_bob = 0.f;
    s_frameN++;
}


// ---- the straight-down shot: the land under the aircraft, orthographic (north... no: the nose up the frame), lit by the
// scene, with the water marked for the jig file and the height of every pixel kept for its depth planes
static float* s_tdH = nullptr;
const float* renderTopHeights() { return s_tdH; }
static float s_tdX = 0.f, s_tdZ = 0.f, s_tdC = 1.f, s_tdS = 0.f, s_tdK = 1.f, s_tdFwd = 0.f;   // (fwd: the picture's middle, metres ahead)
static bool tdPt(float wx, float wz, float& sx, float& sy) {          // a world point in the straight-down picture
    const float dx = wx - s_tdX, dz = wz - s_tdZ;
    const float w = dx * s_tdC + dz * s_tdS, u = -dx * s_tdS + dz * s_tdC;
    sx = W * 0.5f + u / s_tdK; sy = H * 0.5f - w / s_tdK;
    return sx >= -8.f && sx < W + 8 && sy >= -8.f && sy < H + 8;
}
bool renderDropSees(float wx, float wz, float* span) {
    float sx, sy;
    if (span) *span = s_tdK * W;
    return tdPt(wx, wz, sx, sy) && sx > 10.f && sx < W - 10.f && sy > 12.f && sy < H - 12.f;
}
static void topDown(float span, bool capture);
void renderTopDown(float span) { topDown(span, true); }
static void topDown(float span, bool capture) {
    const ViewInfo& v = g_view;
    if (capture && !s_tdH) s_tdH = (float*)memSlow(W * H * sizeof(float));
    if (capture) renderCaptureWater(true);
    const float fyaw = v.yaw, c = cosf(fyaw), s = sinf(fyaw);
    if (capture) s_tdFwd = 0.f;
    const float ox = v.x + c * s_tdFwd, oz = v.z + s * s_tdFwd;   // the picture's middle
    s_tdX = ox; s_tdZ = oz; s_tdC = c; s_tdS = s; s_tdK = span / W;
    const float k = span / W;                                     // metres a pixel
    const Grid* gs[3] = {&terrainNear(), &terrainFar(), &terrainVast()};
    const bool coast = g_scene.biomeA == B_COAST || g_scene.biomeB == B_COAST;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const float u = (x + 0.5f - W * 0.5f) * k, w = (H * 0.5f - (y + 0.5f)) * k;   // right, forward
        const float wx = ox + c * w - s * u, wz = oz + s * w + c * u;
        const Grid* g = nullptr; int ci = 0, cj = 0;
        for (int q = 0; q < 3 && !g; q++) {
            const Grid& G = *gs[q];
            if (!G.c) continue;
            ci = (int)floorf(wx * G.inv); cj = (int)floorf(wz * G.inv);
            if (ci >= G.ox && cj >= G.oz && ci < G.ox + G.N && cj < G.oz + G.N && (q > 0 || k < (capture ? 6.f : 1.6f))) g = &G;   // (the drop view: one grid, no seam)
        }
        const int i = y * W + x;
        if (!g) { fb[i] = (uint8_t)(P_HAZE + 8); if (capture) s_tdH[i] = 0.f; continue; }
        const Cell& cell = g->c[gridWrap(cj, g->N) * g->N + gridWrap(ci, g->N)];
        const uint8_t mat = cell.mat & 15, surf = cell.mat >> 4;
        float lt = cellLight(*g, cell);
        if (s_cloudAmt > 0.f) lt *= cloudK(wx, wz);
        const float h = cell.h * 0.1f;
        bool refl = false;
        uint8_t col;
        if (mat == MAT_WATER) {
            col = (uint8_t)(P_WATER + clampi((int)(lt * 6.f) + (tex((int)(wx * 0.3f), (int)(wz * 0.3f)) > 150 ? 1 : 0), 0, 7));
            if (capture) s_wmask[i] = terrainRiverDist(wx, wz) < 30.f ? 2 : (coast ? 3 : 1);
        } else if (g == gs[0] && (cell.extra & 15) && (cell.extra & 15) < VEG_N && mat != MAT_RUNWAY && mat != MAT_LOT && !(cell.extra & 0x80)) {
            col = (uint8_t)(P_LEAF + clampi((int)(lt * 13.f) - 2, 0, 15));   // a tree's crown
        } else {
            col = groundPix(x, y, wx, wz, lt, k * F, h, mat, surf, g != gs[0], cell.extra, refl);
            if (g != gs[0] && cell.extra && cell.extra < 0x80 && mat < MAT_WATER) col = (uint8_t)(P_LEAF + clampi((int)(lt * 12.f) - 1, 0, 15));
        }
        fb[i] = col;
        if (capture) s_tdH[i] = h + ((g != gs[0] && cell.extra && cell.extra < 0x80 && mat < MAT_WATER) ? cell.extra : 0.f);
    }
    // the aircraft's own shadow in the middle (where the sun puts it)
    if (s_sunK > 0.15f && g_scene.sunElev > 0.08f) {
        const float off = v.agl / tanf(fmaxf(g_scene.sunElev, 0.1f));
        const float du = -(-sinf(g_scene.sunAz) * c + cosf(g_scene.sunAz) * s) * off, dw = -(cosf(g_scene.sunAz) * c + sinf(g_scene.sunAz) * s) * off;
        const int sx = (int)(W * 0.5f + du / k), sy = (int)(H * 0.5f - (dw - s_tdFwd) / k);
        const AircraftSpec& A = aircraftSpec(v.aircraft);
        const int hs = (int)(A.span * 0.5f / k), hl = (int)(A.span * 0.4f / k);
        for (int d = -hs; d <= hs; d++) { px(sx + d, sy, s_refl[s_refl[fb[clampi(sy, 0, H - 1) * W + clampi(sx + d, 0, W - 1)]]]); }
        for (int d = -hl; d <= hl / 2; d++) { px(sx, sy + d, s_refl[s_refl[fb[clampi(sy + d, 0, H - 1) * W + clampi(sx, 0, W - 1)]]]); }
    }
    s_capWater = false;
}

// the drop view: the land straight below, the zone, the targets, the falling things, the impact pip, a reticle
static void drawDropView() {
    const ViewInfo& v = g_view;
    // the sight looks ahead as far as the release would carry: the picture runs from behind you to past the pip
    float throwF = 0.f;
    for (int k = 0; k < g_markN; k++) if (g_marks[k].kind == MK_PIP)
        throwF = fmaxf(0.f, (g_marks[k].x - v.x) * cosf(v.yaw) + (g_marks[k].z - v.z) * sinf(v.yaw));
    static float sFwd = 0.f, sSpan = 300.f;                      // eased, so the picture doesn't jump
    const float wantSpan = fmaxf(clampf(v.agl * 1.3f + 160.f, 200.f, 3000.f), fminf((throwF * 1.25f + 300.f) * (float)W / H, 4500.f));
    sSpan += (wantSpan - sSpan) * 0.08f; sFwd += (throwF * 0.5f + throwF * 0.1f - sFwd) * 0.08f;   // (clear of the tape at the top, the numbers below)
    const float span = sSpan;
    s_tdFwd = sFwd;
    topDown(span, false);
    auto ring = [&](float wx, float wz, float r, uint8_t c) {
        float sx, sy; tdPt(wx, wz, sx, sy);
        const float rp = r / s_tdK;
        const int n = (int)clampf(rp * 3.f, 12.f, 64.f);
        for (int k = 0; k < n; k++) { const float a = k * 6.2831853f / n; px((int)(sx + cosf(a) * rp), (int)(sy + sinf(a) * rp), c); }
    };
    for (int k = 0; k < g_markN; k++) {
        const Marker& m = g_marks[k];
        float sx, sy;
        if (!tdPt(m.x, m.z, sx, sy)) continue;
        if (m.kind == MK_ZONE) { ring(m.x, m.z, m.r, P_AMBER); ring(m.x, m.z, m.r * 0.5f, P_AMBER); ring(m.x, m.z, 12.f, P_HUD_W); }
        else if (m.kind == MK_GTARGET || m.kind == MK_GTARGET_LOCK) {
            const int r = (int)fmaxf(1.f, 5.f / s_tdK);
            for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) px((int)sx + x, (int)sy + y, (uint8_t)(P_ROAD + 3));
            for (int q = -4; q <= 4; q++) { px((int)sx + q, (int)sy - 5, P_HUD_R); px((int)sx + q, (int)sy + 5, P_HUD_R); px((int)sx - 5, (int)sy + q, P_HUD_R); px((int)sx + 5, (int)sy + q, P_HUD_R); }
        } else if (m.kind == MK_BEACON) ring(m.x, m.z, 30.f, P_AMBER);
        else if (m.kind == MK_BOMB || m.kind == MK_CRATE) { px((int)sx, (int)sy, P_HUD_W); px((int)sx + 1, (int)sy, P_HUD_W); }
        else if (m.kind == MK_PIP) {                                  // where a release now would land
            for (int q = -3; q <= 3; q++) { px((int)sx + q, (int)sy, P_HUD_G); px((int)sx, (int)sy + q, P_HUD_G); }
            ring(m.x, m.z, 6.f * s_tdK, P_HUD_G);
        }
    }
    // the reticle (straight under the aircraft), the line ahead it releases along, and the scale
    float ax, ay; tdPt(v.x, v.z, ax, ay);
    const int cx = (int)ax, cy = (int)ay;
    for (int q = 6; q < 16; q++) { px(cx + q, cy, P_HUD_G); px(cx - q, cy, P_HUD_G); px(cx, cy + q, P_HUD_G); }
    for (int y = cy - 6; y > 2; y -= 3) px(cx, y, P_HUD_G);
    char t[24]; snprintf(t, sizeof t, "%dM", (int)span);           // the frame's width
    font3(fb, W, H, cx + 6, cy + 4, t, P_HUD_G, P_HUD_K, 1);
}

const uint8_t* frameBuffer() { return fb; }
bool renderDepthFast() { return memIsFast(zb); }
const char* g_memNote = nullptr;
const uint16_t* framePalette565() { return pal565q; }
const uint8_t* framePaletteRGB() { return palRGB; }
int frameBob() { return 0; }

}
