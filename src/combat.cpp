// Combat (see combat.h): a handful of bogies, per frame. Compiled for size.
#pragma GCC optimize("Os")
#include "combat.h"
#include "field.h"
#include "terrain.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {

// ======================= TUNING =======================
constexpr float BOGEY_SPEED = 135.f;      // m/s
constexpr float BOGEY_TURN = 0.42f;       // rad/s
constexpr float BOGEY_HP = 9.f;           // cannon hits to bring one down
constexpr float GUN_RATE = 14.f;          // rounds a second (each one a tracer)
constexpr float GUN_RANGE = 1100.f;
constexpr float LOCK_CONE = 0.5f;         // rad: the seeker's view
constexpr float LOCK_RANGE = 4200.f;
constexpr float MISSILE_SPEED = 330.f, MISSILE_TURN = 1.0f, MISSILE_LIFE = 9.f;
constexpr float THEIR_DAMAGE = 0.05f;     // hull per second under fire
// ======================================================

static Bogey s_b[BOGEY_N];
static Missile s_m[MISSILE_N];
static GroundTarget s_g[GT_N];
static Drop s_d[DROP_N];
static Stores s_full = {2, 0, 2}, s_st = {2, 0, 2};
static int s_glock = -1;
static bool s_engaged = false;
static float s_hull = 1.f, s_gunAcc = 0.f;
static int s_lock = -1;
static uint32_t s_rng = 0xB06E;
static float rnd() { s_rng = hash32(s_rng + 0x9E3779B9u); return (s_rng >> 8) * (1.f / 16777216.f); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

void combatClear() { memset(s_b, 0, sizeof s_b); memset(s_m, 0, sizeof s_m); memset(s_g, 0, sizeof s_g); s_engaged = false; s_hull = 1.f; s_lock = -1; s_glock = -1; }
bool combatEngaged() { return s_engaged; }
int combatAlive() { int n = 0; for (const Bogey& b : s_b) n += b.alive; return n; }
const Bogey* combatBogeys() { return s_b; }
const Missile* combatMissiles() { return s_m; }
int combatLock() { return s_lock; }
float combatHull() { return s_hull; }
int combatMissileCount() { return s_st.aam; }
void combatRearm() { s_st = s_full; s_hull = 1.f; }
void combatSetStores(const Stores& f) { s_full = f; }
const Stores& combatStores() { return s_st; }
const GroundTarget* combatGround() { return s_g; }
const Drop* combatDrops() { return s_d; }
int combatGroundLock() { return s_glock; }
int combatGroundAlive() { int n = 0; for (const GroundTarget& g : s_g) n += g.alive; return n; }
void combatSpawnGround(float x, float z, int n) {
    s_engaged = true;
    for (int k = 0; k < n && k < GT_N; k++) {
        GroundTarget& g = s_g[k];
        const float a = k * 2.3f + rnd() * 0.6f, d = 40.f + 70.f * (k % 3) + rnd() * 40.f;
        g.alive = true; g.x = x + cosf(a) * d; g.z = z + sinf(a) * d; g.y = terrainHeight(g.x, g.z);
        g.kind = (uint8_t)(k == 0 ? GT_RADAR : (k % 3 == 1 ? GT_AA : GT_DEPOT)); g.yaw = rnd() * 6.28f; g.fireT = rnd() * 2.f;
    }
}
// the fall of a bomb or a crate: a crate's chute opens after a second and a half
static void dropStep(Drop& d, float dt, float wx, float wz) {
    d.t += dt;
    if (d.kind == DROP_CRATE && d.t > 1.5f) d.chute = true;
    const float k = d.chute ? 1.6f : 0.04f;            // drag
    d.vx += ((d.chute ? wx : 0.f) - d.vx) * fminf(1.f, k * dt);
    d.vz += ((d.chute ? wz : 0.f) - d.vz) * fminf(1.f, k * dt);
    d.vy += -9.81f * dt; if (d.chute) d.vy += (-5.f - d.vy) * fminf(1.f, 2.f * dt);
    d.x += d.vx * dt; d.y += d.vy * dt; d.z += d.vz * dt;
}
static float s_wx = 0.f, s_wz = 0.f;
void combatSetWind(float wx, float wz) { s_wx = wx; s_wz = wz; }
bool combatPredictImpact(const AircraftState& s, bool crate, float& x, float& z) {
    Drop d; memset(&d, 0, sizeof d);
    d.kind = crate ? DROP_CRATE : DROP_BOMB; d.x = s.x; d.y = s.y - 1.5f; d.z = s.z; d.vx = s.vx; d.vy = s.vy; d.vz = s.vz;
    for (int k = 0; k < 300; k++) {                       // 0.2 s steps: a minute of fall at most
        dropStep(d, 0.2f, s_wx, s_wz);
        if (d.y <= terrainHeight(d.x, d.z)) { x = d.x; z = d.z; return true; }
    }
    return false;
}

void combatSpawn(float x, float z, int n, const AircraftState& p) {
    s_engaged = true;
    for (int k = 0; k < n && k < BOGEY_N; k++) {
        Bogey& b = s_b[k];
        memset(&b, 0, sizeof b);
        const float a = rnd() * 6.2831853f, d = 600.f + rnd() * 1400.f;
        b.alive = true; b.x = x + cosf(a) * d; b.z = z + sinf(a) * d;
        b.y = fmaxf(p.y, terrainHeight(b.x, b.z) + 300.f) + (rnd() - 0.5f) * 300.f;
        b.yaw = a + 1.57f; b.hp = BOGEY_HP; b.t = rnd() * 10.f;
        b.vx = cosf(b.yaw) * BOGEY_SPEED; b.vz = sinf(b.yaw) * BOGEY_SPEED;
    }
}

static void boom(float x, float y, float z) {
    for (int k = 0; k < 22; k++)
        fieldEmit(k < 8 ? PK_FIRE : PK_SMOKE, x + (rnd() - 0.5f) * 6.f, y + (rnd() - 0.5f) * 6.f, z + (rnd() - 0.5f) * 6.f,
                  (rnd() - 0.5f) * 30.f, (rnd() - 0.3f) * 20.f, (rnd() - 0.5f) * 30.f, k < 8 ? 1.5f : 2.5f, 2.5f + rnd() * 2.f);
}

CombatEvent combatUpdate(AircraftState& s, float dt, bool guns, bool secondary, bool bomb, bool crate) {
    CombatEvent e; memset(&e, 0, sizeof e);
    if (s.onGround && s.groundSpeed < 2.f) combatRearm();      // rearmed and patched up
    // the seeker: the bogy nearest the nose inside the cone
    s_lock = -1;
    {   float best = LOCK_CONE;
        for (int k = 0; k < BOGEY_N; k++) {
            const Bogey& b = s_b[k];
            if (!b.alive) continue;
            const float dx = b.x - s.x, dy = b.y - s.y, dz = b.z - s.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > LOCK_RANGE || d < 50.f) continue;
            const float c = (dx * s.fx + dy * s.fy + dz * s.fz) / d, ang = acosf(clampf(c, -1.f, 1.f));
            if (ang < best) { best = ang; s_lock = k; }
        }
    }
    // the cannon: tracers out of the nose; each round checked against the bogies along its line
    if (guns && !s.onGround) {
        s_gunAcc += dt * GUN_RATE;
        while (s_gunAcc >= 1.f) {
            s_gunAcc -= 1.f;
            e.fired = true;
            const float mv = 900.f, jx = (rnd() - 0.5f) * 0.012f, jy = (rnd() - 0.5f) * 0.012f;
            const float dx = s.fx + s.rx * jx + s.ux * jy, dy = s.fy + s.ry * jx + s.uy * jy, dz = s.fz + s.rz * jx + s.uz * jy;
            fieldEmit(PK_TRACER, s.x + s.fx * 6.f, s.y + s.fy * 6.f - 0.5f, s.z + s.fz * 6.f, s.vx + dx * mv, s.vy + dy * mv, s.vz + dz * mv, 0.2f, 1.2f);
            for (GroundTarget& g : s_g) {                        // strafing
                if (!g.alive) continue;
                const float gx = g.x - s.x, gy = g.y + 2.f - s.y, gz = g.z - s.z;
                const float al = gx * dx + gy * dy + gz * dz;
                if (al < 0.f || al > GUN_RANGE) continue;
                const float mx = gx - dx * al, my = gy - dy * al, mz = gz - dz * al;
                if (mx * mx + my * my + mz * mz < 7.f * 7.f && rnd() < 0.08f) { g.alive = false; e.groundKills++; boom(g.x, g.y + 2.f, g.z); }
            }
            for (Bogey& b : s_b) {
                if (!b.alive) continue;
                const float bx = b.x - s.x, by = b.y - s.y, bz = b.z - s.z;
                const float along = bx * dx + by * dy + bz * dz;
                if (along < 0.f || along > GUN_RANGE) continue;
                // where it will be when the round gets there (the round's flight time)
                const float tt = along / mv;
                const float qx = bx + (b.vx - s.vx) * tt, qy = by + (b.vy - s.vy) * tt, qz = bz + (b.vz - s.vz) * tt;
                const float a2 = qx * dx + qy * dy + qz * dz;
                const float mx = qx - dx * a2, my = qy - dy * a2, mz = qz - dz * a2;
                if (mx * mx + my * my + mz * mz < 9.f * 9.f && rnd() < 0.7f) {
                    b.hp -= 1.f; e.hitOn = true;
                    fieldEmit(PK_SMOKE, b.x, b.y, b.z, b.vx * 0.3f, 2.f, b.vz * 0.3f, 1.2f, 1.5f);
                    if (b.hp <= 0.f) { b.alive = false; e.kills++; boom(b.x, b.y, b.z); }
                }
            }
        }
    } else s_gunAcc = 0.f;
    // the ground seeker: the target nearest the nose, ahead and below
    s_glock = -1;
    {   float best = 0.6f;
        for (int k = 0; k < GT_N; k++) {
            const GroundTarget& g = s_g[k];
            if (!g.alive) continue;
            const float dx = g.x - s.x, dy = g.y - s.y, dz = g.z - s.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > 5000.f || d < 80.f) continue;
            const float ang = acosf(clampf((dx * s.fx + dy * s.fy + dz * s.fz) / d, -1.f, 1.f));
            if (ang < best) { best = ang; s_glock = k; }
        }
    }
    // the secondary: an air lock takes a heat seeker, a ground lock an AGM (target index + 100)
    if (secondary) {
        int tgt = -1; int* store = nullptr;
        if (s_lock >= 0 && s_st.aam > 0) { tgt = s_lock; store = &s_st.aam; }
        else if (s_glock >= 0 && s_st.agm > 0) { tgt = 100 + s_glock; store = &s_st.agm; }
        if (tgt < 0) e.noLock = true;
        else for (Missile& m : s_m) if (!m.on) {
            m.on = true; m.x = s.x - s.ux * 1.2f; m.y = s.y - s.uy * 1.2f; m.z = s.z - s.uz * 1.2f;
            m.vx = s.vx + s.fx * 40.f; m.vy = s.vy + s.fy * 40.f; m.vz = s.vz + s.fz * 40.f;
            m.life = MISSILE_LIFE; m.target = tgt; (*store)--; e.fired = true;
            break;
        }
    }
    if ((bomb && s_st.bombs > 0) || crate) {                  // release
        for (Drop& d : s_d) if (!d.on) {
            memset(&d, 0, sizeof d);
            d.on = true; d.kind = crate ? DROP_CRATE : DROP_BOMB; d.x = s.x; d.y = s.y - 1.5f; d.z = s.z; d.vx = s.vx; d.vy = s.vy; d.vz = s.vz;
            if (!crate) s_st.bombs--;
            e.fired = true;
            break;
        }
    }
    for (Drop& d : s_d) {
        if (!d.on) continue;
        dropStep(d, dt, s_wx, s_wz);
        const float gy = terrainHeight(d.x, d.z);
        if (d.y > gy) continue;
        d.on = false;
        if (d.kind == DROP_BOMB) {
            boom(d.x, gy + 2.f, d.z);
            for (GroundTarget& g : s_g) if (g.alive && (g.x - d.x) * (g.x - d.x) + (g.z - d.z) * (g.z - d.z) < 40.f * 40.f) { g.alive = false; e.groundKills++; }   // a 40 m blast
        } else {
            e.crateLanded = true; e.crateX = d.x; e.crateZ = d.z;
            for (int k = 0; k < 8; k++) fieldEmit(PK_DUST, d.x, gy + 0.5f, d.z, (rnd() - 0.5f) * 4.f, 1.f, (rnd() - 0.5f) * 4.f, 0.8f, 1.5f);
        }
    }
    // the AA guns: within 1.6 km and below 1500 m they fire at you
    for (GroundTarget& g : s_g) {
        if (!g.alive || g.kind != GT_AA || s.onGround) continue;
        const float dx = s.x - g.x, dy = s.y - g.y, dz = s.z - g.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
        g.yaw = atan2f(dz, dx);
        if (d > 1600.f || dy > 1500.f) continue;
        g.fireT -= dt;
        if (g.fireT <= 0.f) { g.fireT = 0.25f; fieldEmit(PK_TRACER, g.x, g.y + 3.f, g.z, dx / d * 600.f + (rnd() - 0.5f) * 60.f, dy / d * 600.f + (rnd() - 0.5f) * 60.f, dz / d * 600.f + (rnd() - 0.5f) * 60.f, 0.2f, 2.5f); }
        e.damage += THEIR_DAMAGE * 0.6f * dt * (1.f - d / 1600.f);
    }
    for (Missile& m : s_m) {
        if (!m.on) continue;
        m.life -= dt;
        Bogey gt; const Bogey* t = nullptr;
        if (m.target >= 100) { const GroundTarget& g = s_g[(m.target - 100) % GT_N]; if (g.alive) { memset(&gt, 0, sizeof gt); gt.alive = true; gt.x = g.x; gt.y = g.y + 2.f; gt.z = g.z; t = &gt; } }
        else if (m.target >= 0 && s_b[m.target].alive) t = &s_b[m.target];
        float sp = sqrtf(m.vx * m.vx + m.vy * m.vy + m.vz * m.vz);
        sp += (MISSILE_SPEED - sp) * clampf(dt * 1.5f, 0.f, 1.f);
        float dx = m.vx, dy = m.vy, dz = m.vz;
        { float l = sqrtf(dx * dx + dy * dy + dz * dz); if (l > 1e-3f) { dx /= l; dy /= l; dz /= l; } }
        if (t) {                                                // turn toward where the target is going
            const float tt = sqrtf((t->x - m.x) * (t->x - m.x) + (t->y - m.y) * (t->y - m.y) + (t->z - m.z) * (t->z - m.z)) / MISSILE_SPEED;
            float ax = t->x + t->vx * tt - m.x, ay = t->y + t->vy * tt - m.y, az = t->z + t->vz * tt - m.z;
            const float l = sqrtf(ax * ax + ay * ay + az * az);
            if (l > 1e-3f) { ax /= l; ay /= l; az /= l; }
            const float k = clampf(MISSILE_TURN * dt, 0.f, 1.f);
            dx += (ax - dx) * k; dy += (ay - dy) * k; dz += (az - dz) * k;
            const float l2 = sqrtf(dx * dx + dy * dy + dz * dz); dx /= l2; dy /= l2; dz /= l2;
        }
        m.vx = dx * sp; m.vy = dy * sp; m.vz = dz * sp;
        m.x += m.vx * dt; m.y += m.vy * dt; m.z += m.vz * dt;
        fieldEmit(PK_TRAIL, m.x, m.y, m.z, 0.f, 0.f, 0.f, 0.5f, 2.5f);
        if (t && (t->x - m.x) * (t->x - m.x) + (t->y - m.y) * (t->y - m.y) + (t->z - m.z) * (t->z - m.z) < 15.f * 15.f) {
            if (m.target >= 100) { s_g[(m.target - 100) % GT_N].alive = false; e.groundKills++; }
            else { s_b[m.target].alive = false; e.kills++; }
            boom(t->x, t->y, t->z); m.on = false;
        }
        if (m.life <= 0.f || m.y < terrainHeight(m.x, m.z)) { if (m.life > 0.f) boom(m.x, m.y, m.z); m.on = false; }
    }
    // the bogies
    for (Bogey& b : s_b) {
        if (!b.alive) continue;
        b.t += dt;
        const float dx = s.x - b.x, dy = s.y - b.y, dz = s.z - b.z, d = sqrtf(dx * dx + dy * dy + dz * dz) + 1e-3f;
        const float fx = cosf(b.yaw) * cosf(b.pitch), fy = sinf(b.pitch), fz = sinf(b.yaw) * cosf(b.pitch);
        // you on its tail? (it's in front of you, you're behind it)
        const float behind = -(dx * fx + dy * fy + dz * fz) / d;
        float wantYaw = b.yaw, wantY = b.y;
        if (d > 3000.f) { wantYaw = b.yaw + BOGEY_TURN * 0.3f; wantY = s.y; b.mode = 0; }              // patrolling in a wide circle
        else if (behind > 0.6f && d < 1500.f) {                                                         // break! hard away, jinking
            b.mode = 2;
            wantYaw = atan2f(-dz, -dx) + (sinf(b.t * 0.9f) > 0.f ? 1.6f : -1.6f);
            wantY = b.y + sinf(b.t * 1.3f) * 120.f;
        } else { b.mode = 1; wantYaw = atan2f(dz + s.vz * 1.5f, dx + s.vx * 1.5f); wantY = s.y; }         // fight: lead pursuit
        const float dyaw = wrapPi(wantYaw - b.yaw);
        const float turn = clampf(dyaw, -BOGEY_TURN * dt, BOGEY_TURN * dt) * (b.mode == 2 ? 1.4f : 1.f);
        b.yaw += turn;
        b.roll += (clampf(dyaw * 2.f, -1.3f, 1.3f) - b.roll) * clampf(dt * 3.f, 0.f, 1.f);
        const float floorY = terrainHeight(b.x, b.z) + 150.f;
        wantY = fmaxf(wantY, floorY);
        b.pitch += (clampf((wantY - b.y) * 0.004f, -0.4f, 0.4f) - b.pitch) * clampf(dt * 1.5f, 0.f, 1.f);
        b.vx = cosf(b.yaw) * cosf(b.pitch) * BOGEY_SPEED; b.vy = sinf(b.pitch) * BOGEY_SPEED; b.vz = sinf(b.yaw) * cosf(b.pitch) * BOGEY_SPEED;
        b.x += b.vx * dt; b.y += b.vy * dt; b.z += b.vz * dt;
        if (b.y < floorY - 100.f) b.y = floorY - 100.f;
        // you in front of it, close: it fires in bursts
        const float ahead = (dx * fx + dy * fy + dz * fz) / d;
        b.burstT -= dt;
        if (ahead > 0.96f && d < 900.f && !s.onGround) {
            if (b.burstT < -1.5f) b.burstT = 1.f;
            if (b.burstT > 0.f) {
                e.damage += THEIR_DAMAGE * dt * (1.2f - d / 900.f);
                if (rnd() < dt * 10.f) fieldEmit(PK_TRACER, b.x + fx * 8.f, b.y + fy * 8.f, b.z + fz * 8.f, b.vx + fx * 700.f + (rnd() - 0.5f) * 40.f, b.vy + fy * 700.f + (rnd() - 0.5f) * 40.f, b.vz + fz * 700.f + (rnd() - 0.5f) * 40.f, 0.2f, 1.f);
            }
        }
    }
    s_hull = clampf(s_hull - e.damage, 0.f, 1.f);
    if (s_hull <= 0.f && !s.crash) { s.crash = CRASH_SHOT; boom(s.x, s.y, s.z); }
    return e;
}

}
