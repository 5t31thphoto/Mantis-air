#pragma once
#include <stdint.h>
#include "render.h"

namespace mantis {

// The cockpits: the sheet's art (art_cockpit.h) at the screen's full 320 x 240 in the display's overlay, over the
// doubled world; the live instruments drawn into the art's own bezels and screens (the HUD is the panel).
struct CockpitLight { float k[3]; float night; };   // the cabin's light on the art (1: as drawn), 0..1 night

void cockpitDraw(const ViewInfo& v, const CockpitLight& L, float t, float wayRel, bool wayOn);   // each cockpit frame
void cockpitOff();                                   // any other view: hands the overlay back
const uint8_t* cockpitBottom(uint8_t aircraft);      // per world column (160): the first row the cockpit covers to the bottom

}
