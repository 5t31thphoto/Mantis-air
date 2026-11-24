// Mantis Flight: Mantis Rally's world, from the sky. Fly the Cub out of the home strip, take gigs from airfield to
// airfield, climb the ladder from amateur to fighter pilot, and buy the next aircraft with the miles you fly.
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <M5Unified.h>
#include "helm.h"
#include "radio.h"
#include "universe.h"
#include "journey.h"
#include "terrain.h"
#include "airfield.h"
#include "aircraft.h"
#include "field.h"
#include "scene.h"
#include "render.h"
#include "display.h"
#include "haptics.h"
#include "audio.h"
#include "progress.h"
#include "store.h"
#include "storage.h"
#include "photo.h"
#include "gigs.h"
#include "combat.h"
#include "mem.h"
#include "atc.h"

using namespace mantis;

// ======================= GAME TUNING =======================
static constexpr float CRASH_HOLD = 2.6f;        // seconds of wreckage before you're back at an airfield
static constexpr float HANGAR_STOP_SPEED = 1.5f; // m/s: taxi up to the hangar and stop
static constexpr float HANGAR_REACH = 55.f;      // metres from the hangar's doors
static constexpr float WIND_MS = 9.f;            // the scene's wind 1.0 = this many m/s
static constexpr float RECOVER_ALT = 350.f;      // a double tap in trouble: this high, at cruise
// ===========================================================

static AircraftState ac;
static uint8_t air = A_CUB;
static float t = 0.f, distance = 0.f, fpsAvg = 30.f, crashT = 0.f;
static char msgBuf[40], subBuf[40];
static float msgT = 0.f;
static uint8_t flaps = 0;
static bool apOn = false, smokeOn = false;
static uint8_t apMode = 0;                       // 0 off, 1 hold (heading + altitude), 2 NAV (the waypoint), 3 APPR (the runway)
static int clearI = 0x7fff, clearJ = 0x7fff;     // the field that cleared you to land
static bool takeoffCalled = false;
static uint8_t station = 0;                      // 0..RADIO_N-1 the music, RADIO_N the weather
static constexpr uint8_t ST_WEATHER = RADIO_N;
// the CONTACT TOWER button (panel pixels)
static float giverT = 0.f; static uint8_t giverWho = 0; static char giverLine[48];
static void giverSay(uint8_t who, const char* line, float secs) { giverWho = who; snprintf(giverLine, sizeof giverLine, "%s", line); giverT = secs; }
static void refreshStores() { int a, g, b; progressStores(a, g, b); Stores st = {a, g, b}; combatSetStores(st); combatRearm(); }
static bool dropViewAllowed() {
    if (air == A_FIGHTER) return true;
    if (const Waypoint* w = gigWaypoint()) return w->kind == W_SUPPLY || w->kind == W_RECON || w->kind == W_DROP;
    return false;
}
static float apAlt = 0.f, apHdg = 0.f;
static float hangarStopT = 0.f;
static bool hangarArmed = true;
static float gustPh = 0.f;

static void message(const char* m, const char* sub = nullptr, float secs = 3.f) {
    snprintf(msgBuf, sizeof msgBuf, "%s", m);
    if (sub) snprintf(subBuf, sizeof subBuf, "%s", sub); else subBuf[0] = 0;
    msgT = secs;
}

static uint32_t s_feelMs = 0;
static void feelTick() {
    const uint32_t now = millis();
    if (now - s_feelMs < 4) return;
    const float dt = s_feelMs ? fminf(0.1f, (now - s_feelMs) * 0.001f) : 0.016f;
    s_feelMs = now;
    hx::update(dt);
}
static void renderTickHook() { helmPoll(); feelTick(); }

// on the runway of the airfield nearest (x, z), at the threshold, lined up, engine idling
static void placeAtAirfield(float x, float z) {
    Airfield a;
    if (!airfieldNearest(x, z, a, nullptr)) airfieldAt(0, 0, a);
    airfieldLevel(a);
    const float c = cosf(a.yaw), s = sinf(a.yaw), lon = -a.len * 0.5f + 45.f;
    const float px = a.x + c * lon, pz = a.z + s * lon;
    terrainUpdate(px, pz, a.yaw, 0.f, 1.f / 30.f);         // (put somewhere new: the caches dream it first)
    aircraftReset(ac, air, px, pz, a.yaw);
    helmSetThrottle(0.f); flaps = 0; apOn = false; apMode = 0; smokeOn = false; takeoffCalled = false; clearI = clearJ = 0x7fff;
    g_prog.homeI = (int16_t)a.i; g_prog.homeJ = (int16_t)a.j;
}
static void switchAircraft(uint8_t a) {
    air = a % A_COUNT;
    const float x = ac.x, z = ac.z, yaw = ac.yaw;
    aircraftReset(ac, air, x, z, yaw);
    renderSetAircraft(air);
    g_car.engine = aircraftSpec(air).engine; g_car.blades = aircraftSpec(air).blades;
    refreshStores();
}

void setup() {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    cfg.internal_spk = true;
    cfg.internal_mic = false;
    M5.begin(cfg);
    audioSpeakerBegin();
    audioBegin();
    displayInit();
    M5.Display.setRotation(1);
    M5.Display.setBrightness(110);
    if (M5.Imu.isEnabled()) {
        M5.Imu.loadOffsetFromNVS();
        for (int i = 0; i < 15; i++) { M5.Imu.update(); delay(4); }
    }
    uint32_t seed = (uint32_t)(millis() ^ 0xC0FFEEu);
#ifndef HOST
    seed ^= (uint32_t)esp_random();
#endif
    storageInit();
    progressInit();
    radioLoadClips();
    if ((g_prog.radio & 0x80) && !(g_prog.radio & 0x60)) {
        radioSetStation(g_prog.radio & 3);
        int v = (g_prog.radio >> 2) & 7; radioSetVolume(v ? v : 4);
    }
    g_prog.view = VIEW_COCKPIT;
    universeInit(seed);
    atcInit(seed);
    journeyInit(seed);
    journeySetVisited(g_prog.visited);
    terrainInit(seed);
    sceneInit(seed);
    fieldInit(seed);
    renderInit();
    helmInit();
    hx::begin();
    g_renderTick = renderTickHook;
    {   static char note[40];
        bool lcd = displayStreaming(), grid = memIsFast(terrainNear().c), depth = renderDepthFast(), snd = audioRunning();
        if (lcd && grid && depth && snd) snprintf(note, sizeof note, "MEM OK");
        else snprintf(note, sizeof note, "MEM SLOW:%s%s%s%s", lcd ? "" : " LCD", grid ? "" : " GRID", depth ? "" : " DEPTH", snd ? "" : " SOUND");
        g_memNote = note;
    }
    air = g_prog.current;
    renderSetAircraft(air);
    g_car.engine = aircraftSpec(air).engine; g_car.blades = aircraftSpec(air).blades;
    placeAtAirfield(0.5f * AF_CELL, 0.5f * AF_CELL);         // the home strip
    refreshStores();
    message("MANTIS HOME", rankName(g_prog.rank), 4.f);
}

static const char* crashWord(uint8_t k, const char** sub) {
    *sub = "BACK TO THE FIELD";
    switch (k) {
        case CRASH_WATER: return "SPLASH";
        case CRASH_TREE: return "INTO THE TREES";
        case CRASH_HARD: return "HARD LANDING";
        case CRASH_GEAR: *sub = "NOT A RUNWAY FOR THIS ONE"; return "GEAR COLLAPSED";
        case CRASH_WING: return "WINGTIP STRIKE";
        case CRASH_SHOT: return "SHOT DOWN";
        default: return "CRASHED";
    }
}

void loop() {
    static uint32_t lastMs = 0;
    uint32_t now = millis();
    float dt = lastMs ? (now - lastMs) * 0.001f : 1.f / 30.f;
    lastMs = now;
    if (dt > 0.1f) dt = 0.1f;
    t += dt;
    fpsAvg += (1.f / fmaxf(dt, 0.001f) - fpsAvg) * 0.05f;
    const AircraftSpec& A = aircraftSpec(air);

    HelmEvents ev;
    helmUpdate(dt, ev);
    if (ev.mute) { audioSetMuted(!audioMuted()); hx::confirmMute(audioMuted()); }
    radioSetPaused(storeActive() || photoActive());

    // ---------------- the hangar ----------------
    if (storeActive()) {
        bool changed = false, took = false;
        if (!storeUpdate(ev.tap, ev.tapX, ev.tapY, t, changed, took)) { message("CLEAR PROP!", gigActive() ? gigCurrent().title : nullptr, 2.f); progressSave(); }
        if (changed) { switchAircraft(g_prog.current); sfxUi(UI_SOLD); }
        if (took) {
            sfxUi(UI_OK); hx::pop(0.4f, 0.05f);
            static const char* const HELLO[4] = {"SAFE FLYING. THE ARROW KNOWS THE WAY.", "MIND THE WIND OVER THE RIDGES, KID.", "THE PASSENGERS ARE BOARDED, CAPTAIN.", "GOOD HUNTING, PILOT. RETURN TO BASE."};
            const uint8_t gv = gigGiver(gigCurrent().kind);
            giverSay(gv, HELLO[gv], 5.f);
        }
        if (changed) refreshStores();
        displayPresent();
        feelTick();
        return;
    }
    // ---------------- photo mode (the world holds still) ----------------
    if (photoActive()) {
        if (ev.view) { photoExit(); sfxUi(UI_TICK); ev.view = false; }
        else {
            if (ev.cTap) { photoNextAngle(); sfxUi(UI_TICK); }
            photoUpdate(dt, helmRoll(), helmTilt());
            terrainSetSun(g_scene.sunAz, g_scene.sunElev);
            terrainUpdate(ac.x, ac.z, ac.yaw, 0.f, dt);
            sceneUpdate(t, dt, ac.x, ac.z, ac.yaw, 0.f, distance);
            if (ev.aTap) renderCaptureWater(true);
            renderFrame();
            if (ev.aTap) {
                GigInput gi; memset(&gi, 0, sizeof gi);
                if (const Waypoint* w = gigWaypoint()) if (w->kind == W_PHOTO) {   // the job's target: in the picture?
                    float sx, sy, d;
                    gi.framed = renderProject(w->x, terrainHeight(w->x, w->z) + 10.f, w->z, sx, sy, d) && sx > 8.f && sx < 152.f && sy > 6.f && sy < 114.f;
                }
                sfxImpact(IMP_SHUTTER, 1.f); int n = photoCompose(true); g_prog.photos++; (void)n;
                gi.photo = true; gi.photoX = g_view.pX; gi.photoZ = g_view.pZ; gi.cleared = false; gi.groundAlive = combatGroundAlive();
                GigEvent ge = gigUpdate(ac, air, t, 0.f, gi);
                if (ge.kind == GE_STEP) message("GOT THE SHOT", ge.sub, 2.5f);
                else if (ge.kind == GE_INFO) message(ge.msg, ge.sub, 2.f);
            }
            else photoCompose(false);
            displayPresent();
            feelTick();
            return;
        }
    }
    const bool fighter = air == A_FIGHTER;
    const bool dropView = g_prog.view == VIEW_DROP;
    if (ev.aHold && !(fighter && !ac.onGround) && !ac.crash && !dropView) {   // hold A: the camera (the fighter's A is its cannon in the air)
        photoEnter(ac, air); sfxUi(UI_OK);
        return;
    }
    if (ev.doubleTap && !ac.crash) {                             // in trouble: back up in the air, wings level
        if (ac.onGround) placeAtAirfield(ac.x, ac.z);
        else aircraftResetAir(ac, air, ac.x, ac.z, ac.yaw, RECOVER_ALT);
        helmSetThrottle(0.7f);
        message(ac.onGround ? "BACK ON THE RUNWAY" : "RECOVERED", nullptr, 1.5f); sfxUi(UI_WARN);
    }
    if (ev.view) {
        g_prog.view = (uint8_t)((g_prog.view + 1) % VIEW_N);
        if (g_prog.view == VIEW_DROP && !dropViewAllowed()) g_prog.view = VIEW_COCKPIT;
        sfxUi(UI_TICK);
    }
    if (g_prog.view == VIEW_DROP && !dropViewAllowed()) g_prog.view = VIEW_COCKPIT;
    if (ev.swipeX || ev.swipeY) {                                // the radio
        uint8_t st = radioStation();
        if (ev.swipeX) {
            station = (uint8_t)((station + (ev.swipeX > 0 ? 1 : RADIO_N)) % (RADIO_N + 1));   // the music, then the weather
            if (station == ST_WEATHER) { radioSetStation(RADIO_OFF); message("118.4 AM", "WEATHER", 2.f); atcClear(); }
            else { radioSetStation(station); message(radioFreq(station), radioName(station), 2.f); }
            st = radioStation();
        } else {
            radioSetVolume(radioVolume() + ev.swipeY);
            static char bar[12];
            int v = radioVolume();
            for (int i = 0; i < RADIO_VOL_MAX; i++) bar[i] = i < v ? '#' : '-';
            bar[RADIO_VOL_MAX] = 0;
            message(st == RADIO_OFF ? "RADIO OFF" : "VOLUME", bar, 1.5f);
        }
        sfxUi(UI_TICK);
        g_prog.radio = (uint8_t)(0x80 | (radioStation() & 3) | ((radioVolume() & 7) << 2));
    }
    if (ev.recenter) { sfxUi(UI_OK); hx::pop(0.3f, 0.04f); }

    // ---------------- A and C: each aircraft's own ----------------
    bool drop = false, missile = false, bomb = false, crate = false, recon = false;
    if (ev.aTap) {
        const Waypoint* w = gigWaypoint();
        if (air == A_BEAVER && gigActive() && gigTank() > 0.3f && !ac.onGround) { drop = true; sfxSplash(1.f); }   // the water bomber's drop
        else if (gigCrates() > 0 && !ac.onGround) { crate = true; sfxUi(UI_TICK); }                              // a supply crate
        else if (fighter && g_prog.view == VIEW_DROP && w && w->kind == W_RECON) { recon = true; sfxImpact(IMP_SHUTTER, 1.f); }   // the spy camera
        else if (fighter && g_prog.view == VIEW_DROP && !ac.onGround) bomb = true;
        else if (A.flapCL > 0.f && !fighter) { flaps = (uint8_t)((flaps + 1) % 4); static char f[12]; snprintf(f, sizeof f, "FLAPS %d", flaps); message(f, nullptr, 1.f); sfxUi(UI_TICK); }
    }
    if (ev.cTap && !ac.onGround) {
        if (fighter) { if (g_prog.view == VIEW_DROP) bomb = true; else missile = true; }
        else {
            // the jet's autopilot steps through its modes: HOLD, NAV (the waypoint), APPR (the cleared runway), off.
            // The others: hold on, hold off.
            if (air == A_JET) {
                apMode = (uint8_t)((apMode + 1) % 4);
                if (apMode == 2 && !gigActive() && !g_view.navOk) apMode = 3;
                if (apMode == 3 && clearI == 0x7fff) apMode = 0;
            } else apMode = apMode ? 0 : 1;
            apOn = apMode != 0; apAlt = ac.y; apHdg = ac.yaw;
            static const char* const AP[4] = {"AUTOPILOT OFF", "AUTOPILOT: HOLD", "AUTOPILOT: NAV", "AUTOPILOT: APPROACH"};
            message(AP[apMode], apMode == 3 ? "IT FLIES THE GLIDE PATH. YOU FLARE." : nullptr, 1.6f); sfxUi(UI_TICK);
        }
    }
    if (fighter) smokeOn = !ac.onGround && helmThrottle() > 0.97f;   // the afterburner's haze

    // ---------------- the yoke (or the autopilot) ----------------
    AircraftInput in;
    memset(&in, 0, sizeof in);
    in.roll = helmRoll(); in.pitch = helmPitch(); in.throttle = helmThrottle(); in.flaps = flaps;
    in.brake = ac.onGround && helmCDown() ? 1.f : 0.f;
    if (apOn) {
        if (fabsf(in.roll) > 0.3f || fabsf(in.pitch) > 0.3f || ac.onGround) { apOn = false; apMode = 0; message("AUTOPILOT OFF", nullptr, 1.f); }
        else {
            float wantVs = fmaxf(-6.f, fminf(6.f, (apAlt - ac.y) * 0.12f));
            if (apMode == 2) {                                   // NAV: to the waypoint (or the field)
                if (g_view.navOk) apHdg = atan2f(g_view.navZ - ac.z, g_view.navX - ac.x);
            } else if (apMode == 3) {                            // APPROACH: the localizer and a 3 degree glide path
                Airfield a;
                if (airfieldAt(clearI, clearJ, a)) {
                    airfieldLevel(a);
                    float c = cosf(a.yaw), sn = sinf(a.yaw), lon, lat;
                    airfieldLocal(a, ac.x, ac.z, c, sn, lon, lat);
                    const float dir = lon < 0.f ? 1.f : -1.f;    // land toward the far end
                    const float thr = -dir * a.len * 0.5f;       // the threshold's lon
                    const float dist = fmaxf(0.f, (thr - lon) * dir);   // to the threshold (0 past it)
                    const float rwyYaw = dir > 0.f ? a.yaw : a.yaw + 3.14159265f;
                    apHdg = rwyYaw + fmaxf(-0.6f, fminf(0.6f, -lat * dir * 0.004f));
                    const float gsY = a.y + 9.f + dist * 0.0524f;
                    wantVs = fmaxf(-8.f, fminf(3.f, (gsY - ac.y) * 0.15f - ac.groundSpeed * 0.0524f));
                    if (ac.agl < 18.f) { apOn = false; apMode = 0; message("AUTOPILOT OFF", "FLARE!", 1.5f); sfxUi(UI_WARN); }
                }
            }
            const float bank = fmaxf(-0.45f, fminf(0.45f, wrapPi(apHdg - ac.yaw) * 1.4f));
            in.roll = fmaxf(-1.f, fminf(1.f, (bank - ac.roll) * 2.2f));
            in.pitch = fmaxf(-0.6f, fminf(0.6f, (wantVs - ac.vs) * 0.09f - ac.q * 0.3f));
            if (air == A_JET) {                                  // autothrottle: cruise, or the approach speed
                const float want = apMode == 3 ? A.vStall * 1.35f : A.vCruise * 0.9f;
                helmSetThrottle(fmaxf(0.05f, fminf(1.f, helmThrottle() + (want - ac.ias) * 0.004f)));
                in.throttle = helmThrottle();
                if (apMode == 3 && flaps < 3) flaps = 3;
            }
        }
    }
    // the wind: a steady breeze from the weather, gusting (more down low, more in storms; thermals over warm land)
    gustPh += dt;
    const float wind = g_scene.wind * WIND_MS;
    in.windX = cosf(g_scene.windAz) * wind; in.windZ = sinf(g_scene.windAz) * wind;
    combatSetWind(in.windX, in.windZ);
    {   const float turb = (0.25f + g_scene.wind + g_scene.storm * 2.f) * (ac.agl < 300.f ? 1.f : 0.5f);
        in.gustY = (vnoise2(gustPh * 0.7f, ac.x * 0.002f, 0x6057u) - 0.5f) * 3.f * turb;
        in.windX += (vnoise2(gustPh * 0.5f, 3.3f, 0x6058u) - 0.5f) * 3.f * turb;
        in.windZ += (vnoise2(gustPh * 0.5f, 7.7f, 0x6059u) - 0.5f) * 3.f * turb;
    }

    // ---------------- fly ----------------
    const float px = ac.x, pz = ac.z;
    const bool wasGround = ac.onGround;
    FieldHit hit = {0.f, HIT_NONE};
    if (!ac.crash) {
        const int nSub = dt > 0.034f ? 3 : 2;
        for (int k = 0; k < nSub; k++) {
            aircraftStep(ac, air, in, dt / nSub);
            FieldHit h = fieldCollide(ac, air);
            if (h.strength > hit.strength) hit = h;
            if (ac.crash) break;
        }
    }
    // weapons and bogies
    CombatEvent ce; memset(&ce, 0, sizeof ce);
    {   // (always: crates and bombs fall on after release, and on the ground it rearms)
        const bool guns = fighter && helmADown() && !ac.onGround && g_prog.view != VIEW_DROP;
        ce = combatUpdate(ac, dt, guns, missile, bomb, crate);
        g_car.guns = guns;
        if (ce.groundKills) { message(combatGroundAlive() ? "TARGET HIT" : "TARGETS DESTROYED", nullptr, 2.f); sfxImpact(IMP_CRUNCH, 1.5f); hx::bump(0.7f); }
        if (bomb && !ce.fired) message("NO BOMBS LEFT", "REARM ON THE GROUND", 1.5f);
        if (ce.kills) { static char k[24]; g_prog.kills += (uint16_t)ce.kills; snprintf(k, sizeof k, "SPLASH %d", ce.kills); message(k, combatAlive() ? "MORE BOGIES" : "SKY CLEAR", 2.f); sfxImpact(IMP_CRUNCH, 1.5f); hx::bump(0.8f); }
        if (ce.noLock) message("NO LOCK", nullptr, 0.8f);
        if (ce.damage > 0.002f) { hx::bump(0.4f); if (((int)(t * 4.f)) & 1) message("TAKING HITS", nullptr, 0.5f); }
    }
    // the crash: a moment of wreckage, then the nearest field
    if (ac.crash) {
        if (crashT <= 0.f) {
            const char* sub; const char* w = crashWord(ac.crash, &sub);
            message(w, sub, CRASH_HOLD);
            sfxCrash(8.f, ac.crash == CRASH_TREE); sfxImpact(ac.crash == CRASH_WATER ? IMP_DEER : IMP_CRUNCH, 2.f); hx::bump(1.f);
            if (ac.crash == CRASH_WATER) for (int i = 0; i < 20; i++) fieldEmit(PK_SPRAY, ac.x, ac.surfY, ac.z, (i % 5 - 2) * 3.f, 4.f + (i % 3) * 2.f, (i % 7 - 3) * 2.f, 0.3f, 1.5f);
            else for (int i = 0; i < 16; i++) fieldEmit(i < 6 ? PK_FIRE : PK_SMOKE, ac.x, ac.y, ac.z, (i % 5 - 2) * 2.f, 3.f + (i % 3), (i % 7 - 3) * 1.5f, 1.2f, 3.f);
            renderSplat(1.f);
        }
        crashT += dt;
        if (crashT > CRASH_HOLD) { crashT = 0.f; placeAtAirfield(ac.x, ac.z); }
    }
    if (hit.kind == HIT_DEER) sfxImpact(IMP_DEER, 1.f);
    else if (hit.strength > 0.5f && !ac.crash) { sfxImpact(hit.kind == HIT_TREE ? IMP_TREE : IMP_ROCK, 0.6f); hx::bump(0.5f); }
    // touchdowns, judged by their sink rate (the gentle ones pay a little at an airfield)
    static float maxAglAloft = 0.f;                      // a hop on the takeoff roll isn't a landing
    if (!ac.onGround) maxAglAloft = fmaxf(maxAglAloft, ac.agl);
    const bool realLanding = ac.evTouch > 0.f && maxAglAloft > 15.f;
    if (ac.evTouch > 0.f && !realLanding) ac.evTouch = 0.f;
    if (ac.onGround && ac.groundSpeed < 3.f) maxAglAloft = 0.f;
    if (ac.evTouch > 0.f && !ac.crash && ac.mat == MAT_RUNWAY) {   // on a runway: were you cleared?
        Airfield a;
        if (airfieldAround(ac.x, ac.z, a) && !(a.i == clearI && a.j == clearJ)) {
            char t2[64]; snprintf(t2, sizeof t2, "%s, YOU WERE NOT CLEARED TO LAND. CALL US.", atcCallsign());
            atcSay(V_TOWER, "TOWER", t2);
            progressCash(-50);
            message("NO CLEARANCE", "-$50 FINE", 2.5f);
            sfxUi(UI_WARN);
        }
    }
    if (ac.evTouch > 0.f && !ac.crash) {
        const float fpm = ac.evTouch * 196.85f;
        static char ln[24];
        snprintf(ln, sizeof ln, "%d FPM", (int)(fpm + 0.5f));
        const bool field = ac.mat == MAT_RUNWAY || ac.onWater;
        if (fpm < 120.f) { message("BUTTER!", ln, 2.f); if (field) { progressCash(25); progressXp(5); } }
        else if (fpm < 300.f) { message("NICE LANDING", ln, 1.8f); if (field) progressCash(10); }
        else if (fpm < 550.f) message("FIRM", ln, 1.5f);
        else message("OUCH", ln, 1.5f);
        if (g_prog.bestLanding <= 0.f || fpm < g_prog.bestLanding) g_prog.bestLanding = fpm;
        sfxLand(fminf(1.f, ac.evTouch / 3.f)); hx::bump(fminf(1.f, ac.evTouch / 3.5f));
    }
    if (ac.evBump > 0.f) { sfxBump(ac.evBump); hx::bump(ac.evBump * 0.6f); }
    if (ac.evSplash > 0.f && !wasGround) sfxSplash(ac.evSplash);
    if (ac.onGround && !wasGround) { apOn = false; }
    ac.evTouch = ac.evBump = ac.evSplash = ac.evLift = 0.f;
    float moved = sqrtf((ac.x - px) * (ac.x - px) + (ac.z - pz) * (ac.z - pz));
    if (moved > 40.f) moved = 0.f;
    distance += moved;
    progressFly(moved);

    // ---------------- the world ----------------
    terrainSetSun(g_scene.sunAz, g_scene.sunElev);
    terrainUpdate(ac.x, ac.z, ac.yaw, ac.groundSpeed, dt);
    journeyUpdate(moved, ac.yaw, false);
    journeySetCar(ac.x, ac.z, ac.yaw, moved);
    if (journeyBiomeEvent()) {
        g_prog.visited |= (uint16_t)(1u << journeyCurrent());
        message(biome(journeyCurrent()).name, nullptr, 3.f); sfxUi(UI_TICK);
        if (progressAllBiomes() && !g_prog.bushUnlocked) {
            g_prog.bushUnlocked = true; g_prog.bushOn = true; aircraftSetBushMode(true);
            message("EVERY BIOME SEEN", "BUSH MODE: LAND ANYWHERE, ANY AIRCRAFT", 5.f);
        }
        progressSave();
    }
    // ---------------- the job ----------------
    {   GigInput gi; memset(&gi, 0, sizeof gi);
        gi.waterDrop = drop; gi.recon = recon;
        if (recon) if (const Waypoint* w = gigWaypoint()) {               // the spy camera: the target in the sight's picture, close enough to read
            float span = 0.f;
            gi.framed = renderDropSees(w->x, w->z, &span) && span < 2600.f;
        }
        gi.cleared = ac.onGround && clearI != 0x7fff && (gigWaypoint() ? (gigWaypoint()->ai == clearI && gigWaypoint()->aj == clearJ) : true);
        gi.crate = ce.crateLanded; gi.crateX = ce.crateX; gi.crateZ = ce.crateZ; gi.groundAlive = combatGroundAlive();
        GigEvent ge = gigUpdate(ac, air, t, dt, gi);
        if (ge.kind == GE_DONE) {
            static char p[32]; snprintf(p, sizeof p, "+$%d  +%d REP", ge.pay, ge.xp);
            progressCash(ge.pay); g_prog.gigsDone++;
            const bool up = progressXp(ge.xp);
            message(up ? "PROMOTED!" : "GIG COMPLETE", up ? rankName(g_prog.rank) : p, 4.f);
            static const char* const THANKS[4] = {"NICE WORK. THERE'S MORE ON THE BOARD.", "YOU'RE A REAL BUSH PILOT NOW.", "SMOOTH FLIGHT. THE AIRLINE THANKS YOU.", "MISSION ACCOMPLISHED. OUTSTANDING."};
            giverSay(gigGiver(gigCurrent().kind), THANKS[gigGiver(gigCurrent().kind)], 4.f);
            sfxUi(UI_ARRIVE); hx::kick(0.6f);
            progressSave();
        } else if (ge.kind == GE_FAILED) { message(ge.msg, ge.sub, 3.f); sfxUi(UI_WARN); }
        else if (ge.kind == GE_STEP) { message(ge.msg, ge.sub, 2.5f); sfxUi(UI_FORK); hx::pop(0.3f, 0.04f); }
        else if (ge.kind == GE_INFO) { message(ge.msg, ge.sub, 2.f); sfxUi(UI_TICK); }
        if (drop) for (int i = 0; i < 24; i++) fieldEmit(PK_SPRAY, ac.x - ac.fx * 2.f, ac.y - 1.5f, ac.z - ac.fz * 2.f, ac.vx * 0.8f + (i % 5 - 2) * 2.f, -2.f - (i % 4), ac.vz * 0.8f + (i % 3 - 1) * 2.f, 0.4f, 3.f);
    }
    // ---------------- the hangar: taxi up to MANTIS AERO and stop ----------------
    static Airfield near; float nearD = 1e9f;
    const bool haveNear = airfieldNearest(ac.x, ac.z, near, &nearD);
    float hangarA = 0.f;
    if (haveNear) {
        const float hd = sqrtf((near.hx - ac.x) * (near.hx - ac.x) + (near.hz - ac.z) * (near.hz - ac.z));
        if (ac.onGround && hd < 350.f) hangarA = 1.f;
        if (hd > 80.f) hangarArmed = true;
        if (hangarArmed && ac.onGround && hd < HANGAR_REACH && ac.groundSpeed < HANGAR_STOP_SPEED && !ac.crash) {
            hangarStopT += dt;
            if (hangarStopT > 0.6f || ev.tap) {
                storeOpen(near, g_prog.gigsDone); hangarArmed = false; hangarStopT = 0.f; sfxUi(UI_OK);
                ac.vx = ac.vz = 0.f; helmSetThrottle(0.f);
                g_prog.homeI = (int16_t)near.i; g_prog.homeJ = (int16_t)near.j;
            }
        } else hangarStopT = 0.f;
    }
    // ---------------- the tower ----------------
    const bool clearedHere = haveNear && near.i == clearI && near.j == clearJ;
    const bool towerOffer = haveNear && !ac.onGround && !ac.crash && nearD < 9000.f && ac.agl < 1500.f && !clearedHere;
    if (towerOffer && ev.tap && ev.tapX >= TWR_X0 && ev.tapX < TWR_X1 && ev.tapY >= TWR_Y0 && ev.tapY < TWR_Y1) {
        char a1[64], a2[64], wx[64];
        snprintf(a1, sizeof a1, "%s TOWER, %s, INBOUND TO LAND", near.name, atcCallsign());
        atcSay(V_YOU, atcCallsign(), a1);
        const char* why = nullptr;
        float c = cosf(near.yaw), sn = sinf(near.yaw), lon, lat;
        airfieldLocal(near, ac.x, ac.z, c, sn, lon, lat);
        const int rwy = lon < 0.f ? near.num : (near.num + 17) % 36 + 1;
        if (airfieldFits(near, air, &why)) {
            snprintf(a2, sizeof a2, "%s, RUNWAY %02d, CLEARED TO LAND", atcCallsign(), rwy);
            clearI = near.i; clearJ = near.j;
            atcSay(V_TOWER, "TOWER", a2);
            atcWeather(near.name, wx, sizeof wx); atcSay(V_WEATHER, "WEATHER", wx);
        } else { snprintf(a2, sizeof a2, "%s, UNABLE. %s", atcCallsign(), why); atcSay(V_TOWER, "TOWER", a2); }
        sfxUi(UI_TICK); ev.tap = false;
    }
    if (ac.onGround && ac.mat == MAT_RUNWAY && helmThrottle() > 0.6f && !takeoffCalled && haveNear) {
        char a2[64]; snprintf(a2, sizeof a2, "%s, WIND %03d, CLEARED FOR TAKEOFF", atcCallsign(), ((int)((g_scene.windAz * 57.3f + 90.f) / 10.f + 36.f) % 36) * 10 + 10);
        atcSay(V_TOWER, "TOWER", a2); takeoffCalled = true;
    }
    if (!ac.onGround && ac.agl > 300.f) { takeoffCalled = false; if (!clearedHere && nearD > 12000.f) clearI = clearJ = 0x7fff; }
    atcUpdate(dt, haveNear && nearD < 8000.f, station == ST_WEATHER, haveNear ? near.name : nullptr);
    // ---------------- the sky's markers: the job's waypoint, gates, bogies, missiles; fires ----------------
    g_markN = 0;
    if (const Waypoint* w = gigWaypoint()) {
        if (w->kind == W_GATE) {
            const Gig& g = gigCurrent();
            for (int k = gigStep(); k < g.nwp && g_markN < 2; k++) {
                Marker& m = g_marks[g_markN++];
                m.x = g.wp[k].x; m.y = g.wp[k].y; m.z = g.wp[k].z; m.yaw = g.wp[k].yaw; m.r = g.wp[k].r; m.pitch = m.roll = 0.f;
                m.kind = k == gigStep() ? MK_GATE_NEXT : MK_GATE;
            }
        } else if (w->kind != W_KILL && w->kind != W_LAND_AF) {
            Marker& m = g_marks[g_markN++];
            m.x = w->x; m.z = w->z; m.y = terrainHeight(w->x, w->z); m.kind = MK_BEACON; m.r = 0.f; m.yaw = m.pitch = m.roll = 0.f;
        }
    }
    {   const Bogey* b = combatBogeys();
        for (int k = 0; k < BOGEY_N && g_markN < MARK_N; k++) if (b[k].alive) {
            Marker& m = g_marks[g_markN++];
            m.x = b[k].x; m.y = b[k].y; m.z = b[k].z; m.yaw = b[k].yaw; m.pitch = b[k].pitch; m.roll = b[k].roll; m.r = 0.f;
            m.kind = combatLock() == k ? MK_BOGEY_LOCK : MK_BOGEY;
        }
        const Missile* ms = combatMissiles();
        for (int k = 0; k < MISSILE_N && g_markN < MARK_N; k++) if (ms[k].on) {
            Marker& m = g_marks[g_markN++];
            m.x = ms[k].x; m.y = ms[k].y; m.z = ms[k].z; m.kind = MK_MISSILE; m.r = m.yaw = m.pitch = m.roll = 0.f;
        }
    }
    if (const Waypoint* w = gigWaypoint()) if (w->kind == W_SUPPLY && g_markN < MARK_N) {
        Marker& m = g_marks[g_markN++]; m.x = w->x; m.z = w->z; m.y = terrainHeight(w->x, w->z); m.r = w->r; m.kind = MK_ZONE; m.yaw = m.pitch = m.roll = 0.f;
    }
    {   const GroundTarget* gt = combatGround();
        for (int k = 0; k < GT_N && g_markN < MARK_N; k++) if (gt[k].alive) {
            Marker& m = g_marks[g_markN++]; m.x = gt[k].x; m.y = gt[k].y; m.z = gt[k].z; m.yaw = gt[k].yaw; m.r = (float)gt[k].kind; m.pitch = m.roll = 0.f;
            m.kind = combatGroundLock() == k ? MK_GTARGET_LOCK : MK_GTARGET;
        }
        const Drop* dr = combatDrops();
        for (int k = 0; k < DROP_N && g_markN < MARK_N; k++) if (dr[k].on) {
            Marker& m = g_marks[g_markN++]; m.x = dr[k].x; m.y = dr[k].y; m.z = dr[k].z; m.kind = dr[k].kind == DROP_BOMB ? MK_BOMB : MK_CRATE;
            m.pitch = dr[k].chute ? 1.f : 0.f; m.r = m.yaw = m.roll = 0.f;
        }
        float ix, iz;
        if (g_prog.view == VIEW_DROP && !ac.onGround && g_markN < MARK_N && combatPredictImpact(ac, gigCrates() > 0, ix, iz)) {
            Marker& m = g_marks[g_markN++]; m.x = ix; m.z = iz; m.y = terrainHeight(ix, iz); m.kind = MK_PIP; m.r = m.yaw = m.pitch = m.roll = 0.f;
        }
    }
    {   float f[3];
        static float fireAcc = 0.f;
        if (gigFires(f, 1) && (f[0] - ac.x) * (f[0] - ac.x) + (f[2] - ac.z) * (f[2] - ac.z) < 5000.f * 5000.f) {
            fireAcc += dt * 12.f;
            while (fireAcc > 1.f) {
                fireAcc -= 1.f;
                const float r1 = vnoise2(t * 3.f, fireAcc, 0xF1u) - 0.5f, r2 = vnoise2(t * 3.f, 9.f, 0xF2u) - 0.5f;
                fieldEmit(((int)(t * 12.f) & 1) ? PK_FIRE : PK_SMOKE, f[0] + r1 * 60.f, f[1] + 1.f, f[2] + r2 * 60.f, in.windX * 0.5f, 6.f, in.windZ * 0.5f, 3.f, 6.f);
            }
        }
    }

    // ---------------- the world, the sound, the feel ----------------
    sceneUpdate(t, dt, ac.x, ac.z, ac.yaw, ac.groundSpeed, distance);
    fieldUpdate(ac, air, dt, smokeOn);
    if (g_scene.thunderIn > 0.f && g_scene.boltAge < dt * 1.5f) sfxThunder(g_scene.thunderIn, g_scene.thunderPow);
    g_car.engine = A.engine; g_car.blades = A.blades;
    g_car.rpm = A.vProp > 0.f ? ac.rpm : 900.f + 9000.f * ac.rpm01; g_car.rpm01 = ac.rpm01; g_car.throttle = ac.power;
    g_car.boost = 0.f; g_car.spin = 0.f; g_car.slide = ac.onGround ? fminf(1.f, helmCDown() ? ac.groundSpeed / 15.f : 0.f) : 0.f;
    g_car.speed = ac.onGround ? ac.groundSpeed : ac.ias; g_car.water = ac.onWater ? 1.f : 0.f; g_car.surf = ac.surf;
    g_car.air = !ac.onGround; g_car.limiter = false; g_car.reverse = false; g_car.cockpit = g_prog.view == VIEW_COCKPIT; g_car.deck = 0;
    g_car.stall = ac.stall;
    hx::engine(ac.power, ac.rpm01 * 10.f);
    if (ac.onGround) hx::surface(ac.surf, 0, ac.groundSpeed, 0.f, false);
    else hx::surface(SURF_TARMAC, 0, 0.f, 0.f, true);
    if (ac.buffet > 0.1f) hx::rumble(ac.buffet);
    feelTick();

    // ---------------- the picture ----------------
    ViewInfo& v = g_view;
    v.view = g_prog.view; v.aircraft = air; v.photo = false;
    v.x = ac.x; v.y = ac.y; v.z = ac.z; v.yaw = ac.yaw; v.pitch = ac.pitch; v.roll = ac.roll;
    v.fx = ac.fx; v.fy = ac.fy; v.fz = ac.fz; v.ux = ac.ux; v.uy = ac.uy; v.uz = ac.uz; v.rx = ac.rx; v.ry = ac.ry; v.rz = ac.rz;
    v.groundY = ac.surfY; v.agl = ac.agl;
    v.ias = ac.ias; v.vs = ac.vs; v.throttle = helmThrottle(); v.power = ac.power; v.rpm01 = ac.rpm01; v.flap = ac.flap; v.stall = ac.stall; v.gLoad = ac.gLoad;
    v.gearDown = ac.gearDown; v.retract = A.retract; v.onGround = ac.onGround; v.brake = in.brake > 0.1f; v.ap = apOn;
    v.shake = fminf(1.f, ac.buffet + (ac.onGround ? fminf(1.f, ac.groundSpeed / 40.f) * (ac.surf == SURF_TARMAC ? 0.1f : 0.35f) : 0.f));
    // navigation: the job's waypoint, or else the nearest field
    if (const Waypoint* w = gigWaypoint()) {
        v.navOk = true; v.navX = w->x; v.navZ = w->z; v.navName = w->label;
    } else if (haveNear) {
        v.navOk = !(ac.onGround && nearD < near.len); v.navX = near.x; v.navZ = near.z; v.navName = near.name; v.navRwy = near.num; v.navRwyYaw = near.yaw;
    } else v.navOk = false;
    v.navDist = sqrtf((v.navX - ac.x) * (v.navX - ac.x) + (v.navZ - ac.z) * (v.navZ - ac.z));
    v.hangarA = hangarA && !storeActive() ? 1.f : 0.f;
    v.radioWho = atcWho(); v.radioText = atcText(); v.radioA = atcAlpha();
    v.towerBtn = towerOffer; v.cleared = clearedHere && !ac.onGround;
    static const char* const APN[4] = {nullptr, "AP HOLD", "AP NAV", "AP APPR"};
    v.apMode = APN[apOn ? apMode : 0];
    {   const Stores& st = combatStores();
        v.aam = fighter ? st.aam : -1; v.agm = fighter && st.agm > 0 ? st.agm : -1; v.bombs = fighter ? st.bombs : -1;
        v.crates = gigCrates(); v.hull = fighter || combatEngaged() ? combatHull() : -1.f;
    }
    v.giver = giverWho; v.giverLine = giverT > 0.f ? giverLine : nullptr; v.giverA = giverT > 1.f ? 1.f : giverT;
    if (giverT > 0.f) giverT -= dt;
    v.msg = msgT > 0.f ? msgBuf : nullptr; v.sub = subBuf[0] ? subBuf : nullptr; v.msgA = msgT > 1.f ? 1.f : msgT;
    v.showFps = false; v.fps = (int)fpsAvg;
    if (msgT > 0.f) msgT -= dt;
    renderFrame();
    displayPresent();
}

#ifdef HOST
int main() { setup(); for (;;) loop(); }
#endif
