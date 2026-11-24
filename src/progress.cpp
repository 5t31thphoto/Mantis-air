#pragma GCC optimize("Os")
#include "progress.h"
#include "journey.h"
#include "storage.h"
#include <string.h>
#ifndef HOST
#include <Preferences.h>
#endif

namespace mantis {

Progress g_prog;
static float s_unsaved = 0.f;
static const uint32_t MAGIC = 0x4D464C59u;   // "MFLY"
struct Blob { uint32_t magic; Progress p; };

// ======================= THE CAREER LADDER =======================
static const char* const RANK[R_N] = {"AMATEUR", "FREELANCER", "CONTRACTOR", "COMMERCIAL", "FIGHTER PILOT"};
static const uint32_t RANK_XP[R_N] = {0, 250, 800, 2000, 4200};
static const int UPG_COST[3] = {300, 800, 1800};
// =================================================================
const char* rankName(uint8_t r) { return RANK[r < R_N ? r : R_N - 1]; }
uint32_t rankXp(uint8_t r) { return RANK_XP[r < R_N ? r : R_N - 1]; }

static void fresh() {
    memset(&g_prog, 0, sizeof(g_prog));
    g_prog.owned = 1u << A_CUB; g_prog.current = A_CUB; g_prog.view = 0;
    g_prog.cash = 150;
}

void progressInit() {
    fresh();
    Blob b;
    memset(&b, 0, sizeof(b));
    bool got = storageOk() && storageLoad(&b, sizeof(b)) && b.magic == MAGIC;
#ifndef HOST
    if (!got) {
        Preferences pr;
        if (pr.begin("mflight", true)) {
            memset(&b, 0, sizeof(b));
            size_t n = pr.getBytes("prog", &b, sizeof(b));
            got = n == sizeof(b) && b.magic == MAGIC;
            pr.end();
        }
    }
#endif
    if (got) g_prog = b.p;
    for (int a = 0; a < A_COUNT; a++) g_prog.upg[a] &= 0x3F;
    if (g_prog.rank >= R_N) g_prog.rank = R_N - 1;
    aircraftSetUpgrades(g_prog.upg);
    aircraftSetBushMode(g_prog.bushOn);
    g_prog.owned |= 1u << A_CUB;
    if (g_prog.current >= A_COUNT || !(g_prog.owned & (1u << g_prog.current))) g_prog.current = A_CUB;
}

void progressSave() {
    Blob b; b.magic = MAGIC; b.p = g_prog;
    if (!storageSave(&b, sizeof(b))) {
#ifndef HOST
        Preferences pr;                                  // no card: the flash, rarely
        if (pr.begin("mflight", false)) { pr.putBytes("prog", &b, sizeof(b)); pr.end(); }
#endif
    }
    s_unsaved = 0.f;
}

void progressFly(float m) {
    if (m <= 0.f) return;
    float mi = m / MILE;
    g_prog.miles[g_prog.current] += mi;
    g_prog.wallet[g_prog.current] += mi;
    s_unsaved += mi;
    if (s_unsaved > 10.f) progressSave();              // every 10 miles of flying
}

int progressNextLocked() {
    for (int a = 0; a < A_COUNT; a++) if (!(g_prog.owned & (1u << a))) return a;
    return -1;
}
bool progressCanBuy(int a) {
    if (a < 0 || a >= A_COUNT || (g_prog.owned & (1u << a))) return false;
    const AircraftSpec& S = aircraftSpec((uint8_t)a);
    return g_prog.wallet[S.priceAircraft] >= S.priceMiles;
}
bool progressBuy(int a) {
    if (!progressCanBuy(a)) return false;
    const AircraftSpec& S = aircraftSpec((uint8_t)a);
    g_prog.wallet[S.priceAircraft] -= S.priceMiles;
    g_prog.owned |= (uint8_t)(1u << a);
    g_prog.current = (uint8_t)a;
    progressSave();
    return true;
}
void progressCash(int amount) {
    if (amount > 0) g_prog.cash = g_prog.cash + (uint32_t)amount > 9999999u ? 9999999u : g_prog.cash + (uint32_t)amount;
    else if (amount < 0) g_prog.cash = (uint32_t)(-amount) > g_prog.cash ? 0 : g_prog.cash - (uint32_t)(-amount);
}
bool progressXp(int pts) {
    if (pts <= 0) return false;
    g_prog.xp += (uint32_t)pts;
    bool up = false;
    while (g_prog.rank + 1 < R_N && g_prog.xp >= RANK_XP[g_prog.rank + 1]) { g_prog.rank++; up = true; }
    return up;
}
int progressUpgradeLevel(int kind) {
    if (kind < 0 || kind >= UPG_N) return 0;
    return (g_prog.upg[g_prog.current % A_COUNT] >> (kind * 2)) & 3;
}
int progressUpgradeCost(int kind) {
    int l = progressUpgradeLevel(kind);
    return l >= 3 ? 0 : UPG_COST[l] * (1 + g_prog.current);
}
bool progressBuyUpgrade(int kind) {
    int c = progressUpgradeCost(kind);
    if (c <= 0 || g_prog.cash < (uint32_t)c) return false;
    g_prog.cash -= (uint32_t)c;
    uint8_t& u = g_prog.upg[g_prog.current % A_COUNT];
    u = (uint8_t)((u & ~(3u << (kind * 2))) | ((progressUpgradeLevel(kind) + 1) << (kind * 2)));
    aircraftSetUpgrades(g_prog.upg);
    progressSave();
    return true;
}
const char* upgradeName(uint8_t a, int kind) {
    static const char* const N[A_COUNT][UPG_N] = {{"HANDLING", "STOL KIT", "ENGINE"}, {"HANDLING", "BIG FLAPS", "ENGINE"},
                                                 {"HANDLING", "HIGH LIFT", "ENGINES"}, {"HANDLING", "MISSILES", "THRUST"}};
    return N[a % A_COUNT][kind % UPG_N];
}
const char* upgradeWhat(uint8_t a, int kind) {
    static const char* const W[A_COUNT][UPG_N] = {{"QUICKER ROLL", "SLOWER STALL", "MORE THRUST"}, {"QUICKER ROLL", "SHORTER RUNS", "MORE THRUST"},
                                                 {"QUICKER ROLL", "SLOWER APPROACH", "MORE THRUST"}, {"QUICKER ROLL", "+2 HEAT SEEKERS", "MORE THRUST"}};
    return W[a % A_COUNT][kind % UPG_N];
}
int progressWeaponLevel(int w) { return (g_prog.wpn >> (w * 2)) & 3; }
int progressWeaponCost(int w) { int l = progressWeaponLevel(w); return l >= 3 ? 0 : (w == 0 ? 1500 : 900) * (l + 1); }
bool progressBuyWeapon(int w) {
    int c = progressWeaponCost(w);
    if (c <= 0 || g_prog.cash < (uint32_t)c) return false;
    g_prog.cash -= (uint32_t)c;
    g_prog.wpn = (uint8_t)((g_prog.wpn & ~(3u << (w * 2))) | ((progressWeaponLevel(w) + 1) << (w * 2)));
    progressSave();
    return true;
}
void progressStores(int& aam, int& agm, int& bombs) {
    aam = 2 + 2 * ((g_prog.upg[A_FIGHTER] >> 2) & 3);
    agm = 2 * progressWeaponLevel(0);
    bombs = 4 + 2 * progressWeaponLevel(1);
}
bool progressAllBiomes() { return (g_prog.visited & ((1u << B_COUNT) - 1)) == ((1u << B_COUNT) - 1); }

}
