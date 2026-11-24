#pragma once
#include <stdint.h>
#include "aircraft.h"

// What you own and what you've earned (Mantis Rally's rules, flown):
//  * aircraft are paid for in miles flown in a particular aircraft (AircraftSpec::priceAircraft / priceMiles);
//  * gigs pay cash (tuning, and the fighter's weapons) and reputation, which climbs the career ladder:
//    AMATEUR -> FREELANCER -> CONTRACTOR -> COMMERCIAL -> FIGHTER PILOT.
// Saved to the SD card (/flight_saves), the flash only without a card.

namespace mantis {

enum : uint8_t { R_AMATEUR = 0, R_FREELANCER, R_CONTRACTOR, R_COMMERCIAL, R_FIGHTER, R_N };
const char* rankName(uint8_t r);
uint32_t rankXp(uint8_t r);              // reputation needed for that rank

struct Progress {
    uint8_t owned;                 // bit per aircraft
    uint8_t current;
    uint8_t view;
    uint8_t radio;                 // bits 0-1 station, 2-4 volume, bit 7 set once saved
    float miles[A_COUNT];          // lifetime odometer per aircraft
    float wallet[A_COUNT];         // unspent miles per aircraft
    uint16_t visited;              // biomes seen
    bool bushUnlocked, bushOn;     // every biome seen: off-airport landings for every aircraft
    uint32_t cash;                 // from gigs: tuning and weapons
    uint8_t upg[A_COUNT];          // tuning per aircraft: bits 0-1 handling, 2-3 lift (fighter: missiles), 4-5 power
    uint32_t xp;                   // reputation
    uint8_t rank;                  // R_*
    uint8_t wpn;                   // the fighter's stores bought at the air bases: bits 0-1 AGMs, 2-3 bombs
    uint8_t pad[2];
    uint16_t gigsDone, kills;
    int16_t homeI, homeJ;          // the airfield you last stopped at (where a new session starts)
    uint32_t photos;
    float bestLanding;             // the gentlest touchdown (fpm), 0 none yet
};
extern Progress g_prog;

void progressInit();               // loads (or starts fresh: the Cub, at the home strip)
void progressSave();
void progressFly(float metres);    // credit the current aircraft; saves now and then
int  progressNextLocked();         // the next aircraft for sale (-1: you own them all)
bool progressCanBuy(int a);
bool progressBuy(int a);
bool progressAllBiomes();
void progressCash(int amount);
// reputation: returns true when it lifts you to a new rank
bool progressXp(int points);
int  progressUpgradeLevel(int kind);           // the current aircraft's level (0-3) in UPG_*
int  progressUpgradeCost(int kind);            // cash for its next level (0: maxed)
bool progressBuyUpgrade(int kind);
const char* upgradeName(uint8_t aircraft, int kind);
const char* upgradeWhat(uint8_t aircraft, int kind);
// the fighter's ordnance (bought at air bases): 0 AGMs, 1 bombs
int  progressWeaponLevel(int w);
int  progressWeaponCost(int w);
bool progressBuyWeapon(int w);
void progressStores(int& aam, int& agm, int& bombs);   // what the fighter carries now

}
