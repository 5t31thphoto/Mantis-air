#pragma GCC optimize("Os")
#include "photo.h"
#include "render.h"
#include "terrain.h"
#include "display.h"
#include "storage.h"
#include "sprites.h"
#include "mem.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace mantis {

static bool s_on = false;
static uint8_t s_angle = SHOT_PILOT, s_air = 0;
static AircraftState s_s;
static float s_pan = 0.f, s_tilt = 0.f;
static float s_label = 0.f, s_flash = 0.f;
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

bool photoActive() { return s_on; }
void photoEnter(const AircraftState& s, uint8_t a) {
    s_on = true; s_air = a; s_s = s; s_angle = SHOT_PILOT; s_pan = 0.f; s_tilt = 0.f; s_label = 1.5f; s_flash = 0.f;
}
void photoExit() { s_on = false; g_view.photo = false; overlayClear(); overlayEnable(false); }
void photoNextAngle() { s_angle = (uint8_t)((s_angle + 1) % SHOT_N); s_pan = s_tilt = 0.f; s_label = 1.2f; }

static float downSpan() { return clampf(s_s.agl * 1.6f + 60.f, 120.f, 4000.f); }

void photoUpdate(float dt, float roll, float tip) {
    const AircraftSpec& A = aircraftSpec(s_air);
    ViewInfo& v = g_view;
    v.photo = true;
    const float k = 1.f - expf(-dt * 5.f);
    if (s_angle == SHOT_PILOT) {                                     // the pilot's seat: all the way round
        s_pan += roll * 1.6f * dt;
        s_tilt += (-tip * 1.1f - s_tilt) * k;
        v.pX = s_s.x + s_s.fx * A.eyeFwd + s_s.ux * A.eyeUp; v.pY = s_s.y + s_s.fy * A.eyeFwd + s_s.uy * A.eyeUp; v.pZ = s_s.z + s_s.fz * A.eyeFwd + s_s.uz * A.eyeUp;
        v.pYaw = s_s.yaw + s_pan; v.pPitch = s_tilt;
    } else {                                                         // off a wing, head-on, or above: the aircraft in frame
        s_pan += (roll * 0.6f - s_pan) * k;
        s_tilt += (-tip * 0.5f - s_tilt) * k;
        const float d = A.chaseBack * 0.85f;
        float rel = s_angle == SHOT_RIGHT ? 1.5708f : (s_angle == SHOT_LEFT ? -1.5708f : (s_angle == SHOT_FRONT ? 3.14159f : 3.14159f * 0.999f));
        const float a = s_s.yaw + rel + s_pan;
        const float up = s_angle == SHOT_DOWN ? d * 1.6f : d * 0.15f;
        v.pX = s_s.x + cosf(a) * d * (s_angle == SHOT_DOWN ? 0.35f : 1.f); v.pZ = s_s.z + sinf(a) * d * (s_angle == SHOT_DOWN ? 0.35f : 1.f);
        v.pY = fmaxf(s_s.y + up + s_tilt * d * 0.5f, terrainHeight(v.pX, v.pZ) + 1.5f);
        v.pYaw = a + 3.14159f;
        v.pPitch = -atan2f(v.pY - s_s.y, d * (s_angle == SHOT_DOWN ? 0.35f : 1.f)) * 0.9f;
    }
    if (s_label > 0.f) s_label -= dt;
    if (s_flash > 0.f) s_flash -= dt;
}

// ---- Mantis Gallery's jig file (MGJIG1): water regions and depth planes, at the photo's 320 x 240 ----
struct Txt { char* p; size_t n, cap; };
static void put(Txt& t, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void put(Txt& t, const char* fmt, ...) {
    if (!t.p || t.n + 96 >= t.cap) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(t.p + t.n, t.cap - t.n, fmt, ap);
    va_end(ap);
    if (w > 0) t.n += (size_t)w < t.cap - t.n ? (size_t)w : t.cap - t.n - 1;
}
// the rows of a 160 x 120 class map (cls[i] == want) at the photo's scale: each picture pixel is 2 x 2
static void rows(Txt& t, const char* tag, const uint8_t* cls, uint8_t want) {
    for (int y = 0; y < FB_H; y++) {
        char line[1024]; int ln = 0; bool any = false;
        int x = 0;
        while (x < FB_W) {
            while (x < FB_W && cls[y * FB_W + x] != want) x++;
            if (x >= FB_W) break;
            int a = x;
            while (x < FB_W && cls[y * FB_W + x] == want) x++;
            if (ln < 960) ln += snprintf(line + ln, sizeof line - ln, " %d-%d", a * 2, x * 2);
            any = true;
        }
        if (!any) continue;
        line[ln] = 0;
        put(t, "%s %d%s\n", tag, y * 2, line);
        put(t, "%s %d%s\n", tag, y * 2 + 1, line);
    }
}
static const char* WATER_NAME[4] = {"", "still", "river", "ocean"};
// water regions (the two biggest kinds), then depth planes from the class map (k = 1..4: plane k, depth DEPTH[k])
static void jig(Txt& t, const uint8_t* water, int horizon, const uint8_t* planes, const float* depth, const char* const* names, int np) {
    put(t, "MGJIG1\n");
    if (water) {
        int cnt[4] = {0, 0, 0, 0};
        for (int i = 0; i < FB_W * FB_H; i++) cnt[water[i] & 3]++;
        for (int r = 0; r < 2; r++) {
            int best = 0;
            for (int k = 1; k < 4; k++) if (cnt[k] > cnt[best] && cnt[k] > 12) best = k;
            if (!best) break;
            cnt[best] = 0;
            if (horizon >= 0) put(t, "water %s horizon=%d name=%s\n", WATER_NAME[best], horizon, WATER_NAME[best]);
            else put(t, "water %s name=%s\n", WATER_NAME[best], WATER_NAME[best]);
            rows(t, "wrow", water, (uint8_t)best);
        }
    }
    if (planes && np > 0) {
        put(t, "parallax\n");
        for (int k = 1; k <= np; k++) {
            bool any = false;
            for (int i = 0; i < FB_W * FB_H && !any; i++) any = planes[i] == k;
            if (!any) continue;
            put(t, "plane depth=%.2f fixed=1 name=%s\n", depth[k], names[k]);
            rows(t, "prow", planes, (uint8_t)k);
        }
    }
}

static uint8_t* s_cls = nullptr; static uint8_t* s_wat = nullptr;
static char* s_txt = nullptr;
static constexpr size_t TXT_CAP = 96 * 1024;

static void shoot() {
    if (!s_cls) { s_cls = (uint8_t*)memSlow(FB_W * FB_H); s_wat = (uint8_t*)memSlow(FB_W * FB_H); s_txt = (char*)memSlow(TXT_CAP); }
    if (!s_cls || !s_wat || !s_txt) return;
    const int n = storagePhotoNumber();
    char path[64], jp[64];
    // 1) what you framed (renderFrame captured its water this frame)
    {   const uint8_t* wm = renderWaterMask();
        memcpy(s_wat, wm ? wm : s_cls, FB_W * FB_H); if (!wm) memset(s_wat, 0, FB_W * FB_H);
        static const float DEPTH[5] = {0.f, 1.0f, 0.7f, 0.42f, 0.18f};
        static const char* const NAMES[5] = {"", "near", "mid", "far", "distance"};
        for (int y = 0; y < FB_H; y++) for (int x = 0; x < FB_W; x++) {
            const float d = renderPixelDepth(x, y);
            s_cls[y * FB_W + x] = d > 1e8f ? 0 : (d < 60.f ? 1 : (d < 400.f ? 2 : (d < 2000.f ? 3 : 4)));
        }
        snprintf(path, sizeof path, "/images/flights/flight_%04d.mg", n);
        storageWriteMG(path, compositeRow);
        Txt t = {s_txt, 0, TXT_CAP};
        jig(t, s_wat, clampf((float)renderHorizonRow() * 2.f, -1.f, 239.f) >= 0 ? renderHorizonRow() * 2 : -1, s_cls, DEPTH, NAMES, 4);
        snprintf(jp, sizeof jp, "/images/flights/.flight_%04d.mg.jig", n);
        storageWriteText(jp, s_txt, t.n);
    }
    // 2) the land straight below: the high ground nearer the lens than the low
    {   ViewInfo saveV = g_view;
        g_view.x = s_s.x; g_view.z = s_s.z; g_view.yaw = s_s.yaw; g_view.agl = s_s.agl; g_view.aircraft = s_air;
        renderTopDown(downSpan());
        g_view = saveV;
        const float* h = renderTopHeights();
        const uint8_t* wm = renderWaterMask();
        if (wm) memcpy(s_wat, wm, FB_W * FB_H); else memset(s_wat, 0, FB_W * FB_H);
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < FB_W * FB_H; i++) if (h) { lo = fminf(lo, h[i]); hi = fmaxf(hi, h[i]); }
        const float span = fmaxf(hi - lo, 1.f);
        static const float DEPTH[4] = {0.f, 0.85f, 0.6f, 0.35f};
        static const char* const NAMES[4] = {"", "high", "upper", "middle"};
        for (int i = 0; i < FB_W * FB_H; i++) {
            const float r = h ? (h[i] - lo) / span : 0.f;
            s_cls[i] = s_wat[i] ? 0 : (r > 0.75f ? 1 : (r > 0.5f ? 2 : (r > 0.25f ? 3 : 0)));   // the lowest ground: the background
        }
        snprintf(path, sizeof path, "/images/flights/flight_%04d_below.mg", n);
        storageWriteMG(path, compositeRow);
        Txt t = {s_txt, 0, TXT_CAP};
        jig(t, s_wat, -1, span > 8.f ? s_cls : nullptr, DEPTH, NAMES, 3);
        snprintf(jp, sizeof jp, "/images/flights/.flight_%04d_below.mg.jig", n);
        storageWriteText(jp, s_txt, t.n);
    }
    storagePhotoUsed();
}

// viewfinder marks: corners, and the angle's name for a moment (never in the photo)
static void marks() {
    uint16_t* o = overlayBuffer();
    if (!o) return;
    const uint16_t W = 0xFFFF;
    for (int k = 0; k < 18; k++) {
        int a = 10 + k;
        o[10 * 320 + a] = o[a * 320 + 10] = W;
        o[10 * 320 + 309 - k] = o[a * 320 + 309] = W;
        o[229 * 320 + a] = o[(229 - k) * 320 + 10] = W;
        o[229 * 320 + 309 - k] = o[(229 - k) * 320 + 309] = W;
    }
    if (s_label > 0.f) {
        static const char* NAMES[SHOT_N] = {"PILOT'S SEAT", "RIGHT WING", "LEFT WING", "HEAD ON", "FROM ABOVE"};
        uint8_t tmp[80 * 7]; memset(tmp, 0, sizeof tmp);
        int w = font3(tmp, 80, 7, 1, 1, NAMES[s_angle], 1, 0);
        int x0 = 160 - w, y0 = 18;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 80; x++) if (tmp[y * 80 + x])
            for (int q = 0; q < 4; q++) { int X = x0 + x * 2 + (q & 1), Y = y0 + y * 2 + (q >> 1); if ((unsigned)X < 320u && (unsigned)Y < 240u) o[Y * 320 + X] = W; }
    }
    if (s_flash > 0.f) for (int y = 0; y < 240; y += (s_flash > 0.08f ? 1 : 2)) for (int x = 0; x < 320; x++) o[y * 320 + x] = 0xFFFF;
    overlayEnable(true);
}

int photoCompose(bool doShoot) {
    if (!s_on) return 0;
    overlayClear();
    overlayEnable(false);
    int n = 0;
    if (doShoot) {
        n = storagePhotoNumber();
        shoot();
        s_flash = 0.15f;
    }
    marks();
    return n;
}

}
