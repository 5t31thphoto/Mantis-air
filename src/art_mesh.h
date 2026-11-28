// The aircraft models (tools/art/model_*.py, generated into art_mesh_<name>.h): built from the sprite sheets'
// measurements and textured from the same cards (art_cards.h), so they add only their geometry.
#pragma once
#include <stdint.h>
namespace mantis {
// a face: its corners; the card it takes its texels from (0 top, 1 side, 2 front, 3 rear; 4: a solid colour, col);
// its corners' texels on that card's mip 0, x16; its outward normal, x127 (the body frame: forward, up, right)
struct MeshTri { uint16_t i[3]; uint8_t card, col; int16_t uv[3][2]; int8_t n[3]; };
// a model: vertices (forward, up, right; metres, for its span; the origin where the game's pose is, gearH above the
// wheels); props: hub (forward, up, right), radius, metres
struct MeshArt { uint16_t nV, nT; const float* v; const MeshTri* t; float span, gearH; uint8_t nProp; float prop[2][4]; };
}
