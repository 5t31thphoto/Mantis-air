// The whole game on the desktop: real setup()/loop(), an autopilot flying through the IMU path (tilt) and the
// throttle slider. Takes off from the home strip, climbs, flies toward the nav target (the nearest field, or a gig),
// prints a log line every 10 s and the frame timings at the end.
//   fly_sim [minutes] [aircraft 0-3] [view 0-2] [altitude m]
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <cstring>
#include "Arduino.h"
#include "M5Unified.h"
M5Class M5; M5Canvas* g_hostLastCanvas = nullptr;
static uint64_t g_us = 1000000;
uint32_t millis() { return (uint32_t)(g_us / 1000); }
uint32_t micros() { return (uint32_t)g_us; }
void delay(uint32_t ms) { g_us += (uint64_t)ms * 1000; }
uint32_t esp_random() { static uint32_t x = 0xC0FFEEu; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
#define main game_main
#include "../../src/main.cpp"
#undef main
static void tilt(float roll, float pitch) {             // the yoke as the IMU sees it (roll + right, pitch + nose up)
  float r = -roll * (28.f / 57.3f) * 1.05f;
  float tip = -pitch;                                    // top away = nose down
  float p = 1.5708f + tip * (20.f / 57.3f) * 1.05f;
  M5.Imu.data.accel = {tanf(r), cosf(p), sinf(p)};
}
// SNAP=every_n_frames: writes snap_NNNN.ppm (320 x 240, the panel with its overlay) as it flies
static void snap(int n) {
  char p[64]; snprintf(p, sizeof p, "snap_%04d.ppm", n);
  FILE* f = fopen(p, "wb"); if (!f) return;
  fprintf(f, "P6 320 240 255\n");
  static uint16_t row[320];
  for (int y = 0; y < 240; y++) { compositeRow(y, row); for (int x = 0; x < 320; x++) { uint16_t c = row[x];
    unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)}; fwrite(rgb, 1, 3, f); } }
  fclose(f);
}
// VIDEO=out.mp4: the frames (320 x 240, 30 fps) and the game's own sound (24 kHz), muxed at the end
static float s_vfrom = 0, s_vto = 1e9;
static FILE* s_vid = nullptr; static FILE* s_aud = nullptr; static const char* s_vidPath = nullptr;
static void videoOpen() {
  s_vidPath = getenv("VIDEO"); if (!s_vidPath) return;
  if (getenv("VFROM")) s_vfrom = atof(getenv("VFROM")); if (getenv("VTO")) s_vto = atof(getenv("VTO"));
  s_vid = popen("ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 320x240 -r 30 -i - -vf scale=640:480:flags=neighbor -c:v libx264 -pix_fmt yuv420p -crf 20 /tmp/_v.mp4", "w");
  s_aud = fopen("/tmp/_a.raw", "wb");
}
static void videoFrame(float t) {
  if (!s_vid) return;
  static int16_t pcmAll[800]; static double accA = 0;
  if (t < s_vfrom || t > s_vto) {                   // outside the window: the sound still runs (its state), unrecorded
    accA += 24000.0 / 30.0; int n = (int)accA; accA -= n;
    for (int done = 0; done < n;) { int k = n - done > 128 ? 128 : n - done; audioRender(pcmAll, k); done += k; }
    return;
  }
  static uint16_t row[320]; static unsigned char rgb[320 * 3];
  for (int y = 0; y < 240; y++) { compositeRow(y, row); for (int x = 0; x < 320; x++) { uint16_t c = row[x];
    rgb[x * 3] = (unsigned char)(((c >> 11) & 31) * 255 / 31); rgb[x * 3 + 1] = (unsigned char)(((c >> 5) & 63) * 255 / 63); rgb[x * 3 + 2] = (unsigned char)((c & 31) * 255 / 31); }
    fwrite(rgb, 1, sizeof rgb, s_vid); }
  static int16_t pcm[800]; static double acc = 0;
  acc += 24000.0 / 30.0; int n = (int)acc; acc -= n;
  for (int done = 0; done < n;) { int k = n - done > 128 ? 128 : n - done; audioRender(pcm + 0, k); fwrite(pcm, 2, k, s_aud); done += k; }
}
static void videoClose() {
  if (!s_vid) return;
  pclose(s_vid); fclose(s_aud);
  char cmd[512]; snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -i /tmp/_v.mp4 -f s16le -ar 24000 -ac 1 -i /tmp/_a.raw -c:v copy -c:a aac -b:a 128k -shortest %s", s_vidPath);
  if (system(cmd) == 0) printf("video: %s\n", s_vidPath);
}
static float clampf_(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static bool towerOfferForTest() { return g_view.towerBtn; }
// SCENARIO=circuit: a scripted pilot flies a circuit at the home strip: takeoff, out 3 km, back round, CONTACT TOWER,
// the approach (localizer and glide path), a flare, the landing and the brakes.
static int s_phase = 0; static int s_tapT = 0;
static bool circuit(float& roll, float& pitch, int i) {
  Airfield a; airfieldAt(0, 0, a); airfieldLevel(a);
  const AircraftSpec& A = aircraftSpec(air);
  float c = cosf(a.yaw), sn = sinf(a.yaw), lon, lat; airfieldLocal(a, ac.x, ac.z, c, sn, lon, lat);
  auto steer = [&](float want, float bankMax) { const float trk = ac.groundSpeed > 5.f ? atan2f(ac.vz, ac.vx) : ac.yaw;   // the track (the wind drifts the nose off it)
    float bank = clampf_(wrapPi(want - trk) * 1.3f, -bankMax, bankMax); roll = clampf_((bank - ac.roll) * 2.f - ac.p * 0.2f, -1.f, 1.f); };
  auto vsHold = [&](float wantVs) {                 // a pilot flies attitude: the pitch that gives that climb
    const float tp = clampf_(0.04f + (wantVs - ac.vs) * 0.035f, -0.25f, 0.2f);
    pitch = clampf_((tp - ac.pitch) * 3.f - ac.q * 0.6f, -0.8f, 0.8f); };
  M5.Touch.down = false; M5.BtnC.down = false;
  if (s_tapT > 0) { s_tapT--; M5.Touch.down = s_tapT > 1; M5.Touch.x = 50; M5.Touch.y = 40; }
  switch (s_phase) {
    case 0: helmSetThrottle(i < 20 ? 0.f : 1.f); pitch = ac.ias > A.vStall * 1.15f ? 0.8f : (A.gear == GEAR_TAIL ? -0.2f : 0.f); roll = 0;
      if (!ac.onGround && ac.agl > 5.f) s_phase = 1; break;
    // (phase 1 starts with the climb attitude)
    case 1: helmSetThrottle(A.vProp > 0 ? 0.9f : 0.8f); steer(a.yaw, 0.4f); vsHold(clampf_((220.f - ac.agl) * 0.05f, -4.f, 5.f));
      if (lon > a.len * 0.5f + 1200.f) s_phase = 2; break;
    case 2: {                                         // round to a point 3 km out on the far side's extended centre line
      helmSetThrottle(A.vProp > 0 ? 0.75f : 0.6f);
      float tx = a.x - c * (a.len * 0.5f + 3000.f), tz = a.z - sn * (a.len * 0.5f + 3000.f);
      float side = 600.f; tx += -sn * side; tz += c * side;
      steer(atan2f(tz - ac.z, tx - ac.x), 0.45f); vsHold(clampf_((150.f - ac.agl) * 0.05f, -4.f, 4.f));
      if (towerOfferForTest() && s_tapT == 0 && clearI == 0x7fff) s_tapT = 3;
      if ((tx - ac.x) * (tx - ac.x) + (tz - ac.z) * (tz - ac.z) < 400.f * 400.f) s_phase = 3; break; }
    case 3: {                                         // final: localizer + 3 degree path toward +lon
      flaps = A.flapCL > 0 ? 2 : 0;
      const float thr = -a.len * 0.5f, dist = fmaxf(0.f, thr - lon);
      steer(a.yaw + clampf_(-lat * 0.006f, -0.5f, 0.5f), 0.35f);
      const float gsY = a.y + 9.f + dist * 0.0524f;
      vsHold(clampf_((gsY - ac.y) * 0.15f - ac.groundSpeed * 0.0524f, -6.f, 2.f));
      helmSetThrottle(clampf_(helmThrottle() + (A.vStall * 1.3f - ac.ias) * 0.01f - (ac.y - gsY) * 0.002f, 0.f, 1.f));
      if (ac.agl < 7.f) s_phase = 4; break; }
    case 4: helmSetThrottle(0.f); roll = clampf_(-ac.roll * 2.f, -1.f, 1.f); vsHold(-0.6f);   // the flare
      if (ac.onGround) s_phase = 5; break;
    case 5: helmSetThrottle(0.f); roll = 0; pitch = 0; M5.BtnC.down = true; if (ac.groundSpeed < 0.5f) s_phase = 6; break;
    default: M5.BtnC.down = false; helmSetThrottle(0.f); break;
  }
  return true;
}
// MISSION=supply|bomb|recon|photo: takes that job from the nearest board that has it, starts 3 km out at the
// right height and flies it: the drop view, the predicted-impact pip, A when the pip crosses the mark.
static int s_mission = 0;   // 1 supply, 2 bomb, 3 recon
static float s_missionAgl = 150.f;
static bool missionSetup() {
  const char* m = getenv("MISSION"); if (!m) return false;
  uint8_t want = !strcmp(m, "bomb") ? G_BOMBING : (!strcmp(m, "recon") ? G_RECON : (!strcmp(m, "strike") ? G_STRIKE : (!strcmp(m, "photo") ? G_PHOTO : (!strcmp(m, "intercept") ? G_INTERCEPT : G_SUPPLY))));
  s_mission = want == G_SUPPLY ? 1 : (want == G_RECON ? 3 : (want == G_PHOTO ? 4 : (want == G_INTERCEPT ? 5 : 2)));
  s_missionAgl = s_mission == 1 ? 110.f : (s_mission == 3 ? 350.f : (s_mission == 4 ? 250.f : (s_mission == 5 ? 600.f : 420.f)));
  Gig board[3];
  for (int r = 0; r < 6; r++) for (int i = -r; i <= r; i++) for (int j = -r; j <= r; j++) {
    if (abs(i) != r && abs(j) != r) continue;
    Airfield a; if (!airfieldAt(i, j, a)) continue;
    for (uint32_t salt = 0; salt < 60; salt++) {
      int n = gigBoard(a, R_FIGHTER, air, salt, board);
      for (int k = 0; k < n; k++) if (board[k].kind == want) {
        gigAccept(board[k], t);
        const Waypoint* w = gigWaypoint();
        float ang = 0.7f, sx = w->x - cosf(ang) * 3000.f, sz = w->z - sinf(ang) * 3000.f;
        terrainUpdate(sx, sz, ang, 0.f, 1.f / 30.f);
        aircraftResetAir(ac, air, sx, sz, ang, terrainHeight(sx, sz) + s_missionAgl + 80.f);
        helmSetThrottle(0.7f);
        printf("mission: %s from %s (%s), target %.0f %.0f\n", board[k].title, a.name, a.military ? "air base" : "field", w->x, w->z);
        return true;
      }
    }
  }
  printf("mission: none found\n"); return false;
}
static int s_btnT = 0; static float s_lastRel = -9.f;
static int s_photoSeq = 0;
static void missionFly(float& roll, float& pitch, int i) {
  M5.BtnA.down = s_btnT > 0; if (s_btnT > 0) s_btnT--;
  if (s_mission == 4 && s_photoSeq > 0) {               // the camera: hold A (in), a beat, tap A (the shot), B (out)
    s_photoSeq++;
    M5.BtnA.down = s_photoSeq < 26 || (s_photoSeq >= 40 && s_photoSeq < 42);
    M5.BtnB.down = s_photoSeq >= 70 && s_photoSeq < 72;
    if (s_photoSeq == 41) printf("  shutter at t=%.1f (photo mode %d)\n", t, (int)photoActive());
    if (s_photoSeq > 80) s_photoSeq = -1;
    roll = 0.f; pitch = s_photoSeq > 26 && s_photoSeq < 70 ? -0.35f : 0.f; return;   // (tilt the camera down onto it)
  }
  const Waypoint* w = gigWaypoint();
  float tx = g_view.navOk ? g_view.navX : ac.x + cosf(ac.yaw) * 1000.f, tz = g_view.navOk ? g_view.navZ : ac.z + sinf(ac.yaw) * 1000.f;
  bool aim = false;
  if (w && (w->kind == W_SUPPLY || w->kind == W_RECON)) { tx = w->x; tz = w->z; aim = true; }
  if (w && w->kind == W_PHOTO) { tx = w->x; tz = w->z; }
  if (w && w->kind == W_BOMB) {                         // the nearest target standing
    const GroundTarget* g = combatGround(); float best = 1e18f;
    for (int k = 0; k < GT_N; k++) if (g[k].alive) { float d = (g[k].x - ac.x) * (g[k].x - ac.x) + (g[k].z - ac.z) * (g[k].z - ac.z); if (d < best) { best = d; tx = g[k].x; tz = g[k].z; aim = true; } }
    if (!aim) { tx = w->x; tz = w->z; }
  }
  const float dx = tx - ac.x, dz = tz - ac.z, d = sqrtf(dx * dx + dz * dz);
  float want = atan2f(dz, dx);
  // in the drop view, aim the pip (not the nose) at the mark: fly so the impact point tracks onto it
  float ix = ac.x, iz = ac.z;
  const bool havePip = combatPredictImpact(ac, gigCrates() > 0, ix, iz);
  // a bomb run: in on the target with the pip leading; too close to make the line, extend out and come round again
  static bool extend = false;
  const float thr = havePip ? sqrtf((ix - ac.x) * (ix - ac.x) + (iz - ac.z) * (iz - ac.z)) : 0.f;   // the throw
  if (aim && havePip) {
    const float pipLeft = (tx - ix) * cosf(ac.yaw) + (tz - iz) * sinf(ac.yaw);   // + : the pip still short of the mark
    if (!extend && d < thr * 0.9f && pipLeft < 0.f) extend = true;
    if (extend && d > thr + (air == A_FIGHTER ? 2200.f : 900.f)) extend = false;
    if (extend) want = ac.yaw;
    else want += clampf_(wrapPi(atan2f(tz - iz, tx - ix) - want) * 0.8f, -0.35f, 0.35f);   // the pip's drift, trimmed out
  }
  const float bmax = air == A_FIGHTER ? 1.05f : 0.5f;
  const float bank = clampf_(wrapPi(want - ac.yaw) * 1.2f, -bmax, bmax);
  roll = clampf_((bank - ac.roll) * 2.f - ac.p * 0.2f, -1.f, 1.f);
  const float wantVs = clampf_((s_missionAgl - ac.agl) * 0.04f, -6.f, 5.f);
  const float tp = clampf_(0.03f + (wantVs - ac.vs) * 0.035f, -0.25f, 0.2f);   // attitude flying, as in the circuit
  pitch = clampf_((tp - ac.pitch) * 3.f - ac.q * 0.6f, -0.8f, 0.8f);
  if (ac.stall > 0.3f) pitch = -0.5f;
  const AircraftSpec& A = aircraftSpec(air);
  const float vWant = air == A_FIGHTER ? 105.f : A.vStall * 1.6f;            // slow for the drop
  helmSetThrottle(clampf_(helmThrottle() + (vWant - ac.ias) * 0.004f, 0.15f, 1.f));
  g_prog.view = (aim && d < 1600.f) ? VIEW_DROP : VIEW_CHASE;
  if (s_mission == 5) {                                  // a dogfight: at the nearest bogey, guns when it's in the nose
    const Bogey* b = combatBogeys(); float best = 1e18f; int bi = -1;
    for (int k = 0; k < BOGEY_N; k++) if (b[k].alive) { float dd = (b[k].x - ac.x) * (b[k].x - ac.x) + (b[k].z - ac.z) * (b[k].z - ac.z); if (dd < best) { best = dd; bi = k; } }
    g_prog.view = VIEW_CHASE;
    if (bi >= 0) {
      const float bx = b[bi].x - ac.x, by = b[bi].y - ac.y, bz = b[bi].z - ac.z, bd = sqrtf(bx * bx + by * by + bz * bz);
      const float wantY = atan2f(bz, bx), e = wrapPi(wantY - ac.yaw);
      roll = clampf_((clampf_(e * 1.5f, -1.1f, 1.1f) - ac.roll) * 2.f - ac.p * 0.2f, -1.f, 1.f);
      const float wantP = atan2f(by, sqrtf(bx * bx + bz * bz));
      pitch = clampf_((wantP - ac.pitch) * 2.f + fabsf(ac.roll) * 0.4f - ac.q * 0.4f, -0.8f, 0.8f);
      if (ac.agl < 150.f) pitch = 0.6f;
      helmSetThrottle(0.8f);
      M5.BtnA.down = bd < 900.f && fabsf(e) < 0.15f && fabsf(wantP - ac.pitch) < 0.15f;
      return;
    }
  }
  if (s_mission == 4 && w && w->kind == W_PHOTO && s_photoSeq == 0 && d < 1300.f) { s_photoSeq = 1; printf("  camera up at t=%.1f, %.0f m out\n", t, d); }
  if (s_mission == 3 && w && w->kind == W_RECON) {      // the spy camera: fly at it; the sight looks ahead, shoot when it's in
    g_prog.view = d < 3000.f ? VIEW_DROP : VIEW_CHASE;
    if (s_btnT == 0 && t - s_lastRel > 1.5f && g_prog.view == VIEW_DROP && renderDropSees(tx, tz) && d < 1500.f) {
      s_btnT = 2; s_lastRel = t; printf("  spy camera at t=%.1f, %.0f m out\n", t, d); }
    return;
  }
  if (aim && havePip && s_btnT == 0 && t - s_lastRel > (s_mission == 1 ? 3.f : 1.2f)) {
    const float e = sqrtf((ix - tx) * (ix - tx) + (iz - tz) * (iz - tz));
    static float pe = 1e9f; const bool passing = e > pe; pe = e;       // release as the pip crosses the mark
    if (s_mission == 3 ? (renderDropSees(tx, tz) && d < 900.f) : (e < 30.f && (passing || e < 8.f))) { s_btnT = 2; s_lastRel = t; printf("  release at t=%.1f, pip %.0f m off (%s)\n", t, e, s_mission == 1 ? "crate" : (s_mission == 3 ? "photo" : "bomb")); }
  }
  (void)i;
}
namespace mantis { extern double g_planeArtUs; extern int g_planeArtN; }
namespace mantis { void renderDebugPlane(int, int, float, float, float, float, float, float, const char*); }
// PLANEVIEW=<prefix>: every aircraft from exact camera angles on a blank screen, then
// exit (judging the aircraft art); also times each draw
static bool planeView() {
  const char* pre = getenv("PLANEVIEW"); if (!pre) return false;
  renderInit();
  struct { const char* n; float az, el, roll; int rpm; } V[] = {{"chase", 180.f, 10.f, 0.f, 100}, {"chasebank", 180.f, 10.f, 35.f, 100}, {"left", -90.f, 3.f, 0.f, 100},
                                                       {"front", 0.f, 3.f, 0.f, 100}, {"frontidle", 0.f, 3.f, 0.f, 10}, {"quarter", 135.f, 22.f, 0.f, 100},
                                                       {"quarteridle", 35.f, 15.f, 0.f, 10}, {"above", 175.f, 60.f, 0.f, 100}};
  double total = 0; int n = 0;
  for (int m = 0; m < 8; m++) for (auto& v : V) {
    char p[160]; snprintf(p, sizeof p, "%s%d_%s.ppm", pre, m, v.n);
    const float dist = m % 4 == 2 ? 24.f : 17.f;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int rep = 0; rep < 50; rep++) mantis::renderDebugPlane(m, v.rpm, 0.f, 0.f, v.roll, v.az, v.el, dist, rep == 49 ? p : nullptr);
    total += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count() / 50; n++;
  }
  printf("%.1f us a draw (host, incl. clearing the test screen)\n", total / n);
  return true;
}
int main(int argc, char** argv) {
  if (planeView()) return 0;
  float minutes = argc > 1 ? atof(argv[1]) : 3.f;
  int asel = argc > 2 ? atoi(argv[2]) : 0;
  float cruiseAgl = argc > 4 ? atof(argv[4]) : 350.f;
  tilt(0, 0);
  setup();
  videoOpen();
  g_prog.owned = 15; g_prog.current = (uint8_t)asel; switchAircraft((uint8_t)asel);
  g_prog.view = argc > 3 ? atoi(argv[3]) : VIEW_CHASE;
  const bool mission = missionSetup();
  if (!mission && getenv("AIRSTART")) {                 // AIRSTART=1: already flying, 400 m up (the jet can't leave a grass strip)
    terrainUpdate(ac.x, ac.z, ac.yaw, 0.f, 1.f / 30.f);
    aircraftResetAir(ac, air, ac.x, ac.z, ac.yaw, terrainHeight(ac.x, ac.z) + 400.f);
    helmSetThrottle(0.8f);
  }
  int frames = (int)(minutes * 60 * 30);
  double rtot = 0, rmax = 0; float maxAlt = 0, maxIas = 0; int nCrash = 0; bool airborne = false;
  for (int i = 0; i < frames; i++) {
    const AircraftSpec& A = aircraftSpec(air);
    float roll = 0, pitch = 0;
    static const bool scen = getenv("SCENARIO") != nullptr;
    if (scen) { circuit(roll, pitch, i); tilt(roll, pitch); goto run; }
    if (mission) { missionFly(roll, pitch, i); tilt(roll, pitch); goto run; }
    if (ac.crash) airborne = false;
    static float course = 0.f; if (i == 0) course = ac.yaw;
    if (i < 20) { helmSetThrottle(0); }
    else if (ac.onGround && !airborne) {                 // the takeoff roll: full power, rotate at 1.15 Vs
      helmSetThrottle(1.f);
      pitch = ac.ias > A.vStall * 1.15f ? 0.8f : (A.gear == GEAR_TAIL ? -0.2f : 0.f);
    } else {
      airborne = true;
      helmSetThrottle(A.vProp > 0 ? 0.85f : 0.75f);
      // heading to the nav target, altitude to cruiseAgl
      float want = gigActive() && g_view.navOk ? atan2f(g_view.navZ - ac.z, g_view.navX - ac.x) : course + 0.4f * sinf(i / 900.f);
      if (ac.agl < 120.f) want = ac.yaw;                  // wings level until clear of the trees
      float bank = clampf_(wrapPi(want - ac.yaw) * 1.2f, -0.5f, 0.5f);
      roll = clampf_((bank - ac.roll) * 2.f - ac.p * 0.2f, -1.f, 1.f);
      float wantVs = clampf_((cruiseAgl - ac.agl) * 0.05f, -5.f, 6.f);
      pitch = clampf_((wantVs - ac.vs) * 0.12f - ac.q * 0.4f, -0.8f, 0.8f);
      if (ac.stall > 0.3f) pitch = -0.5f;
    }
    tilt(roll, pitch);
    run:
    g_us += 33333;
    auto a = std::chrono::high_resolution_clock::now();
    loop();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - a).count();
    rtot += ms; if (ms > rmax && i > 30) rmax = ms;
    { static int every = getenv("SNAP") ? atoi(getenv("SNAP")) : 0; if (every && i % every == every - 1) snap(i / every); if (getenv("VIEWCYCLE") && i % 900 == 0) g_prog.view = (uint8_t)((i / 900) % 3); }
    { static const char* tt = getenv("TURN"); static int tn = 0;      // TURN=frame: photo mode's five angles, a still each
      if (tt && i >= atoi(tt) && tn < 5 * 12) {
        if (tn == 0) { if (getenv("TURNLEVEL")) { ac.roll = 0.f; ac.pitch = 0.02f; } photoEnter(ac, air); }   // (TURNLEVEL: wings level, to judge the model)
        else if (tn % 12 == 0) photoNextAngle();
        if (tn % 12 == 11) { char p[64]; snprintf(p, sizeof p, "turn_%d_%d.ppm", air, tn / 12); FILE* f = fopen(p, "wb");
          if (f) { fprintf(f, "P6 320 240 255\n"); static uint16_t row[320]; for (int y = 0; y < 240; y++) { compositeRow(y, row); for (int x = 0; x < 320; x++) { uint16_t c = row[x];
            unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)}; fwrite(rgb, 1, 3, f); } } fclose(f); } }
        tn++;
      }
    }
    videoFrame(i / 30.f);
    { static const char* vc = getenv("VIEWS"); if (vc) { int k = (int)(i / 30.f / atof(vc)); g_prog.view = (uint8_t)(k % 3); } }
    { static char last[64] = ""; if (msgT > 0 && strcmp(last, msgBuf)) { snprintf(last, sizeof last, "%s", msgBuf); printf("  [%5.1fs] %s / %s\n", i / 30.f, msgBuf, subBuf); } }
    if (ac.y > maxAlt) maxAlt = ac.y;
    if (ac.ias > maxIas) maxIas = ac.ias;
    if (ac.crash && crashT < 0.04f) nCrash++;
    if (i % 300 == 0)
      printf("ph%d t=%4.0fs %-18s %-6s ias %5.1f mph alt %5.0f ft agl %5.0f m vs %+5.0f fpm pitch %+4.1f roll %+5.1f thr %.2f gnd %d nav %-14s %.1f mi  mi %.2f  %s\n", s_phase, i / 30.f,
        biome(journeyCurrent()).name, A.shortName, ac.ias * 2.237f, ac.y * 3.28f, ac.agl, ac.vs * 196.85f, ac.pitch * 57.3f, ac.roll * 57.3f,
        ac.power, ac.onGround, g_view.navName ? g_view.navName : "-", g_view.navDist / 1609.f, distance / 1609.f, msgT > 0 ? msgBuf : (atcText() ? atcText() : ""));
  }
  videoClose();
  printf("frames %d  mean %.2f ms  worst %.2f ms   top %.0f ft  top %.0f mph  crashes %d  miles %.2f\n", frames, rtot / frames, rmax, maxAlt * 3.28f, maxIas * 2.237f, nCrash, distance / 1609.f);
  if (mantis::g_planeArtN) printf("aircraft drawing: %.1f us a draw (%d draws), %.1f%% of the frame\n", mantis::g_planeArtUs / mantis::g_planeArtN, mantis::g_planeArtN, 100.0 * mantis::g_planeArtUs / 1000.0 / rtot);
}
