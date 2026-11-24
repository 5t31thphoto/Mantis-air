#pragma once
#include <stdint.h>
#include "aircraft.h"

// Aerial photo mode. Hold A (anywhere: in the air the world holds its breath): the screen is a viewfinder.
// Tilt and tip turn and tilt the camera. A takes the photo, C changes the angle, B goes back to flying.
//   angles: the pilot's seat (look all round), off the right wing, off the left wing, head-on, and straight down
// Every shot saves two pictures to /images/flights: what you framed (flight_NNNN.mg) and the land straight below
// the aircraft at that moment (flight_NNNN_below.mg). Each gets a Mantis Gallery jig file beside it
// (.flight_NNNN.mg.jig): the water in the picture (still lake, river or ocean, as the game knows it) as water
// regions, and the depths it knows as parallax planes, so the Gallery can make them move.

namespace mantis {

enum : uint8_t { SHOT_PILOT = 0, SHOT_RIGHT, SHOT_LEFT, SHOT_FRONT, SHOT_DOWN, SHOT_N };

void photoEnter(const AircraftState& s, uint8_t aircraft);
void photoExit();
bool photoActive();
void photoNextAngle();
// per frame, before renderFrame(): the camera from the tilt (roll -1..1 turns, tip -1..1 tilts)
void photoUpdate(float dt, float roll, float tip);
// after renderFrame(): shoot = take the photo now. Returns the photo number, -1 (no card), or 0.
int  photoCompose(bool shoot);

}
