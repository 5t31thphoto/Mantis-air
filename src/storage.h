#pragma once
#include <stdint.h>
#include <stddef.h>

// The SD card (as MantisPlanet): it shares the SPI bus with the LCD, so every card operation holds the bus.
//   /flight_saves/progress.bin   the save (+ progress.bak, the one before it). Written to a .tmp first.
//   /images/flights/flight_NNNN.mg         the pilot's photo (MG01: 320 x 240 RGB565 little-endian)
//   /images/flights/flight_NNNN_below.mg   the land straight below, at the same moment
//   /images/flights/.flight_NNNN.mg.jig    Mantis Gallery's jig file for each: water regions and depth planes

namespace mantis {

bool storageInit();                     // mounts the card (false: no card)
bool storageOk();
bool storageSave(const void* data, size_t n);           // the save, crash-safe
bool storageLoad(void* data, size_t n);                  // the newest good save (progress.bin, else .bak)
// The next photo number (the card is scanned at start)
int  storagePhotoNumber();
void storagePhotoUsed();
// An MG01 image (320 x 240, one native-RGB565 row at a time) and a small text file, by path. false: no card / failed.
bool storageWriteMG(const char* path, void (*row)(int y, uint16_t* out));
bool storageWriteText(const char* path, const char* text, size_t n);

}
