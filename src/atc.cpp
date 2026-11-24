#pragma GCC optimize("Os")
#include "atc.h"
#include "audio.h"
#include "scene.h"
#include "journey.h"
#include "universe.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace mantis {

struct Line { uint8_t voice; char who[14]; char text[64]; };
static constexpr int QN = 6;
static Line s_q[QN]; static int s_qn = 0;
static Line s_now; static float s_nowT = 0.f, s_nowLen = 0.f;
static float s_ambT = 25.f, s_wxT = 0.f;
static char s_call[16];
static uint32_t s_rng = 77;
static float rnd() { s_rng = hash32(s_rng + 0x9E3779B9u); return (s_rng >> 8) * (1.f / 16777216.f); }

void atcInit(uint32_t seed) {
    s_rng = seed ^ 0xA7C;
    snprintf(s_call, sizeof s_call, "MANTIS %d-%d", 1 + (int)(rnd() * 9.f), (int)(rnd() * 10.f));
    s_qn = 0; s_nowT = 0.f;
}
const char* atcCallsign() { return s_call; }
void atcClear() { s_qn = 0; s_nowT = 0.f; }
void atcSay(uint8_t voice, const char* who, const char* text) {
    if (s_qn >= QN) { memmove(s_q, s_q + 1, sizeof(Line) * (QN - 1)); s_qn--; }
    Line& l = s_q[s_qn++];
    l.voice = voice;
    snprintf(l.who, sizeof l.who, "%s", who); snprintf(l.text, sizeof l.text, "%s", text);
}

void atcWeather(const char* field, char* out, int n) {
    const Scene& s = g_scene;
    float from = s.windAz * 57.29578f + 90.f; from -= 360.f * floorf(from / 360.f);
    const int wdir = ((int)(from / 10.f + 0.5f) % 36) * 10, wkt = (int)(s.wind * 17.f + 0.5f);
    const float vis = 10.f - s.fog * 9.f - s.rain * 5.f - s.snow * 6.f - s.dust * 4.f;
    const char* cov = s.cloud < 0.2f ? "SKY CLEAR" : (s.cloud < 0.45f ? "FEW CLOUDS" : (s.cloud < 0.7f ? "SCATTERED" : (s.cloud < 0.88f ? "BROKEN" : "OVERCAST")));
    const char* wx = s.storm > 0.3f ? " THUNDERSTORMS." : (s.rain > 0.3f ? " RAIN." : (s.snow > 0.3f ? " SNOW." : (s.fog > 0.4f ? " FOG." : "")));
    static const int8_t TEMP[B_COUNT] = {12, 14, 16, 9, 22, 2, 28, 15, 19, 11, 26, -9};
    const int temp = TEMP[s.biomeA % B_COUNT] + (int)(s.sunElev * 8.f);
    const char letter = (char)('A' + (int)(fmodf(s.t / 60.f, 26.f)));
    snprintf(out, n, "%s INFO %c. WIND %03d AT %d. VIS %d MI.%s %s. TEMP %d.", field, letter, wdir ? wdir : 360, wkt, vis < 1.f ? 1 : (int)vis, wx, cov, temp);
}

void atcUpdate(float dt, bool nearField, bool wxChannel, const char* field) {
    if (s_nowT > 0.f) s_nowT -= dt;
    if (s_nowT <= 0.f && s_qn > 0) {                          // the next line goes out
        s_now = s_q[0]; memmove(s_q, s_q + 1, sizeof(Line) * (QN - 1)); s_qn--;
        s_nowLen = 1.2f + strlen(s_now.text) * 0.055f;
        s_nowT = s_nowLen + 0.8f;
        if (s_now.voice != V_YOU) sfxVoice(s_nowLen, s_now.voice == V_TOWER ? 0 : (s_now.voice == V_WEATHER ? 2 : 1));
        else sfxVoice(s_nowLen * 0.8f, 1);
    }
    // the weather channel: the information, round and round
    if (wxChannel) {
        s_wxT -= dt;
        if (s_wxT <= 0.f && s_qn == 0) { char w[64]; atcWeather(field ? field : "AREA", w, sizeof w); atcSay(V_WEATHER, "WEATHER", w); s_wxT = 22.f; }
    } else s_wxT = 0.f;
    // other traffic near a field
    if (nearField && !wxChannel) {
        s_ambT -= dt;
        if (s_ambT <= 0.f) {
            s_ambT = 25.f + rnd() * 35.f;
            static const char* const WHO[6] = {"OTTER 4-1", "HERON 1-9", "MAPLE 2-2", "SKYLINE 80", "FALCON 3", "MOSS 7-7"};
            static const char* const SAY[8] = {"DOWNWIND FOR THE ACTIVE", "TEN MILES OUT, INBOUND", "CLEAR OF THE RUNWAY", "DEPARTING TO THE NORTH",
                                              "REQUEST TAXI", "LEVEL AT FOUR THOUSAND", "TRAFFIC IN SIGHT", "ON FINAL, GEAR DOWN"};
            atcSay(V_PILOT, WHO[(int)(rnd() * 5.99f)], SAY[(int)(rnd() * 7.99f)]);
        }
    }
}
const char* atcWho() { return s_nowT > 0.f ? s_now.who : nullptr; }
const char* atcText() { return s_nowT > 0.f ? s_now.text : nullptr; }
float atcAlpha() { return s_nowT > 0.6f ? 1.f : (s_nowT > 0.f ? s_nowT / 0.6f : 0.f); }

}
