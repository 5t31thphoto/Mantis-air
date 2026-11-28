// The cockpits. Each is the cockpits sheet's view of that aircraft (tools/art/cockpit.py: windows cut out, the
// windscreen stretched taller so there is sky to fly by, the panel at the sheet's scale), at the screen's full
// 320 x 240 in the display's overlay, so its frame and gauges stay sharp over the doubled 160 x 120 world.
//
// The art is decoded once (lit by the cabin's light: relit as the day turns) into a base copy; each frame only
// the instruments' rectangles are restored from it and drawn again. The instruments live in the art's own bezels
// and screens: the six-pack and the Cub's pair, the Beaver's nav screen and engine bars, the Jet's glass panel
// (attitude, speed and height, nav, engines, the radio), the Fighter's radar, stores and nav screens and the
// head-up display on its combiner glass. The radio's line runs along the roof; the throttle and the tower's
// button are hardware on the frame.
#include "cockpit.h"
#include "display.h"
#include "sprites.h"
#include "helm.h"
#include "mem.h"
#include "universe.h"
#include "art_cockpit.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace mantis {

static constexpr int OW = 320, OH = 240;
static uint16_t* s_base = nullptr;              // the lit art (PSRAM)
static int s_art = -1;
static uint32_t s_gen = 0;                      // the overlay's generation when we last drew it whole
static float s_lit[3] = {-1, -1, -1};
static float s_lastRelight = -99.f;
static uint16_t* O = nullptr;                   // the overlay, this frame

const uint8_t* cockpitBottom(uint8_t a) { return COCK_ART[a & 3].bottom; }

// ---------------------------------------------------------------------------- colours
static inline uint16_t rgb(float r, float g, float b) {
    const int R = r < 0 ? 0 : r > 255 ? 255 : (int)r, G = g < 0 ? 0 : g > 255 ? 255 : (int)g, B = b < 0 ? 0 : b > 255 ? 255 : (int)b;
    const uint16_t c = (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
    return c ? c : 0x0020;                      // (0 is the window)
}
// the instruments' own colours: backlit (they never go darker than this), the greens glowing
static float s_back = 1.f;
static uint16_t C_FACE, C_FACE2, C_TICK, C_NEEDLE, C_GRN, C_GRN_D, C_AMB, C_RED, C_SKY, C_GND, C_HOR, C_SCREEN, C_KNOB, C_KNOB_L, C_LAMP_OFF;
static void instrumentColours(const CockpitLight& L) {
    const float k = fmaxf(0.62f, fminf(1.f, (L.k[0] + L.k[1] + L.k[2]) / 3.f)) ;
    s_back = k;
    auto lit = [&](float r, float g, float b) { return rgb(r * k, g * k, b * k); };
    C_FACE = lit(16, 20, 21); C_FACE2 = lit(26, 32, 33); C_TICK = lit(196, 214, 204); C_NEEDLE = lit(240, 242, 236);
    C_GRN = rgb(120, 255, 120); C_GRN_D = rgb(34, 96, 48); C_AMB = rgb(255, 190, 50); C_RED = rgb(255, 70, 60);
    C_SKY = lit(58, 168, 186); C_GND = lit(104, 44, 104); C_HOR = lit(230, 240, 236);   // the Basalt Mantis attitude ball
    C_SCREEN = rgb(6, 14, 12); C_KNOB = lit(22, 26, 28); C_KNOB_L = lit(90, 100, 104); C_LAMP_OFF = lit(40, 46, 44);
}

// ---------------------------------------------------------------------------- the art
// Where the world shows: every row above the panel's highest point (top), and below it the side windows, as a few
// rectangles (from the art's per-column table). Everything else is the panel: sent once, then only what changes.
static int s_top = 240, s_nWin = 0;
static DisplayRect s_win[6];
static void windows(int a) {
    const uint8_t* b = COCK_ART[a & 3].bottom;
    int lo = 120;
    for (int x = 0; x < 160; x++) if (b[x] < lo) lo = b[x];
    s_top = lo * 2; s_nWin = 0;
    for (int x = 0; x < 160;) {
        if (b[x] <= lo) { x++; continue; }
        int x1 = x, hi = lo;
        while (x1 < 160 && b[x1] > lo) { if (b[x1] > hi) hi = b[x1]; x1++; }
        if (s_nWin < 6) s_win[s_nWin++] = {(int16_t)(x * 2), (int16_t)s_top, (int16_t)((x1 - x) * 2), (int16_t)(hi * 2 - s_top)};
        else { DisplayRect& r = s_win[5]; const int e = x1 * 2; r.w = (int16_t)(e - r.x); if (hi * 2 - s_top > r.h) r.h = (int16_t)(hi * 2 - s_top); }
        x = x1;
    }
}
static void decode(int a, const CockpitLight& L) {
    windows(a);
    uint16_t lut[64];
    for (int i = 0; i < 64; i++) {
        const uint8_t* c = COCK_PAL[i];
        if (!i) { lut[i] = 0; continue; }
        if (c[3]) lut[i] = rgb(c[0] * fmaxf(L.k[0], 0.9f), c[1] * fmaxf(L.k[1], 0.9f), c[2] * fmaxf(L.k[2], 0.9f));   // glowing
        else lut[i] = rgb(c[0] * L.k[0], c[1] * L.k[1], c[2] * L.k[2]);
    }
    const CockArt& A = COCK_ART[a & 3];
    for (int y = 0; y < OH; y++) {
        const uint8_t* p = A.runs + A.row[y];
        uint16_t* d = s_base + y * OW;
        for (int x = 0; x < OW;) { const int n = *p++; const uint16_t c = lut[*p++]; for (int k = 0; k < n && x < OW; k++) d[x++] = c; }
    }
}
// what changed this frame (screen rectangles), for the display to send: the panel itself stays on the LCD
static DisplayRect s_dirty[24]; static int s_nDirty = 0; static bool s_dirtyAll = false;
static void dirty(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 320) x1 = 320;
    if (y1 > 240) y1 = 240;
    if (x0 >= x1 || y0 >= y1) return;
    if (s_nDirty == 24) { s_dirtyAll = true; return; }
    s_dirty[s_nDirty++] = {(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}
static void restore(int x0, int y0, int x1, int y1) {
    dirty(x0, y0, x1, y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > OW) x1 = OW;
    if (y1 > OH) y1 = OH;
    if (x0 >= x1) return;
    for (int y = y0; y < y1; y++) memcpy(O + y * OW + x0, s_base + y * OW + x0, (x1 - x0) * 2);
}

// ---------------------------------------------------------------------------- drawing (clipped to a box)
static int cx0 = 0, cy0 = 0, cx1 = OW, cy1 = OH;
static void clip(int x0, int y0, int x1, int y1) { cx0 = x0; cy0 = y0; cx1 = x1; cy1 = y1; }
static void noclip() { clip(0, 0, OW, OH); }
static inline void pset(int x, int y, uint16_t c) { if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) O[y * OW + x] = c; }
static void fill(int x0, int y0, int x1, int y1, uint16_t c) { for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) pset(x, y, c); }
static void frame(int x0, int y0, int x1, int y1, uint16_t c) {
    for (int x = x0; x < x1; x++) { pset(x, y0, c); pset(x, y1 - 1, c); }
    for (int y = y0; y < y1; y++) { pset(x0, y, c); pset(x1 - 1, y, c); }
}
static void line(float x0, float y0, float x1, float y1, uint16_t c) {
    const int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int k = 0; k <= n; k++) { const float t = (float)k / n; pset((int)lroundf(x0 + (x1 - x0) * t), (int)lroundf(y0 + (y1 - y0) * t), c); }
}
static void disc(float cx, float cy, float r, uint16_t c) {
    for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); y++) for (int x = (int)floorf(cx - r); x <= (int)ceilf(cx + r); x++)
        if ((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy) <= r * r) pset(x, y, c);
}
static void text(int x, int y, const char* s, uint16_t c, int sc = 1, uint16_t outline = 0) {
    // a character at a time through the 3x5 font (any length: long lines run off the clip)
    static uint8_t tmp[(4 * 3 + 2) * (5 * 3 + 2)];
    char one[2] = {0, 0};
    for (const char* p = s; *p; p++, x += 4 * sc) {
        if (x + 4 * sc < cx0 || x - 1 > cx1) continue;
        one[0] = *p;
        const int w = 3 * sc + 2, h = 5 * sc + 2;
        if (sc > 3) return;
        memset(tmp, 0, sizeof tmp);
        font3(tmp, w, h, 1, 1, one, 1, outline ? 2 : 0, sc);
        for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
            const uint8_t v = tmp[yy * w + xx];
            if (v == 1) pset(x - 1 + xx, y - 1 + yy, c);
            else if (v == 2) pset(x - 1 + xx, y - 1 + yy, outline);
        }
    }
}
static void textC(int cx, int y, const char* s, uint16_t c, int sc = 1, uint16_t outline = 0) { text(cx - font3Width(s, sc) / 2, y, s, c, sc, outline); }
static void textR(int rx, int y, const char* s, uint16_t c, int sc = 1, uint16_t outline = 0) { text(rx - font3Width(s, sc), y, s, c, sc, outline); }

// ---------------------------------------------------------------------------- round gauges
// angles: 0 up, clockwise (radians)
static void face(int cx, int cy, int r) { disc(cx + 0.5f, cy + 0.5f, r + 0.4f, C_FACE); }
static void ticks(int cx, int cy, int r, int n, float a0, float a1, int major = 0) {
    for (int k = 0; k <= n; k++) {
        const float a = a0 + (a1 - a0) * k / n, s = sinf(a), c = cosf(a);
        const bool big = major && k % major == 0;
        const float r0 = big ? r - 3.f : r - 1.6f;
        line(cx + 0.5f + s * r0, cy + 0.5f - c * r0, cx + 0.5f + s * (r - 0.6f), cy + 0.5f - c * (r - 0.6f), C_TICK);
    }
}
static void hand(int cx, int cy, float a, float len, uint16_t c, float tail = 0.f) {
    const float s = sinf(a), co = cosf(a);
    line(cx + 0.5f - s * tail, cy + 0.5f + co * tail, cx + 0.5f + s * len, cy + 0.5f - co * len, c);
}
static void hub(int cx, int cy) { pset(cx, cy, C_NEEDLE); }
static void arcBand(int cx, int cy, float r, float a0, float a1, uint16_t c) {
    for (float a = a0; a <= a1; a += 0.05f) pset((int)lroundf(cx + 0.5f + sinf(a) * r - 0.5f), (int)lroundf(cy + 0.5f - cosf(a) * r - 0.5f), c);
}

struct Inst { const ViewInfo* v; float t, wayRel; bool wayOn; };

static void gASI(const Inst& I, int cx, int cy, int r, float vne) {
    face(cx, cy, r);
    const float top = vne * 1.15f, A0 = -2.6f, A1 = 2.6f;
    auto ang = [&](float mph) { const float k = mph / top; return A0 + (A1 - A0) * (k < 0 ? 0 : k > 1 ? 1 : k); };
    arcBand(cx, cy, r - 2.2f, ang(vne * 0.3f), ang(vne * 0.8f), C_GRN_D);
    arcBand(cx, cy, r - 2.2f, ang(vne * 0.8f), ang(vne), C_AMB);
    ticks(cx, cy, r, 10, A0, A1, 2);
    hand(cx, cy, ang(I.v->ias * 2.237f), r - 1.5f, C_NEEDLE, 2.f);
    hub(cx, cy);
}
static void gALT(const Inst& I, int cx, int cy, int r) {
    face(cx, cy, r);
    ticks(cx, cy, r, 10, 0.f, 6.2831853f * 0.9f, 1);
    const float ft = I.v->y * 3.2808f;
    hand(cx, cy, fmodf(ft, 10000.f) / 10000.f * 6.2831853f, r * 0.5f, C_TICK);
    hand(cx, cy, fmodf(ft, 1000.f) / 1000.f * 6.2831853f, r - 1.5f, C_NEEDLE, 2.f);
    hub(cx, cy);
}
static void gVSI(const Inst& I, int cx, int cy, int r) {
    face(cx, cy, r);
    const float A0 = -1.5708f - 2.4f, A1 = -1.5708f + 2.4f;
    ticks(cx, cy, r, 8, A0, A1, 4);
    float fpm = I.v->vs * 196.85f; fpm = fpm < -2000.f ? -2000.f : fpm > 2000.f ? 2000.f : fpm;
    hand(cx, cy, -1.5708f + fpm / 2000.f * 2.4f, r - 1.5f, C_NEEDLE, 2.f);
    hub(cx, cy);
}
static void ball(const Inst& I, int x0, int y0, int x1, int y1, float pxPerRad, bool ladder) {   // the attitude: sky, ground, the bars
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float sr = sinf(I.v->roll), cr = cosf(I.v->roll), po = I.v->pitch * pxPerRad;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        const float yy = -dx * sr + dy * cr;                 // the horizon turns against the roll
        pset(x, y, fabsf(yy + po) < 0.6f ? C_HOR : (yy < -po ? C_SKY : C_GND));
    }
    if (ladder) for (int k = -2; k <= 2; k++) if (k) {        // rungs every 10 degrees
        const float off = -po - k * 0.1745f * pxPerRad, hw = (k & 1) ? 2.5f : 4.5f;
        const float mx = cx - off * sr, my = cy + off * cr;
        line(mx - hw * cr, my - hw * sr, mx + hw * cr, my + hw * sr, C_HOR);
    }
}
static void gATT(const Inst& I, int cx, int cy, int r) {
    clip(cx - r, cy - r, cx + r + 1, cy + r + 1);
    face(cx, cy, r);
    // the ball inside the bezel's circle
    const float sr = sinf(I.v->roll), cr = cosf(I.v->roll), po = fmaxf(-0.6f, fminf(0.6f, I.v->pitch)) * r * 1.6f;
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
        if (x * x + y * y > (r - 0.6f) * (r - 0.6f)) continue;
        const float yy = -x * sr + y * cr;
        pset(cx + x, cy + y, fabsf(yy + po) < 0.55f ? C_HOR : (yy < -po ? C_SKY : C_GND));
    }
    for (int k = -1; k <= 1; k += 2) {                  // pitch bars at 10 degrees
        const float off = -po - k * 0.1745f * r * 1.6f;
        line(cx + 0.5f - 2.5f * cr - off * sr, cy + 0.5f - 2.5f * sr + off * cr, cx + 0.5f + 2.5f * cr - off * sr, cy + 0.5f + 2.5f * sr + off * cr, C_HOR);
    }
    for (int x = -r + 3; x <= r - 3; x++) if (abs(x) > 1 && abs(x) < r - 4) pset(cx + x, cy, C_AMB);   // the little aircraft
    pset(cx, cy, C_AMB); pset(cx, cy + 1, C_AMB);
    noclip();
}
static void compassRose(const Inst& I, int cx, int cy, int r, bool letters) {
    const float hdg = I.v->yaw + 1.5707963f;            // compass: north up the card
    static const char* CARD[4] = {"N", "E", "S", "W"};
    for (int k = 0; k < 12; k++) {
        const float a = k * 0.5235988f - hdg, s = sinf(a), c = cosf(a);
        if (letters && k % 3 == 0) text((int)lroundf(cx + s * (r - 4.5f)) - 1, (int)lroundf(cy - c * (r - 4.5f)) - 2, CARD[k / 3], k == 0 ? C_AMB : C_TICK);
        else line(cx + 0.5f + s * (r - 2.f), cy + 0.5f - c * (r - 2.f), cx + 0.5f + s * (r - 0.6f), cy + 0.5f - c * (r - 0.6f), C_TICK);
    }
    pset(cx, cy - r + 1, C_AMB); pset(cx, cy - r + 2, C_AMB);   // the lubber line
}
static void wayArrow(const Inst& I, int cx, int cy, float len) {
    if (!I.wayOn) return;
    const float a = I.wayRel, s = sinf(a), c = cosf(a);
    line(cx + 0.5f - s * len * 0.5f, cy + 0.5f + c * len * 0.5f, cx + 0.5f + s * len, cy + 0.5f - c * len, C_GRN);
    const float hx = cx + 0.5f + s * len, hy = cy + 0.5f - c * len;
    line(hx, hy, hx - sinf(a + 0.55f) * 3.f, hy + cosf(a + 0.55f) * 3.f, C_GRN);
    line(hx, hy, hx - sinf(a - 0.55f) * 3.f, hy + cosf(a - 0.55f) * 3.f, C_GRN);
}
static void gHDG(const Inst& I, int cx, int cy, int r) {
    face(cx, cy, r);
    compassRose(I, cx, cy, r, r >= 9);
    wayArrow(I, cx, cy, r - 3.f);
}
static void gTURN(const Inst& I, int cx, int cy, int r) {   // the turn coordinator: the little wings bank with you
    face(cx, cy, r);
    ticks(cx, cy, r, 2, -1.5708f - 0.35f, -1.5708f + 0.35f);
    ticks(cx, cy, r, 2, 1.5708f - 0.35f, 1.5708f + 0.35f);
    const float a = fmaxf(-0.6f, fminf(0.6f, I.v->roll)), s = sinf(a), c = cosf(a), w = r - 2.f;
    line(cx + 0.5f - c * w, cy + 0.5f - s * w, cx + 0.5f + c * w, cy + 0.5f + s * w, C_NEEDLE);
    pset(cx, cy - 1, C_NEEDLE);
    // the ball
    const float slip = fmaxf(-1.f, fminf(1.f, I.v->roll * 0.3f));
    disc(cx + 0.5f + slip * 3.f, cy + r * 0.55f + 0.5f, 1.2f, C_TICK);
}
static void gRPM(const Inst& I, int cx, int cy, int r) {
    face(cx, cy, r);
    const float A0 = -2.3f, A1 = 2.3f;
    ticks(cx, cy, r, 6, A0, A1);
    hand(cx, cy, A0 + (A1 - A0) * I.v->rpm01, r - 1.2f, C_NEEDLE);
}

// ---------------------------------------------------------------------------- screens
static void screen(int x0, int y0, int x1, int y1) { dirty(x0, y0, x1, y1); fill(x0, y0, x1, y1, C_SCREEN); clip(x0, y0, x1, y1); }
static void lamp(int cx, int cy, int r, bool on, uint16_t c) { dirty(cx - r - 1, cy - r - 1, cx + r + 2, cy + r + 2); disc(cx + 0.5f, cy + 0.5f, r, on ? c : C_LAMP_OFF); if (on) pset(cx, cy, rgb(255, 255, 240)); }

static void sNav(const Inst& I, int x0, int y0, int x1, int y1) {          // the Beaver's: where to, how far, the arrow
    screen(x0, y0, x1, y1);
    const ViewInfo& v = *I.v;
    char t[24];
    if (v.navOk && v.navName) { char n[8]; snprintf(n, sizeof n, "%.6s", v.navName); text(x0 + 2, y0 + 2, n, C_GRN); }
    else text(x0 + 2, y0 + 2, "NAV", C_GRN_D);
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2 + 1;
    wayArrow(I, cx, cy, 7.f);
    if (v.navOk) { snprintf(t, sizeof t, "%.1f", v.navDist / 1609.f); textC(cx, y1 - 7, t, C_GRN); }
    if (v.crates > 0) { snprintf(t, sizeof t, "C%d", v.crates); text(x1 - 9, y0 + 9, t, C_AMB); }
    noclip();
}
static void sDigits(int x0, int y0, int x1, int y1, const char* s, uint16_t c) {   // a seven-segment-ish window
    screen(x0, y0, x1, y1);
    const int sc = (y1 - y0) >= 12 && font3Width(s, 2) <= x1 - x0 - 2 ? 2 : 1;
    textC((x0 + x1) / 2, (y0 + y1) / 2 - (5 * sc) / 2, s, c, sc);
    noclip();
}
static void bar(int x0, int y0, int x1, int y1, float k, uint16_t c) {
    dirty(x0, y0, x1, y1);
    fill(x0, y0, x1, y1, C_SCREEN);
    const int h = (int)lroundf((y1 - y0 - 2) * (k < 0 ? 0 : k > 1 ? 1 : k));
    fill(x0 + 1, y1 - 1 - h, x1 - 1, y1 - 1, c);
}
static void sPFD(const Inst& I, int x0, int y0, int x1, int y1) {          // the Jet's attitude screen
    screen(x0, y0, x1, y1);
    ball(I, x0 + 1, y0 + 1, x1 - 1, y1 - 1, (y1 - y0) * 1.4f, true);
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    for (int x = -8; x <= 8; x++) if (abs(x) > 2) pset(cx + x, cy, C_AMB);
    pset(cx, cy, C_AMB);
    // the roll pointer
    const float a = I.v->roll;
    pset(cx + (int)lroundf(sinf(-a) * 10.f), y0 + 3, C_NEEDLE);
    noclip();
}
static void sTapes(const Inst& I, int x0, int y0, int x1, int y1) {         // speed, height, climb as numbers
    screen(x0, y0, x1, y1);
    const ViewInfo& v = *I.v;
    char t[16];
    text(x0 + 2, y0 + 2, "SPD", C_GRN_D);
    snprintf(t, sizeof t, "%d", (int)(v.ias * 1.944f + 0.5f)); textR(x1 - 2, y0 + 8, t, C_GRN);   // knots, the jet's unit
    text(x0 + 2, y0 + 15, "ALT", C_GRN_D);
    snprintf(t, sizeof t, "%d", (int)(v.y * 3.2808f / 10.f + 0.5f) * 10); textR(x1 - 2, y0 + 21, t, C_GRN);
    snprintf(t, sizeof t, "%+d", (int)(v.vs * 196.85f / 100.f) * 100); textR(x1 - 2, y0 + 29, t, v.vs < -7.f ? C_AMB : C_GRN);
    noclip();
}
static void sND(const Inst& I, int x0, int y0, int x1, int y1) {            // nav: the rose, the arrow, how far
    screen(x0, y0, x1, y1);
    const int cx = (x0 + x1) / 2, r = (x1 - x0) / 2 - 1, cy = y0 + 2 + r;
    compassRose(I, cx, cy, r, true);
    wayArrow(I, cx, cy, r - 4.f);
    if (I.v->navOk) { char t[16]; snprintf(t, sizeof t, "%.1f", I.v->navDist / 1852.f); text(x0 + 1, y1 - 6, t, C_GRN); }
    noclip();
}
static void sEICAS(const Inst& I, int x0, int y0, int x1, int y1) {         // engines, flaps, gear, the autopilot
    screen(x0, y0, x1, y1);
    const ViewInfo& v = *I.v;
    bar(x0 + 3, y0 + 3, x0 + 7, y1 - 12, v.rpm01, v.rpm01 > 0.97f ? C_AMB : C_GRN);
    bar(x0 + 9, y0 + 3, x0 + 13, y1 - 12, v.rpm01, v.rpm01 > 0.97f ? C_AMB : C_GRN);
    text(x0 + 3, y1 - 10, "N1", C_GRN_D);
    char t[12];
    snprintf(t, sizeof t, "F%d", (int)lroundf(v.flap * 3.f)); text(x0 + 16, y0 + 3, t, C_GRN);
    if (v.retract) text(x0 + 16, y0 + 10, v.gearDown ? "DN" : "UP", v.gearDown ? C_GRN : C_TICK);
    if (v.ap) text(x0 + 16, y0 + 17, "AP", C_GRN);
    if (v.brake && v.onGround) text(x0 + 15, y0 + 24, "BRK", C_AMB);
    noclip();
}
// the radio's last call, wrapped into a screen and rolled up a line at a time
static void sRadio(const Inst& I, int x0, int y0, int x1, int y1) {
    screen(x0, y0, x1, y1);
    const ViewInfo& v = *I.v;
    const int cols = (x1 - x0 - 2) / 4, rows = (y1 - y0 - 2) / 6;
    if (v.radioText && v.radioA > 0.05f && cols > 2) {
        static char lines[24][16]; int n = 0;
        char buf[96]; snprintf(buf, sizeof buf, "%s: %s", v.radioWho ? v.radioWho : "", v.radioText);
        const char* p = buf;
        while (*p && n < 24) {
            while (*p == ' ') p++;
            int len = (int)strlen(p); if (len <= cols) { snprintf(lines[n++], 16, "%s", p); break; }
            int cut = cols; while (cut > 0 && p[cut] != ' ') cut--;
            if (cut == 0) cut = cols;
            snprintf(lines[n++], 16, "%.*s", cut < 15 ? cut : 15, p); p += cut;
        }
        const int first = n > rows ? (int)fminf((float)(n - rows), fmaxf(0.f, fmodf(I.t * 0.6f, (float)(n - rows + 3)) - 1.f)) : 0;
        for (int k = 0; k < rows && first + k < n; k++) text(x0 + 2, y0 + 2 + k * 6, lines[first + k], k == 0 && first == 0 ? C_AMB : C_GRN);
    } else {
        text(x0 + 2, y0 + 2, "COM1", C_GRN_D);
        if (v.cleared) text(x0 + 2, y0 + 9, "CLRD", C_GRN);
        else if (v.towerBtn) text(x0 + 2, y0 + 9, "TWR", C_AMB);
    }
    noclip();
}
static void sRadar(const Inst& I, int x0, int y0, int x1, int y1) {          // the Fighter's: what's around, nose up
    screen(x0, y0, x1, y1);
    const ViewInfo& v = *I.v;
    const int cx = (x0 + x1) / 2, cy = y1 - 3;
    const float range = 9000.f, k = (y1 - y0 - 5) / range;
    for (int q = 1; q <= 2; q++) { const float rr = (y1 - y0 - 5) * q * 0.5f; for (float a = -1.0f; a <= 1.0f; a += 0.06f) pset((int)lroundf(cx + sinf(a) * rr), (int)lroundf(cy - cosf(a) * rr), C_GRN_D); }
    line((float)cx, (float)cy, cx - sinf(1.0f) * (y1 - y0), cy - cosf(1.0f) * (y1 - y0), C_GRN_D);
    line((float)cx, (float)cy, cx + sinf(1.0f) * (y1 - y0), cy - cosf(1.0f) * (y1 - y0), C_GRN_D);
    // the sweep
    const float sw = sinf(I.t * 1.7f) * 1.0f;
    line((float)cx, (float)cy, cx + sinf(sw) * (y1 - y0 - 5), cy - cosf(sw) * (y1 - y0 - 5), C_GRN_D);
    const float fx = cosf(v.yaw), fz = sinf(v.yaw), rx = -sinf(v.yaw), rz = cosf(v.yaw);
    auto put = [&](float wx, float wz, int& sx, int& sy) {
        const float dx = wx - v.x, dz = wz - v.z;
        sx = cx + (int)lroundf((dx * rx + dz * rz) * k); sy = cy - (int)lroundf((dx * fx + dz * fz) * k);
        return sx >= x0 + 1 && sx < x1 - 1 && sy >= y0 + 1 && sy < y1 - 1;
    };
    for (int i = 0; i < g_markN; i++) {
        const Marker& m = g_marks[i]; int sx, sy;
        if (!put(m.x, m.z, sx, sy)) continue;
        switch (m.kind) {
            case MK_BOGEY: fill(sx - 1, sy - 1, sx + 2, sy + 2, C_RED); break;
            case MK_BOGEY_LOCK: fill(sx - 1, sy - 1, sx + 2, sy + 2, C_RED); frame(sx - 3, sy - 3, sx + 4, sy + 4, C_AMB); break;
            case MK_GTARGET: frame(sx - 1, sy - 1, sx + 2, sy + 2, C_AMB); break;
            case MK_GTARGET_LOCK: fill(sx - 1, sy - 1, sx + 2, sy + 2, C_AMB); frame(sx - 3, sy - 3, sx + 4, sy + 4, C_AMB); break;
            case MK_MISSILE: pset(sx, sy, C_NEEDLE); break;
            case MK_BEACON: case MK_ZONE: case MK_GATE_NEXT: pset(sx, sy, C_GRN); pset(sx - 1, sy, C_GRN); pset(sx + 1, sy, C_GRN); pset(sx, sy - 1, C_GRN); pset(sx, sy + 1, C_GRN); break;
            default: break;
        }
    }
    if (v.navOk) { int sx, sy; if (put(v.navX, v.navZ, sx, sy)) { pset(sx, sy - 1, C_GRN); pset(sx - 1, sy, C_GRN); pset(sx + 1, sy, C_GRN); pset(sx, sy + 1, C_GRN); } }
    fill(cx - 1, cy - 1, cx + 2, cy + 1, C_NEEDLE);   // you
    noclip();
}
static void sStores(const Inst& I, int x0, int y0, int x1, int y1) {         // the Fighter's: what's left on the rails
    screen(x0, y0, x1, y1);
    const ViewInfo& v = *I.v;
    char t[16]; int y = y0 + 2;
    if (v.aam >= 0) { snprintf(t, sizeof t, "AAM %d", v.aam); text(x0 + 2, y, t, v.aam ? C_GRN : C_AMB); y += 7; }
    if (v.agm >= 0) { snprintf(t, sizeof t, "AGM %d", v.agm); text(x0 + 2, y, t, v.agm ? C_GRN : C_AMB); y += 7; }
    if (v.bombs >= 0) { snprintf(t, sizeof t, "BMB %d", v.bombs); text(x0 + 2, y, t, v.bombs ? C_GRN : C_AMB); y += 7; }
    // the hull and the engine, as bars along the bottom
    const float hull = v.hull >= 0.f ? v.hull : 1.f;
    text(x0 + 2, y1 - 13, "HUL", C_GRN_D);
    fill(x0 + 15, y1 - 13, x1 - 2, y1 - 8, C_SCREEN);
    fill(x0 + 15, y1 - 12, x0 + 15 + (int)((x1 - x0 - 17) * hull), y1 - 9, hull < 0.35f ? C_RED : hull < 0.7f ? C_AMB : C_GRN);
    text(x0 + 2, y1 - 6, "ENG", C_GRN_D);
    fill(x0 + 15, y1 - 5, x0 + 15 + (int)((x1 - x0 - 17) * v.rpm01), y1 - 2, v.rpm01 > 0.7f ? C_AMB : C_GRN);
    noclip();
}
// the Fighter's head-up display on its combiner glass: the pitch ladder, the waterline, speed, height, heading
static void sHUD(const Inst& I, int x0, int y0, int x1, int y1) {
    dirty(x0, y0, x1, y1);
    clip(x0, y0, x1, y1);
    const ViewInfo& v = *I.v;
    const uint16_t K = rgb(8, 34, 18);                   // (a dark edge: the symbols read against a bright sky)
    auto L2 = [&](float ax, float ay, float bx, float by) { line(ax + 1, ay + 1, bx + 1, by + 1, K); line(ax, ay, bx, by, C_GRN); };
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f + 2.f;
    const float ppr = 52.f;                               // pixels a radian (the glass's own scale)
    const float sr = sinf(v.roll), cr = cosf(v.roll);
    for (int d = -30; d <= 30; d += 10) {                 // the ladder every 10 degrees: below the horizon, dashed
        const float off = (v.pitch - d * 0.0174533f) * ppr;   // how far below the waterline that rung is
        const float mx = cx - off * sr, my = cy + off * cr;
        const float hw = d ? 7.f : 14.f, gap = 3.f;
        for (int side = -1; side <= 1; side += 2) {
            const float ax = mx + side * gap * cr, ay = my + side * gap * sr, bx = mx + side * hw * cr, by = my + side * hw * sr;
            if (d < 0) { const int n = 4; for (int q = 0; q < n; q += 2) L2(ax + (bx - ax) * q / n, ay + (by - ay) * q / n, ax + (bx - ax) * (q + 1) / n, ay + (by - ay) * (q + 1) / n); }
            else L2(ax, ay, bx, by);
        }
    }
    // the waterline (where the nose points)
    L2(cx - 6, cy, cx - 2, cy); L2(cx + 2, cy, cx + 6, cy); L2(cx - 2, cy, cx, cy + 2); L2(cx, cy + 2, cx + 2, cy);
    // the waypoint's caret along the top
    if (I.wayOn) { const float a = fmaxf(-1.f, fminf(1.f, I.wayRel)); const int wx = (int)lroundf(cx + a * (x1 - x0) * 0.45f); L2((float)wx, (float)y0 + 1, (float)wx - 2, (float)y0 + 3); L2((float)wx, (float)y0 + 1, (float)wx + 2, (float)y0 + 3); }
    char t[12];
    snprintf(t, sizeof t, "%d", (int)(v.ias * 1.944f + 0.5f)); text(x0 + 1, y0 + 5, t, C_GRN, 1, K);
    snprintf(t, sizeof t, "%d", (int)(v.y * 3.2808f / 10.f) * 10); textR(x1 - 1, y0 + 5, t, C_GRN, 1, K);
    float hdg = (v.yaw + 1.5707963f) * 57.29578f; hdg -= 360.f * floorf(hdg / 360.f);
    snprintf(t, sizeof t, "%03d", (int)(hdg + 0.5f) % 360); textC((int)cx, y1 - 6, t, C_GRN, 1, K);
    if (v.stall > 0.5f && ((int)(I.t * 6.f) & 1)) textC((int)cx, (int)cy + 6, "AOA", C_RED);
    noclip();
}
// a whiskey compass on top of the glare shield (the Cub's): the card's numbers slide past the lubber line
static void wWhiskey(const Inst& I, int x0, int y0, int x1, int y1) {
    dirty(x0, y0, x1, y1);
    fill(x0, y0, x1, y1, C_KNOB); frame(x0, y0, x1, y1, C_KNOB_L);
    clip(x0 + 2, y0 + 2, x1 - 2, y1 - 2);
    fill(x0 + 2, y0 + 2, x1 - 2, y1 - 2, C_FACE2);
    float hdg = (I.v->yaw + 1.5707963f) * 57.29578f; hdg -= 360.f * floorf(hdg / 360.f);
    const float cx = (x0 + x1) * 0.5f, ppd = 0.9f;       // pixels a degree
    for (int d = ((int)hdg / 15 - 3) * 15; d <= hdg + 50; d += 15) {
        const float x = cx + (d - hdg) * ppd;
        const int dd = ((d % 360) + 360) % 360;
        if (dd % 90 == 0) { static const char* C[4] = {"N", "E", "S", "W"}; textC((int)x, y0 + 3, C[dd / 90], dd ? C_TICK : C_AMB); }
        else if (dd % 30 == 0) { char t[4]; snprintf(t, sizeof t, "%d", dd / 10); textC((int)x, y0 + 3, t, C_TICK); }
        else line(x, (float)y0 + 5, x, (float)y0 + 7, C_TICK);
    }
    line(cx, (float)y0 + 2, cx, (float)y1 - 3, C_AMB);
    if (I.wayOn) { const float a = fmaxf(-0.9f, fminf(0.9f, I.wayRel)) * 57.29578f; const int wx = (int)lroundf(cx + a * ppd); pset(wx, y1 - 3, C_GRN); pset(wx - 1, y1 - 3, C_GRN); pset(wx + 1, y1 - 3, C_GRN); pset(wx, y1 - 4, C_GRN); }
    noclip();
}

// ---------------------------------------------------------------------------- the frame's hardware
static void radioStrip(const Inst& I, int a) {
    const ViewInfo& v = *I.v;
    restore(0, 0, 294, 15);
    if (!(v.radioText && v.radioA > 0.05f)) return;
    clip(0, 0, 294, 15);
    char line_[96]; snprintf(line_, sizeof line_, "%s: %s", v.radioWho ? v.radioWho : "", v.radioText);
    const int w = font3Width(line_, 2);
    int x = 6;
    if (w > OW - 40) x = 6 - (int)fmodf(I.t * 36.f, (float)(w - (OW - 40) + 80)) + 40, x = x > 6 ? 6 : x;
    const uint16_t bg = rgb(4, 10, 8);
    (void)a;
    text(x, 3, line_, C_GRN, 2, bg);
    noclip();
}
static void throttleLever(const Inst& I) {
    const int x0 = 302, x1 = 312, y0 = SLIDER_Y0, y1 = SLIDER_Y1;
    restore(296, y0 - 8, OW, y1 + 9);
    fill(x0 + 3, y0, x1 - 3, y1, C_KNOB); frame(x0 + 2, y0 - 1, x1 - 2, y1 + 1, C_KNOB_L);
    for (int k = 0; k <= 4; k++) { const int y = y0 + (y1 - y0) * k / 4; fill(x0 - 1, y, x0 + 2, y + 1, C_TICK); }
    const int ty = y1 - (int)(I.v->throttle * (y1 - y0) + 0.5f);
    fill(x0 - 4, ty - 5, x1 + 4, ty + 6, C_KNOB);
    frame(x0 - 4, ty - 5, x1 + 4, ty + 6, C_KNOB_L);
    fill(x0 - 2, ty - 1, x1 + 2, ty + 2, I.v->throttle > 0.95f ? C_AMB : C_GRN);
}
static void towerButton(const Inst& I) {
    restore(TWR_X0 - 2, TWR_Y0 - 2, TWR_X1 + 2, TWR_Y1 + 2);
    if (!I.v->towerBtn) return;
    fill(TWR_X0, TWR_Y0, TWR_X1, TWR_Y1, C_KNOB);
    frame(TWR_X0, TWR_Y0, TWR_X1, TWR_Y1, C_KNOB_L);
    frame(TWR_X0 + 2, TWR_Y0 + 2, TWR_X1 - 2, TWR_Y1 - 2, C_GRN_D);
    const bool blink = ((int)(I.t * 2.f) & 1);
    disc(TWR_X0 + 9.5f, (TWR_Y0 + TWR_Y1) * 0.5f, 2.5f, blink ? C_GRN : C_GRN_D);
    text(TWR_X0 + 17, (TWR_Y0 + TWR_Y1) / 2 - 5, "TOWER", C_GRN, 2);
}

// ---------------------------------------------------------------------------- the four panels
static void panelCub(const Inst& I) {
    const ViewInfo& v = *I.v;
    restore(130, 161, 191, 186);
    gASI(I, 142, 173, 10, 110.f);
    gALT(I, 178, 173, 10);
    restore(108, 167, 122, 181); gRPM(I, 115, 174, 6);
    restore(198, 167, 212, 181); gVSI(I, 205, 174, 6);
    restore(88, 193, 112, 203);
    lamp(93, 198, 2, v.cleared, C_GRN);
    lamp(100, 198, 2, v.stall > 0.5f && ((int)(I.t * 6.f) & 1), C_RED);
    lamp(107, 198, 2, v.brake && v.onGround, C_AMB);
    restore(140, 124, 181, 138); wWhiskey(I, 141, 125, 180, 137);
}
static void panelBeaver(const Inst& I) {
    const ViewInfo& v = *I.v;
    restore(68, 168, 138, 219);
    gASI(I, 79, 179, 9, 150.f); gATT(I, 102, 178, 9); gALT(I, 126, 179, 9);
    gTURN(I, 78, 208, 9); gHDG(I, 102, 208, 9); gVSI(I, 126, 208, 9);
    sNav(I, 149, 169, 176, 198);
    char t[8]; float hdg = (v.yaw + 1.5707963f) * 57.29578f; hdg -= 360.f * floorf(hdg / 360.f);
    snprintf(t, sizeof t, "%03d", (int)(hdg + 0.5f) % 360); sDigits(150, 202, 173, 210, t, C_GRN);
    snprintf(t, sizeof t, "F%d", (int)lroundf(v.flap * 3.f)); sDigits(191, 169, 203, 177, t, C_GRN);
    bar(190, 192, 195, 217, v.rpm01, C_GRN); bar(200, 192, 205, 217, v.rpm01, C_GRN);   // the two engines
    restore(48, 210, 58, 220); lamp(52, 215, 2, v.stall > 0.5f && ((int)(I.t * 6.f) & 1), C_RED);
}
static void panelJet(const Inst& I) {
    const ViewInfo& v = *I.v;
    sPFD(I, 70, 169, 95, 205);
    sTapes(I, 100, 169, 124, 205);
    sND(I, 149, 169, 174, 205);
    sEICAS(I, 197, 169, 222, 205);
    sRadio(I, 229, 169, 253, 205);
    char t[8];
    float hdg = (v.yaw + 1.5707963f) * 57.29578f; hdg -= 360.f * floorf(hdg / 360.f);
    snprintf(t, sizeof t, "%03d", (int)(hdg + 0.5f) % 360); sDigits(167, 130, 185, 143, t, v.ap ? C_GRN : C_GRN_D);
    snprintf(t, sizeof t, "%d", (int)(v.ias * 1.944f + 0.5f)); sDigits(118, 129, 133, 143, t, C_GRN);
    snprintf(t, sizeof t, "%d", (int)(v.y * 3.2808f / 100.f + 0.5f)); sDigits(190, 129, 204, 143, t, C_GRN);
    restore(130, 170, 143, 185);
    lamp(136, 177, 4, true, v.stall > 0.5f ? (((int)(I.t * 6.f) & 1) ? C_RED : C_LAMP_OFF) : (v.cleared ? C_GRN : C_GRN_D));
    restore(130, 190, 144, 204); fill(131, 191, 143, 203, C_SCREEN);
    if (v.retract) for (int k = 0; k < 3; k++) disc(133.5f + k * 4.f, 197.5f, 1.3f, v.gearDown ? C_GRN : C_LAMP_OFF);
}
static void panelFighter(const Inst& I) {
    const ViewInfo& v = *I.v;
    restore(137, 56, 182, 108); sHUD(I, 139, 60, 180, 106);
    sRadar(I, 94, 155, 130, 190);
    sND(I, 142, 159, 177, 194);
    sStores(I, 191, 155, 227, 190);
    sRadio(I, 146, 211, 172, 236);
    restore(55, 192, 66, 204); lamp(60, 198, 3, (v.stall > 0.5f || (v.hull >= 0.f && v.hull < 0.35f)) && ((int)(I.t * 5.f) & 1), C_RED);
}

// ---------------------------------------------------------------------------- each frame
void cockpitDraw(const ViewInfo& v, const CockpitLight& L, float t, float wayRel, bool wayOn) {
    const int a = v.aircraft & 3;
    if (!s_base) { s_base = (uint16_t*)memSlow(OW * OH * 2); if (!s_base) return; }
    O = overlayBuffer();
    if (!O) return;
    float dl = 0.f;
    for (int k = 0; k < 3; k++) dl = fmaxf(dl, fabsf(L.k[k] - s_lit[k]));
    const bool relight = dl > 0.04f && t - s_lastRelight > 1.5f;
    if (a != s_art || overlayGeneration() != s_gen || relight) {
        decode(a, L);
        memcpy(O, s_base, OW * OH * 2);
        for (int k = 0; k < 3; k++) s_lit[k] = L.k[k];
        s_lastRelight = t; s_art = a;
        overlayEnable(true);
        overlayAllRows();                                // (the radio and the HUD draw in rows the art leaves clear)
        s_gen = overlayGeneration();
    }
    instrumentColours(L);
    noclip();
    Inst I = {&v, t, wayRel, wayOn};
    switch (a) {
        case 0: panelCub(I); break;
        case 1: panelBeaver(I); break;
        case 2: panelJet(I); break;
        default: panelFighter(I); break;
    }
    throttleLever(I);
    towerButton(I);
    radioStrip(I, a);
    // what the display sends: the world's rows and windows, and what changed on the panel below them
    if (!s_dirtyAll) {
        DisplayRect r[32]; int n = 0;
        for (int i = 0; i < s_nWin; i++) r[n++] = s_win[i];
        for (int i = 0; i < s_nDirty && n < 32; i++) {
            DisplayRect d = s_dirty[i];
            if (d.y + d.h <= s_top) continue;                         // (inside the rows that go anyway)
            if (d.y < s_top) { d.h = (int16_t)(d.y + d.h - s_top); d.y = (int16_t)s_top; }
            r[n++] = d;
        }
        displaySetRegions(s_top, r, n);
    }
    s_nDirty = 0; s_dirtyAll = false;
}
void cockpitOff() {
    if (s_art < 0 || overlayGeneration() != s_gen) { s_art = -1; return; }   // someone else has it now
    overlayClear(); overlayEnable(false);
    s_art = -1;
}

}
