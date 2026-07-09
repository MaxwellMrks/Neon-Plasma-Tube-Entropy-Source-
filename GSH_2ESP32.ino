/* =========================================================================
   GSh-2 PLASMA DISCHARGE ANALYZER   (doubles as a validated TRNG)
   6-mode OLED + thermal record w/ plasma dice + WiFi + plasma chirp audio
   -------------------------------------------------------------------------
   Pins:
     Noise ADC ........ GPIO35
     OLED SSD1309 ..... CS=5  DC=21  RES=22   (HW SPI: SCK=18  MOSI=23)
     Printer (EM5820) . UART2  TX=17->printer RXD   RX=16<-printer TXD  (9600)
     PRINT button ..... GPIO27 -> button -> GND   (internal pull-up)
     MODE  button ..... GPIO14 -> button -> GND   (internal pull-up)
     Audio DAC ........ GPIO25 -> 1uF (+ leg toward ESP32) -> PAM A+ (A- -> GND)

   Display modes (MODE cycles):
     FFT LO      -> 0-5 kHz zoom, live. Peak search ONLY in this band.
     FFT HI      -> full span (0 -> measured Nyquist), live. EMI/converter.
     FFT AVG LO  -> 8-frame average, 0-5 kHz. Smoothest drift number.
     FFT AVG HI  -> 8-frame average, full span. Spur survey screen.
     WAVEFORM    -> rolling time domain
     DIAG        -> stats incl. measured fs
   All FFT screens: 1 kHz ruler + live triangle marker under the peak.
   Certificate peak search is band-limited to 0-5 kHz (spur-proof).

   PLASMA CHIRP: short ping ~1/sec, only on the two LO screens. Pitch =
   AVG-LO interpolated peak / CHIRP_DIV. Ping timing jittered +/-150 ms
   from raw tube ADC LSBs. Hardware cosine gen: zero CPU cost.
   Auto-silenced during certificate printing.

   Printer pacing = PROVEN brutal constants for this EM5820 unit.

   WiFi:  /raw (waveform/stats)   /capture (uniform sampling, for FFT;
          true rate in X-Sample-Rate response header)
   Requires arduino-esp32 core 3.x (driver/dac_cosine.h).
   >>> EDIT PORTFOLIO_URL and WiFi credentials below.
   ========================================================================= */

#include <WiFi.h>
#include <WebServer.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <arduinoFFT.h>
#include "mbedtls/sha256.h"
#include "driver/dac_cosine.h"

// ============================ CONFIG =====================================
#define PORTFOLIO_URL  "https://maxwellansel.com"
// Local WiFi -- fill in on YOUR machine only. NEVER commit real credentials.
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

#define NOISE_PIN      35
#define PRINT_BTN_PIN  27
#define MODE_BTN_PIN   14

#define SAMPLES        256

// 20000 = the keeper: ~78 Hz bins, 10 kHz Nyquist covers the full mode
// family unfolded, and it's 4x a future ~5 kHz anti-alias corner.
// VERIFY after flash: DIAG fs must read 20.0k. All Hz math uses MEASURED fs.
#define SAMPLING_FREQ  20000

#define LO_BAND_HZ     5000      // FFT LO / AVG LO / certificate search band
#define TICK_HZ        1000      // ruler tick spacing (LO screens)

#define PRINT_WAVEFORM  1
#define PRINT_SPECTRUM  1
#define WAVE_IMG_H      64
#define SPEC_IMG_H      48
#define ADC_MV_FS       3300
#define FFT_AVG_N       8        // frames averaged for the FFT-AVG display modes

// --- plasma chirp audio (GPIO25 -> 1uF -> PAM A+) --------------  <<< CHIRP
#define CHIRP_ENABLED    1       // 0 = no audio at all
#define CHIRP_DIV        4
#define CHIRP_MS         60      // ping length (lands 60-90 ms w/ loop phase)
#define CHIRP_PERIOD_MS  1000    // nominal ping spacing
#define CHIRP_JITTER_MS  150     // +/- entropy jitter on spacing
#define CHIRP_MIN_HZ     140     // cosine gen floor guard
#define CHIRP_MAX_HZ     4000

// --- printer pacing (no CTS -> open-loop flow control) ---------  <<< PRINT FIX
// PROVEN values that fixed the stall on this unit. ~30 s per certificate.
#define IMG_ROWS_PER_CHUNK  1
#define IMG_CHUNK_DELAY_MS  100
#define IMG_GAP_MS          1000
// =========================================================================

WebServer server(80);
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI u8g2(U8G2_R0, /*CS=*/ 5, /*DC=*/ 21, /*RES=*/ 22);

double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);
unsigned int samplingPeriod;

#define WAVE_W 128
int waveform[WAVE_W];
int waveIdx = 0;
int curMin = 4095, curMax = 0, curMean = 0;
float curStdDev = 0, curCrest = 0;
int eventRate = 0, curDomHz = 0;
int eventThreshold = 200;

float curFs = SAMPLING_FREQ;   // measured achieved rate, updated every frame

bool serverStarted = false;
unsigned long certCount = 0;

// modes
enum { MODE_FFT_LO = 0, MODE_FFT_HI, MODE_FFT_AVG_LO, MODE_FFT_AVG_HI, MODE_WAVE, MODE_DIAG, MODE_COUNT };
int displayMode = MODE_FFT_LO;

float fftAvgBuf[FFT_AVG_N][SAMPLES/2];
int  fftAvgIdx = 0; bool fftAvgFilled = false;
float specTmp[SAMPLES/2];

// ---------- chirp state ----------                              <<< CHIRP
dac_cosine_handle_t chirpChan = NULL;
bool chirpOn = false;
unsigned long chirpOffAt = 0;
unsigned long nextChirpAt = 3000;   // first ping after boot settle
uint8_t frameEnt = 0;               // 8 raw ADC LSBs folded from each frame

// ---------- buttons (reusable debounced falling-edge) ----------
struct Button { int pin; int lastReading; int state; unsigned long lastChange; };
Button btnPrint = {PRINT_BTN_PIN, HIGH, HIGH, 0};
Button btnMode  = {MODE_BTN_PIN,  HIGH, HIGH, 0};

bool pressed(Button &b) {
  int reading = digitalRead(b.pin);
  bool event = false;
  if (reading != b.lastReading) b.lastChange = millis();
  if (millis() - b.lastChange > 50) {
    if (reading != b.state) { b.state = reading; if (b.state == LOW) event = true; }
  }
  b.lastReading = reading;
  return event;
}

// ---------- parabolic peak interpolation (sub-bin Hz) ----------
float parabolicDelta(double a, double b, double c) {
  double den = a - 2.0 * b + c;
  if (fabs(den) < 1e-9) return 0.0f;
  double d = 0.5 * (a - c) / den;
  if (d > 0.5) d = 0.5;
  if (d < -0.5) d = -0.5;
  return (float)d;
}

// ---------- chirp engine ----------                              <<< CHIRP
float avgLoPeakHz() {
  int half = SAMPLES / 2;
  int cnt = fftAvgFilled ? FFT_AVG_N : fftAvgIdx;
  if (cnt < 1) return 0;
  float binHz = curFs / SAMPLES;
  int loBin = 2;
  int hiBin = (int)(LO_BAND_HZ / binHz);
  if (hiBin > half - 2) hiBin = half - 2;
  if (hiBin < loBin + 2) hiBin = loBin + 2;
  int pk = loBin; float pm = 0;
  for (int i = loBin; i <= hiBin; i++) {
    float s = 0; for (int j = 0; j < cnt; j++) s += fftAvgBuf[j][i];
    if (s > pm) { pm = s; pk = i; }
  }
  float a = 0, b = 0, c = 0;
  for (int j = 0; j < cnt; j++) { a += fftAvgBuf[j][pk-1]; b += fftAvgBuf[j][pk]; c += fftAvgBuf[j][pk+1]; }
  float d = parabolicDelta(a, b, c);
  return (pk + d) * binHz;
}

void chirpStart(uint32_t hz) {
  if (chirpChan) { dac_cosine_del_channel(chirpChan); chirpChan = NULL; }
  dac_cosine_config_t cfg = {};
  cfg.chan_id = DAC_CHAN_0;                // GPIO25
  cfg.freq_hz = hz;
  cfg.clk_src = DAC_COSINE_CLK_SRC_DEFAULT;
  cfg.atten   = DAC_COSINE_ATTEN_DB_18;    // ~1/8 amplitude, right for 24dB amp
  cfg.phase   = DAC_COSINE_PHASE_0;
  cfg.offset  = 0;
  cfg.flags.force_set_freq = true;
  if (dac_cosine_new_channel(&cfg, &chirpChan) == ESP_OK) {
    dac_cosine_start(chirpChan);
    chirpOn = true;
  }
}

void chirpEnd() {
  if (chirpChan && chirpOn) dac_cosine_stop(chirpChan);
  chirpOn = false;
}

// =========================================================================
//  ESC/POS text helpers
// =========================================================================
void pInit()           { Serial2.write(0x1B); Serial2.write('@'); }
void pAlign(uint8_t a) { Serial2.write(0x1B); Serial2.write('a'); Serial2.write(a); }
void pBold(bool on)    { Serial2.write(0x1B); Serial2.write('E'); Serial2.write(on ? 1 : 0); }
void pSize(uint8_t n)  { Serial2.write(0x1D); Serial2.write('!'); Serial2.write(n); }
void pFeed(uint8_t n)  { Serial2.write(0x1B); Serial2.write('d'); Serial2.write(n); }
void rule()            { Serial2.println("--------------------------------"); }

void printQR(const char* data) {
  int len = strlen(data), store = len + 3;
  uint8_t pL = store & 0xFF, pH = (store >> 8) & 0xFF;
  Serial2.write(0x1D); Serial2.write('('); Serial2.write('k');
  Serial2.write(4); Serial2.write(0); Serial2.write(49); Serial2.write(65); Serial2.write(50); Serial2.write(0);
  Serial2.write(0x1D); Serial2.write('('); Serial2.write('k');
  Serial2.write(3); Serial2.write(0); Serial2.write(49); Serial2.write(67); Serial2.write(6);
  Serial2.write(0x1D); Serial2.write('('); Serial2.write('k');
  Serial2.write(3); Serial2.write(0); Serial2.write(49); Serial2.write(69); Serial2.write(49);
  Serial2.write(0x1D); Serial2.write('('); Serial2.write('k');
  Serial2.write(pL); Serial2.write(pH); Serial2.write(49); Serial2.write(80); Serial2.write(48);
  Serial2.print(data);
  Serial2.write(0x1D); Serial2.write('('); Serial2.write('k');
  Serial2.write(3); Serial2.write(0); Serial2.write(49); Serial2.write(81); Serial2.write(48);
}

// =========================================================================
//  1-bit raster engine (printed graphs + dice)
// =========================================================================
#define IMG_W_BYTES   48
#define IMG_W_DOTS    (IMG_W_BYTES*8)
#define IMG_MAX_ROWS  128
uint8_t img[IMG_W_BYTES * IMG_MAX_ROWS];
int imgH = 0;

void imgClear(int h) { if (h > IMG_MAX_ROWS) h = IMG_MAX_ROWS; imgH = h; memset(img, 0, IMG_W_BYTES * h); }
void imgSet(int x, int y) {
  if (x < 0 || x >= IMG_W_DOTS || y < 0 || y >= imgH) return;
  img[y * IMG_W_BYTES + (x >> 3)] |= (0x80 >> (x & 7));
}
void imgLine(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  while (true) {
    imgSet(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
void imgBorder() {
  for (int x = 0; x < IMG_W_DOTS; x++) { imgSet(x, 0); imgSet(x, imgH - 1); }
  for (int y = 0; y < imgH; y++) { imgSet(0, y); imgSet(IMG_W_DOTS - 1, y); }
}

// ------------------------------------------------------------  <<< PRINT FIX
// GS v 0 declares "exactly 48*imgH pixel bytes follow"; the printer COUNTS
// bytes until met. RX overflow while the head is busy = dropped bytes =
// starved counter = stall. Pace blind with proven margins.
void imgSend() {
  Serial2.write(0x1D); Serial2.write('v'); Serial2.write('0'); Serial2.write((uint8_t)0);
  Serial2.write((uint8_t)(IMG_W_BYTES & 0xFF)); Serial2.write((uint8_t)((IMG_W_BYTES >> 8) & 0xFF));
  Serial2.write((uint8_t)(imgH & 0xFF)); Serial2.write((uint8_t)((imgH >> 8) & 0xFF));
  Serial2.flush();
  for (int row = 0; row < imgH; row += IMG_ROWS_PER_CHUNK) {
    int rows = min(IMG_ROWS_PER_CHUNK, imgH - row);
    Serial2.write(img + (size_t)row * IMG_W_BYTES, (size_t)rows * IMG_W_BYTES);
    Serial2.flush();
    delay(IMG_CHUNK_DELAY_MS);
  }
  delay(IMG_GAP_MS);
}

void renderWaveform(int* data, int n, int h, int meanVal) {
  imgClear(h);
  int lo = 4095, hi = 0;
  for (int i = 0; i < n; i++) { if (data[i] < lo) lo = data[i]; if (data[i] > hi) hi = data[i]; }
  int range = hi - lo; if (range < 20) range = 20;
  int pad = range / 20 + 1; lo -= pad; hi += pad; range = hi - lo; if (range < 1) range = 1;
  imgBorder();
  int by = (h - 1) - (int)((long)(meanVal - lo) * (h - 1) / range); by = constrain(by, 1, h - 2);
  for (int x = 2; x < IMG_W_DOTS - 2; x += 6) { imgSet(x, by); imgSet(x + 1, by); }
  int prevY = (h - 1) - (int)((long)(data[0] - lo) * (h - 1) / range); prevY = constrain(prevY, 0, h - 1);
  for (int x = 1; x < IMG_W_DOTS; x++) {
    int idx = (int)((long)x * n / IMG_W_DOTS); if (idx >= n) idx = n - 1;
    int y = (h - 1) - (int)((long)(data[idx] - lo) * (h - 1) / range); y = constrain(y, 0, h - 1);
    imgLine(x - 1, prevY, x, y); prevY = y;
  }
}

void renderSpectrum(int h) {
  imgClear(h);
  int half = SAMPLES / 2;
  double mxm = 1; for (int i = 2; i < half; i++) if (vReal[i] > mxm) mxm = vReal[i];
  double lmax = log(mxm + 1); if (lmax <= 0) lmax = 1;
  imgBorder();
  int prevY = h - 2;
  for (int x = 1; x < IMG_W_DOTS - 1; x++) {
    int bin = 2 + (int)((long)x * (half - 2) / IMG_W_DOTS); if (bin >= half) bin = half - 1;
    double m = vReal[bin]; if (m < 0) m = 0;
    int barH = (int)((log(m + 1) / lmax) * (h - 2)); barH = constrain(barH, 0, h - 2);
    int y = (h - 1) - barH; y = constrain(y, 1, h - 2);
    imgLine(x - 1, prevY, x, y); prevY = y;
  }
}

// ---- dice (pip-art) ----
void drawPip(int cx, int cy, int r) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= r * r) imgSet(cx + dx, cy + dy);
}
void drawDie(int x, int y, int s, int val) {
  for (int i = 0; i < s; i++) { imgSet(x + i, y); imgSet(x + i, y + s - 1); imgSet(x, y + i); imgSet(x + s - 1, y + i); }
  int a = x + s / 4, b = x + s / 2, c = x + 3 * s / 4;
  int d = y + s / 4, e = y + s / 2, f = y + 3 * s / 4;
  int r = s / 8; if (r < 2) r = 2;
  switch (val) {
    case 1: drawPip(b, e, r); break;
    case 2: drawPip(a, d, r); drawPip(c, f, r); break;
    case 3: drawPip(a, d, r); drawPip(b, e, r); drawPip(c, f, r); break;
    case 4: drawPip(a, d, r); drawPip(c, d, r); drawPip(a, f, r); drawPip(c, f, r); break;
    case 5: drawPip(a, d, r); drawPip(c, d, r); drawPip(b, e, r); drawPip(a, f, r); drawPip(c, f, r); break;
    case 6: drawPip(a, d, r); drawPip(c, d, r); drawPip(a, e, r); drawPip(c, e, r); drawPip(a, f, r); drawPip(c, f, r); break;
  }
}
void printDice(uint8_t vals[5]) {
  int s = 54;
  int gap = (IMG_W_DOTS - 5 * s) / 6;
  imgClear(s + 4);
  int x = gap;
  for (int i = 0; i < 5; i++) { drawDie(x, 2, s, vals[i]); x += s + gap; }
  imgSend();
}

// =========================================================================
//  Entropy from the tube (SHA-256 whitened) -> dice + hex serial
// =========================================================================
void makeEntropy(uint8_t hash[32]) {
  const int N = 128;
  uint8_t buf[N];
  for (int i = 0; i < N; i++) {
    uint8_t b = 0;
    for (int k = 0; k < 8; k++) { b = (b << 1) | (analogRead(NOISE_PIN) & 0x01); delayMicroseconds(80); }
    buf[i] = b;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, buf, N);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
}

// =========================================================================
//  OLED status + display modes
// =========================================================================
void oledStatus(const char* l1, const char* l2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(8, 28, l1);
  if (l2) u8g2.drawStr(8, 44, l2);
  u8g2.sendBuffer();
}

// ---------- frequency ruler + live peak marker ----------
void drawRuler(float spanHz, float tickHz, float peakHz) {
  const int base = 57;
  u8g2.drawHLine(0, base, 128);
  for (float f = tickHz; f < spanHz; f += tickHz) {
    int x = (int)(f / spanHz * 127.0f + 0.5f);
    if (x < 1 || x > 126) continue;
    u8g2.drawVLine(x, base + 1, ((int)(f / tickHz)) % 5 == 0 ? 4 : 2);
  }
  if (peakHz > 0 && peakHz <= spanHz) {
    int px = (int)(peakHz / spanHz * 127.0f + 0.5f);
    px = constrain(px, 2, 125);
    u8g2.drawVLine(px,     base + 1, 5);
    u8g2.drawVLine(px - 1, base + 3, 3);
    u8g2.drawVLine(px + 1, base + 3, 3);
  }
  char sl[12]; snprintf(sl, sizeof(sl), "%.1fk", spanHz / 1000.0f);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(128 - 4 * strlen(sl), 63, sl);
  u8g2.drawStr(0, 63, "0");
}

// ---------- shared FFT screen renderer ----------
void drawFFTscreen(bool avg, bool zoomLo, const char* label) {
  int half = SAMPLES / 2;
  if (avg) {
    int cnt = fftAvgFilled ? FFT_AVG_N : fftAvgIdx; if (cnt < 1) cnt = 1;
    for (int i = 0; i < half; i++) { float s = 0; for (int j = 0; j < cnt; j++) s += fftAvgBuf[j][i]; specTmp[i] = s / cnt; }
  } else {
    for (int i = 0; i < half; i++) specTmp[i] = (float)vReal[i];
  }

  float binHz = curFs / SAMPLES;
  int loBin = 2;
  int hiBin = half - 1;
  if (zoomLo) {
    int b = (int)(LO_BAND_HZ / binHz);
    if (b < loBin + 2) b = loBin + 2;
    if (b < hiBin) hiBin = b;
  }
  int nBins = hiBin - loBin + 1; if (nBins < 1) nBins = 1;

  int pk = loBin; float pm = 0;
  for (int i = loBin; i <= hiBin; i++) if (specTmp[i] > pm) { pm = specTmp[i]; pk = i; }
  float d = 0;
  if (pk > 0 && pk < half - 1) d = parabolicDelta(specTmp[pk - 1], specTmp[pk], specTmp[pk + 1]);
  float peakHz = (pk + d) * binHz;

  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(0, 6, label);
  char hb[16]; snprintf(hb, sizeof(hb), "%d Hz", (int)(peakHz + 0.5f));
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 16, hb);

  float mx = 1; for (int i = loBin; i <= hiBin; i++) if (specTmp[i] > mx) mx = specTmp[i];
  int top = 18, bot = 55, h = bot - top;
  for (int x = 0; x < 128; x++) {
    int i = loBin + (int)((long)x * nBins / 128); if (i > hiBin) i = hiBin;
    int barH = (int)(specTmp[i] / mx * h); barH = constrain(barH, 0, h);
    u8g2.drawVLine(x, bot - barH, barH);
  }

  float spanHz = (hiBin + 1) * binHz;
  float tick = TICK_HZ;
  if (!zoomLo) { tick = (spanHz > 16000) ? 5000 : 2000; }
  drawRuler(spanHz, tick, peakHz);
}

void drawWaveMode() {
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(0, 6, "WAVEFORM");
  int top = 10, bot = 63, h = bot - top;
  int lo = curMin, hi = curMax, range = hi - lo; if (range < 30) range = 30;
  for (int x = 0; x < WAVE_W - 1; x++) {
    int i0 = (waveIdx + x) % WAVE_W, i1 = (waveIdx + x + 1) % WAVE_W;
    int y0 = constrain(bot - ((waveform[i0] - lo) * h / range), top, bot);
    int y1 = constrain(bot - ((waveform[i1] - lo) * h / range), top, bot);
    u8g2.drawLine(x, y0, x + 1, y1);
  }
}
void drawDiagMode() {
  u8g2.setFont(u8g2_font_5x8_tr);
  char b[42];
  u8g2.drawStr(0, 8, "-- DIAGNOSTICS --");
  snprintf(b, sizeof(b), "mean %d   sd %d", curMean, (int)curStdDev);       u8g2.drawStr(0, 18, b);
  snprintf(b, sizeof(b), "p2p %d  crest %.1f", curMax - curMin, curCrest);  u8g2.drawStr(0, 28, b);
  snprintf(b, sizeof(b), "events/s %d", eventRate);                         u8g2.drawStr(0, 38, b);
  snprintf(b, sizeof(b), "dom %dHz fs %.1fk", curDomHz, curFs / 1000.0f);   u8g2.drawStr(0, 48, b);
  if (serverStarted) { snprintf(b, sizeof(b), "%s", WiFi.localIP().toString().c_str()); u8g2.drawStr(0, 58, b); }
  else u8g2.drawStr(0, 58, "standalone");
}

// =========================================================================
//  Certificate
// =========================================================================
void printCertificate() {
#if CHIRP_ENABLED
  chirpEnd();                     // never let a ping drone through a print
#endif
  oledStatus("CAPTURING", "discharge...");

  int snap[SAMPLES];
  long sum = 0, sumSq = 0; int mn = 4095, mx = 0;
  unsigned long tCap0 = micros();
  for (int i = 0; i < SAMPLES; i++) {
    unsigned long t = micros();
    int v = analogRead(NOISE_PIN);
    snap[i] = v; vReal[i] = v; vImag[i] = 0;
    sum += v; sumSq += (long)v * v;
    if (v < mn) mn = v; if (v > mx) mx = v;
    while (micros() - t < samplingPeriod) {}
  }
  unsigned long capUs = micros() - tCap0;
  float certFs = (capUs > 0) ? (float)SAMPLES * 1000000.0f / capUs : (float)SAMPLING_FREQ;

  int mean = sum / SAMPLES, events = 0;
  for (int i = 0; i < SAMPLES; i++) if (abs(snap[i] - mean) > eventThreshold) events++;
  float meanF = (float)sum / SAMPLES;
  float var = ((float)sumSq / SAMPLES) - (meanF * meanF);
  float sd = (var > 0) ? sqrt(var) : 0;
  int p2p = mx - mn, peakDev = max(mx - mean, mean - mn);
  float crest = (sd > 0) ? peakDev / sd : 0;
  float evPerSec = events * certFs / SAMPLES;
  int sdmV = (int)(sd * (ADC_MV_FS / 4095.0));

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();
  int domHi = (int)(LO_BAND_HZ / (certFs / SAMPLES));    // <<< CERT GUARD
  if (domHi > SAMPLES / 2 - 2) domHi = SAMPLES / 2 - 2;  // spur-proof paper:
  int domBin = 2; double domMag = 0;                     // search LO band only
  for (int i = 2; i <= domHi; i++) if (vReal[i] > domMag) { domMag = vReal[i]; domBin = i; }
  float domD = parabolicDelta(vReal[domBin - 1], vReal[domBin], vReal[domBin + 1]);
  int domHz = (int)((domBin + domD) * certFs / SAMPLES + 0.5f);

  uint8_t hash[32]; makeEntropy(hash);
  uint8_t dice[5]; for (int i = 0; i < 5; i++) dice[i] = (hash[i] % 6) + 1;
  char hex[33]; for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02X", hash[i]); hex[32] = 0;
  certCount++;

  oledStatus("PRINTING", "record...");

  pInit(); delay(20);
  pAlign(1); pBold(true); pSize(0x11);
  Serial2.println("PLASMA DISCHARGE");
  Serial2.println("CERTIFICATE");
  pSize(0x00); pBold(false);
  Serial2.println("Ionization Event Record");
  rule();
  Serial2.println("GSh-2 neon gas-discharge tube");
  Serial2.println("1979 Soviet manufacture");
  Serial2.println("Anode bias ~198 V DC");
  rule();
  Serial2.println();

  char line[40];
  pAlign(0); pBold(true);
  Serial2.println("DISCHARGE MEASUREMENT");
  pBold(false);
  snprintf(line, sizeof(line), "Window: %.1fms @ %.1f kSps", capUs / 1000.0f, certFs / 1000.0f);
  Serial2.println(line);
  snprintf(line, sizeof(line), "Operating pt:   %4d cts", mean);    Serial2.println(line);
  snprintf(line, sizeof(line), "RMS fluct.:     %4d cts", (int)sd); Serial2.println(line);
  snprintf(line, sizeof(line), "                ~%4d mV", sdmV);     Serial2.println(line);
  snprintf(line, sizeof(line), "Peak-to-peak:   %4d cts", p2p);     Serial2.println(line);
  snprintf(line, sizeof(line), "Crest factor:   %4.1f", crest);     Serial2.println(line);
  snprintf(line, sizeof(line), "Ionization evt: %4d", events);      Serial2.println(line);
  snprintf(line, sizeof(line), "  -> %d events/s", (int)evPerSec);  Serial2.println(line);
  snprintf(line, sizeof(line), "Peak (0-5kHz):  %4d Hz", domHz);    Serial2.println(line);   // <<< CERT GUARD
  rule();

#if PRINT_WAVEFORM
  pAlign(1);
  snprintf(line, sizeof(line), "ANODE NOISE WAVEFORM (%.1fms)", capUs / 1000.0f);
  Serial2.println(line);
  renderWaveform(snap, SAMPLES, WAVE_IMG_H, mean); imgSend(); Serial2.println();
#endif
#if PRINT_SPECTRUM
  pAlign(1);
  snprintf(line, sizeof(line), "NOISE SPECTRUM 0-%.1fkHz (log)", certFs / 2000.0f);
  Serial2.println(line);
  renderSpectrum(SPEC_IMG_H); imgSend(); Serial2.println();
#endif

  rule();
  pAlign(1); pBold(true); pSize(0x11);
  Serial2.println("PLASMA DICE ROLL");
  pSize(0x00); pBold(false);
  printDice(dice);
  Serial2.println("rolled by ionization events");
  Serial2.println("each roll physically unrepeatable");
  pAlign(0);
  snprintf(line, sizeof(line), "SN: %s", hex); Serial2.println(line);
  rule();

  pAlign(1);
  Serial2.println(PORTFOLIO_URL);
  printQR(PORTFOLIO_URL);
  Serial2.println();
  snprintf(line, sizeof(line), "Record No. %04lu", certCount); Serial2.println(line);
  Serial2.println("Open Sauce SF 2026");
  pFeed(4);
}

// =========================================================================
//  WiFi endpoints
// =========================================================================
void handleRaw() {
  String out = ""; out.reserve(12000);
  for (int i = 0; i < 2000; i++) { out += String(analogRead(NOISE_PIN)); out += "\n"; delayMicroseconds(300); }
  server.send(200, "text/plain", out);
}
void handleCapture() {
  const int N = 2048;
  static uint16_t cap[N];
  unsigned long t0 = micros();
  for (int i = 0; i < N; i++) { unsigned long t = micros(); cap[i] = analogRead(NOISE_PIN); while (micros() - t < samplingPeriod) {} }
  unsigned long el = micros() - t0;
  float fsCap = (el > 0) ? (float)N * 1000000.0f / el : (float)SAMPLING_FREQ;
  String out; out.reserve(N * 6);
  for (int i = 0; i < N; i++) { out += cap[i]; out += '\n'; }
  server.sendHeader("X-Sample-Rate", String(fsCap, 1));
  server.send(200, "text/plain", out);
}

// =========================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  pinMode(PRINT_BTN_PIN, INPUT_PULLUP);
  pinMode(MODE_BTN_PIN,  INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(NOISE_PIN, ADC_11db);
  u8g2.begin();

  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ready - http://"); Serial.println(WiFi.localIP());
    Serial.println("  /raw   /capture");
    server.on("/raw", handleRaw);
    server.on("/capture", handleCapture);
    server.begin();
    serverStarted = true;
  } else {
    Serial.println("WiFi not connected - running standalone");
  }
  samplingPeriod = round(1000000.0 / SAMPLING_FREQ);
}

// =========================================================================
void loop() {
  if (pressed(btnMode)) displayMode = (displayMode + 1) % MODE_COUNT;
  if (pressed(btnPrint) && millis() > 2500) printCertificate();

  if (serverStarted) server.handleClient();

  long sum = 0, sumSq = 0;
  int mn = 4095, mx = 0, events = 0;
  unsigned long tFrame0 = micros();
  for (int i = 0; i < SAMPLES; i++) {
    unsigned long t = micros();
    int v = analogRead(NOISE_PIN);
    vReal[i] = v; vImag[i] = 0;
    sum += v; sumSq += (long)v * v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    if (curMean > 0 && abs(v - curMean) > eventThreshold) events++;
    while (micros() - t < samplingPeriod) {}
  }
  unsigned long frUs = micros() - tFrame0;
  if (frUs > 0) curFs = (float)SAMPLES * 1000000.0f / frUs;

  // fold 8 raw LSBs from this frame BEFORE the FFT overwrites vReal
  frameEnt = 0;
  for (int k = 0; k < 8; k++) frameEnt = (frameEnt << 1) | (((int)vReal[k * 31 + 5]) & 1);

  curMean = sum / SAMPLES; curMin = mn; curMax = mx;
  float meanF = (float)sum / SAMPLES;
  float variance = ((float)sumSq / SAMPLES) - (meanF * meanF);
  curStdDev = (variance > 0) ? sqrt(variance) : 0;
  int peakDev = max(curMax - curMean, curMean - curMin);
  curCrest = (curStdDev > 0) ? peakDev / curStdDev : 0;
  eventRate = (int)(events * curFs / SAMPLES);

  for (int i = 0; i < SAMPLES; i += 4) { waveform[waveIdx] = vReal[i]; waveIdx = (waveIdx + 1) % WAVE_W; }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  int db = 2; double dm = 0;
  for (int i = 2; i < SAMPLES / 2; i++) if (vReal[i] > dm) { dm = vReal[i]; db = i; }
  float dd = parabolicDelta(vReal[db - 1], vReal[db], vReal[db + 1]);
  curDomHz = (int)((db + dd) * curFs / SAMPLES + 0.5f);

  for (int i = 0; i < SAMPLES / 2; i++) fftAvgBuf[fftAvgIdx][i] = (float)vReal[i];
  fftAvgIdx = (fftAvgIdx + 1) % FFT_AVG_N;
  if (fftAvgIdx == 0) fftAvgFilled = true;

#if CHIRP_ENABLED
  unsigned long nowMs = millis();
  if (chirpOn && nowMs >= chirpOffAt) chirpEnd();
  bool loMode = (displayMode == MODE_FFT_LO || displayMode == MODE_FFT_AVG_LO);
  if (!chirpOn && loMode && nowMs >= nextChirpAt) {
    int jit = ((int)frameEnt - 128) * (int)CHIRP_JITTER_MS / 128;   // tube-rolled
    nextChirpAt = nowMs + CHIRP_PERIOD_MS + jit;
    float pkHz = avgLoPeakHz();
    if (pkHz > 0) {
      uint32_t hz = (uint32_t)(pkHz / CHIRP_DIV + 0.5f);
      if (hz < CHIRP_MIN_HZ) hz = CHIRP_MIN_HZ;
      if (hz > CHIRP_MAX_HZ) hz = CHIRP_MAX_HZ;
      chirpStart(hz);
      if (chirpOn) chirpOffAt = nowMs + CHIRP_MS;
    }
  }
#endif

  u8g2.clearBuffer();
  switch (displayMode) {
    case MODE_FFT_LO:     drawFFTscreen(false, true,  "FFT LO 0-5k");   break;
    case MODE_FFT_HI:     drawFFTscreen(false, false, "FFT HI FULL");   break;
    case MODE_FFT_AVG_LO: drawFFTscreen(true,  true,  "FFT AVG 0-5k");  break;
    case MODE_FFT_AVG_HI: drawFFTscreen(true,  false, "FFT AVG FULL");  break;
    case MODE_WAVE:       drawWaveMode();  break;
    case MODE_DIAG:       drawDiagMode();  break;
  }
  u8g2.sendBuffer();
}
