#pragma once
#include <stdint.h>
#include "airfield.h"

// MANTIS AERO: the hangar at every airfield. Three pages: AIRCRAFT (the next one for sale, your hangar to swap
// between, the bush-mode switch once every biome is seen), TUNING (cash: handling, lift, power; the fighter's
// missiles) and GIGS (the job board: three jobs for your rank and aircraft; your active job, and its cancel).
// Drawn into the full-resolution overlay; driven by taps.

namespace mantis {
void storeOpen(const Airfield& at, uint32_t salt);
bool storeActive();
// returns true while the hangar stays open; *changed = the aircraft changed; *gig = a job was taken
bool storeUpdate(bool tap, int tx, int ty, float now, bool& changed, bool& gig);
}
