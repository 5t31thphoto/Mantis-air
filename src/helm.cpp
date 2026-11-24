#pragma GCC optimize("Os")
#include "helm.h"
#include "haptics.h"
#include <M5Unified.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

namespace mantis {

// ======================= TUNING =======================
constexpr float ROLL_DEAD_DEG   = 1.5f;
constexpr float ROLL_FULL_DEG   = 28.f;    // a yoke's worth of tilt
constexpr float PITCH_DEAD_DEG  = 2.0f;
constexpr float PITCH_FULL_DEG  = 20.f;
constexpr float STICK_EXPO      = 0.55f;   // fine near the centre, quick at the ends
constexpr float SMOOTH          = 0.04f;   // s
constexpr bool  INVERT_ROLL     = true;    // M5's roll is + leaning left on the Core2
constexpr bool  INVERT_PITCH    = false;   // false: tip the top away = nose down (a yoke pushed forward)
constexpr uint32_t HOLD_MS      = 650;     // a button held this long is a hold, not a tap
constexpr uint32_t MUTE_HOLD_MS = 1500;
constexpr uint32_t TAP_MS       = 450;
constexpr uint32_t DOUBLE_MS    = 900;
constexpr uint32_t SWIPE_MS     = 700;
constexpr int SWIPE_X = 70, SWIPE_Y = 60;
// ======================================================

static float s_roll = 0.f, s_pitch = 0.f, s_tip = 0.f, s_thr = 0.f;
static float s_nPitch = 0.f, s_nRoll = 0.f;
static bool s_nValid = false;
static int s_settle = 0;
struct Btn { bool was, held; uint32_t down; };
static Btn s_a, s_b, s_c;
static bool s_touch = false, s_moved = false, s_muteFired = false, s_slide = false;
static int s_tx0 = 0, s_ty0 = 0, s_tx1 = 0, s_ty1 = 0; static uint32_t s_tt0 = 0;
static uint32_t s_lastTap = 0; static int s_ltx = 0, s_lty = 0;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static void attitude(float ax, float ay, float az, float& pitch, float& roll) {
    pitch = atan2f(az, ay);
    roll = atan2f(ax, sqrtf(ay * ay + az * az));
}
static bool readAttitude(float& p, float& r) {
    if (!M5.Imu.update()) return false;
    auto d = M5.Imu.getImuData();
    attitude(d.accel.x, d.accel.y, d.accel.z, p, r);
    return true;
}

void helmInit() { s_roll = s_pitch = s_tip = 0.f; s_nValid = false; s_settle = 0; memset(&s_a, 0, sizeof s_a); memset(&s_b, 0, sizeof s_b); memset(&s_c, 0, sizeof s_c); }
void helmRecenter() { float p, r; if (readAttitude(p, r)) { s_nPitch = p; s_nRoll = r; s_nValid = true; } s_roll = s_pitch = 0.f; }
float helmRoll() { return s_roll; }
float helmPitch() { return s_pitch; }
float helmTilt() { return s_tip; }
bool helmADown() { return s_a.was; }
bool helmCDown() { return s_c.was; }
float helmThrottle() { return s_thr; }
void helmSetThrottle(float t) { s_thr = clampf(t, 0.f, 1.f); }
bool helmSliding() { return s_slide; }

static float stick(float deg, float dead, float full, float expo) {
    float a = fabsf(deg);
    if (a < dead) return 0.f;
    float u = clampf((a - dead) / (full - dead), 0.f, 1.f);
    float e = u * (1.f - expo) + u * u * expo;
    return deg < 0.f ? -e : e;
}
static void setThrottleFromY(int y) {
    float t = 1.f - (float)(y - SLIDER_Y0) / (SLIDER_Y1 - SLIDER_Y0);
    t = clampf(t, 0.f, 1.f);
    if ((s_thr < 0.5f) != (t < 0.5f)) hx::pop(0.12f, 0.01f);   // a soft click through the middle
    s_thr = t;
}

static HelmEvents s_pend;
static bool s_btnWas = false;
static void touchStep(uint32_t now, bool btn) {
    s_btnWas = btn;
    auto t = M5.Touch.getDetail();
    bool down = t.isPressed() && t.y < 236 && !btn;
    if (down && !s_touch) {
        s_touch = true; s_moved = false; s_muteFired = false; s_tx0 = t.x; s_ty0 = t.y; s_tt0 = now; s_tx1 = t.x; s_ty1 = t.y;
        s_slide = t.x >= SLIDER_X - 6 && t.y >= SLIDER_Y0 - 12 && t.y <= SLIDER_Y1 + 12;
        if (s_slide) setThrottleFromY(t.y);
    } else if (down && s_touch) {
        s_tx1 = t.x; s_ty1 = t.y;
        if (s_slide) { setThrottleFromY(t.y); return; }
        if (abs(t.x - s_tx0) + abs(t.y - s_ty0) > 40) s_moved = true;
        if (!s_moved && !s_muteFired && now - s_tt0 > MUTE_HOLD_MS) { s_muteFired = true; s_pend.mute = true; }
    } else if (!down && s_touch) {
        if (s_slide) { s_slide = false; s_touch = false; return; }
        if (!s_moved && !s_muteFired && now - s_tt0 < TAP_MS) {
            s_pend.tap = true; s_pend.tapX = s_tx0; s_pend.tapY = s_ty0;
            if (s_lastTap && now - s_lastTap < DOUBLE_MS && abs(s_tx0 - s_ltx) + abs(s_ty0 - s_lty) < 160) { s_pend.doubleTap = true; s_lastTap = 0; }
            else { s_lastTap = now; s_ltx = s_tx0; s_lty = s_ty0; }
        } else if (s_moved && !s_muteFired && now - s_tt0 < SWIPE_MS) {
            int dx = s_tx1 - s_tx0, dy = s_ty1 - s_ty0;
            if (abs(dx) >= SWIPE_X && abs(dx) > 2 * abs(dy)) s_pend.swipeX = (int8_t)(dx > 0 ? 1 : -1);
            else if (abs(dy) >= SWIPE_Y && abs(dy) > 2 * abs(dx)) s_pend.swipeY = (int8_t)(dy < 0 ? 1 : -1);
        }
        s_touch = false;
    }
}
void helmPoll() {
    const uint32_t now = millis();
    M5.Touch.update(now);
    touchStep(now, s_btnWas);
}
// a button: tap on release (if not held), hold once at the threshold
static void button(Btn& b, bool down, uint32_t now, bool& tap, bool& hold) {
    if (down && !b.was) { b.down = now; b.held = false; }
    if (down && !b.held && now - b.down > HOLD_MS) { b.held = true; hold = true; }
    if (!down && b.was && !b.held) tap = true;
    b.was = down;
}

void helmUpdate(float dt, HelmEvents& ev) {
    memset(&ev, 0, sizeof(ev));
    M5.update();
    const uint32_t now = millis();
    const bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed(), c = M5.BtnC.isPressed();
    bool bTap = false, bHold = false;
    button(s_a, a, now, ev.aTap, ev.aHold);
    button(s_b, b, now, bTap, bHold);
    bool cHold = false;
    button(s_c, c, now, ev.cTap, cHold);
    if (bHold) { ev.recenter = true; helmRecenter(); }
    if (bTap) ev.view = true;
    // ---- tilt ----
    float p = 0.f, r = 0.f;
    if (readAttitude(p, r)) {
        if (!s_nValid || s_settle < 10) { if (++s_settle == 10 || !s_nValid) { s_nPitch = p; s_nRoll = r; s_nValid = true; } }
        float dR = (r - s_nRoll) * 57.2958f, dP = (p - s_nPitch) * 57.2958f;
        if (INVERT_ROLL) dR = -dR;
        if (INVERT_PITCH) dP = -dP;
        float k = 1.f - expf(-dt / SMOOTH);
        s_roll += (stick(dR, ROLL_DEAD_DEG, ROLL_FULL_DEG, STICK_EXPO) - s_roll) * k;
        const float tip = stick(dP, PITCH_DEAD_DEG, PITCH_FULL_DEG, STICK_EXPO);
        s_tip += (tip - s_tip) * k;
        s_pitch = -s_tip;                                   // top away: nose down
    }
    touchStep(now, a || b || c);
    ev.tap = s_pend.tap; ev.tapX = s_pend.tapX; ev.tapY = s_pend.tapY; ev.doubleTap = s_pend.doubleTap;
    ev.swipeX = s_pend.swipeX; ev.swipeY = s_pend.swipeY; if (s_pend.mute) ev.mute = true;
    memset(&s_pend, 0, sizeof(s_pend));
}

}
