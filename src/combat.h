#pragma once
#include <stdint.h>
#include "aircraft.h"

// The fighter pilot's sky: bogies (enemy jets) that patrol, turn to fight when you come in, break hard when you get on
// their tail and fire back when you're in front of them. Your weapons: the cannon (hold A: tracers, lead your shots)
// and heat seekers (tap C with a target in the lock cone; 2 more with each MISSILES upgrade, rearmed on the ground).

namespace mantis {

struct Bogey { bool alive; float x, y, z, vx, vy, vz, yaw, pitch, roll, hp, t, burstT; uint8_t mode; };
// ground targets (a mission's site): radars, AA guns (they shoot back), fuel depots
enum : uint8_t { GT_RADAR = 0, GT_AA, GT_DEPOT };
struct GroundTarget { bool alive; uint8_t kind; float x, y, z, yaw, fireT; };
// things that fall: bombs (explode on the ground), crates (a parachute opens; where they land is scored)
enum : uint8_t { DROP_BOMB = 0, DROP_CRATE };
struct Drop { bool on; uint8_t kind; float x, y, z, vx, vy, vz, t; bool chute; };
constexpr int GT_N = 8, DROP_N = 6;
struct Missile { bool on; float x, y, z, vx, vy, vz, life; int target; };
constexpr int BOGEY_N = 6, MISSILE_N = 4;

void combatClear();
void combatSpawn(float x, float z, int n, const AircraftState& player);
bool combatEngaged();
int  combatAlive();
struct CombatEvent { int kills, groundKills; float damage; bool fired, noLock, hitOn; bool crateLanded; float crateX, crateZ; };
// weapons: AAM heat seekers (air lock), AGMs (ground lock), bombs; the stores are refilled on the ground
struct Stores { int aam, agm, bombs; };
void combatSetStores(const Stores& full);
const Stores& combatStores();
// secondary: fire what fits (an air lock: AAM; a ground lock: AGM). bomb: release a bomb. crate: drop a crate (Beaver).
CombatEvent combatUpdate(AircraftState& s, float dt, bool guns, bool secondary, bool bomb, bool crate);
void combatSpawnGround(float x, float z, int n);
int  combatGroundAlive();
const GroundTarget* combatGround();
const Drop* combatDrops();
int  combatGroundLock();
// where a bomb (or a crate) released now would land
bool combatPredictImpact(const AircraftState& s, bool crate, float& x, float& z);
const Bogey* combatBogeys();
const Missile* combatMissiles();
int   combatLock();                 // the bogey the seeker sees (-1 none)
float combatHull();                 // 0..1
int   combatMissileCount();
void  combatRearm();
void  combatSetWind(float wx, float wz);   // the steady breeze the parachutes drift in (m/s)

}
