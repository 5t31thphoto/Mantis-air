#pragma once
#include <stdint.h>

namespace mantis {

// 160x120 palette-indexed frame, doubled to the 320x240 panel by display.cpp.
constexpr int FB_W = 160;
constexpr int FB_H = 120;

enum : uint8_t { VIEW_COCKPIT = 0, VIEW_HUD, VIEW_CHASE, VIEW_DROP, VIEW_N };   // DROP: straight down (bombs, crates, the spy camera)

// Everything the renderer needs from the game this frame.
struct ViewInfo {
    uint8_t view, aircraft;
    // the aircraft
    float x, y, z, yaw, pitch, roll;   // CG; heading (east 0, grows right), nose up +, right wing down +
    float fx, fy, fz, ux, uy, uz, rx, ry, rz;   // its axes (the chase model)
    float groundY, agl;
    // instruments
    float ias, vs, throttle, power, rpm01, flap, stall, gLoad;   // m/s, m/s, 0..1 ...
    bool gearDown, retract, onGround, brake, ap;
    float shake;                   // turbulence / buffet on the camera (0..1)
    // navigation: the nearest field
    bool navOk; float navX, navZ, navDist; int navRwy; const char* navName; float navRwyYaw;
    // hud
    const char* msg; float msgA;   // centre message
    const char* sub;               // small line under it
    float hangarA;                 // "MANTIS AERO" near
    const char* radioWho; const char* radioText; float radioA;   // the radio's line on air
    bool towerBtn;                 // the CONTACT TOWER button is up (you may call the field)
    bool cleared;                  // cleared to land at the field ahead
    const char* apMode;            // "AP", "NAV", "APPR" (nullptr: off)
    int aam, agm, bombs, crates;   // stores (the fighter), crates left (supply jobs); -1: not carried
    float hull;                    // 0..1 (the fighter)
    uint8_t giver; const char* giverLine; float giverA;   // a quest giver's portrait and words (0..3)
    bool showFps; int fps;
    // photo mode: the camera is placed by photo.cpp; no HUD, no cockpit (the aircraft is drawn)
    bool photo; float pX, pY, pZ, pYaw, pPitch;
};
extern ViewInfo g_view;


// Things the game puts in the sky for a gig: race gates (a ring facing yaw, radius r) and the target's beacon.
enum : uint8_t { MK_GATE = 0, MK_GATE_NEXT, MK_BEACON, MK_BOGEY, MK_BOGEY_LOCK, MK_MISSILE, MK_ZONE, MK_GTARGET, MK_GTARGET_LOCK, MK_BOMB, MK_CRATE, MK_PIP };
// MK_ZONE: a ring on the ground (r). MK_GTARGET: r = its kind (0 radar, 1 AA gun, 2 depot). MK_CRATE: pitch > 0 its chute is open.
struct Marker { float x, y, z, yaw, r, pitch, roll; uint8_t kind; };
constexpr int MARK_N = 24;
extern Marker g_marks[MARK_N];
extern int g_markN;

void renderInit();
void renderSetAircraft(uint8_t a);              // its paint and its model
void renderFrame();
extern void (*g_renderTick)();   // called between the frame's phases (main: the haptics and the touch panel, finer than once a frame)
// a world point on the framebuffer (160x120). false if behind the camera.
bool renderProject(float wx, float wy, float wz, float& sx, float& sy, float& depth);
void renderSplat(float strength);               // a bug on the windscreen (or a bird)
// Photo capture: the next renderFrame also records which pixels are water (and what kind) for the jig file.
void renderCaptureWater(bool on);
const uint8_t* renderWaterMask();               // 160 x 120: 0 none, 1 still, 2 river, 3 ocean (after a capture frame)
float renderPixelDepth(int x, int y);           // metres (1e9: sky)
int   renderHorizonRow();                       // the horizon's row at the centre (160 x 120)
// The straight-down shot: the land under the aircraft, orthographic, `span` metres across the frame.
void renderTopDown(float span);
const float* renderTopHeights();               // after renderTopDown: each pixel's height (m)
// the last drop-view frame: is world point (x, z) in the picture (inside a margin), and how wide it was (m)
bool  renderDropSees(float wx, float wz, float* span = nullptr);
void renderWater(float strength);               // spray / rain on the glass
const uint8_t*  frameBuffer();
bool            renderDepthFast();       // the depth buffer got internal RAM
extern const char* g_memNote;          // shown under the FPS readout
const uint16_t* framePalette565();       // 4 x 256 (byte-swapped 565): one per 2x2 position, (dy * 2 + dx) * 256
const uint8_t*  framePaletteRGB();
int             frameBob();

}
