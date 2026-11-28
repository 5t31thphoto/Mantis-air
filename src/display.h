#pragma once
#include <stdint.h>
namespace mantis {
void displayInit();
bool displayStreaming();                // true: the DMA strip pipeline (false: the slow PSRAM canvas fallback)
// Expand the 160x120 indexed frame through the palette into a 320x240 canvas and hand it to
// the core-0 push task; rendering of the next frame overlaps the LCD transfer.
void displayPresent();
// What the next present sends, for a screen that is mostly still (the cockpit): the rows [0, top) and the rectangles
// (screen pixels); the LCD keeps everything else from before. Without a call, a present sends the whole screen.
struct DisplayRect { int16_t x, y, w, h; };
void displaySetRegions(int top, const DisplayRect* rects, int n);
bool displayTearSync();                 // frames go out in step with the panel's refresh (it answered at start)

// A full-resolution overlay drawn over the doubled world: 320 x 240 native RGB565, 0 = transparent.
uint16_t* overlayBuffer();
void overlayEnable(bool on);           // rescans which rows carry anything
bool overlayEnabled();
void overlayClear();
uint32_t overlayGeneration();          // changes on every overlayEnable / overlayClear (who drew it last: see cockpit.cpp)
void overlayAllRows();                 // (after overlayEnable(true)) composite every row: for content drawn after the scan
// Exactly what the screen shows on row y (world + overlay), native RGB565.
void compositeRow(int y, uint16_t* out320);
// While saving to SD (it shares the SPI bus with the LCD) hold the bus: waits for the LCD push to finish.
void displayHoldBus();
void displayBusPins();                 // the LCD's clock and data back on the IO MUX (after the SD card's setup): 80 MHz needs it
void displayReleaseBus();
// tests: the whole screen as each pipeline would send it (byte-swapped RGB565)
void displayDebugLegacy(uint16_t* out320x240);
void displayDebugStream(uint16_t* out320x240);
}
