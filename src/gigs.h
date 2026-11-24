#pragma once
#include <stdint.h>
#include "aircraft.h"
#include "airfield.h"

// Gigs: the jobs on the board in every MANTIS AERO hangar. Real pilot work, airfield to airfield: ferry flights,
// sightseeing, bush supply drops, mail runs, fishing charters, aerial survey, fire fighting, search and rescue,
// charters and cargo, medevac, air races and (at the top of the ladder) intercepts. Every destination is a real place
// in the seeded world (an airfield, a bush camp, a lake), so it is there when you arrive. Each job is a short list
// of waypoints; the HUD's arrow and the panel's nav compass point at the current one.
// Which jobs you're offered depends on your rank (AMATEUR .. FIGHTER PILOT) and on what you're flying: each
// aircraft has work only it can do (the Cub's bush camps, the Beaver's lakes and fires, the jet's charters, the
// fighter's races and intercepts).

namespace mantis {

enum : uint8_t { G_FERRY = 0, G_PHOTO, G_SIGHTSEE, G_MAIL, G_MEDEVAC,              // any civil aircraft (photos: any at all)
                 G_BACKCOUNTRY, G_FISHING, G_SUPPLY, G_FIRE, G_SAR, G_SURVEY,        // the Beaver's country (and some for the Cub)
                 G_CHARTER, G_CARGO,                                                 // the jet's airline work
                 G_RACE, G_INTERCEPT, G_BOMBING, G_STRIKE, G_RECON,                  // the fighter's (missions at the air bases)
                 G_N };
enum : uint8_t { W_FLY = 0,        // pass within r of (x, z) between agl0 and agl1 metres above the ground
                 W_LAND_AF,        // land at the airfield in square (ai, aj) and stop
                 W_LAND_WATER,     // land on the water within r of (x, z) and come off the step
                 W_LAND_FIELD,     // land on the bush camp's meadow (x, z) and stop
                 W_SCOOP,          // skim the water within r of (x, z) to fill the tank
                 W_DROP,           // drop the water (A) over the fire at (x, z)
                 W_ORBIT,          // stay within r of (x, z), below agl1, for `hold` seconds (searching)
                 W_GATE,           // fly through the ring at (x, y, z) facing yaw, radius r
                 W_KILL,           // shoot down the bogies round (x, z)
                 W_PHOTO,          // photograph (x, z): photo mode, the target in frame, within r
                 W_SUPPLY,         // drop `hold` crates (A, in the drop view) into the zone (x, z, r)
                 W_BOMB,           // destroy the ground targets round (x, z): bombs, missiles, the cannon
                 W_RECON };        // photograph (x, z) from straight above: the fighter's spy camera (drop view, A)
struct Waypoint {
    uint8_t kind; int16_t ai, aj;
    float x, y, z, r, yaw, agl0, agl1, hold;
    char label[18];
};
constexpr int GIG_WP = 10;
struct Gig {
    uint8_t kind, minRank, aircraftMask;
    char title[22], client[22], dest[18];
    int pay, xp;
    float limit;                    // seconds (0: no limit)
    Waypoint wp[GIG_WP]; uint8_t nwp;
    float distMi;                   // the planned trip
};

const char* gigKindName(uint8_t kind);
// the board at the hangar of airfield `a`: up to 3 jobs for this rank and aircraft (salt: how many you've done)
int  gigBoard(const Airfield& a, uint8_t rank, uint8_t aircraft, uint32_t salt, Gig* out);
// the quest giver for a job (sprites: 0 the dispatcher, 1 the bush guide, 2 the airline captain, 3 the general)
uint8_t gigGiver(uint8_t kind);
void gigAccept(const Gig& g, float time);
void gigCancel();
bool gigActive();
const Gig& gigCurrent();
int  gigStep();                     // the current waypoint's index
const Waypoint* gigWaypoint();     // nullptr: none
// Per frame, after the flight model. Returns an event (GE_*): the game shows the message and pays.
enum : uint8_t { GE_NONE = 0, GE_STEP, GE_DONE, GE_FAILED, GE_INFO };
struct GigEvent { uint8_t kind; int pay, xp; char msg[32], sub[32]; };
// What the game saw this frame for the job:
struct GigInput {
    bool waterDrop;                 // A: the fire bomber's water
    bool cleared;                   // landed where the tower cleared you (an airfield landing only counts with it)
    bool photo; bool framed;        // a photo taken; the target was in it
    float photoX, photoZ;           // the camera's position
    bool recon;                     // the spy camera fired (drop view)
    bool crate; float crateX, crateZ;   // a crate came down here
    int groundAlive;                // the mission's ground targets still standing
};
GigEvent gigUpdate(const AircraftState& s, uint8_t aircraft, float time, float dt, const GigInput& in);
float gigTimeLeft(float time);      // < 0: no limit
float gigComfort();                 // the passengers' comfort 0..1 (charters)
float gigTank();                    // the fire tank 0..1 (Beaver)
// fires burning for the active job (the renderer and the field show their smoke)
int  gigFires(float* xyz, int max);
int  gigCrates();                  // crates still to drop (supply jobs)

}
