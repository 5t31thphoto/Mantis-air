#pragma once
#include <stdint.h>

// Aircraft: one parameter table, one flight model. Lift from a lift curve that stalls, induced and parasite drag,
// prop thrust that fades with airspeed (or a jet that spools), and a fly-by-wire style control law that suits a tilt
// yoke: the yoke commands roll and pitch RATES, so a resting hand holds the attitude you set (the way SpaceMantis
// and Seadiver fly). Turns are coordinated for you (auto-rudder), the nose holds the horizon in a gentle bank, and
// the physics underneath is real: too slow and too much angle of attack still stalls, flaps still lower the stall,
// and the trainer still wants to level its wings by itself. On the ground: wheels (or floats), brakes, tiller
// steering with the tilt, a taildragger's three-point attitude, and touchdowns judged by their sink rate.
// A new unlockable is a new row in AIRCRAFT (aircraft.cpp) plus its art.

namespace mantis {

enum AircraftId : uint8_t { A_CUB = 0, A_BEAVER, A_JET, A_FIGHTER, A_COUNT };
enum : uint8_t { GEAR_TAIL = 0, GEAR_TRI, GEAR_FLOATS };
enum : uint8_t { ENG_BOXER4T = 0, ENG_RADIAL, ENG_V8, ENG_TURBINE };   // the sound models (engine.cpp)
enum : uint8_t { CRASH_NONE = 0, CRASH_TERRAIN, CRASH_WATER, CRASH_TREE, CRASH_HARD, CRASH_GEAR, CRASH_WING, CRASH_SHOT };

struct AircraftSpec {
    const char* name;          // shown in the hangar
    const char* shortName;     // the garage list
    const char* blurb1;        // what it is
    const char* blurb2;        // a hint (never the trick itself)
    uint8_t gear; bool retract;
    // airframe
    float mass, wingArea, span;
    float clAlpha, cl0, clMax, alphaStall, cd0, kInduced;   // per radian; alphaStall in radians (clean)
    float flapCL, flapCD;      // full flaps
    // power
    float thrust;              // static thrust (N), sea level
    float vProp;               // airspeed where the prop's thrust is gone (0: a jet, which spools instead)
    // handling (full yoke)
    float rollRate, pitchRate, levelAssist;   // rad/s, rad/s, how strongly it rolls its wings level hands-off
    // speeds (m/s): for the instruments, the stall horn and the bars
    float vStall, vCruise, vNe;
    // on the ground
    float gearH;               // CG above the wheels / float keels
    float groundPitch;         // the three-point attitude (taildraggers), rad
    float roughField;          // 0..1: how happy it is off a prepared strip (1 bush plane, 0 the jet)
    // cameras
    float eyeUp, eyeFwd, chaseBack;
    // sound
    uint8_t engine; float rpmIdle, rpmMax; uint8_t blades;
    // hangar
    uint8_t priceAircraft; float priceMiles;   // paid in miles flown in priceAircraft
    float statSpeed, statAgility, statRough;   // 0..1 bars
    uint8_t paint[3][3];       // body, trim, accent (until the sprite sheet arrives)
};

const AircraftSpec& aircraftSpec(uint8_t id);
// the hangar's tuning: per aircraft, bits 0-1 handling, 2-3 lift (STOL), 4-5 power (levels 0-3)
enum : uint8_t { UPG_HANDLING = 0, UPG_LIFT, UPG_POWER, UPG_N };
void aircraftSetUpgrades(const uint8_t* levels);
void aircraftSetBushMode(bool on);         // the endgame unlock: off-airport landings for every aircraft

struct AircraftInput {
    float roll;        // -1..1, + = roll right
    float pitch;       // -1..1, + = nose up (pull)
    float throttle;    // 0..1
    float brake;       // 0..1 (wheel brakes; on the ground)
    uint8_t flaps;     // 0..3 detents
    float windX, windZ, gustY;   // the air's own motion (m/s)
};

struct AircraftState {
    float x, y, z;               // CG, world (y up)
    float vx, vy, vz;            // velocity, world
    float fx, fy, fz, ux, uy, uz, rx, ry, rz;   // body axes: forward, up, right
    float yaw, pitch, roll;      // derived (rad): heading (east 0, grows right), nose up +, right wing down +
    float p, q, r;               // body rates: roll, pitch, yaw
    float power;                 // engine power 0..1 (spooled)
    float rpm, rpm01;
    float flap;                  // 0..1 actual
    bool gearDown;
    bool onGround, onWater;
    float ias, aoa, beta, gLoad, vs, agl, groundSpeed, speed;
    float stall;                 // stall warning 0..1
    float surfY;                 // the surface under the aircraft (land or water)
    uint8_t mat, surf;
    uint8_t crash;               // CRASH_* (set once; the game resets)
    float rollOut;               // seconds rolling on the ground since the last touchdown
    // per-frame events (read and cleared by the game)
    float evTouch;               // touched down: sink rate (m/s)
    float evBump, evSplash, evLift;   // evLift: wheels left the ground
    float buffet;                // stall buffet / turbulence felt 0..1
};

// on the ground at (x, z) facing yaw, engine idling (or in the air at alt metres above the ground at cruise)
void aircraftReset(AircraftState& s, uint8_t id, float x, float z, float yaw);
void aircraftResetAir(AircraftState& s, uint8_t id, float x, float z, float yaw, float alt);
void aircraftStep(AircraftState& s, uint8_t id, const AircraftInput& in, float dt);
// Collision with a round obstacle (tree trunk, rock, hangar post) at (ox, oz) radius rad and height top: on the
// ground it stops you; in the air (below its top) it's a crash. Returns the impact speed.
float aircraftHitPost(AircraftState& s, uint8_t id, float ox, float oz, float rad, float baseY, float top);

}
