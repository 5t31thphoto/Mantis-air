#pragma once
#include <stdint.h>

// The radio's voices: the tower, the other pilots, the weather. Each call is a line of text across the top of the
// picture while a voice babbles it on the radio (audio: sfxVoice). Lines queue and play in turn.
//   * the tower: answers when you contact it (the CONTACT TOWER button), clears you to land or tells you why not
//   * traffic: other pilots near the fields, now and then
//   * the weather: the field's information (wind, visibility, cloud, temperature, pressure), when you contact a tower
//     and on the radio's WEATHER channel (swipe to it), read from the scene

namespace mantis {

enum : uint8_t { V_TOWER = 0, V_PILOT, V_WEATHER, V_YOU };
void atcInit(uint32_t seed);
const char* atcCallsign();                       // yours: "MANTIS 2-7"
void atcSay(uint8_t voice, const char* who, const char* text);
void atcClear();
void atcUpdate(float dt, bool nearField, bool weatherChannel, const char* fieldName);
// the line on air now (nullptr: quiet)
const char* atcWho();
const char* atcText();
float atcAlpha();
// the field's weather, as the information reads it
void atcWeather(const char* field, char* out, int n);

}
