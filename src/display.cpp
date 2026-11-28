#include "display.h"
#include "render.h"
#include <M5Unified.h>
#include <string.h>
#include <stdlib.h>
#include "mem.h"
#ifndef HOST
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#endif

namespace mantis {

static constexpr int DW = FB_W * 2, DH = FB_H * 2;

static M5Canvas s_fbA(&M5.Display), s_fbB(&M5.Display);
static M5Canvas* s_fb[2] = {&s_fbA, &s_fbB};
static int s_cur = 0;
static bool s_double = false;
static TaskHandle_t s_dispTask = nullptr;
static SemaphoreHandle_t s_dispIdle = nullptr;
static volatile int s_pushIdx = 0;
static uint16_t* s_ov = nullptr;
static bool s_ovOn = false;
static uint8_t s_ovRow[DH];
static uint32_t s_ovGen = 1;                         // bumped whenever someone clears or switches the overlay
static bool s_forceFull = true;                      // the next present sends the whole screen (the overlay changed)
static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void displayWaitIdle();
uint16_t* overlayBuffer() {
    displayWaitIdle();                                  // the screen core may still be reading it
    if (!s_ov) {
#ifdef HOST
        s_ov = (uint16_t*)calloc(DW * DH, sizeof(uint16_t));
#else
        s_ov = (uint16_t*)heap_caps_calloc(DW * DH, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_ov) s_ov = (uint16_t*)calloc(DW * DH, sizeof(uint16_t));
#endif
    }
    return s_ov;
}
void overlayClear() { s_ovGen++; s_forceFull = true; if (overlayBuffer()) memset(s_ov, 0, DW * DH * sizeof(uint16_t)); }   // (overlayBuffer waits)
uint32_t overlayGeneration() { return s_ovGen; }
void overlayAllRows() { if (s_ovOn) memset(s_ovRow, 1, sizeof s_ovRow); }
void overlayEnable(bool on) {
    s_ovGen++; s_forceFull = true;
    s_ovOn = on && overlayBuffer();
    if (!s_ovOn) return;
    for (int y = 0; y < DH; y++) { s_ovRow[y] = 0; for (int x = 0; x < DW; x++) if (s_ov[y * DW + x]) { s_ovRow[y] = 1; break; } }
}
bool overlayEnabled() { return s_ovOn; }

void compositeRow(int y, uint16_t* out) {
    const uint8_t* fb = frameBuffer();
    const uint16_t* pal = framePalette565() + (((y + frameBob()) & 1) << 9);   // this row's two dither positions
    int src = (y + frameBob()) >> 1;
    if (src > FB_H - 1) src = FB_H - 1;
    const uint8_t* srow = fb + src * FB_W;
    for (int x = 0; x < DW; x++) out[x] = swap16(pal[((x & 1) << 8) + srow[x >> 1]]);
    if (s_ovOn && s_ovRow[y]) { const uint16_t* o = s_ov + y * DW; for (int x = 0; x < DW; x++) if (o[x]) out[x] = o[x]; }
}

// ---------------------------------------------------------------------------------------------------------
// The pipeline. The drawing core renders the 160 x 120 indexed frame; the screen core shows it at 320 x 240.
//
// Default ("strip streaming"): at present time the drawing core copies just the frame (19 KB), its palette,
// the bob offset and the overlay row flags into a packet in fast internal RAM (~0.1 ms) and goes straight back
// to drawing. The screen core doubles the packet 16 rows at a time (pixel pairs as single 32-bit writes) into two
// strip buffers in DMA-capable internal RAM, and DMAs each strip to the LCD in its own byte order while it fills
// the other, with the bus held for the whole frame. The LCD clock is MP_LCD_MHZ. Nothing passes through PSRAM; the
// doubling costs the drawing core nothing and hides behind the transfer on the screen core.
// The screen task outranks the sound on that core, so a strip is queued the moment the last one is done (the sound
// renders a 21 ms block in one go: below it, the transfer stalled behind every block). While a strip is on the wire
// the task sleeps on a microsecond timer set for the strip's end, so the sound has the core.
//
// -DMP_LEGACY_DISPLAY restores the previous pipeline exactly (whole-frame PSRAM canvases).
// Both produce byte-identical pixels (tools/host/displaytest.cpp checks this on every golden scene).
// ---------------------------------------------------------------------------------------------------------
static constexpr int STRIP = 16;
static constexpr int MAX_RECTS = 32;
struct Packet {
    uint8_t fb[FB_W * FB_H]; uint16_t pal[4 * 256]; int bob; bool ovOn; uint8_t ovRow[DH];   // pal: 2x2 dither
    // what to send: the whole screen, or (the cockpit) the rows [0, top) and some rectangles below them; the LCD keeps
    // the rest from the frame before
    bool full; int16_t top; uint8_t nRects; DisplayRect rects[MAX_RECTS];
};
static Packet* s_pk = nullptr;
static uint16_t* s_strip[2] = {nullptr, nullptr};    // two 320 x 16 strips in DMA-capable internal RAM
static bool s_stream = false;
// what the next present sends (set by the cockpit each frame; anything else sends the whole screen)
static bool s_regFull = true;
static int16_t s_regTop = DH;
static uint8_t s_regN = 0;
static DisplayRect s_regRects[MAX_RECTS];
void displaySetRegions(int top, const DisplayRect* r, int n) {
    if (n > MAX_RECTS) { s_regFull = true; return; }                  // (too scattered: the whole screen)
    s_regFull = false; s_regTop = (int16_t)(top < 0 ? 0 : top > DH ? DH : top);
    s_regN = (uint8_t)n; for (int i = 0; i < n; i++) s_regRects[i] = r[i];
}
#ifndef MP_LCD_MHZ
#define MP_LCD_MHZ 80                                // the LCD write clock (reads keep M5GFX's own, slower read clock)
#endif
static constexpr int64_t PIXEL_NS_X16 = 16 * 1000 / MP_LCD_MHZ;   // one pixel (16 bits) on the wire, in ns
static inline int64_t wireUs(int pixels) { return (int64_t)pixels * PIXEL_NS_X16 / 1000; }
static constexpr int64_t STRIP_EARLY_US = 30;        // wake this much before the strip's end (the timer's dispatch)
static constexpr int64_t STRIP_MIN_SLEEP_US = 80;    // less than this left: just wait it out
#ifndef HOST
static esp_timer_handle_t s_stripTimer = nullptr;
static SemaphoreHandle_t s_stripDone = nullptr;
static int64_t s_dmaEnd = 0;                         // when the strip on the wire is done (esp_timer clock)
static void stripTimerCb(void*) { xSemaphoreGive(s_stripDone); }
static void sleepUs(int64_t us) {                    // asleep (the core is the sound's), or a short spin
    if (us <= 0) return;
    if (s_stripTimer && us > STRIP_MIN_SLEEP_US && esp_timer_start_once(s_stripTimer, (uint64_t)us) == ESP_OK)
        xSemaphoreTake(s_stripDone, portMAX_DELAY);
    else { const int64_t end = esp_timer_get_time() + us; while (esp_timer_get_time() < end) {} }
}
// Until the strip on the wire is done: asleep for nearly all of it, then the last few us.
static void stripWait() {
    const int64_t left = s_dmaEnd - esp_timer_get_time() - STRIP_EARLY_US;
    if (left > STRIP_MIN_SLEEP_US) sleepUs(left);
    M5.Display.waitDMA();
}
#endif

// ---- the panel's scan, and sending a frame in step with it (no tearing) ----------------------------------
// The ILI9342C refreshes its glass from its own memory, line after line, on its own clock; the Core2 doesn't wire the
// panel's TE (tearing) pin to the ESP32, but the panel reports the line it is refreshing (Get Scanline, 45h). A frame
// shows torn where the refresh passes the line being written. Started right behind the refresh (just after it has
// passed our first row, in the order it refreshes them), a frame written in less than two refresh periods is never
// crossed: if the refresh is faster than the write it stays ahead all of its first pass and its second pass can't
// catch the write before the write is done; if slower, start just ahead of it instead and the write stays ahead.
// At 40 MHz a frame took about 31 ms on the wire, over two refresh periods: it always tore. At 80 MHz it is about 16 ms.
// Which way it refreshes: LovyanGFX (under M5GFX) sets the refresh order bit (ML) together with the row order bit (MY)
// in every rotation it uses (Panel_LCD's MADCTL table), so in landscape the panel refreshes our rows top to bottom,
// the order the strips go out in. How many lines a refresh takes and how long are read from the panel at start.
static bool s_sync = false;          // the panel answered sensibly: frames are sent in step with its refresh
static int s_scanLines = 0;          // lines in one refresh (the 240 and the porches)
static int64_t s_scanUs = 0;         // one refresh
static int s_gtsShift = -1;          // where the 10-bit line count sits in the 24 bits read back (see syncCalibrate)
#ifndef HOST
static uint32_t readScanRaw() {
    const uint32_t r = M5.Display.getPanel()->readCommand(0x45, 0, 3);   // three bytes, first received in the low byte (the panel reads; M5GFX itself has no readCommand)
    return ((r & 0xFFu) << 16) | (r & 0xFF00u) | ((r >> 16) & 0xFFu);   // as a big-endian bit stream
}
static int readScanline() {
    if (s_gtsShift < 0) return 1023;
    const uint32_t f = (readScanRaw() >> s_gtsShift) & 0xFFFFu;      // [0 0 0 0 0 0 GTS9 GTS8][GTS7..GTS0]
    return (f & 0xFC00u) ? 1023 : (int)f;
}
// The read's leading dummy bits (a dummy byte in the datasheet's table, a dummy clock on a serial bus, and whatever the
// library already skips) aren't assumed: the line count is looked for at each bit position, and the one that
// behaves like a refresh counter (always under 512, climbing, and wrapping three times in a little over two
// refreshes) is kept. None does: no sync, frames go out as before.
static void syncCalibrate() {
    s_sync = false; s_gtsShift = -1;
    static uint32_t raw[320]; static int64_t at[320];
    const int64_t start = esp_timer_get_time();
    int n = 0;
    while (n < 320 && esp_timer_get_time() - start < 100000) { at[n] = esp_timer_get_time(); raw[n++] = readScanRaw(); delayMicroseconds(250); }   // (a little over two refreshes at the slowest)
    for (int sh = 8; sh >= 0; sh--) {
        int maxL = -1, wraps = 0, back = 0, last = -1; int64_t t0 = 0, t2 = 0; bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            const uint32_t f = (raw[i] >> sh) & 0xFFFFu;
            if (f & 0xFC00u) { ok = false; break; }
            const int l = (int)f;
            if (l > maxL) maxL = l;
            if (last >= 0) {
                if (l + 40 < last) { wraps++; if (wraps == 1) t0 = at[i]; if (wraps == 3) t2 = at[i]; }
                else if (l < last) back++;                           // a counter only runs forward between wraps
            }
            last = l;
        }
        if (!ok || wraps < 3 || back > 2 || maxL < 240 || maxL > 400) continue;
        const int64_t period = (t2 - t0) / 2;
        if (period < 6000 || period > 40000) continue;
        s_gtsShift = sh; s_scanLines = maxL + 1; s_scanUs = period; s_sync = true;
        return;
    }
}
// Wait until the refresh has just passed our first row (or, for a refresh slower than our write, is just about to
// reach it). writeUs: how long this frame's write will take.
static void syncWait(int64_t writeUs) {
    const int64_t lineUs = s_scanUs / s_scanLines;
    const bool behind = s_scanUs < writeUs;                           // the refresh is the faster: start behind it
    const int want = behind ? 10 : s_scanLines - 4;                   // (10 lines past the wrap: past the porch and a few rows; or 4 before it)
    for (int tries = 0; tries < 4; tries++) {
        const int l = readScanline();
        if (l > 511) return;
        int ahead = want - l; if (ahead < 0) ahead += s_scanLines;    // lines until the refresh reaches 'want'
        if (ahead <= 2 || (behind && l >= want && l < want + 24)) return;
        const int64_t us = (int64_t)ahead * lineUs;
        sleepUs(us > 300 ? us - 150 : us);                            // most of the way asleep, then look again
    }
}
#endif
bool displayTearSync() { return s_sync; }

static uint16_t* stripAlloc() {
#ifdef HOST
    return (uint16_t*)calloc(DW * STRIP, sizeof(uint16_t));
#else
    return (uint16_t*)heap_caps_malloc(DW * STRIP * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#endif
}

// One screen row from a frame: the shared doubling (stream, legacy and compositeRow all agree on these pixels). Each
// picture pixel is a 2x2 block on the panel; each position in it takes its own dithered copy of the palette.
static inline void doubleRow(const uint8_t* fb, const uint16_t* pal, int bob, bool ovOn, const uint8_t* ovRow, int y, uint16_t* d) {
    int src = (y + bob) >> 1;
    if (src > FB_H - 1) src = FB_H - 1;
    const uint8_t* s = fb + src * FB_W;
    const uint16_t* pa = pal + (((y + bob) & 1) << 9), *pb = pa + 256;
    uint32_t* d32 = (uint32_t*)d;
    for (int x = 0; x < FB_W; x++) { const int c = s[x]; d32[x] = (uint32_t)pa[c] | ((uint32_t)pb[c] << 16); }   // both pixels at once
    if (ovOn && ovRow[y]) {                              // the overlay: two pixels a read (most pairs are clear)
        const uint32_t* o = (const uint32_t*)(s_ov + y * DW);
        for (int x = 0; x < FB_W; x++) {
            const uint32_t v = o[x];
            if (!v) continue;
            if (v & 0xFFFFu) d[2 * x] = swap16((uint16_t)v);
            if (v >> 16) d[2 * x + 1] = swap16((uint16_t)(v >> 16));
        }
    }
}

// The strips of one packet, written by `emit(x, y, w, h, pixels)`: the rows [0, top) (the whole screen when full) as
// 16-row strips in the order the panel refreshes them, then the rectangles (at most a strip's pixels at a time).
template <class Emit> static void streamPacket(const Packet& P, uint16_t* const* strips, bool upward, Emit emit) {
    int k = 0;
    const int top = P.full ? DH : P.top;
    const int nStrips = (top + STRIP - 1) / STRIP;
    for (int n = 0; n < nStrips; n++, k++) {
        const int s = upward ? nStrips - 1 - n : n;
        const int y0 = s * STRIP, rows = y0 + STRIP <= top ? STRIP : top - y0;
        uint16_t* d = strips[k & 1];
        if (!d) return;
        for (int r = 0; r < rows; r++) doubleRow(P.fb, P.pal, P.bob, P.ovOn, P.ovRow, y0 + r, d + r * DW);   // (rows of a pair differ: their own dither)
        emit(0, y0, DW, rows, d);
    }
    if (P.full) return;
    static uint16_t row[DW];
    for (int i = 0; i < P.nRects; i++) {
        const DisplayRect& R = P.rects[i];
        if (R.w <= 0 || R.h <= 0) continue;
        const int per = (DW * STRIP) / R.w;                       // rows of this rectangle a strip holds
        for (int y0 = R.y; y0 < R.y + R.h; y0 += per, k++) {
            const int rows = y0 + per <= R.y + R.h ? per : R.y + R.h - y0;
            uint16_t* d = strips[k & 1];
            if (!d) return;
            for (int r = 0; r < rows; r++) {
                doubleRow(P.fb, P.pal, P.bob, P.ovOn, P.ovRow, y0 + r, row);
                memcpy(d + r * R.w, row + R.x, R.w * sizeof(uint16_t));
            }
            emit(R.x, y0, R.w, rows, d);
        }
    }
}
static int packetPixels(const Packet& P) {
    if (P.full) return DW * DH;
    int n = DW * P.top;
    for (int i = 0; i < P.nRects; i++) n += P.rects[i].w * P.rects[i].h;
    return n;
}

static void fillPacket() {
    memcpy(s_pk->fb, frameBuffer(), sizeof s_pk->fb);
    memcpy(s_pk->pal, framePalette565(), sizeof s_pk->pal);
    s_pk->bob = frameBob();
    s_pk->ovOn = s_ovOn;
    memcpy(s_pk->ovRow, s_ovRow, sizeof s_pk->ovRow);
    s_pk->full = s_regFull || s_forceFull;
    s_pk->top = s_regTop; s_pk->nRects = s_regN;
    memcpy(s_pk->rects, s_regRects, s_regN * sizeof(DisplayRect));
    s_regFull = true; s_forceFull = false;                            // (next frame: the whole screen unless told again)
}

// Wait until the screen core has finished with the current packet (and the overlay it reads).
static void displayWaitIdle() {
#ifndef HOST
    if (s_dispIdle) { xSemaphoreTake(s_dispIdle, portMAX_DELAY); xSemaphoreGive(s_dispIdle); }   // woken the moment it's done
#endif
}

// The LCD's clock (GPIO18) and data (GPIO23) on the IO MUX, wired straight to the SPI peripheral (VSPI's own pins).
// The SD card's setup (SPI.begin with the Core2's MISO on GPIO38, not a VSPI pin) routes them through the GPIO matrix
// instead, which can't carry 80 MHz: that, not the clock as such, is what had corrupted the picture at 80 MHz. The
// card works the same on the IO MUX (the same peripheral, the same pins); its MISO input stays on the matrix, where
// its 25 MHz reads are fine. Done after the card's setup and after every hold of the bus.
void displayBusPins() {
#ifndef HOST
    gpio_iomux_out(GPIO_NUM_18, 1, false);            // FUNC 1 of GPIO18: VSPICLK
    gpio_iomux_out(GPIO_NUM_23, 1, false);            // FUNC 1 of GPIO23: VSPID
#endif
}
void displayHoldBus() {
#ifndef HOST
    if (s_dispIdle) xSemaphoreTake(s_dispIdle, portMAX_DELAY);
#endif
}
void displayReleaseBus() {
#ifndef HOST
    displayBusPins();                                   // (in case the card's driver attached them to the matrix again)
    if (s_dispIdle) xSemaphoreGive(s_dispIdle);
#endif
}

#ifdef MP_LEGACY_DISPLAY
static void displayTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_fb[s_pushIdx]->pushSprite(&M5.Display, 0, 0);
        xSemaphoreGive(s_dispIdle);
    }
}
#else
// Double-buffered DMA: while one strip is on its way to the LCD, the CPU fills the other. The bus is held for
// the whole frame (no per-strip transaction setup). Strips are byte-swapped RGB565 = the panel's own order.
static void displayTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#ifndef HOST
        static bool first; first = true;
        static int64_t writeUs; writeUs = wireUs(packetPixels(*s_pk)) + 1500;   // (+ the strips' filling and the timer's slack)
        M5.Display.startWrite();
        displayBusPins();                                           // (whatever touched the bus last: the IO MUX for 80 MHz)
        streamPacket(*s_pk, s_strip, false, [](int x, int y0, int w, int h, uint16_t* d) {
            if (first) {                                            // the first strip is filled: now wait for the refresh
                first = false;
                if (s_sync) { M5.Display.endWrite(); syncWait(writeUs); M5.Display.startWrite(); displayBusPins(); }
            }
            stripWait();                                            // the strip before last is done with this buffer's twin
            M5.Display.pushImageDMA(x, y0, w, h, (const lgfx::swap565_t*)d);
            s_dmaEnd = esp_timer_get_time() + wireUs(w * h);        // (it started inside the call: this is never early)
        });
        stripWait();
        M5.Display.endWrite();
#endif
        xSemaphoreGive(s_dispIdle);
    }
}
#endif

bool displayStreaming() { return s_stream; }

void displayInit() {
#ifndef MP_LEGACY_DISPLAY
#ifndef HOST
    if (auto* panel = M5.Display.getPanel()) {                        // the LCD write clock
        if (auto* bus = (lgfx::Bus_SPI*)panel->getBus()) {
            auto cfg = bus->config();
            cfg.freq_write = MP_LCD_MHZ * 1000000;               // (freq_read stays M5GFX's: the panel reads slower)
            bus->config(cfg);
            bus->init();
        }
    }
    displayBusPins();
    syncCalibrate();                                                  // the panel's refresh (before the screen task starts)
#endif
    s_strip[0] = stripAlloc(); s_strip[1] = stripAlloc();
    bool strips = s_strip[0] && s_strip[1];
    s_pk = (Packet*)memFast(sizeof(Packet));
    s_stream = strips && s_pk;
    if (s_stream) {
        s_dispIdle = xSemaphoreCreateBinary();
        xSemaphoreGive(s_dispIdle);
#ifndef HOST
        s_stripDone = xSemaphoreCreateBinary();
        if (s_stripDone) {                                            // no timer: the strips are waited out as before
            esp_timer_create_args_t ta = {};
            ta.callback = stripTimerCb;
            ta.name = "lcdstrip";
            if (esp_timer_create(&ta, &s_stripTimer) != ESP_OK) s_stripTimer = nullptr;
        }
#endif
        // above the sound task (4) and the speaker driver (5) on core 0: it only fills strips (~0.1 ms each) and sleeps
        xTaskCreatePinnedToCore(displayTask, "lcd", 4096, nullptr, 6, &s_dispTask, 0);
        return;
    }
    // could not get the strip buffers: fall back to the canvas pipeline below
#endif
    for (int i = 0; i < 2; i++) { s_fb[i]->setColorDepth(16); s_fb[i]->setPsram(true); }
    s_fb[0]->createSprite(DW, DH);
    s_double = s_fb[1]->createSprite(DW, DH) != nullptr;
    if (s_double) {
        s_dispIdle = xSemaphoreCreateBinary();
        xSemaphoreGive(s_dispIdle);
#ifdef MP_LEGACY_DISPLAY
        xTaskCreatePinnedToCore(displayTask, "lcd", 4096, nullptr, 2, &s_dispTask, 0);
#else
        s_double = false;                          // (the stream task isn't running: present synchronously)
#endif
    }
}

// The canvas pipeline: 2x nearest upscale into a whole-frame canvas (byte-swapped RGB565, same as the palette).
static void expand(M5Canvas* c) {
    uint16_t* dst = (uint16_t*)c->getBuffer();
    if (!dst) return;
    const uint8_t* fb = frameBuffer();
    const uint16_t* pal = framePalette565();
    const int bob = frameBob();
    for (int y = 0; y < DH; y++) {
        int src = (y + bob) >> 1;
        if (src > FB_H - 1) src = FB_H - 1;
        uint16_t* d = dst + y * DW;
        const uint8_t* s = fb + src * FB_W;
        const uint16_t* pa = pal + (((y + bob) & 1) << 9), *pb = pa + 256;
        for (int x = 0; x < FB_W; x++) { d[2 * x] = pa[s[x]]; d[2 * x + 1] = pb[s[x]]; }
    }
    if (s_ovOn) {                                       // the overlay at full resolution
        for (int y = 0; y < DH; y++) {
            if (!s_ovRow[y]) continue;
            const uint16_t* o = s_ov + y * DW;
            uint16_t* d = dst + y * DW;
            for (int x = 0; x < DW; x++) if (o[x]) d[x] = swap16(o[x]);
        }
    }
}

void displayPresent() {
    if (s_stream) {
        xSemaphoreTake(s_dispIdle, portMAX_DELAY);                       // the screen core has finished the last frame
        fillPacket();
        xTaskNotifyGive(s_dispTask);
        return;
    }
    if (!s_double) {
        expand(s_fb[0]);
        s_fb[0]->pushSprite(&M5.Display, 0, 0);
        return;
    }
    // wait for the LCD to finish the previous frame
    xSemaphoreTake(s_dispIdle, portMAX_DELAY);
    expand(s_fb[s_cur]);
    s_pushIdx = s_cur;
    xTaskNotifyGive(s_dispTask);
    s_cur ^= 1;
}

// ---- tests: both pipelines into a whole 320 x 240 screen (the pixels as sent to the LCD: byte-swapped) ----
void displayDebugLegacy(uint16_t* out) {
    static M5Canvas c(&M5.Display);
    static bool made = false;
    if (!made) { c.setColorDepth(16); c.createSprite(DW, DH); made = true; }
    expand(&c);
    memcpy(out, c.getBuffer(), DW * DH * sizeof(uint16_t));
}
void displayDebugStream(uint16_t* out) {
    static Packet* pk = nullptr;
    static uint16_t* st[2] = {nullptr, nullptr};
    if (!pk) { pk = (Packet*)memFast(sizeof(Packet)); st[0] = stripAlloc(); st[1] = stripAlloc(); }
    Packet* save = s_pk; s_pk = pk; fillPacket(); s_pk = save;
    pk->full = true;
    streamPacket(*pk, st, false, [out](int x, int y0, int w, int h, uint16_t* d) { for (int r = 0; r < h; r++) memcpy(out + (y0 + r) * DW + x, d + r * w, w * sizeof(uint16_t)); });
}

}
