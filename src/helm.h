#pragma once
#include <stdint.h>

// Controls. Hold the Core2 like a yoke (the pose you hold at the start is neutral; hold B to take it again):
//   tilt left / right   roll (on the ground: steer)        tip the top away / pull it back   nose down / nose up
//   the slider on the right edge   throttle: drag it (SpaceMantis / Seadiver's)
//   A tap      flaps (Beaver on a fire: drop the water)    A hold   photo mode (the fighter: A held fires the cannon)
//   B tap      view (cockpit, HUD, chase)                  B hold   re-centre the tilt
//   C hold     wheel brakes (on the ground)               C tap    in the air: autopilot (fighter: heat seeker)
//   touch the picture   use what's in front of you (the hangar)     double-tap   in trouble? back in the air
//   hold the picture 1.5 s   mute / unmute                 swipe across / up-down   the radio's dial / volume
// The A/B/C buttons are zones of the touch panel below the picture: touches there are buttons only.

namespace mantis {

constexpr int SLIDER_X = 292, SLIDER_Y0 = 14, SLIDER_Y1 = 160;   // panel pixels
constexpr int TWR_X0 = 4, TWR_Y0 = 30, TWR_X1 = 100, TWR_Y1 = 52;    // the CONTACT TOWER button (panel pixels)

struct HelmEvents {
    bool aTap, aHold, cTap, view, recenter, mute;
    bool tap; int tapX, tapY;      // panel pixels (320x240)
    bool doubleTap;
    int8_t swipeX, swipeY;
};

void  helmInit();
void  helmUpdate(float dt, HelmEvents& ev);
float helmRoll();       // -1 .. 1, + = roll right
float helmPitch();      // -1 .. 1, + = nose up
float helmTilt();       // -1 .. 1, the raw tip (+ = top away): photo mode's tilt
bool  helmADown();      // A held (the fighter's cannon)
bool  helmCDown();      // C held (brakes)
float helmThrottle();   // 0 .. 1 (the slider)
void  helmSetThrottle(float t);
bool  helmSliding();
void  helmRecenter();
void  helmPoll();       // read the touch panel now (between the frame's phases)

}
