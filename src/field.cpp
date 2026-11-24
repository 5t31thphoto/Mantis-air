#pragma GCC optimize("Os")
#include "field.h"
#include "mem.h"
#include "airfield.h"
#include "terrain.h"
#include "journey.h"
#include "scene.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {

static Particle* s_p = nullptr;                    // (PSRAM: PART_N of them, made on first use)
static Particle* parts() {
    if (!s_p) { s_p = (Particle*)memSlow(PART_N * sizeof(Particle)); if (s_p) memset(s_p, 0, PART_N * sizeof(Particle)); }
    return s_p;
}
static int s_pNext = 0;
static Critter s_c[CRIT_N];
static Bird s_bird;
static uint32_t s_seed = 1, s_rng = 1;
static float s_deerNext = 8.f, s_birdNext = 20.f;

static inline float frand() { s_rng = hash32(s_rng + 0x9E3779B9u); return (s_rng >> 8) * (1.f / 16777216.f); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

void fieldInit(uint32_t seed) {
    if (parts()) memset(s_p, 0, PART_N * sizeof(Particle));
    memset(s_c, 0, sizeof(s_c)); memset(&s_bird, 0, sizeof(s_bird));
    s_seed = seed; s_rng = seed ^ 0xF1E1Du;
    s_deerNext = 6.f; s_birdNext = 15.f;
}

const Particle* fieldParticles() { return parts(); }
const Critter* fieldCritters() { return s_c; }
const Bird* fieldBirds() { return &s_bird; }

void fieldEmit(uint8_t kind, float x, float y, float z, float vx, float vy, float vz, float size, float life) {
    if (!parts()) return;
    Particle& p = s_p[s_pNext];
    s_pNext = (s_pNext + 1) % PART_N;
    p.x = x; p.y = y; p.z = z; p.vx = vx; p.vy = vy; p.vz = vz; p.size = size; p.life = life; p.age = 0.f; p.kind = kind;
}

void fieldHorn(float x, float z) {
    for (Critter& c : s_c) if (c.on) { float d = hypotf(c.x - x, c.z - z); if (d < 80.f) c.fear = 1.f; }
}

// Bigfoot: rare, in the redwoods, the pines and on the coast. It stands at a forest's edge 300-500 m ahead of a low
// aircraft; once you're within 150 m it turns and walks off into the trees, and is gone. (Fly low and slow, look out.)
static bool spawnBigfoot(const AircraftState& s) {
    const uint8_t b = g_scene.biomeA;
    if (b != B_REDWOOD && b != B_PINE && b != B_COAST) return false;
    const Land& L = journeyLand();
    if (L.pine + L.redwood + L.broad < 0.3f || frand() > 0.08f || s.agl > 200.f) return false;
    for (const Critter& c : s_c) if (c.on && c.kind == 1) return false;
    const float d = 300.f + frand() * 200.f, side = (frand() - 0.5f) * 120.f;
    const float x = s.x + cosf(s.yaw) * d - sinf(s.yaw) * side, z = s.z + sinf(s.yaw) * d + cosf(s.yaw) * side;
    Ground g; terrainGround(x, z, g);
    if (g.mat == MAT_WATER || g.mat == MAT_RUNWAY || g.ny < 0.85f) return false;
    for (Critter& c : s_c) if (!c.on) {
        c.on = true; c.x = x; c.z = z; c.y = g.h; c.kind = 1; c.pose = 0; c.fear = 0.f; c.speed = 0.f; c.t = 0.f;
        c.yaw = atan2f(s.z - z, s.x - x) + (frand() - 0.5f) * 0.6f;              // looking at you
        c.dodger = false;
        return true;
    }
    return false;
}

// deer graze in open country ahead of a low aircraft (and on the grass beside the strips)
static void spawnDeer(const AircraftState& s) {
    const Land& L = journeyLand();
    float forest = L.pine + L.broad + L.redwood + L.flowers * 0.5f + L.bush * 0.3f;
    if (forest < 0.2f || g_scene.biomeA == B_TUNDRA || s.agl > 150.f) return;
    for (Critter& c : s_c) if (!c.on) {
        float fx = cosf(s.yaw), fz = sinf(s.yaw);
        float x = 0.f, z = 0.f; Ground g; bool ok = false;
        for (int tries = 0; tries < 5 && !ok; tries++) {
            float d = 120.f + frand() * (s.onGround ? 120.f : 260.f);
            float side = (frand() - 0.5f) * 80.f;
            x = s.x + fx * d - fz * side; z = s.z + fz * d + fx * side;
            terrainGround(x, z, g);
            ok = g.mat != MAT_WATER && g.mat != MAT_ROCK && g.mat != MAT_RUNWAY;
        }
        if (!ok) return;
        c.on = true; c.x = x; c.z = z; c.y = g.h; c.kind = 0; c.pose = 3; c.fear = 0.f; c.speed = 0.f;
        c.dodger = true;
        c.yaw = frand() * 6.283f; c.t = 0.f;
        if (frand() < 0.5f) for (Critter& d2 : s_c) if (!d2.on) { d2 = c; d2.x += (frand() - 0.5f) * 8.f; d2.z += (frand() - 0.5f) * 8.f; d2.yaw += frand() - 0.5f; break; }
        return;
    }
}

// ---- pastures: fences and cattle (the renderer draws exactly these) ----
static uint32_t s_broken[24]; static int s_brokenN = 0;          // burst fence spans (a ring: the oldest heal)
static uint32_t s_dead[16];   static int s_deadN = 0;            // cattle that met a car at speed
static inline uint32_t fenceKey(int pi, int pj, int side, int k) { return hash32((uint32_t)pi * 0x9E3779B1u ^ (uint32_t)pj * 0x85EBCA77u ^ (uint32_t)(side * 16 + k) * 0xC2B2AE3Du) | 1u; }
static inline uint32_t cowKey(int pi, int pj, int k) { return hash32((uint32_t)pi * 0x27D4EB2Fu ^ (uint32_t)pj * 0x165667B1u ^ (uint32_t)k * 0x9E3779B1u) | 1u; }
static bool inList(const uint32_t* a, int n, int cap, uint32_t key) { for (int i = 0; i < (n < cap ? n : cap); i++) if (a[i] == key) return true; return false; }
bool fieldFenceBroken(int pi, int pj, int side, int k) { return s_brokenN && inList(s_broken, s_brokenN, 24, fenceKey(pi, pj, side, k)); }
static void sideGeom(int side, float s, float& u, float& v, float& iu, float& iv) {
    const float a = 3.2f, b = PLOT_M - 3.2f;
    switch (side) {
        case 0: u = s; v = a; iu = 0.f; iv = 3.f; break;
        case 1: u = b; v = s; iu = -3.f; iv = 0.f; break;
        case 2: u = s; v = b; iu = 0.f; iv = -3.f; break;
        default: u = a; v = s; iu = 3.f; iv = 0.f; break;
    }
}
bool fieldFencePost(int pi, int pj, int side, int k, float& x, float& z) {
    const float a = 3.2f, b = PLOT_M - 3.2f, st = (b - a) / FENCE_POSTS;
    float u, v, iu, iv;
    sideGeom(side, a + st * k, u, v, iu, iv);
    x = pi * PLOT_M + u; z = pj * PLOT_M + v;
    return (terrainNearExtra(x + iu, z + iv) & 0xE0) == 0xE0;       // stands only where the pasture is (flat, off the road)
}
int fieldCows(float cx, float cz, float R, float tt, Cow* out, int max) {
    int m = 0;
    const int i0 = (int)floorf((cx - R) * (1.f / PLOT_M)), i1 = (int)floorf((cx + R) * (1.f / PLOT_M));
    const int j0 = (int)floorf((cz - R) * (1.f / PLOT_M)), j1 = (int)floorf((cz + R) * (1.f / PLOT_M));
    for (int pj = j0; pj <= j1; pj++) for (int pi = i0; pi <= i1; pi++) {
        if ((terrainPlot(pi, pj) & 0xE0) != 0xE0) continue;
        const float bx = pi * PLOT_M, bz = pj * PLOT_M;
        uint32_t hp = hash32((uint32_t)pi * 0x2545F491u ^ (uint32_t)pj * 0x9E3779B1u ^ 0xC0Du);
        int n = 3 + (int)(hp & 3);
        float hx0 = bx + 12.f + 32.f * hashf(hp >> 3), hz0 = bz + 12.f + 32.f * hashf(hp >> 7);   // the herd's centre
        for (int k = 0; k < n && m < max; k++) {
            uint32_t hc = hash32(hp + (uint32_t)k * 0x632BE5ABu);
            float yaw = hashf(hc) * 6.2831853f, ph = hashf(hc >> 5) * 6.2831853f;
            float amble = sinf(tt * 0.04f + ph) * 2.5f;
            float x = hx0 + (hashf(hc >> 9) - 0.5f) * 16.f + cosf(yaw) * amble, z = hz0 + (hashf(hc >> 13) - 0.5f) * 16.f + sinf(yaw) * amble;
            if ((x - cx) * (x - cx) + (z - cz) * (z - cz) > R * R) continue;
            if ((terrainNearExtra(x, z) & 0xE0) != 0xE0) continue;
            if (s_deadN && inList(s_dead, s_deadN, 16, cowKey(pi, pj, k))) continue;
            Cow& c = out[m++];
            c.x = x; c.z = z; c.pose = sinf(tt * 0.3f + ph * 3.f) > -0.4f ? 1 : 0;        // mostly grazing
            c.heading = yaw + (cosf(tt * 0.04f + ph) < 0.f ? 3.1415927f : 0.f);          // facing the way it ambles
        }
    }
    return m;
}
// rolling into a pasture's fence or its cattle (on the ground, or very low)
static void pastureCollide(AircraftState& s, uint8_t id, FieldHit& out) {
    if (s.agl > 4.f) return;
    const int pi0 = (int)floorf(s.x * (1.f / PLOT_M)), pj0 = (int)floorf(s.z * (1.f / PLOT_M));
    for (int pj = pj0 - 1; pj <= pj0 + 1; pj++) for (int pi = pi0 - 1; pi <= pi0 + 1; pi++) {
        if ((terrainPlot(pi, pj) & 0xE0) != 0xE0) continue;
        for (int side = 0; side < 4; side++) {
            float ax, az, bx, bz;
            fieldFencePost(pi, pj, side, 0, ax, az); fieldFencePost(pi, pj, side, FENCE_POSTS, bx, bz);
            float dx = bx - ax, dz = bz - az, L2 = dx * dx + dz * dz;
            float tq = ((s.x - ax) * dx + (s.z - az) * dz) / L2;
            if (tq < -0.02f || tq > 1.02f) continue;
            tq = tq < 0.f ? 0.f : (tq > 1.f ? 1.f : tq);
            float px = ax + dx * tq, pz = az + dz * tq;
            int k = (int)(tq * FENCE_POSTS); if (k >= FENCE_POSTS) k = FENCE_POSTS - 1;
            if (fieldFenceBroken(pi, pj, side, k)) continue;
            float gy = terrainHeight(px, pz);
            float h = aircraftHitPost(s, id, px, pz, 0.15f, gy, gy + 1.3f);
            if (h > out.strength) { out.strength = h; out.kind = HIT_TREE; }
        }
    }
    Cow cows[12];
    int n = fieldCows(s.x, s.z, 12.f, g_scene.t, cows, 12);
    for (int i = 0; i < n; i++) {
        float gy = terrainHeight(cows[i].x, cows[i].z);
        float h = aircraftHitPost(s, id, cows[i].x, cows[i].z, 1.0f, gy, gy + 1.6f);
        if (h > out.strength) { out.strength = h; out.kind = HIT_ROCK; }
    }
}

static Geyser s_gey[8]; static int s_geyN = 0;
const Geyser* fieldGeysers(int& n) { n = s_geyN; return s_gey; }
// ---- geysers: in Devil's Pass, the tundra and the moor; one chance in each 120 m square, off the road. Each erupts
// for 7 s in its own cycle (30-50 s): a column up to 20 m, steaming between.
static void findGeysers(float cx, float cz, float t) {
    s_geyN = 0;
    const uint8_t b = journeyCurrent();
    if (b != B_DEVILS && b != B_TUNDRA && b != B_MOOR) return;
    const float Q = 120.f, R = 260.f;
    const int i0 = (int)floorf((cx - R) / Q), i1 = (int)floorf((cx + R) / Q), j0 = (int)floorf((cz - R) / Q), j1 = (int)floorf((cz + R) / Q);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1 && s_geyN < 8; i++) {
        const uint32_t h = hash32((uint32_t)i * 0x85EBCA77u ^ (uint32_t)j * 0xC2B2AE3Du ^ worldSeed() ^ 0x6E75u);
        if (hashf(h) > 0.4f) continue;
        const float x = i * Q + 20.f + 80.f * ((h >> 8) & 255) * (1.f / 255.f), z = j * Q + 20.f + 80.f * ((h >> 16) & 255) * (1.f / 255.f);
        if ((x - cx) * (x - cx) + (z - cz) * (z - cz) > R * R) continue;
        Ground gd; terrainGround(x, z, gd);
        if (gd.mat == MAT_WATER || gd.mat == MAT_RUNWAY || gd.mat == MAT_LOT || gd.ny < 0.92f) continue;   // on open, level ground
        const float P = 30.f + 20.f * hashf(h >> 3), ph = fmodf(t + 97.f * hashf(h >> 5), P);
        float hh = 0.f;
        if (ph < 7.f) { float u = ph * (1.f / 7.f); hh = 20.f * (u < 0.15f ? u / 0.15f : 1.f - (u - 0.15f) * (u - 0.15f) * (1.f / 0.7225f)); }
        Geyser& g = s_gey[s_geyN++];
        g.x = x; g.z = z; g.y = gd.h; g.h = hh;
    }
}

void fieldUpdate(const AircraftState& car, uint8_t id, float dt, bool smoke) {
    findGeysers(car.x, car.z, g_scene.t);
    for (int k = 0; k < s_geyN; k++) {                              // spray from an erupting geyser near enough to see it
        const Geyser& g = s_gey[k];
        if (g.h < 3.f || (g.x - car.x) * (g.x - car.x) + (g.z - car.z) * (g.z - car.z) > 300.f * 300.f || frand() > 0.5f) continue;
        fieldEmit(PK_SNOW, g.x + (frand() - 0.5f), g.y + g.h * (0.7f + 0.3f * frand()), g.z + (frand() - 0.5f),
                  (frand() - 0.5f) * 5.f, 2.f + frand() * 3.f, (frand() - 0.5f) * 5.f, 0.25f, 2.f + frand());
    }
    if (parts()) for (int pk = 0; pk < PART_N; pk++) { Particle& p = s_p[pk];   // particles
        if (p.life <= 0.f) continue;
        p.age += dt;
        if (p.age >= p.life) { p.life = 0.f; continue; }
        const bool puff = p.kind == PK_DUST || p.kind == PK_SMOKE || p.kind == PK_MIST || p.kind == PK_TRAIL || p.kind == PK_FIRE;
        float drag = puff ? 1.6f : (p.kind == PK_FUR ? 2.2f : (p.kind == PK_TRACER ? 0.f : 0.4f));
        if (p.kind == PK_TRAIL) drag = 0.9f;
        p.vx -= p.vx * drag * dt; p.vz -= p.vz * drag * dt;
        float ay = puff ? (p.kind == PK_TRAIL ? -p.vy * 1.2f : (p.kind == PK_FIRE ? 3.f - p.vy : 0.6f - p.vy * 1.5f)) : (p.kind == PK_FUR ? -1.6f - p.vy * 1.2f : -9.8f);
        if (p.kind == PK_TRACER) ay = -2.f;
        p.vy += ay * dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        if (p.kind == PK_DUST || p.kind == PK_SMOKE || p.kind == PK_FIRE) p.size += dt * 0.9f;
        if (p.kind == PK_TRAIL) p.size += dt * 0.8f;
        if (p.kind == PK_MIST) p.size += dt * 2.2f;
    }
    const AircraftSpec& A = aircraftSpec(id);
    const float fx = car.fx, fz = car.fz;
    // the prop wash (or jet blast) on the ground, and the spray off the water
    if (car.agl < 6.f && (car.power > 0.4f || car.groundSpeed > 8.f)) {
        static float acc = 0.f;
        acc += dt * 30.f * (car.onWater ? fminf(1.f, car.groundSpeed / 12.f + 0.2f) : 0.35f * car.power);
        while (acc > 1.f) {
            acc -= 1.f;
            uint8_t k = car.onWater ? PK_SPRAY : (car.surf == SURF_SNOW ? PK_SNOW : (car.surf == SURF_TARMAC ? 0xFF : PK_DUST));
            if (k == 0xFF) continue;
            const float side = (frand() - 0.5f) * A.span * 0.4f;
            const float x = car.x - fx * 2.5f - fz * side, z = car.z - fz * 2.5f + fx * side;
            fieldEmit(k, x, car.surfY + 0.2f, z, -fx * 6.f + (frand() - 0.5f) * 3.f, k == PK_DUST ? 0.6f : 2.f + frand() * 3.f, -fz * 6.f + (frand() - 0.5f) * 3.f,
                      k == PK_DUST ? 0.6f : 0.12f, k == PK_DUST ? 1.6f : 0.9f);
        }
    }
    if (car.evSplash > 0.f) for (int i = 0; i < 4; i++)
        fieldEmit(PK_SPRAY, car.x + (frand() - 0.5f) * 3.f, car.surfY, car.z + (frand() - 0.5f) * 3.f, car.vx * 0.3f + (frand() - 0.5f) * 5.f, 2.f + frand() * 3.f, car.vz * 0.3f + (frand() - 0.5f) * 5.f, 0.2f, 0.9f);
    // a smoke ribbon from the tail (airshow smoke; the fighter's afterburner haze)
    if (smoke && !car.onGround) {
        static float sacc = 0.f;
        sacc += dt * 24.f;
        while (sacc > 1.f) {
            sacc -= 1.f;
            const float t = frand();
            fieldEmit(PK_TRAIL, car.x - car.fx * (3.f + t * car.speed * dt), car.y - car.fy * 3.f, car.z - car.fz * (3.f + t * car.speed * dt),
                      car.vx * 0.05f, 0.f, car.vz * 0.05f, 0.7f, 5.f);
        }
    }
    // deer: graze in the open, look up, bound away from a low pass
    s_deerNext -= dt;
    if (s_deerNext <= 0.f) { s_deerNext = 10.f + frand() * 20.f; if (!spawnBigfoot(car)) spawnDeer(car); }
    for (Critter& c : s_c) {
        if (!c.on) continue;
        c.t += dt;
        float dx = c.x - car.x, dz = c.z - car.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d > 700.f) { c.on = false; continue; }
        if (c.kind == 1) {                                                   // Bigfoot
            if (c.fear < 0.3f && d < 150.f) c.fear = 1.f;                     // seen: it goes
            if (c.fear > 0.3f) {
                c.yaw += clampf(wrapPi(atan2f(dz, dx) - c.yaw), -1.2f * dt, 1.2f * dt);   // turning away from the car
                c.speed += (2.4f - c.speed) * dt * 1.5f;                     // long, unhurried strides
            }
            if (c.fear > 0.3f && c.t > 12.f && d > 200.f) { c.on = false; continue; }   // into the trees
            c.x += cosf(c.yaw) * c.speed * dt; c.z += sinf(c.yaw) * c.speed * dt;
            c.y = terrainHeight(c.x, c.z);
            c.pose = c.speed > 0.4f ? (uint8_t)(1 + ((int)(c.t * 1.8f) & 1)) : 0;
            continue;
        }
        // at night a deer caught in the headlights freezes, until the car is nearly on it
        float hx = c.x - car.x, hz = c.z - car.z, ahd = hx * cosf(car.yaw) + hz * sinf(car.yaw);
        bool frozen = g_scene.lights > 0.5f && car.onGround && ahd > 7.f && ahd < 80.f && fabsf(-hx * sinf(car.yaw) + hz * cosf(car.yaw)) < 7.f + ahd * 0.35f;   // the beams light the roadside too
        if (frozen) { c.speed = 0.f; c.fear = 0.f; c.pose = 3; }      // stock still in the light
        else if (d < 40.f + car.speed * 1.2f && car.agl < 120.f) c.fear = fmaxf(c.fear, 0.7f);
        // a car bearing down: a last-moment leap sideways, clear of its path
        float fx = cosf(car.yaw), fz = sinf(car.yaw);
        float ahead = dx * fx + dz * fz, lat = -dx * fz + dz * fx;           // the deer, in the car's frame
        if (c.dodger && ahead > 0.f && ahead < 4.f + car.speed * 0.35f && fabsf(lat) < 3.f) {
            c.yaw = car.yaw + (lat >= 0.f ? 1.5708f : -1.5708f);
            c.speed = 15.f; c.fear = 1.f;
        }
        if (c.fear > 0.3f) {
            float away = atan2f(dz, dx) + sinf(c.t * 0.7f + c.x) * 0.5f;
            c.yaw += clampf(wrapPi(away - c.yaw), -3.f * dt, 3.f * dt);
            c.speed += (11.f - c.speed) * dt * 2.f;
            c.fear -= dt * 0.05f;
        } else {
            c.speed += (0.f - c.speed) * dt * 2.f;
            if (fmodf(c.t, 9.f) < 0.02f) c.yaw += frand() - 0.5f;
        }
        c.x += cosf(c.yaw) * c.speed * dt; c.z += sinf(c.yaw) * c.speed * dt;
        c.y = terrainHeight(c.x, c.z);
        c.pose = c.speed > 1.f ? (uint8_t)(1 + ((int)(c.t * c.speed * 0.9f) & 1)) : (fmodf(c.t + c.x, 6.f) < 3.5f ? 3 : 0);
    }
    // a skein of birds now and then (daylight)
    s_birdNext -= dt;
    if (s_bird.on) { s_bird.az += s_bird.vaz * dt; s_bird.t += dt; if (s_bird.t > 40.f) s_bird.on = false; }
    else if (s_birdNext <= 0.f && g_scene.night < 0.4f && g_scene.rain < 0.3f) {
        s_birdNext = 40.f + frand() * 80.f;
        s_bird.on = true; s_bird.az = car.yaw - 0.9f + frand() * 0.4f; s_bird.vaz = 0.03f + frand() * 0.03f;
        s_bird.el = 0.12f + frand() * 0.2f; s_bird.t = 0.f; s_bird.n = 5 + (int)(frand() * 6.f);
    }
}

FieldHit fieldCollide(AircraftState& car, uint8_t id) {
    FieldHit out = {0.f, HIT_NONE};
    if (car.crash || car.agl > 45.f) return out;                       // above the tallest redwood: nothing to hit
    const Grid& g = terrainNear();
    const float reach = aircraftSpec(id).span * 0.5f + 1.f;
    const int R = (int)(reach * g.inv) + 1;
    int ci = (int)floorf(car.x * g.inv), cj = (int)floorf(car.z * g.inv);
    for (int j = cj - R; j <= cj + R; j++) for (int i = ci - R; i <= ci + R; i++) {
        Veg v;
        if (!vegAt(i, j, v) || v.radius <= 0.f) continue;
        float h = aircraftHitPost(car, id, v.x, v.z, v.radius, v.y, v.y + v.size);
        if (h > out.strength) { out.strength = h; out.kind = v.type == VEG_BOULDER ? HIT_ROCK : HIT_TREE; }
    }
    Airfield a;                                                          // the hangar
    if (airfieldAround(car.x, car.z, a)) {
        float h = aircraftHitPost(car, id, a.hx, a.hz, 15.f, a.y, a.y + 13.f);
        if (h > out.strength) { out.strength = h; out.kind = HIT_STORE; }
    }
    pastureCollide(car, id, out);
    for (Critter& c : s_c) {                                             // a deer that didn't get out of the way
        if (!c.on || c.kind == 1 || car.agl > 3.f) continue;
        float dx = c.x - car.x, dz = c.z - car.z;
        if (dx * dx + dz * dz > 2.5f * 2.5f || car.speed < 3.f) continue;
        c.on = false;
        for (int k = 0; k < 14; k++)
            fieldEmit(PK_FUR, c.x + (frand() - 0.5f), c.y + 1.f + frand() * 0.6f, c.z + (frand() - 0.5f), car.vx * 0.8f + (frand() - 0.5f) * 5.f, 2.5f + frand() * 3.5f, car.vz * 0.8f + (frand() - 0.5f) * 5.f, 0.08f, 1.2f + frand() * 0.8f);
        out.strength = fmaxf(out.strength, 1.f); out.kind = HIT_DEER;
    }
    return out;
}

}
