// Gigs (see gigs.h). Per frame and per board: compiled for size.
#pragma GCC optimize("Os")
#include "gigs.h"
#include "progress.h"
#include "terrain.h"
#include "combat.h"
#include "universe.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace mantis {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ======================= THE JOBS =======================
// kind: who it's for (lowest rank, which aircraft), base pay and reputation, pay per mile
struct KindInfo { const char* name; uint8_t rank, mask; int pay, xp; float perMile; };
#define AM(a) (1u << (a))
static const uint8_t CIVIL = AM(A_CUB) | AM(A_BEAVER) | AM(A_JET);
static const KindInfo KIND[G_N] = {
    {"FERRY FLIGHT",      R_AMATEUR,    CIVIL, 90, 35, 22.f},
    {"AERIAL PHOTOS",     R_AMATEUR,    CIVIL | AM(A_FIGHTER), 120, 45, 15.f},
    {"SIGHTSEEING",       R_AMATEUR,    AM(A_CUB) | AM(A_BEAVER), 110, 45, 18.f},
    {"MAIL RUN",          R_AMATEUR,    CIVIL, 160, 60, 25.f},
    {"MEDEVAC",           R_FREELANCER, CIVIL, 500, 160, 30.f},
    {"BACKCOUNTRY RUN",   R_AMATEUR,    AM(A_BEAVER), 180, 65, 25.f},
    {"FISHING CHARTER",   R_AMATEUR,    AM(A_BEAVER), 220, 80, 28.f},
    {"SUPPLY DROP",       R_FREELANCER, AM(A_BEAVER), 260, 95, 20.f},
    {"FIRE FIGHTING",     R_CONTRACTOR, AM(A_BEAVER), 650, 220, 20.f},
    {"SEARCH & RESCUE",   R_CONTRACTOR, AM(A_CUB) | AM(A_BEAVER), 480, 170, 20.f},
    {"AERIAL SURVEY",     R_CONTRACTOR, CIVIL, 380, 140, 20.f},
    {"PASSENGER CHARTER", R_AMATEUR,    AM(A_JET), 900, 260, 40.f},
    {"AIR CARGO",         R_AMATEUR,    AM(A_JET), 700, 210, 32.f},
    {"AIR RACE",          R_AMATEUR,    AM(A_FIGHTER), 600, 180, 0.f},
    {"DOGFIGHT",          R_AMATEUR,    AM(A_FIGHTER), 1400, 380, 10.f},
    {"BOMBING RUN",       R_AMATEUR,    AM(A_FIGHTER), 1300, 340, 10.f},
    {"MISSILE STRIKE",    R_AMATEUR,    AM(A_FIGHTER), 1500, 400, 10.f},
    {"RECON PHOTOS",      R_AMATEUR,    AM(A_FIGHTER), 1000, 300, 10.f},
};
static bool militaryKind(uint8_t k) { return k == G_INTERCEPT || k == G_BOMBING || k == G_STRIKE || k == G_RECON; }
uint8_t gigGiver(uint8_t k) {
    if (militaryKind(k) || k == G_RACE) return 3;
    if (k == G_CHARTER || k == G_CARGO) return 2;
    if (k >= G_BACKCOUNTRY && k <= G_SURVEY) return 1;
    return 0;
}
static const char* const CLIENT[16] = {"MANTIS POST", "RIVER LODGE", "DR. HALE", "THE SURVEY BUREAU", "BLUE HERON TOURS",
    "FOREST SERVICE", "COAST RESCUE", "NORTHERN CARGO", "MAPLE AIRWAYS", "THE GAZETTE", "FALCON HOLIDAYS", "MOSS & SONS",
    "STATE PATROL", "AIR GUARD", "OTTER OUTFITTERS", "SKYLINE CHARTERS"};
// =========================================================
const char* gigKindName(uint8_t k) { return KIND[k % G_N].name; }

static Gig s_gig; static bool s_on = false; static int s_step = 0;
static float s_start = 0.f, s_comfort = 1.f, s_tank = 0.f, s_hold = 0.f, s_lastSide = 0.f, s_retarget = 0.f;
static int s_fireHits = 0, s_crates = 0;
int gigCrates() { return s_on && s_step < s_gig.nwp && s_gig.wp[s_step].kind == W_SUPPLY ? s_crates : 0; }

// ---- places ----
static uint32_t s_rng = 1;
static float rnd() { s_rng = hash32(s_rng + 0x9E3779B9u); return (s_rng >> 8) * (1.f / 16777216.f); }
// an airfield between minR and maxR squares away (tarmac only if asked), never the one you're at
static bool pickAirfield(const Airfield& from, int minR, int maxR, bool tarmac, Airfield& out) {
    for (int R = maxR; R <= maxR + 4; R++) {
        for (int tries = 0; tries < 40; tries++) {
            const int di = (int)lroundf((rnd() * 2.f - 1.f) * R), dj = (int)lroundf((rnd() * 2.f - 1.f) * R);
            const int ring = abs(di) > abs(dj) ? abs(di) : abs(dj);
            if (ring < minR || ring > R) continue;
            if (!airfieldAt(from.i + di, from.j + dj, out)) continue;
            if (tarmac && out.surf != SURF_TARMAC) continue;
            airfieldLevel(out);
            return true;
        }
    }
    return false;
}
static bool isWater(float x, float z) {
    return terrainNatural(x, z) < terrainWaterLevel(x, z) - 1.0f || terrainRiverDist(x, z) < 6.f;
}
static bool findWater(float cx, float cz, float r0, float r1, float& x, float& z) {
    for (int tries = 0; tries < 80; tries++) {
        const float a = rnd() * 6.2831853f, d = r0 + (r1 - r0) * rnd();
        x = cx + cosf(a) * d; z = cz + sinf(a) * d;
        if (!isWater(x, z)) continue;
        if (!isWater(x + 60.f, z) || !isWater(x - 60.f, z) || !isWater(x, z + 60.f) || !isWater(x, z - 60.f)) continue;   // room to land
        return true;
    }
    return false;
}
static void findLand(float cx, float cz, float r0, float r1, float& x, float& z) {
    for (int tries = 0; tries < 30; tries++) {
        const float a = rnd() * 6.2831853f, d = r0 + (r1 - r0) * rnd();
        x = cx + cosf(a) * d; z = cz + sinf(a) * d;
        if (!isWater(x, z)) return;
    }
}
static float dist(float ax, float az, float bx, float bz) { return sqrtf((ax - bx) * (ax - bx) + (az - bz) * (az - bz)); }

static Waypoint& addWp(Gig& g, uint8_t kind, float x, float z, float r, const char* label) {
    Waypoint& w = g.wp[g.nwp < GIG_WP ? g.nwp++ : GIG_WP - 1];
    memset(&w, 0, sizeof w);
    w.kind = kind; w.x = x; w.z = z; w.r = r; w.agl0 = -1e9f; w.agl1 = 1e9f;
    snprintf(w.label, sizeof w.label, "%s", label);
    return w;
}
static void landAt(Gig& g, const Airfield& a) {
    Waypoint& w = addWp(g, W_LAND_AF, a.x, a.z, a.len * 0.5f + 200.f, a.name);
    w.ai = (int16_t)a.i; w.aj = (int16_t)a.j;
}

static bool build(uint8_t kind, const Airfield& from, uint8_t rank, Gig& g) {
    memset(&g, 0, sizeof g);
    const KindInfo& K = KIND[kind];
    g.kind = kind; g.minRank = K.rank; g.aircraftMask = K.mask;
    snprintf(g.title, sizeof g.title, "%s", K.name);
    snprintf(g.client, sizeof g.client, "%s", CLIENT[(unsigned)(rnd() * 16.f) & 15u]);
    float px = from.x, pz = from.z, trip = 0.f;
    auto leg = [&](float x, float z) { trip += dist(px, pz, x, z); px = x; pz = z; };
    Airfield a;
    switch (kind) {
        case G_FERRY: case G_MAIL: case G_CARGO: case G_CHARTER: case G_MEDEVAC: {
            const int minR = kind == G_CHARTER ? 2 : 1, maxR = kind == G_CHARTER ? 3 : (kind == G_CARGO ? 2 : 1);
            if (kind == G_MAIL) {                                 // two stops
                Airfield b;
                if (!pickAirfield(from, 1, 1, false, a)) return false;
                if (!pickAirfield(a, 1, 1, false, b) || (b.i == from.i && b.j == from.j)) return false;
                landAt(g, a); leg(a.x, a.z); landAt(g, b); leg(b.x, b.z);
                snprintf(g.dest, sizeof g.dest, "%s", b.name);
            } else {
                if (!pickAirfield(from, minR, maxR, kind == G_CHARTER, a)) return false;
                landAt(g, a); leg(a.x, a.z);
                snprintf(g.dest, sizeof g.dest, "%s", a.name);
            }
            if (kind == G_MEDEVAC) g.limit = 60.f + trip / 25.f;   // about the Cub's cruise, with a little in hand
            break;
        }
        case G_SIGHTSEE: {                                      // two sights (a lake, the high ground), then home
            float x, z;
            if (findWater(from.x, from.z, 2000.f, 5000.f, x, z)) { Waypoint& w = addWp(g, W_FLY, x, z, 400.f, "THE LAKE"); w.agl1 = 400.f; leg(x, z); }
            findLand(px, pz, 2000.f, 4000.f, x, z);
            Waypoint& w = addWp(g, W_FLY, x, z, 450.f, "THE VIEWPOINT"); w.agl1 = 500.f; leg(x, z);
            landAt(g, from); leg(from.x, from.z);
            snprintf(g.dest, sizeof g.dest, "%s", from.name);
            break;
        }
        case G_PHOTO: case G_RECON: {                           // a landmark: a lake, a peak, a farm, a camp
            float x, z;
            if (!findWater(from.x, from.z, 2500.f, 7000.f, x, z)) findLand(from.x, from.z, 2500.f, 7000.f, x, z);
            static const char* const WHAT[4] = {"THE LANDMARK", "THE OLD MILL", "THE DAM SITE", "THE RIDGE"};
            Waypoint& w = addWp(g, kind == G_RECON ? W_RECON : W_PHOTO, x, z, kind == G_RECON ? 260.f : 2500.f, kind == G_RECON ? "THE TARGET" : WHAT[(int)(rnd() * 3.99f)]);
            w.agl1 = kind == G_RECON ? 1e9f : 1e9f; leg(x, z);
            landAt(g, from); leg(from.x, from.z);              // the film comes home (the recon's to its air base)
            snprintf(g.dest, sizeof g.dest, "%s", w.label);
            break;
        }
        case G_SUPPLY: {                                        // three crates into a camp's zone (no landing)
            Camp c; bool got = false;
            for (int tries = 0; tries < 20 && !got; tries++) got = campAt(from.i + (int)lroundf((rnd() * 2.f - 1.f) * 1.5f), from.j + (int)lroundf((rnd() * 2.f - 1.f) * 1.5f), c);
            float x = c.x, z = c.z;
            if (!got) findLand(from.x, from.z, 2500.f, 6000.f, x, z);
            Waypoint& w = addWp(g, W_SUPPLY, x, z, 60.f, got ? c.name : "THE DROP ZONE"); w.hold = 3.f; leg(x, z);
            snprintf(g.dest, sizeof g.dest, "%s", w.label);
            break;
        }
        case G_BOMBING: case G_STRIKE: {
            float x, z; findLand(from.x, from.z, 5000.f, 9000.f, x, z);
            Waypoint& w = addWp(g, W_BOMB, x, z, 5000.f, kind == G_BOMBING ? "THE DEPOT" : "THE RADAR SITE");
            w.hold = (float)(kind == G_BOMBING ? 5 : 3); leg(x, z);
            landAt(g, from); leg(from.x, from.z);
            snprintf(g.dest, sizeof g.dest, "%s", w.label);
            break;
        }
        case G_BACKCOUNTRY: {
            Camp c; bool got = false;
            for (int tries = 0; tries < 20 && !got; tries++) got = campAt(from.i + (int)lroundf((rnd() * 2.f - 1.f) * 1.5f), from.j + (int)lroundf((rnd() * 2.f - 1.f) * 1.5f), c);
            if (!got) return false;
            Waypoint& w = addWp(g, W_LAND_FIELD, c.x, c.z, 180.f, c.name); w.yaw = c.yaw; leg(c.x, c.z);
            snprintf(g.dest, sizeof g.dest, "%s", c.name);
            break;
        }
        case G_FISHING: {
            float x, z;
            if (!findWater(from.x, from.z, 2500.f, 9000.f, x, z)) return false;
            addWp(g, W_LAND_WATER, x, z, 600.f, "THE LAKE"); leg(x, z);
            snprintf(g.dest, sizeof g.dest, "A LAKE");
            break;
        }
        case G_SURVEY: {                                        // a line of photo points at survey height
            float x, z; findLand(from.x, from.z, 3000.f, 5000.f, x, z);
            const float yaw = rnd() * 6.2831853f;
            for (int k = 0; k < 4; k++) {
                char l[18]; snprintf(l, sizeof l, "PHOTO POINT %d", k + 1);
                Waypoint& w = addWp(g, W_FLY, x + cosf(yaw) * k * 1500.f, z + sinf(yaw) * k * 1500.f, 300.f, l);
                w.agl0 = 200.f; w.agl1 = 700.f; leg(w.x, w.z);
            }
            snprintf(g.dest, sizeof g.dest, "SURVEY LINE");
            break;
        }
        case G_FIRE: {                                          // scoop, drop, scoop, drop, home
            float wx, wz, fx, fz;
            if (!findWater(from.x, from.z, 1500.f, 6000.f, wx, wz)) return false;
            findLand(wx, wz, 1500.f, 3500.f, fx, fz);
            for (int k = 0; k < 2; k++) {
                addWp(g, W_SCOOP, wx, wz, 700.f, "SCOOP AT THE LAKE"); leg(wx, wz);
                addWp(g, W_DROP, fx, fz, 160.f, "THE FIRE"); leg(fx, fz);
            }
            landAt(g, from); leg(from.x, from.z);
            snprintf(g.dest, sizeof g.dest, "THE FIRE");
            break;
        }
        case G_SAR: {
            float x, z; findLand(from.x, from.z, 3500.f, 7000.f, x, z);
            Waypoint& w = addWp(g, W_ORBIT, x, z, 450.f, "SEARCH AREA"); w.agl1 = 350.f; w.hold = 12.f; leg(x, z);
            landAt(g, from); leg(from.x, from.z);
            snprintf(g.dest, sizeof g.dest, "SEARCH AREA");
            break;
        }
        case G_RACE: {                                          // eight gates, low and fast, a winding course
            float yaw = rnd() * 6.2831853f, x = from.x + cosf(yaw) * 1500.f, z = from.z + sinf(yaw) * 1500.f;
            for (int k = 0; k < 8; k++) {
                char l[18]; snprintf(l, sizeof l, "GATE %d OF 8", k + 1);
                Waypoint& w = addWp(g, W_GATE, x, z, 22.f, l);
                w.yaw = yaw; w.y = terrainNatural(x, z) + 70.f + rnd() * 80.f;
                leg(x, z);
                yaw += (rnd() - 0.5f) * 1.6f;
                const float d = 1300.f + rnd() * 900.f;
                x += cosf(yaw) * d; z += sinf(yaw) * d;
            }
            g.limit = 150.f;
            snprintf(g.dest, sizeof g.dest, "THE COURSE");
            break;
        }
        case G_INTERCEPT: {
            float x, z; findLand(from.x, from.z, 6000.f, 10000.f, x, z);
            Waypoint& w = addWp(g, W_KILL, x, z, 5000.f, "BOGIES"); w.hold = (float)(2 + (rank >= R_FIGHTER ? 2 : 0) + (int)(rnd() * 2.f)); leg(x, z);
            landAt(g, from); leg(from.x, from.z);              // and back to base
            snprintf(g.dest, sizeof g.dest, "BOGIES");
            break;
        }
    }
    if (g.nwp == 0) return false;
    g.distMi = trip / MILE;
    g.pay = K.pay + (int)(g.distMi * K.perMile);
    g.xp = K.xp + (int)(g.distMi * 3.f);
    return true;
}

int gigBoard(const Airfield& a, uint8_t rank, uint8_t aircraft, uint32_t salt, Gig* out) {
    s_rng = hash32((uint32_t)a.i * 0x9E3779B1u ^ (uint32_t)a.j * 0x85EBCA77u ^ salt * 0x27D4EB2Fu ^ worldSeed());
    uint8_t cand[G_N]; int nc = 0;
    for (int k = 0; k < G_N; k++) {
        if (KIND[k].rank > rank || !(KIND[k].mask & (1u << aircraft))) continue;
        if (militaryKind((uint8_t)k) != a.military) continue;      // missions at the air bases; civil work at the rest
        cand[nc++] = (uint8_t)k;
    }
    for (int k = nc - 1; k > 0; k--) { int j = (int)(rnd() * (k + 1)); uint8_t t = cand[k]; cand[k] = cand[j]; cand[j] = t; }
    // the newest kinds for your rank come first: there's always something new on the board after a promotion
    for (int k = 0; k < nc; k++) if (KIND[cand[k]].rank == rank && k > 0) { uint8_t t = cand[0]; cand[0] = cand[k]; cand[k] = t; break; }
    int n = 0;
    for (int k = 0; k < nc && n < 3; k++) if (build(cand[k], a, rank, out[n])) n++;
    if (n < 3 && nc > 0) for (int k = 0; k < nc && n < 3; k++) if (build(cand[k], a, rank, out[n])) n++;   // the same kind again elsewhere
    return n;
}

void gigAccept(const Gig& g, float time) {
    s_crates = 0; for (int k = 0; k < g.nwp; k++) if (g.wp[k].kind == W_SUPPLY) s_crates = (int)g.wp[k].hold;
    s_gig = g; s_on = true; s_step = 0; s_start = time; s_comfort = 1.f; s_tank = 0.f; s_hold = 0.f; s_lastSide = 0.f; s_fireHits = 0;
    combatClear();
}
void gigCancel() { s_on = false; combatClear(); }
bool gigActive() { return s_on; }
const Gig& gigCurrent() { return s_gig; }
int gigStep() { return s_step; }
const Waypoint* gigWaypoint() { return s_on && s_step < s_gig.nwp ? &s_gig.wp[s_step] : nullptr; }
float gigTimeLeft(float time) { return s_on && s_gig.limit > 0.f ? s_gig.limit - (time - s_start) : -1.f; }
float gigComfort() { return s_comfort; }
float gigTank() { return s_tank; }
int gigFires(float* xyz, int max) {
    if (!s_on || s_gig.kind != G_FIRE || max < 1) return 0;
    for (int k = s_step; k < s_gig.nwp; k++) if (s_gig.wp[k].kind == W_DROP) {
        xyz[0] = s_gig.wp[k].x; xyz[2] = s_gig.wp[k].z; xyz[1] = terrainHeight(xyz[0], xyz[2]);
        return 1;
    }
    return 0;
}

static GigEvent ev(uint8_t kind, const char* m, const char* sub = "") {
    GigEvent e; memset(&e, 0, sizeof e); e.kind = kind;
    snprintf(e.msg, sizeof e.msg, "%s", m); snprintf(e.sub, sizeof e.sub, "%s", sub);
    return e;
}

GigEvent gigUpdate(const AircraftState& s, uint8_t aircraft, float time, float dt, const GigInput& in) {
    const bool drop = in.waterDrop, cleared = in.cleared;
    if (!s_on) return ev(GE_NONE, "");
    if (s.crash) { s_on = false; combatClear(); return ev(GE_FAILED, "GIG FAILED", s_gig.title); }
    if (s_gig.limit > 0.f && time - s_start > s_gig.limit) { s_on = false; combatClear(); return ev(GE_FAILED, "OUT OF TIME", s_gig.title); }
    if (s_gig.kind == G_CHARTER && !s.onGround) {             // the passengers feel every bump and every steep turn
        const float rough = fmaxf(0.f, fabsf(s.gLoad - 1.f) - 0.35f) * 0.25f + fmaxf(0.f, fabsf(s.roll) - 0.55f) * 0.2f + s.buffet * 0.3f;
        s_comfort = clampf(s_comfort - rough * dt, 0.f, 1.f);
    }
    Waypoint& w = s_gig.wp[s_step];
    const float d = dist(s.x, s.z, w.x, w.z);
    bool hit = false;
    GigEvent info = ev(GE_NONE, "");
    switch (w.kind) {
        case W_FLY: hit = d < w.r && s.agl > w.agl0 && s.agl < w.agl1 && !s.onGround; break;
        case W_LAND_AF: hit = cleared && s.onGround && !s.onWater && s.groundSpeed < 2.f && d < w.r && (s.mat == MAT_RUNWAY || s.mat == MAT_LOT || d < w.r * 0.7f); break;
        case W_LAND_FIELD: hit = s.onGround && !s.onWater && s.groundSpeed < 2.f && d < w.r; break;
        case W_LAND_WATER:
            if (d < 2500.f && (s_retarget -= dt) <= 0.f) {      // the lake as it is now (the land dreams on): keep the target on water
                s_retarget = 2.f;
                Ground g; terrainGround(w.x, w.z, g);
                if (g.mat != MAT_WATER) {
                    const Grid& v = terrainVast();
                    float best = 1e30f;
                    for (int j = -30; j <= 30; j++) for (int i = -30; i <= 30; i++) {
                        const int ci = (int)floorf(w.x * v.inv) + i, cj = (int)floorf(w.z * v.inv) + j;
                        if (ci < v.ox || cj < v.oz || ci >= v.ox + v.N || cj >= v.oz + v.N) continue;
                        if ((v.c[gridWrap(cj, v.N) * v.N + gridWrap(ci, v.N)].mat & 15) != MAT_WATER) continue;
                        const float dd = (float)(i * i + j * j);
                        if (dd < best) { best = dd; w.x = (ci + 0.5f) * v.cell; w.z = (cj + 0.5f) * v.cell; }
                    }
                }
            }
            hit = s.onWater && s.groundSpeed < 4.f && d < w.r;
            break;
        case W_SCOOP:
            if (s.onWater && s.groundSpeed > 14.f && d < w.r) {
                const float was = s_tank;
                s_tank = clampf(s_tank + dt * 0.35f * (1.f + 0.25f * progressUpgradeLevel(UPG_LIFT)), 0.f, 1.f);
                if (was < 1.f && s_tank >= 1.f) info = ev(GE_INFO, "TANK FULL", "ON TO THE FIRE");
            }
            hit = s_tank >= 1.f && !s.onGround;
            break;
        case W_DROP:
            if (drop && s_tank > 0.3f) {
                const bool on = d < w.r + 40.f && s.agl < 220.f;
                s_tank = 0.f;
                if (on) { hit = true; s_fireHits++; }
                else { s_step = s_step > 0 ? s_step - 1 : 0; return ev(GE_INFO, "MISSED THE FIRE", "SCOOP AGAIN"); }
            }
            break;
        case W_ORBIT:
            if (d < w.r && s.agl < w.agl1 && !s.onGround) { s_hold += dt; if (s_hold >= w.hold) hit = true; }
            else s_hold = fmaxf(0.f, s_hold - dt * 0.5f);
            break;
        case W_GATE: {
            const float nx = cosf(w.yaw), nz = sinf(w.yaw);
            const float side = (s.x - w.x) * nx + (s.z - w.z) * nz;
            if (s_lastSide < 0.f && side >= 0.f) {             // crossed its plane: through the ring?
                const float lat = -(s.x - w.x) * nz + (s.z - w.z) * nx, up = s.y - w.y;
                if (lat * lat + up * up < w.r * w.r * 1.15f) hit = true;
                else info = ev(GE_INFO, "MISSED THE GATE", "COME ROUND AGAIN");
            }
            s_lastSide = side;
            break;
        }
        case W_PHOTO:
            if (in.photo) {
                const float dc = dist(in.photoX, in.photoZ, w.x, w.z);
                if (in.framed && dc < w.r) hit = true;
                else info = ev(GE_INFO, in.framed ? "TOO FAR AWAY" : "NOT IN THE PICTURE", "GET CLOSER, FRAME IT");
            }
            break;
        case W_RECON:
            if (in.recon) { if (in.framed) hit = true; else info = ev(GE_INFO, d < 4000.f ? "NOT IN THE SIGHT" : "NOTHING BELOW", "PUT THE TARGET IN THE PICTURE"); }
            break;
        case W_SUPPLY:
            if (in.crate) {
                const float dc = dist(in.crateX, in.crateZ, w.x, w.z);
                char m[32]; snprintf(m, sizeof m, "%d M FROM THE MARK", (int)(dc + 0.5f));
                const int bonus = dc < 12.f ? 120 : (dc < 30.f ? 60 : (dc < w.r ? 25 : 0));
                s_gig.pay += bonus;
                info = ev(GE_INFO, dc < 12.f ? "BULLSEYE!" : (dc < 30.f ? "RIGHT ON" : (dc < w.r ? "IN THE ZONE" : "MISSED THE ZONE")), m);
                if (--s_crates <= 0) hit = true;
            }
            break;
        case W_BOMB:
            if (d < w.r && !combatEngaged()) combatSpawnGround(w.x, w.z, (int)w.hold);
            hit = combatEngaged() && in.groundAlive == 0 && combatGroundAlive() == 0;
            break;
        case W_KILL:
            if (d < w.r && !combatEngaged()) combatSpawn(w.x, w.z, (int)w.hold, s);
            hit = combatEngaged() && combatAlive() == 0;
            break;
    }
    if (!hit) return info;
    if (w.kind == W_BOMB || w.kind == W_KILL) combatClear();   // the fight's over: the next one starts fresh
    s_hold = 0.f; s_lastSide = -1.f;
    s_step++;
    if (s_step < s_gig.nwp) {
        if (s_gig.wp[s_step].kind == W_GATE) {                  // the next gate: which side of it you're on now
            const Waypoint& n = s_gig.wp[s_step];
            s_lastSide = (s.x - n.x) * cosf(n.yaw) + (s.z - n.z) * sinf(n.yaw);
        }
        const char* m = w.kind == W_DROP ? "WATER ON THE FIRE!" : (w.kind == W_ORBIT ? "SURVIVOR SPOTTED" : (w.kind == W_GATE ? "GATE!" : "CHECKPOINT"));
        return ev(GE_STEP, m, s_gig.wp[s_step].label);
    }
    // done: pay, with the job's own bonuses
    s_on = false;
    GigEvent e = ev(GE_DONE, "GIG COMPLETE", s_gig.title);
    e.pay = s_gig.pay; e.xp = s_gig.xp;
    if (s_gig.kind == G_CHARTER) e.pay = (int)(e.pay * (0.5f + 0.5f * s_comfort));
    if (s_gig.limit > 0.f) { const float left = s_gig.limit - (time - s_start); e.pay += (int)(fmaxf(0.f, left) * 4.f); }
    if (s_gig.kind == G_INTERCEPT) e.xp += 40 * (int)s_gig.wp[0].hold;
    (void)aircraft;
    return e;
}

}
