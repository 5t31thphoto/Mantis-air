#pragma once
#include <stdint.h>

// Airfields: where the roads were in Mantis Rally. One (or none) in each 6 km square of the world, placed by the
// seed: a runway (paved, grass, dirt or gravel), its verges, an apron and the MANTIS AERO hangar (the dealership,
// with wings). The land is flattened to the field's level and eased back into the hills round it; the runway's
// markings are worked out per pixel from these numbers, so they're crisp at any distance.
// Like the rest of the world the field is dreamt, not stored: its place is fixed by the seed, its level by the land.

namespace mantis {

constexpr float AF_CELL = 6000.f;        // metres: one chance of a field per square

struct Airfield {
    bool valid;
    int i, j;                  // its square
    uint8_t slot;              // (i & 3) | (j & 3) << 2: how the baked cells name it (two fields sharing a slot are 18 km apart)
    float x, z, yaw;           // runway centre, and its direction (one end to the other)
    float len, hw;             // length, half-width (m)
    uint8_t surf;              // SURF_TARMAC / GRAVEL / DIRT / GRASS
    float y;                   // its level (worked out from the land; see airfieldLevel)
    float hx, hz, hyaw;        // the hangar: centre and the way its doors face (toward the runway)
    float side;                // +1: the hangar on the runway's right (looking along yaw), -1 its left
    int num;                   // runway number at the yaw end (heading / 10, 1..36)
    bool military;             // an air base: the fighter's (its hangar sells weapons)
    char name[18];
};

bool airfieldAt(int i, int j, Airfield& a);             // the field in square (i, j), if any (no level yet)
bool airfieldAround(float x, float z, Airfield& a);      // the field whose square holds (x, z) (with its level)
bool airfieldBySlot(uint8_t slot, float nearX, float nearZ, Airfield& a);   // the field with that slot nearest a point
bool airfieldNearest(float x, float z, Airfield& a, float* dist);           // searched over the 3x3 squares round you
float airfieldLevel(Airfield& a);                        // sets a.y (held steady while the field is in use)
// Can this aircraft use this field? (a long enough runway of the right kind; air bases are the fighter's alone)
// why: the tower's reason when it can't
bool airfieldFits(const Airfield& a, uint8_t aircraft, const char** why);
// runway frame: lon along the runway from its centre (+ toward yaw), lat across (+ right of yaw)
inline void airfieldLocal(const Airfield& a, float x, float z, float c, float s, float& lon, float& lat) {
    const float dx = x - a.x, dz = z - a.z; lon = dx * c + dz * s; lat = -dx * s + dz * c;
}
// The land at (x, z) given its natural height h: flattened into the field (or unchanged). mat / surf are set
// inside it (MAT_RUNWAY on the strip and its verges, MAT_LOT on the apron), slot names the field. Returns the height.
float airfieldShape(float x, float z, float h, uint8_t& mat, uint8_t& surf, uint8_t& slot, bool& inside);
// Bush camps: a mown meadow (240 x 50 m) near the corner of some squares, with a tent and a smoke pot when you're
// expected. No runway: only an aircraft that can land off-airport (the Cub, or anyone in bush mode) can use one.
struct Camp { bool valid; int i, j; float x, z, yaw, y; char name[18]; };
bool campAt(int i, int j, Camp& c);                      // (with its level)
void  airfieldTick(float t);                             // the clock that ages the held levels
void  airfieldReset();

}
