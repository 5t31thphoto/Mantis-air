// The flight model (see aircraft.h). Per frame, a step or two: compiled for size.
#pragma GCC optimize("Os")
#include "aircraft.h"
#include "terrain.h"
#include "journey.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {

// ======================= THE HANGAR =======================
// name, short, blurb, hint,
// gear, retract, mass kg, wing m2, span m,
// clAlpha /rad, cl0, clMax, alphaStall (rad, info), cd0, k, flapCL, flapCD,
// static thrust N, vProp m/s (0 jet),
// roll rad/s, pitch rad/s, level assist,
// vStall, vCruise, vNe (m/s),
// gearH m, ground pitch rad, rough field,
// eye up, eye fwd, chase back,
// engine, rpm idle, rpm max, blades,
// paid with, miles, bars speed / agility / rough, paint body / trim / accent
static const AircraftSpec AIRCRAFT[A_COUNT] = {
    {"MANTIS CUB", "CUB", "A TAILDRAGGER TRAINER", "FIRST JOBS: FERRY, MAIL, PHOTOS",
     GEAR_TAIL, false, 560.f, 16.6f, 10.7f,
     4.8f, 0.35f, 1.5f, 0.262f, 0.040f, 0.060f, 0.5f, 0.05f,
     1700.f, 60.f,
     1.0f, 0.6f, 0.5f,
     19.f, 33.f, 50.f,
     1.25f, 0.19f, 0.6f,
     1.55f, 0.3f, 17.0f,
     ENG_BOXER4T, 650.f, 2300.f, 2,
     A_CUB, 0.f, 0.15f, 0.40f, 1.0f, {{250, 196, 40}, {30, 30, 34}, {200, 40, 30}}},
    {"BUSH BEAVER", "BEAVER", "AN AMPHIBIOUS BUSH PLANE", "LAKES, CAMPS, SUPPLY DROPS, FIRES",
     GEAR_FLOATS, false, 1900.f, 23.2f, 14.6f,
     5.0f, 0.35f, 1.6f, 0.262f, 0.055f, 0.050f, 0.6f, 0.06f,
     7000.f, 120.f,
     0.8f, 0.45f, 0.35f,
     27.f, 52.f, 75.f,
     1.9f, 0.04f, 1.0f,
     1.5f, 0.7f, 22.f,
     ENG_RADIAL, 550.f, 2200.f, 3,
     A_CUB, 30.f, 0.45f, 0.35f, 1.0f, {{226, 64, 44}, {240, 234, 216}, {40, 40, 46}}},
    {"MANTIS JET", "JET", "A REGIONAL AIRLINER", "PASSENGERS, CARGO, A SMART AUTOPILOT",
     GEAR_TRI, true, 3000.f, 17.0f, 12.0f,
     5.0f, 0.2f, 1.4f, 0.244f, 0.022f, 0.050f, 0.6f, 0.04f,
     6500.f, 0.f,
     1.6f, 0.6f, 0.3f,
     40.f, 120.f, 150.f,
     1.4f, 0.0f, 0.0f,
     1.35f, 1.6f, 22.f,
     ENG_TURBINE, 0.f, 1.f, 0,
     A_BEAVER, 60.f, 0.8f, 0.55f, 0.0f, {{236, 238, 242}, {36, 48, 72}, {220, 170, 60}}},
    {"STINGRAY FIGHTER", "FIGHTER", "A TWIN-TAIL STRIKE FIGHTER", "AIR BASES, MISSIONS, RACES",
     GEAR_TRI, true, 9000.f, 30.0f, 10.0f,
     4.5f, 0.1f, 1.5f, 0.4f, 0.022f, 0.12f, 0.3f, 0.03f,
     75000.f, 0.f,
     4.0f, 1.2f, 0.0f,
     56.f, 150.f, 210.f,
     1.6f, 0.0f, 0.0f,
     1.0f, 4.0f, 19.f,
     ENG_TURBINE, 0.f, 1.f, 0,
     A_JET, 120.f, 1.0f, 1.0f, 0.0f, {{124, 132, 146}, {62, 68, 82}, {230, 170, 40}}},
};
// ==========================================================

const AircraftSpec& aircraftSpec(uint8_t id) { return AIRCRAFT[id % A_COUNT]; }

static float s_handK[A_COUNT] = {1.f, 1.f, 1.f, 1.f}, s_liftK[A_COUNT] = {1.f, 1.f, 1.f, 1.f}, s_powK[A_COUNT] = {1.f, 1.f, 1.f, 1.f};
static bool s_bush = false;
void aircraftSetUpgrades(const uint8_t* lv) {
    for (int i = 0; i < A_COUNT; i++) {
        uint8_t u = lv ? lv[i] : 0;
        s_handK[i] = 1.f + 0.15f * (u & 3);
        s_liftK[i] = 1.f + 0.06f * ((u >> 2) & 3);
        s_powK[i] = 1.f + 0.10f * ((u >> 4) & 3);
    }
}
void aircraftSetBushMode(bool on) { s_bush = on; }

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float sq(float v) { return v * v; }
static inline float smooth01(float t) { t = clampf(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }

static void basisFromEuler(AircraftState& s) {
    const float cy = cosf(s.yaw), sy = sinf(s.yaw), cp = cosf(s.pitch), sp = sinf(s.pitch), cr = cosf(s.roll), sr = sinf(s.roll);
    s.fx = cy * cp; s.fy = sp; s.fz = sy * cp;
    const float rx0 = -sy, ry0 = 0.f, rz0 = cy;
    const float ux0 = -cy * sp, uy0 = cp, uz0 = -sy * sp;
    s.rx = rx0 * cr - ux0 * sr; s.ry = ry0 * cr - uy0 * sr; s.rz = rz0 * cr - uz0 * sr;   // right wing down: roll +
    s.ux = ux0 * cr + rx0 * sr; s.uy = uy0 * cr + ry0 * sr; s.uz = uz0 * cr + rz0 * sr;
}
static void eulerFromBasis(AircraftState& s) {
    s.yaw = atan2f(s.fz, s.fx);
    s.pitch = asinf(clampf(s.fy, -1.f, 1.f));
    s.roll = atan2f(-s.ry, s.uy);
}
static inline void norm3(float& x, float& y, float& z) { float l = sqrtf(x * x + y * y + z * z); if (l > 1e-9f) { x /= l; y /= l; z /= l; } }
// small rotations about the body axes, then put the frame square again
static void rotateBody(AircraftState& s, float dRoll, float dPitch, float dYaw) {
    float c, n, ax, ay, az, bx, by, bz;
    c = cosf(dPitch); n = sinf(dPitch);                       // about right: nose up
    ax = s.fx * c + s.ux * n; ay = s.fy * c + s.uy * n; az = s.fz * c + s.uz * n;
    bx = s.ux * c - s.fx * n; by = s.uy * c - s.fy * n; bz = s.uz * c - s.fz * n;
    s.fx = ax; s.fy = ay; s.fz = az; s.ux = bx; s.uy = by; s.uz = bz;
    c = cosf(dYaw); n = sinf(dYaw);                           // about up: nose right
    ax = s.fx * c + s.rx * n; ay = s.fy * c + s.ry * n; az = s.fz * c + s.rz * n;
    bx = s.rx * c - s.fx * n; by = s.ry * c - s.fy * n; bz = s.rz * c - s.fz * n;
    s.fx = ax; s.fy = ay; s.fz = az; s.rx = bx; s.ry = by; s.rz = bz;
    c = cosf(dRoll); n = sinf(dRoll);                         // about forward: right wing down
    ax = s.rx * c - s.ux * n; ay = s.ry * c - s.uy * n; az = s.rz * c - s.uz * n;
    bx = s.ux * c + s.rx * n; by = s.uy * c + s.ry * n; bz = s.uz * c + s.rz * n;
    s.rx = ax; s.ry = ay; s.rz = az; s.ux = bx; s.uy = by; s.uz = bz;
    norm3(s.fx, s.fy, s.fz);
    s.rx = s.fy * s.uz - s.fz * s.uy; s.ry = s.fz * s.ux - s.fx * s.uz; s.rz = s.fx * s.uy - s.fy * s.ux;   // r = f x u
    norm3(s.rx, s.ry, s.rz);
    s.ux = s.ry * s.fz - s.rz * s.fy; s.uy = s.rz * s.fx - s.rx * s.fz; s.uz = s.rx * s.fy - s.ry * s.fx;   // u = r x f
}

static void clearCommon(AircraftState& s) {
    float x = s.x, z = s.z;
    memset(&s, 0, sizeof(s));
    s.x = x; s.z = z;
    s.gearDown = true;
}
void aircraftReset(AircraftState& s, uint8_t id, float x, float z, float yaw) {
    const AircraftSpec& A = AIRCRAFT[id % A_COUNT];
    s.x = x; s.z = z; clearCommon(s);
    Ground g; terrainGround(x, z, g);
    s.surfY = g.mat == MAT_WATER ? g.h + g.water : g.h;
    s.y = s.surfY + A.gearH;
    s.yaw = yaw; s.pitch = A.groundPitch; s.roll = 0.f;
    s.onGround = true; s.onWater = g.mat == MAT_WATER; s.mat = g.mat; s.surf = g.surf;
    basisFromEuler(s);
    s.rpm = A.rpmIdle;
}
void aircraftResetAir(AircraftState& s, uint8_t id, float x, float z, float yaw, float alt) {
    const AircraftSpec& A = AIRCRAFT[id % A_COUNT];
    s.x = x; s.z = z; clearCommon(s);
    s.surfY = terrainHeight(x, z);
    s.y = s.surfY + alt;
    s.yaw = yaw; s.pitch = 0.02f; s.roll = 0.f;
    basisFromEuler(s);
    const float v = A.vCruise * 0.85f;
    s.vx = s.fx * v; s.vy = 0.f; s.vz = s.fz * v;
    s.power = 0.7f; s.gearDown = !A.retract;
    s.ias = v; s.agl = alt;
}

static float rollMu(uint8_t surf) {
    switch (surf) {
        case SURF_TARMAC: return 0.02f;
        case SURF_GRAVEL: return 0.04f;
        case SURF_DIRT: return 0.05f;
        case SURF_GRASS: return 0.06f;
        case SURF_SNOW: return 0.06f;
        case SURF_MUD: return 0.14f;
        case SURF_SAND: return 0.12f;
        default: return 0.08f;
    }
}

void aircraftStep(AircraftState& s, uint8_t id, const AircraftInput& in, float dt) {
    const AircraftSpec& A = AIRCRAFT[id % A_COUNT];
    if (s.crash) return;                                       // the game takes it from here
    const float G = 9.81f, m = A.mass;
    const float rho = 1.225f * expf(-fmaxf(0.f, s.y) * (1.f / 9000.f));
    // flaps and gear
    const float flapT = A.flapCL > 0.f ? (in.flaps > 3 ? 3 : in.flaps) * (1.f / 3.f) : 0.f;
    s.flap += clampf(flapT - s.flap, -dt * 0.35f, dt * 0.35f);
    if (A.retract) {                                           // automatic: down low and slow, up once climbing away
        if (s.onGround) s.gearDown = true;
        else if (s.agl < 70.f || s.ias < A.vStall * 1.12f) s.gearDown = true;
        else if (s.agl > 110.f && s.ias > A.vStall * 1.3f && s.vs > -1.f) s.gearDown = false;
    }
    // the engine
    const float thr = clampf(in.throttle, 0.f, 1.f);
    if (A.vProp > 0.f) s.power += (thr - s.power) * (1.f - expf(-dt / 0.35f));
    else s.power += (thr - s.power) * (1.f - expf(-dt / (thr > s.power ? 2.4f : 1.6f)));   // a jet spools

    // ---- the air over the wings ----
    const float wx = in.windX * (s.onGround ? 0.6f : 1.f), wz = in.windZ * (s.onGround ? 0.6f : 1.f), wy = s.onGround ? 0.f : in.gustY;
    const float ax = s.vx - wx, ay = s.vy - wy, az = s.vz - wz;
    const float V = sqrtf(ax * ax + ay * ay + az * az);
    const float vf = ax * s.fx + ay * s.fy + az * s.fz, vu = ax * s.ux + ay * s.uy + az * s.uz, vr = ax * s.rx + ay * s.ry + az * s.rz;
    const float alpha = V > 0.5f ? atan2f(-vu, vf) : 0.f;
    const float beta = V > 0.5f ? atan2f(vr, fabsf(vf) + 0.5f) : 0.f;
    const float qbar = 0.5f * rho * V * V;
    const float clMax = (A.clMax + A.flapCL * s.flap) * s_liftK[id];
    const float cl0 = A.cl0 + A.flapCL * s.flap;
    const float aMax = (clMax - cl0) / A.clAlpha, aMin = (-0.85f * A.clMax - cl0) / A.clAlpha;
    float cl = cl0 + A.clAlpha * alpha, stallT = 0.f;
    const float plate = 0.95f * sinf(2.f * alpha);              // past the stall: a flat plate
    if (alpha > aMax) { stallT = smooth01((alpha - aMax) * (1.f / 0.16f)); cl = clMax + (plate - clMax) * stallT - 0.25f * sinf(stallT * 3.14159f); }
    else if (alpha < aMin) { stallT = smooth01((aMin - alpha) * (1.f / 0.16f)); const float c0 = cl0 + A.clAlpha * aMin; cl = c0 + (plate - c0) * stallT; }
    float cd = A.cd0 + A.kInduced * cl * cl + A.flapCD * s.flap + (A.retract && s.gearDown ? 0.016f : 0.f);
    cd += (1.25f * sq(sinf(alpha)) - A.kInduced * cl * cl) * stallT;   // separated flow: drag of a plate
    if (A.vProp <= 0.f && V > A.vNe * 0.9f) cd += 0.0025f * (V - A.vNe * 0.9f);   // drag rise near the limit
    const float S = A.wingArea;
    const float L = qbar * S * cl, D = qbar * S * cd, Y = -qbar * S * 0.9f * beta;
    // thrust
    float T;
    if (A.vProp > 0.f) T = A.thrust * s_powK[id] * s.power * fmaxf(0.f, 1.f - fmaxf(vf, 0.f) / A.vProp) * (rho * (1.f / 1.225f));
    else T = A.thrust * s_powK[id] * (0.05f + 0.95f * s.power) * powf(rho * (1.f / 1.225f), 0.7f);
    // directions: lift square to the airflow in the plane of symmetry, drag against it
    float lx = s.ux * vf - s.fx * vu, ly = s.uy * vf - s.fy * vu, lz = s.uz * vf - s.fz * vu;
    { float ll = sqrtf(lx * lx + ly * ly + lz * lz); if (ll > 1e-4f) { lx /= ll; ly /= ll; lz /= ll; } else { lx = s.ux; ly = s.uy; lz = s.uz; } }
    const float iv = V > 0.1f ? 1.f / V : 0.f;
    float Fx = lx * L - ax * iv * D + s.rx * Y + s.fx * T;
    float Fy = ly * L - ay * iv * D + s.ry * Y + s.fy * T - m * G;
    float Fz = lz * L - az * iv * D + s.rz * Y + s.fz * T;
    s.vx += Fx / m * dt; s.vy += Fy / m * dt; s.vz += Fz / m * dt;
    s.gLoad = L / (m * G);

    // ---- attitude: the yoke commands rates; a resting hand holds what you set ----
    const float qRef = 0.5f * 1.225f * A.vCruise * A.vCruise * 0.42f;
    const float auth = clampf(qbar / qRef, 0.f, 1.f);
    const float hk = s_handK[id];
    if (!s.onGround) {
        float pCmd = in.roll * A.rollRate * hk * auth;
        if (fabsf(in.roll) < 0.05f && fabsf(s.roll) < 1.0f) pCmd -= A.levelAssist * s.roll * auth;   // the trainer levels itself
        const float phi = clampf(s.roll, -1.2f, 1.2f), cth = cosf(s.pitch);
        const float Vt = fmaxf(V, A.vStall * 0.8f);
        const float w = G * tanf(phi) / Vt;                    // the turn's rate for this bank
        const float upright = fabsf(s.roll) < 1.45f ? 1.f : 0.f;
        float qCmd = in.pitch * A.pitchRate * hk * auth + w * sinf(phi) * cth * auth * upright;
        float rCmd = (w * cosf(phi) * cth * upright + beta * 2.5f) * auth;   // auto-rudder: no slip
        // the stall: the nose drops and a wing goes
        if (stallT > 0.f) {
            qCmd -= stallT * 1.6f * (alpha > 0.f ? 1.f : -1.f);
            pCmd += (vnoise2(s.x * 0.05f, s.z * 0.05f + s.y * 0.07f, 0x57A11u) - 0.5f) * 5.f * stallT;
        }
        // too slow to steer: the nose follows the airflow
        const float slow = 1.f - auth;
        if (V > 2.f) { qCmd -= clampf(alpha, -1.f, 1.f) * 1.8f * slow; rCmd += beta * 1.8f * slow; }
        else qCmd -= cosf(s.pitch) * 0.8f;                     // hanging on the prop: it falls through
        const float kp = 1.f - expf(-dt / 0.12f), kq = 1.f - expf(-dt / 0.2f), kr = 1.f - expf(-dt / 0.25f);
        s.p += (pCmd - s.p) * kp; s.q += (qCmd - s.q) * kq; s.r += (rCmd - s.r) * kr;
        rotateBody(s, s.p * dt, s.q * dt, s.r * dt);
        eulerFromBasis(s);
    }

    // ---- move, and meet the ground ----
    s.x += s.vx * dt; s.y += s.vy * dt; s.z += s.vz * dt;
    Ground g; terrainGround(s.x, s.z, g);
    const bool water = g.mat == MAT_WATER;
    const float surf = water ? g.h + g.water : g.h;
    s.surfY = surf; s.mat = g.mat; s.surf = g.surf;
    const float gearH = s.gearDown ? A.gearH : A.gearH * 0.55f;
    s.agl = s.y - A.gearH - surf;
    const float gs = sqrtf(s.vx * s.vx + s.vz * s.vz);
    if (s.y - gearH <= surf) {
        const float dhdx = water ? 0.f : -g.nx / fmaxf(g.ny, 0.2f), dhdz = water ? 0.f : -g.nz / fmaxf(g.ny, 0.2f);
        const float vyG = dhdx * s.vx + dhdz * s.vz;
        const float sink = vyG - s.vy;
        const bool arrive = !s.onGround;
        const bool airfield = g.mat == MAT_RUNWAY || g.mat == MAT_LOT;
        const float rough = s_bush ? 1.f : A.roughField;
        uint8_t crash = CRASH_NONE;
        if (water && A.gear != GEAR_FLOATS) crash = CRASH_WATER;
        else if (arrive) {
            s.evTouch = sink > 0.f ? sink : 0.01f;
            if (!s.gearDown) crash = CRASH_GEAR;
            else if (sink > 4.6f) crash = CRASH_HARD;
            else if (s.pitch < -0.21f && sink > 1.2f) crash = CRASH_TERRAIN;    // nose first
            else if (fabsf(s.roll) > 0.38f) crash = CRASH_WING;                  // a wingtip
            else if (!water && g.ny < 0.82f && gs > 8.f) crash = CRASH_TERRAIN;  // into a hillside
        }
        if (!crash && !water && !airfield && gs > 6.f) {
            if (g.ny < 0.88f && gs > 10.f) crash = CRASH_TERRAIN;
            else if (rough < 0.5f && gs > 14.f) crash = CRASH_GEAR;            // the racer's spats and the jet's tyres
            else if (rough < 0.99f && g.ny < 0.95f && gs > 16.f) crash = CRASH_GEAR;
        }
        if (crash) { s.crash = crash; s.y = surf + gearH; s.vx = s.vy = s.vz = 0.f; s.onGround = true; s.onWater = water; return; }
        s.onGround = true; s.onWater = water;
        s.y = surf + gearH;
        if (arrive && sink > 2.0f) { s.vy = vyG + sink * 0.28f; s.evBump = fminf(1.f, sink / 4.f); }   // a firm one bounces
        else if (s.vy < vyG) { if (vyG - s.vy > 0.6f) s.evBump = fminf(1.f, (vyG - s.vy) / 3.f); s.vy = vyG; }
        if (arrive) s.rollOut = 0.f;
        s.rollOut += dt;
        // wheels (or floats): rolling resistance, brakes, no sideways skid
        const float hx = cosf(s.yaw), hz = sinf(s.yaw);
        float vlon = s.vx * hx + s.vz * hz, vlat = -s.vx * hz + s.vz * hx;
        const float lift = clampf(L * ly / (m * G), 0.f, 1.f);
        const float load = 1.f - lift;
        float decel, latK;
        if (water) {
            decel = load * 0.045f * G + 0.0016f * vlon * vlon * load + in.brake * 0.4f;   // water drag (and the water rudders)
            latK = 1.5f + 2.f * load;
            if (gs > 2.f) s.evSplash = fminf(1.f, gs / 25.f);
        } else {
            const float mu = rollMu(g.surf) + (airfield ? 0.f : 0.03f);
            const float muB = in.brake * (g.surf == SURF_TARMAC ? 0.5f : 0.3f);
            decel = load * G * (mu + muB);
            latK = 2.f + 9.f * load;
        }
        float dv = decel * dt;
        vlon = fabsf(vlon) <= dv ? 0.f : vlon - (vlon > 0.f ? dv : -dv);
        vlat *= expf(-dt * latK);
        s.vx = vlon * hx - vlat * hz; s.vz = vlon * hz + vlat * hx;
        // steering: the tilt turns the tailwheel / nosewheel / water rudders; harder at a crawl, gentle at speed
        const float sp = fabsf(vlon);
        const float steerRate = in.roll * (water ? 0.45f : 0.75f) * fminf(1.f, 0.3f + sp * 0.2f) / (1.f + sp * 0.06f);
        s.yaw += steerRate * dt;
        // pitch: the three-point attitude at rest; the tail comes up with speed, the yoke lifts the nose to rotate
        float minP = A.groundPitch;
        if (A.gear == GEAR_TAIL) minP *= 1.f - smooth01((auth - 0.35f) * 2.5f) * (in.pitch < 0.3f ? 1.f : 0.f);
        float target = minP + fmaxf(0.f, in.pitch) * 0.24f * smooth01(auth * 1.6f);
        target = fminf(target, fmaxf(minP, aMax * 0.85f));         // rotate to lift off, never to a stalled wing
        s.pitch += (target - s.pitch) * (1.f - expf(-dt * 3.f));
        s.roll += (0.f - s.roll) * (1.f - expf(-dt * 8.f));
        s.p = s.q = 0.f; s.r = steerRate;
        basisFromEuler(s);
    } else if (s.onGround) {
        s.onGround = false; s.onWater = false; s.evLift = 1.f;
    }

    // ---- what the instruments read ----
    s.speed = sqrtf(s.vx * s.vx + s.vy * s.vy + s.vz * s.vz);
    s.groundSpeed = gs;
    s.ias = V * sqrtf(rho * (1.f / 1.225f));
    s.aoa = alpha; s.beta = beta; s.vs = s.vy;
    s.stall = (!s.onGround && V > 4.f) ? clampf((alpha - (aMax - 0.06f)) * (1.f / 0.06f), 0.f, 1.f) : 0.f;
    s.buffet = fmaxf(s.stall * 0.7f, stallT);
    if (A.vProp > 0.f) {                                       // the prop: governed by power, windmilling with speed
        s.rpm01 = clampf(0.18f + 0.82f * (0.25f * s.power + 0.75f * sqrtf(s.power)) * (0.85f + 0.15f * clampf(vf / A.vCruise, 0.f, 1.2f)) + 0.12f * clampf(vf / A.vNe, 0.f, 1.f), 0.f, 1.f);
        s.rpm = A.rpmIdle + (A.rpmMax - A.rpmIdle) * (s.rpm01 - 0.18f) / 0.82f;
    } else { s.rpm01 = 0.22f + 0.78f * s.power; s.rpm = s.rpm01; }
}

float aircraftHitPost(AircraftState& s, uint8_t id, float ox, float oz, float rad, float baseY, float top) {
    const AircraftSpec& A = AIRCRAFT[id % A_COUNT];
    if (s.crash || s.y - A.gearH > top) return 0.f;
    const float dx = s.x - ox, dz = s.z - oz, d = sqrtf(dx * dx + dz * dz);
    // the fuselage meets it head on; a wing reaches out to the side (only above the trunk's lowest branches)
    const float lat = fabsf(-dx * sinf(s.yaw) + dz * cosf(s.yaw));
    const float reach = rad + (s.y - baseY > 2.f ? A.span * 0.45f : 0.8f);
    if (d > reach || (lat > rad + 0.8f && s.y - baseY < 2.f)) return 0.f;
    const float v = s.speed;
    if (s.onGround && v < 4.f) {                               // taxiing into it: a bump, stopped
        const float nx = d > 0.01f ? dx / d : 1.f, nz = d > 0.01f ? dz / d : 0.f;
        s.x = ox + nx * reach; s.z = oz + nz * reach;
        s.vx = s.vz = 0.f;
        return v + 0.5f;
    }
    s.crash = CRASH_TREE;
    s.vx = s.vy = s.vz = 0.f;
    return v;
}

}
