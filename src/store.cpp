#pragma GCC optimize("Os")
#include "store.h"
#include "mem.h"
#include "progress.h"
#include "aircraft.h"
#include "gigs.h"
#include "journey.h"
#include "display.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

namespace mantis {

enum : uint8_t { PG_PLANES = 0, PG_TUNE, PG_GIGS };
static bool s_open = false, s_dirty = true;
static uint8_t s_page = PG_GIGS;
static char s_msg[40];
static Airfield s_at;
static Gig* s_board = nullptr; static int s_boardN = 0;   // (PSRAM: three gigs, made when the hangar first opens)
static Gig* board() { if (!s_board) s_board = (Gig*)memSlow(3 * sizeof(Gig)); return s_board; }

void storeOpen(const Airfield& at, uint32_t salt) {
    s_open = true; s_dirty = true; s_page = PG_GIGS; s_msg[0] = 0; s_at = at;
    s_boardN = board() ? gigBoard(at, g_prog.rank, g_prog.current, salt, s_board) : 0;
}
bool storeActive() { return s_open; }

static void rect(uint16_t* b, int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++) { if (yy < 0 || yy >= 240) continue; for (int xx = x; xx < x + w; xx++) if (xx >= 0 && xx < 320) b[yy * 320 + xx] = c; }
}
static void frame(uint16_t* b, int x, int y, int w, int h, uint16_t c) { rect(b, x, y, w, 2, c); rect(b, x, y + h - 2, w, 2, c); rect(b, x, y, 2, h, c); rect(b, x + w - 2, y, 2, h, c); }
static void text(uint16_t* b, int x, int y, const char* s, uint16_t c, int sc = 1) { uiText(b, 320, 240, x, y, s, c, sc, true); }
static void textC(uint16_t* b, int cx, int y, const char* s, uint16_t c, int sc = 1) { text(b, cx - uiTextWidth(s, sc) / 2, y, s, c, sc); }

struct Btn { int x, y, w, h; };
static const Btn TAB[3] = {{10, 30, 96, 22}, {112, 30, 96, 22}, {214, 30, 96, 22}};
static const Btn BUY = {214, 120, 92, 26}, FLY = {182, 204, 128, 28}, BUSH = {10, 204, 166, 28}, CANCEL = {10, 204, 166, 28};
static bool armory() { return s_at.military && g_prog.current == A_FIGHTER; }   // the air base's ordnance counter
static Btn upgBtn(int k) { Btn u = armory() ? Btn{230, 60 + k * 28, 76, 24} : Btn{230, 62 + k * 40, 76, 30}; return u; }
static Btn garageBtn(int i) { Btn g = {14 + i * 74, 166, 68, 28}; return g; }
static Btn gigBtn(int i) { Btn g = {10, 58 + i * 48, 300, 44}; return g; }
static bool hit(const Btn& b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static const uint16_t BG = 0x10A4, PANEL = 0x18C7;
static uint16_t c565(int r, int g, int b) { return rgb565(r, g, b); }

static void drawPlanes(uint16_t* b) {
    const uint16_t white = c565(240, 244, 250), grey = c565(110, 116, 130), amber = c565(255, 190, 50), teal = c565(0, 200, 180), lime = c565(150, 255, 70), mag = c565(200, 80, 180);
    char t[48];
    int nxt = progressNextLocked();
    rect(b, 10, 58, 300, 92, PANEL);
    if (nxt >= 0) {
        const AircraftSpec& S = aircraftSpec((uint8_t)nxt);
        text(b, 18, 64, S.name, white, 2);
        text(b, 18, 82, S.blurb1, grey, 1);
        text(b, 18, 93, S.blurb2, mag, 1);
        const char* lab[3] = {"SPEED", "AGILITY", "ROUGH FIELDS"};
        const float val[3] = {S.statSpeed, S.statAgility, S.statRough};
        for (int i = 0; i < 3; i++) {
            text(b, 18, 107 + i * 11, lab[i], grey, 1);
            rect(b, 100, 108 + i * 11, 100, 6, c565(40, 46, 60));
            rect(b, 100, 108 + i * 11, (int)(100 * val[i]), 6, i == 2 ? amber : teal);
        }
        snprintf(t, sizeof t, "%d MI IN THE %s", (int)S.priceMiles, aircraftSpec(S.priceAircraft).shortName);
        text(b, 214, 64, "COSTS", grey, 1); text(b, 214, 75, t, amber, 1);
        snprintf(t, sizeof t, "YOU HAVE %.1f", g_prog.wallet[S.priceAircraft]);
        text(b, 214, 88, t, white, 1);
        const bool can = progressCanBuy(nxt);
        rect(b, BUY.x, BUY.y, BUY.w, BUY.h, can ? c565(30, 120, 60) : c565(50, 54, 62));
        textC(b, BUY.x + BUY.w / 2, BUY.y + 6, "BUY", can ? white : grey, 2);
    } else {
        textC(b, 160, 90, "EVERY AIRCRAFT IS YOURS", lime, 1);
        textC(b, 160, 104, "THE SKY IS YOURS", white, 1);
    }
    text(b, 14, 155, "YOUR HANGAR", grey, 1);
    for (int a = 0; a < A_COUNT; a++) {
        if (!(g_prog.owned & (1u << a))) continue;
        Btn g = garageBtn(a);
        rect(b, g.x, g.y, g.w, g.h, a == g_prog.current ? c565(0, 90, 80) : c565(36, 42, 56));
        textC(b, g.x + g.w / 2, g.y + 10, aircraftSpec((uint8_t)a).shortName, a == g_prog.current ? lime : white, 1);
    }
    if (g_prog.bushUnlocked) {
        rect(b, BUSH.x, BUSH.y, BUSH.w, BUSH.h, g_prog.bushOn ? c565(120, 80, 20) : c565(36, 42, 56));
        textC(b, BUSH.x + BUSH.w / 2, BUSH.y + 10, g_prog.bushOn ? "BUSH MODE: ON" : "BUSH MODE: OFF", amber, 1);
    }
}
static void drawTune(uint16_t* b) {
    const uint16_t white = c565(240, 244, 250), grey = c565(110, 116, 130), amber = c565(255, 190, 50), teal = c565(0, 200, 180), lime = c565(150, 255, 70);
    char t[48];
    if (!armory()) { snprintf(t, sizeof t, "TUNING THE %s", aircraftSpec(g_prog.current).shortName); text(b, 14, 58 - 2, t, white, 1); }
    const bool arm = armory();
    static const char* const WN[2] = {"AGM PODS", "BOMB RACKS"};
    static const char* const WW[2] = {"+2 GROUND-ATTACK MISSILES", "+2 IRON BOMBS (4 BASE)"};
    for (int k = 0; k < UPG_N + (arm ? 2 : 0); k++) {
        Btn u = upgBtn(k);
        const bool w = k >= UPG_N;
        rect(b, 12, u.y - 2, 296, arm ? 27 : 34, w ? c565(46, 30, 30) : PANEL);
        text(b, 18, u.y + 2, w ? WN[k - UPG_N] : upgradeName(g_prog.current, k), white, 1);
        text(b, 18, u.y + 13, w ? WW[k - UPG_N] : upgradeWhat(g_prog.current, k), grey, 1);
        int lv = w ? progressWeaponLevel(k - UPG_N) : progressUpgradeLevel(k);
        for (int p = 0; p < 3; p++) rect(b, 150 + p * 12, u.y + 3, 9, 6, p < lv ? (w ? c565(255, 90, 50) : teal) : c565(40, 46, 60));
        int c = w ? progressWeaponCost(k - UPG_N) : progressUpgradeCost(k);
        if (c > 0) {
            snprintf(t, sizeof t, "$%d", c);
            text(b, 196, u.y + 3, t, amber, 1);
            bool can = g_prog.cash >= (uint32_t)c;
            rect(b, u.x, u.y, u.w, u.h, can ? c565(30, 120, 60) : c565(50, 54, 62));
            textC(b, u.x + u.w / 2, u.y + u.h / 2 - 4, w ? "BUY" : "UPGRADE", can ? white : grey, 1);
        } else text(b, 196, u.y + 3, "MAXED", lime, 1);
    }
    if (!arm && g_prog.current == A_FIGHTER) text(b, 18, 186, "ORDNANCE IS SOLD AT THE AIR BASES", c565(255, 90, 50), 1);
}
static void drawGigs(uint16_t* b) {
    const uint16_t white = c565(240, 244, 250), grey = c565(110, 116, 130), amber = c565(255, 190, 50), lime = c565(150, 255, 70), teal = c565(0, 200, 180);
    char t[48];
    if (gigActive()) {
        const Gig& g = gigCurrent();
        rect(b, 10, 58, 300, 140, PANEL);
        text(b, 18, 64, "ON THE JOB", teal, 1);
        text(b, 18, 78, g.title, white, 2);
        snprintf(t, sizeof t, "FOR %s", g.client); text(b, 18, 98, t, grey, 1);
        const Waypoint* w = gigWaypoint();
        snprintf(t, sizeof t, "NEXT: %s", w ? w->label : "-"); text(b, 18, 112, t, amber, 1);
        snprintf(t, sizeof t, "PAYS $%d  +%d REP", g.pay, g.xp); text(b, 18, 126, t, lime, 1);
        text(b, 18, 150, "FOLLOW THE ARROW ON THE COMPASS", grey, 1);
        rect(b, CANCEL.x, CANCEL.y, CANCEL.w, CANCEL.h, c565(120, 40, 40));
        textC(b, CANCEL.x + CANCEL.w / 2, CANCEL.y + 10, "CANCEL THE GIG", white, 1);
        return;
    }
    if (!s_boardN) { textC(b, 160, 110, "NO WORK TODAY", grey, 1); return; }
    for (int i = 0; i < s_boardN; i++) {
        const Gig& g = s_board[i];
        Btn r = gigBtn(i);
        rect(b, r.x, r.y, r.w, r.h, PANEL);
        frame(b, r.x, r.y, r.w, r.h, c565(40, 60, 80));
        text(b, r.x + 6, r.y + 5, g.title, white, 1);
        snprintf(t, sizeof t, "$%d", g.pay); text(b, r.x + r.w - 6 - uiTextWidth(t, 2), r.y + 5, t, lime, 2);
        snprintf(t, sizeof t, "TO %s  %.0f MI", g.dest, g.distMi + 0.5f); text(b, r.x + 6, r.y + 17, t, amber, 1);
        snprintf(t, sizeof t, "%s%s", g.client, g.limit > 0.f ? "  (TIMED)" : ""); text(b, r.x + 6, r.y + 29, t, grey, 1);
    }
}
static void draw() {
    uint16_t* b = overlayBuffer();
    if (!b) return;
    const uint16_t teal = c565(0, 200, 180), lime = c565(150, 255, 70), white = c565(240, 244, 250), grey = c565(110, 116, 130);
    rect(b, 0, 0, 320, 240, BG);
    frame(b, 2, 2, 316, 236, teal);
    char t[48];
    text(b, 10, 8, "MANTIS AERO", lime, 2);
    snprintf(t, sizeof t, "%s", s_at.name); text(b, 310 - uiTextWidth(t, 1), 8, t, white, 1);
    snprintf(t, sizeof t, "%s  $%u", rankName(g_prog.rank), (unsigned)g_prog.cash); text(b, 310 - uiTextWidth(t, 1), 18, t, grey, 1);
    const char* TABS[3] = {"AIRCRAFT", armory() ? "ARMORY" : "TUNING", armory() ? "MISSIONS" : "GIGS"};
    for (int k = 0; k < 3; k++) {
        rect(b, TAB[k].x, TAB[k].y, TAB[k].w, TAB[k].h, k == s_page ? c565(0, 90, 80) : c565(30, 36, 48));
        textC(b, TAB[k].x + TAB[k].w / 2, TAB[k].y + 7, TABS[k], k == s_page ? lime : white, 1);
    }
    if (s_page == PG_PLANES) drawPlanes(b);
    else if (s_page == PG_TUNE) drawTune(b);
    else drawGigs(b);
    rect(b, FLY.x, FLY.y, FLY.w, FLY.h, c565(0, 100, 130));
    textC(b, FLY.x + FLY.w / 2, FLY.y + 7, "FLY", white, 2);
    if (s_msg[0]) textC(b, 160, 192, s_msg, lime, 1);
    overlayEnable(true);
}

bool storeUpdate(bool tap, int tx, int ty, float now, bool& changed, bool& gig) {
    changed = false; gig = false;
    if (!s_open) return false;
    if (tap) {
        for (int k = 0; k < 3; k++) if (hit(TAB[k], tx, ty)) { s_page = (uint8_t)k; s_msg[0] = 0; }
        if (hit(FLY, tx, ty)) { s_open = false; overlayEnable(false); return false; }
        if (s_page == PG_PLANES) {
            int nxt = progressNextLocked();
            if (nxt >= 0 && hit(BUY, tx, ty) && progressBuy(nxt)) {
                changed = true; snprintf(s_msg, sizeof s_msg, "SOLD! ENJOY THE %s", aircraftSpec((uint8_t)nxt).shortName);
                s_boardN = gigBoard(s_at, g_prog.rank, g_prog.current, g_prog.gigsDone, s_board);
            }
            for (int a = 0; a < A_COUNT; a++)
                if ((g_prog.owned & (1u << a)) && hit(garageBtn(a), tx, ty) && g_prog.current != a) {
                    g_prog.current = (uint8_t)a; changed = true; progressSave();
                    s_boardN = gigBoard(s_at, g_prog.rank, g_prog.current, g_prog.gigsDone, s_board);   // its own kind of work
                }
            if (g_prog.bushUnlocked && hit(BUSH, tx, ty)) { g_prog.bushOn = !g_prog.bushOn; aircraftSetBushMode(g_prog.bushOn); progressSave(); }
        } else if (s_page == PG_TUNE) {
            for (int k = 0; k < UPG_N; k++)
                if (hit(upgBtn(k), tx, ty) && progressBuyUpgrade(k)) { changed = true; snprintf(s_msg, sizeof s_msg, "%s UP TO LEVEL %d", upgradeName(g_prog.current, k), progressUpgradeLevel(k)); }
            if (armory()) for (int w = 0; w < 2; w++)
                if (hit(upgBtn(UPG_N + w), tx, ty) && progressBuyWeapon(w)) { changed = true; snprintf(s_msg, sizeof s_msg, "%s LOADED", w ? "BOMB RACKS" : "AGM PODS"); }
        } else {
            if (gigActive()) { if (hit(CANCEL, tx, ty)) { gigCancel(); snprintf(s_msg, sizeof s_msg, "GIG CANCELLED"); } }
            else for (int i = 0; i < s_boardN; i++) if (hit(gigBtn(i), tx, ty)) {
                gigAccept(s_board[i], now); gig = true;
                snprintf(s_msg, sizeof s_msg, "TAKEN: %s", s_board[i].title);
            }
        }
        s_dirty = true;
    }
    if (s_dirty) { draw(); s_dirty = false; }
    return true;
}

}
