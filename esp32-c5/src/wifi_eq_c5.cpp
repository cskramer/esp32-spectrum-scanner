/*
  wifi_eq_c5.cpp  —  Dual-band (2.4 + 5 GHz) Wi-Fi spectrum "equalizer", network list,
  live packet view and pcap recorder.

  Board: RockBase NM-CYD-C5 (ESP32-C5, 2.8" 240x320 ST7789, XPT2046 resistive touch, microSD)

  SCREEN (one view, both bands)
  - Top:    2.4 GHz equalizer (channels 1-13, overlapping ~22 MHz shapes)
  - Middle: 5 GHz equalizer, one bar per 20 MHz channel, grouped into its sub-bands
            (UNII-1 36-48 | UNII-2A 52-64 | UNII-2C 100-144 | UNII-3 149-165)
  - Bottom: strongest networks on both bands: channel, signal, name, channel width,
            and an "AX" badge for Wi-Fi 6 networks
  - Touch bar: REC | VIEW | HOLD

  CONTROLS
  - REC button (or short press of BOOT): cycles OFF -> 2.4 GHz -> 5 GHz -> BOTH.
    The choice takes effect 1.5 s after the last tap, so you can cycle past modes
    without creating files. Recording one band locks the scanner to that band.
  - VIEW button (or long press of BOOT): spectrum <-> live packets.
  - HOLD button: stay on the current channel. Or tap a bar to hold that channel;
    tap it again to release.

  RECORDING
  - /cap_NNNN_2G.pcap, _5G.pcap or _ALL.pcap on the microSD card (FAT32), with radiotap
    headers carrying frequency and signal strength. Open in Wireshark, or: tshark -r file.
  - Timestamps count from power-on (no real-time clock), so Wireshark shows 1970 dates.
  - The RGB LED glows red while recording, amber while a new mode is arming.

  The ESP32-C5 has one radio: it hears one channel at a time and hops across both bands.
  It does not receive 6 GHz (Wi-Fi 6E). Receive-only: nothing is transmitted.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <SPI.h>
#include <SdFat.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <XPT2046_Touchscreen.h>
#include "freertos/ringbuf.h"

#if !defined(CONFIG_IDF_TARGET_ESP32C5)
  #error "This sketch is for the dual-band ESP32-C5. Use the root project for the original ESP32."
#endif

// ======================= Pins: RockBase NM-CYD-C5 =======================
// Display, touch and SD card share one SPI bus with separate chip-selects.
#define PIN_SCK    6
#define PIN_MOSI   7
#define PIN_MISO   2
#define TFT_CS    23
#define TFT_DC    24
#define TFT_RST   -1      // tied to the board's reset line
#define TFT_BL    25      // backlight, HIGH = on
#define TOUCH_CS   1
#define SD_CS     10
#define LED_PIN   27      // WS2812 RGB LED

// The BOOT button is normally GPIO28 on the ESP32-C5; one pin map lists GPIO0.
// Both are checked; the serial monitor says which one fired.
const uint8_t BOOT_PIN_CANDIDATES[] = {28, 0};

// ======================= Display / touch settings =======================
const uint8_t ROTATION       = 1;      // try 3 if the image is upside down
const bool    INVERT_COLORS  = false;  // set true if the background shows white
const uint8_t TOUCH_ROTATION = 1;
// Raw XPT2046 range mapped to the screen. Watch the serial monitor while tapping
// the corners (TOUCH_DEBUG) and adjust if taps land in the wrong place.
const int  TS_MINX = 200, TS_MAXX = 3700, TS_MINY = 240, TS_MAXY = 3800;
const bool TS_FLIP_X = true, TS_FLIP_Y = true;    // correct for the NM-CYD-C5
const bool TOUCH_DEBUG = false;

// ======================= Radio / animation tuning =======================
const char    *COUNTRY       = "US";   // sets which 5 GHz channels the radio allows
const uint16_t DWELL_MS      = 45;     // time on each channel (~1.7 s for a full dual-band sweep)
const uint16_t FRAME_MS      = 40;     // ~25 fps
const uint16_t PKT_REDRAW_MS = 100;

const float RSSI_FLOOR     = -95.0f;   // dBm shown as empty
const float RSSI_CEIL      = -30.0f;   // dBm shown as full
const float ATTACK         = 0.55f;    // how fast bars rise
const float FALL_PER_FRAME = 0.03f;    // how fast bars fall
const float CHAN_DECAY     = 0.985f;   // channel level decay between visits
const uint8_t PEAK_HOLD    = 15;       // frames a peak cap hangs
const float PEAK_FALL      = 0.015f;
const uint32_t AP_TIMEOUT_MS = 45000;  // drop networks not heard for this long
const uint16_t REC_ARM_MS    = 1500;   // delay before a new REC mode takes effect
const uint16_t LONG_PRESS_MS = 1000;   // BOOT hold time for VIEW

// ======================= Channels =======================
const uint8_t CHANS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,                    // 2.4 GHz
  36, 40, 44, 48,                                               // UNII-1
  52, 56, 60, 64,                                               // UNII-2A (DFS)
  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,   // UNII-2C (DFS)
  149, 153, 157, 161, 165                                       // UNII-3
};
const int NCH = sizeof(CHANS);
const int N24 = 13;                // indexes 0..12 are 2.4 GHz
const int N5  = NCH - N24;         // indexes 13.. are 5 GHz
const uint8_t LBL5[] = {36, 52, 100, 116, 132, 149, 165};

int chanIndex(uint8_t ch) {
  for (int i = 0; i < NCH; i++) if (CHANS[i] == ch) return i;
  return -1;
}
uint16_t chanFreq(uint8_t ch) { return ch <= 14 ? (ch == 14 ? 2484 : 2407 + 5 * ch) : 5000 + 5 * ch; }

// ======================= Geometry (320x240 landscape) =======================
const int HDR_H   = 12;
const int AX_W    = 24;                      // left label column
const int PANEL_H = 72;                      // each band panel
const int P24_Y   = 12, P5_Y = 86;
const int SEGS = 15, SEG_PITCH = 4, SEG_H = 3;
const int MB = 60, MARK_Y = 61, LABEL_Y = 64; // inside a panel
const int N24BARS = 37, BAR24_PITCH = 8, BAR24_W = 6;
const float F_START = 2400.0f, BIN_W = 84.0f / N24BARS;
const int SLOT_PITCH = 11, SLOT_W = 9, GROUP_GAP = 7;
const int LIST_Y = 160, ROW_H = 10, LIST_ROWS = 5, LIST_COLS = 2, COL_W = 160;
const int BTN_Y = 212, BTN_H = 26, BTN_W = 104;
const int PKT_HDR_Y = 14, PKT_Y = 25, PKT_ROWS = 18, INFO_CHARS = 20;

#define BLACK 0x0000
#define WHITE 0xFFFF

Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS);
GFXcanvas16 *panelCanvas = nullptr;   // one band panel, drawn off-screen then pushed
GFXcanvas16 *rowCanvas   = nullptr;   // one text row for the packet view

uint16_t lit24[SEGS], dim24[SEGS], lit5[SEGS], dim5[SEGS];
uint16_t COL_24, COL_5, COL_TEXT, COL_AXIS, COL_GRID, COL_SSID, COL_HIDDEN, COL_AX, COL_OFF;
uint16_t COL_PROBE, COL_JOIN, COL_DEAUTH, COL_DATA, COL_OTHER;

// ======================= Shared state =======================
volatile uint8_t  curIdx = 0;
volatile int8_t   rxMax[NCH];
volatile uint16_t rxCnt[NCH];
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
bool chanOK[NCH];

struct APInfo {
  uint8_t  bssid[6];
  char     ssid[33];
  int8_t   rssi;
  uint8_t  ch, width;
  bool     ax, used;
  uint32_t seen;
};
const int MAX_APS = 64;
APInfo aps[MAX_APS];
portMUX_TYPE apMux = portMUX_INITIALIZER_UNLOCKED;

struct Pkt {
  uint8_t ch;
  int8_t  rssi;
  uint8_t fc;
  uint8_t src[6];
  char    info[INFO_CHARS + 1];
};
const int RING_SIZE = 64;
Pkt ring[RING_SIZE];
volatile uint32_t ringHead = 0;
uint32_t ringTail = 0;
portMUX_TYPE pktMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool packetMode = false;

// ---- Recording ----
enum RecMode : uint8_t { REC_OFF, REC_24, REC_5, REC_ALL };
const char *REC_NAME[]   = {"OFF", "2.4 GHz", "5 GHz", "BOTH"};
const char *REC_SUFFIX[] = {"", "2G", "5G", "ALL"};
RecMode recMode = REC_OFF, pendingMode = REC_OFF;
uint32_t pendingSince = 0;

const uint16_t CAP_SNAPLEN    = 2324;
const size_t   CAP_RING_BYTES = 32768;
const uint8_t  RT_LEN         = 15;

struct CapHdr { uint32_t tsSec, tsUsec; uint16_t origLen; uint8_t ch; int8_t rssi; };
struct __attribute__((packed)) PcapGlobal {
  uint32_t magic = 0xa1b2c3d4; uint16_t vMajor = 2, vMinor = 4;
  int32_t thiszone = 0; uint32_t sigfigs = 0, snaplen = 65535, linktype = 127;  // 127 = radiotap
};
struct __attribute__((packed)) PcapRec { uint32_t tsSec, tsUsec, inclLen, origLen; };

RingbufHandle_t capRing = nullptr;
volatile bool     recording = false;
volatile uint32_t capDrops  = 0;
SdFs     sd;                 // SdFat: FAT32 and exFAT cards
FsFile   capFile;
bool     sdOK = false;

bool sdBegin() { return sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &SPI)); }
char     capName[24] = "";
uint32_t capPkts = 0, capBytes = 0, lastFlush = 0;
char     recMsg[28] = "";
uint32_t recMsgUntil = 0;

// ---- Display / hopping / input ----
struct Bar { float d, p; uint8_t h; };
Bar bars24[N24BARS], bars5[N5];
float chanLevel[NCH];
bool  rowUsed[LIST_ROWS * LIST_COLS];
Pkt   shown[PKT_ROWS];
int   shownCount = 0;

uint8_t hopList[NCH];
int     hopCount = 0, hopPos = 0;
bool    holdOn = false;
uint8_t holdIdx = 0;

uint8_t  bootPins[2];
int      nBootPins = 0;
bool     btnUp = true, longFired = false;
uint32_t btnChange = 0, pressStart = 0;
bool     wasTouched = false;
uint32_t lastTouchPoll = 0, lastTouchAct = 0;

uint32_t lastHop = 0, lastFrame = 0, lastStat = 0, lastPktDraw = 0, pktAccum = 0;

// ============================================================
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
float rssiLevel(float rssi) { return constrain((rssi - RSSI_FLOOR) / (RSSI_CEIL - RSSI_FLOOR), 0.0f, 1.0f); }
uint16_t rssiColor(int rssi, bool band5) {
  int s = (int)(rssiLevel(rssi) * (SEGS - 1));
  return band5 ? lit5[s] : lit24[s];
}

// ======================= Sniffer (runs in the Wi-Fi task) =======================
struct BeaconInfo { char ssid[33]; uint8_t dsCh, htCh, width; bool ax; };

// Walk the tagged parameters of a management frame
void parseIEs(const uint8_t *f, int pos, int len, BeaconInfo &b) {
  while (pos + 2 <= len) {
    uint8_t id = f[pos], l = f[pos + 1];
    const uint8_t *d = f + pos + 2;
    if (pos + 2 + l > len) break;
    switch (id) {
      case 0:                                         // SSID
        if (l <= 32) {
          bool allZero = true;
          for (int k = 0; k < l; k++) {
            if (d[k]) allZero = false;
            b.ssid[k] = (d[k] >= 32 && d[k] < 127) ? (char)d[k] : '?';
          }
          b.ssid[allZero ? 0 : l] = 0;                // zero-filled name = hidden network
        }
        break;
      case 3:                                         // DS parameter set: channel
        if (l >= 1) b.dsCh = d[0];
        break;
      case 61:                                        // HT operation: primary ch, 40 MHz
        if (l >= 2) {
          b.htCh = d[0];
          if ((d[1] & 3) && b.width < 40) b.width = 40;
        }
        break;
      case 192:                                       // VHT operation: 80 / 160 MHz
        if (l >= 3) {
          if (d[0] == 1) b.width = (d[2] && abs((int)d[2] - (int)d[1]) == 8) ? 160 : 80;
          else if (d[0] == 2) b.width = 160;
        }
        break;
      case 255:                                       // extension: HE (Wi-Fi 6)
        if (l >= 1 && (d[0] == 35 || d[0] == 36)) b.ax = true;
        break;
    }
    pos += 2 + l;
  }
}

void recordAP(const uint8_t *bssid, const BeaconInfo &b, uint8_t ch, int8_t rssi) {
  uint32_t now = millis();
  portENTER_CRITICAL(&apMux);
  int slot = -1;
  for (int i = 0; i < MAX_APS; i++)
    if (aps[i].used && memcmp(aps[i].bssid, bssid, 6) == 0) { slot = i; break; }
  if (slot >= 0) {
    aps[slot].rssi = (aps[slot].rssi * 3 + rssi) / 4;
  } else {
    for (int i = 0; i < MAX_APS; i++) if (!aps[i].used) { slot = i; break; }
    if (slot < 0) {
      slot = 0;
      for (int i = 1; i < MAX_APS; i++) if (aps[i].seen < aps[slot].seen) slot = i;
    }
    memcpy(aps[slot].bssid, bssid, 6);
    aps[slot].rssi = rssi;
    aps[slot].used = true;
  }
  memcpy(aps[slot].ssid, b.ssid, sizeof(b.ssid));
  aps[slot].ch    = ch;
  aps[slot].width = b.width;
  aps[slot].ax    = b.ax;
  aps[slot].seen  = now;
  portEXIT_CRITICAL(&apMux);
}

void captureFrame(const uint8_t *f, int len, uint8_t ch, int8_t rssi) {
  uint16_t clen = len > CAP_SNAPLEN ? CAP_SNAPLEN : len;
  void *slot = nullptr;
  if (xRingbufferSendAcquire(capRing, &slot, sizeof(CapHdr) + clen, 0) == pdTRUE && slot) {
    CapHdr *h = (CapHdr *)slot;
    int64_t t = esp_timer_get_time();
    h->tsSec   = (uint32_t)(t / 1000000);
    h->tsUsec  = (uint32_t)(t % 1000000);
    h->origLen = (uint16_t)len;
    h->ch      = ch;
    h->rssi    = rssi;
    memcpy((uint8_t *)slot + sizeof(CapHdr), f, clen);
    xRingbufferSendComplete(capRing, slot);
  } else {
    capDrops++;                                       // SD card couldn't keep up
  }
}

void snifferCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t idx  = curIdx;                        // channel we are tuned to
  const uint8_t ch   = CHANS[idx];
  const int8_t  rssi = p->rx_ctrl.rssi;

  portENTER_CRITICAL(&rxMux);
  if (rssi > rxMax[idx]) rxMax[idx] = rssi;
  rxCnt[idx]++;
  portEXIT_CRITICAL(&rxMux);

  const uint8_t *f = p->payload;
  int len = (int)p->rx_ctrl.sig_len - 4;              // strip FCS

  if (recording && capRing && len > 0) captureFrame(f, len, ch, rssi);
  if (len < 24) return;

  uint8_t fc = f[0];
  uint8_t ftype = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
  bool beaconLike = (ftype == 0) && (sub == 8 || sub == 5);   // beacon / probe response
  bool probeReq   = (ftype == 0) && (sub == 4);

  BeaconInfo b = {};
  b.width = 20;
  if (beaconLike && len > 36) {
    parseIEs(f, 36, len, b);                          // 24-byte header + 12 fixed bytes
    uint8_t apCh = b.dsCh ? b.dsCh : (b.htCh ? b.htCh : ch);
    if (chanIndex(apCh) >= 0) recordAP(f + 16, b, apCh, rssi);
  } else if (probeReq) {
    parseIEs(f, 24, len, b);
  }

  if (packetMode) {
    portENTER_CRITICAL(&pktMux);
    Pkt &e = ring[ringHead % RING_SIZE];
    e.ch = ch; e.rssi = rssi; e.fc = fc;
    memcpy(e.src, f + 10, 6);                         // transmitter address
    if (beaconLike)    strncpy(e.info, b.ssid[0] ? b.ssid : "<hidden>", INFO_CHARS);
    else if (probeReq) strncpy(e.info, b.ssid[0] ? b.ssid : "(any)", INFO_CHARS);
    else               e.info[0] = 0;
    e.info[INFO_CHARS] = 0;
    ringHead++;
    portEXIT_CRITICAL(&pktMux);
  }
}

// ======================= Palette =======================
void gradRGB(const uint8_t st[4][3], float t, uint8_t &r, uint8_t &g, uint8_t &b) {
  float x = t * 3.0f;
  int i = (int)x; if (i > 2) i = 2;
  float f = x - i;
  r = st[i][0] + (st[i + 1][0] - st[i][0]) * f;
  g = st[i][1] + (st[i + 1][1] - st[i][1]) * f;
  b = st[i][2] + (st[i + 1][2] - st[i][2]) * f;
}

void buildPalette() {
  static const uint8_t G24[4][3] = {{0, 230, 255}, {130, 70, 255}, {255, 0, 170}, {255, 150, 0}};  // cyan -> orange
  static const uint8_t G5[4][3]  = {{0, 255, 140}, {0, 190, 255}, {80, 90, 255}, {210, 60, 255}};   // green -> violet
  for (int s = 0; s < SEGS; s++) {
    uint8_t r, g, b;
    float t = (float)s / (SEGS - 1);
    gradRGB(G24, t, r, g, b);
    lit24[s] = rgb565(r, g, b);
    dim24[s] = rgb565(r * 0.13f, g * 0.13f, b * 0.13f);
    gradRGB(G5, t, r, g, b);
    lit5[s] = rgb565(r, g, b);
    dim5[s] = rgb565(r * 0.13f, g * 0.13f, b * 0.13f);
  }
  COL_24     = rgb565(0, 230, 255);
  COL_5      = rgb565(0, 255, 140);
  COL_TEXT   = rgb565(150, 150, 170);
  COL_AXIS   = rgb565(95, 95, 115);
  COL_GRID   = rgb565(60, 60, 75);
  COL_SSID   = rgb565(225, 225, 235);
  COL_HIDDEN = rgb565(100, 100, 115);
  COL_AX     = rgb565(255, 210, 0);
  COL_OFF    = rgb565(35, 35, 40);
  COL_PROBE  = rgb565(255, 200, 0);
  COL_JOIN   = rgb565(0, 255, 120);
  COL_DEAUTH = rgb565(255, 60, 60);
  COL_DATA   = rgb565(255, 0, 170);
  COL_OTHER  = rgb565(130, 110, 255);
}

// ======================= Spectrum =======================
float chanShape(float d) {                            // ~22 MHz channel: flat top, soft skirts
  d = fabsf(d);
  if (d <= 8.0f)  return 1.0f;
  if (d >= 12.0f) return 0.0f;
  float t = (d - 8.0f) / 4.0f;
  return 1.0f - t * t * (3.0f - 2.0f * t);
}
int x24(uint8_t ch) { return AX_W + (int)((chanFreq(ch) - F_START) / BIN_W * BAR24_PITCH); }
int slotX(int j)    { return AX_W + j * SLOT_PITCH + ((j >= 4) + (j >= 8) + (j >= 20)) * GROUP_GAP; }

void stepBar(Bar &b, float tgt) {
  if (tgt > b.d) b.d += (tgt - b.d) * ATTACK;
  else           b.d = max(tgt, b.d - FALL_PER_FRAME);
  if (b.d >= b.p)  { b.p = b.d; b.h = PEAK_HOLD; }
  else if (b.h)    b.h--;
  else             b.p = max(0.0f, b.p - PEAK_FALL);
}

void drawSegs(GFXcanvas16 &c, int x, int w, const Bar &b, const uint16_t *lit, const uint16_t *dim) {
  int n  = constrain((int)lroundf(b.d * SEGS), 0, SEGS);
  int pk = (b.p > 0.04f) ? constrain((int)(b.p * SEGS), 0, SEGS - 1) : -1;
  for (int s = 0; s < SEGS; s++) {
    uint16_t col = (s == pk) ? WHITE : (s < n ? lit[s] : dim[s]);
    c.fillRect(x, MB - s * SEG_PITCH - SEG_H, w, SEG_H, col);
  }
}

void drawAxis(GFXcanvas16 &c, const char *band, uint16_t col) {
  c.setTextSize(1);
  c.setTextWrap(false);
  c.setTextColor(col);
  c.setCursor(1, 1);  c.print(band);
  c.setCursor(1, 10); c.print("GHz");
  c.setTextColor(COL_AXIS);
  const int ticks[] = {-80, -60};
  for (int dbm : ticks) {
    int y = MB - (int)(rssiLevel(dbm) * SEGS * SEG_PITCH);
    c.setCursor(1, y - 3); c.print(dbm);
    c.drawFastHLine(AX_W - 4, y, 3, COL_AXIS);
  }
}

void printCentered(GFXcanvas16 &c, int cx, int y, int num, uint16_t col) {
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", num);
  int w = strlen(buf) * 6;
  int x = constrain(cx - w / 2 + 1, AX_W, 320 - w);
  c.setTextColor(col);
  c.setCursor(x, y);
  c.print(buf);
}

void drawPanel24() {
  GFXcanvas16 &c = *panelCanvas;
  c.fillScreen(BLACK);
  drawAxis(c, "2.4", COL_24);
  for (int i = 0; i < N24BARS; i++)
    drawSegs(c, AX_W + i * BAR24_PITCH + 1, BAR24_W, bars24[i], lit24, dim24);
  for (int i = 0; i < N24; i++)
    printCentered(c, x24(CHANS[i]), LABEL_Y, CHANS[i], curIdx == i ? WHITE : COL_TEXT);
  if (curIdx < N24) c.fillRect(x24(CHANS[curIdx]) - 3, MARK_Y, 7, 2, COL_24);
  tft.drawRGBBitmap(0, P24_Y, c.getBuffer(), 320, PANEL_H);
}

void drawPanel5() {
  GFXcanvas16 &c = *panelCanvas;
  c.fillScreen(BLACK);
  drawAxis(c, "5", COL_5);
  for (int j = 0; j < N5; j++) {
    int x = slotX(j) + 1;
    if (chanOK[N24 + j]) {
      drawSegs(c, x, SLOT_W, bars5[j], lit5, dim5);
    } else {                                          // channel the radio refused
      for (int s = 0; s < SEGS; s++) c.fillRect(x, MB - s * SEG_PITCH - SEG_H, SLOT_W, SEG_H, COL_OFF);
    }
  }
  for (uint8_t ch : LBL5) {
    int j = chanIndex(ch) - N24;
    printCentered(c, slotX(j) + SLOT_PITCH / 2, LABEL_Y, ch, curIdx == N24 + j ? WHITE : COL_TEXT);
  }
  if (curIdx >= N24) c.fillRect(slotX(curIdx - N24) + 1, MARK_Y, SLOT_W, 2, COL_5);
  tft.drawRGBBitmap(0, P5_Y, c.getBuffer(), 320, PANEL_H);
}

void frameSpectrum() {
  for (int i = 0; i < NCH; i++) chanLevel[i] *= CHAN_DECAY;

  for (int i = 0; i < N24BARS; i++) {
    float f = F_START + (i + 0.5f) * BIN_W, tgt = 0;
    for (int c = 0; c < N24; c++) {
      float v = chanLevel[c] * chanShape(f - chanFreq(CHANS[c]));
      if (v > tgt) tgt = v;
    }
    stepBar(bars24[i], tgt);
  }
  for (int j = 0; j < N5; j++) stepBar(bars5[j], chanLevel[N24 + j]);

  drawPanel24();
  drawPanel5();
}

void finishDwell(uint8_t idx) {
  int8_t mx; uint16_t n;
  portENTER_CRITICAL(&rxMux);
  mx = rxMax[idx]; n = rxCnt[idx];
  rxMax[idx] = -128; rxCnt[idx] = 0;
  portEXIT_CRITICAL(&rxMux);

  pktAccum += n;
  if (n == 0) return;
  float lvl = rssiLevel(mx);                          // bar height = strongest signal
  if (lvl > chanLevel[idx]) chanLevel[idx] = lvl;
  else                      chanLevel[idx] = 0.5f * (chanLevel[idx] + lvl);
}

// ======================= Network list =======================
int updateList(bool draw) {
  static APInfo snap[MAX_APS];
  int n = 0;
  uint32_t now = millis();
  portENTER_CRITICAL(&apMux);
  for (int i = 0; i < MAX_APS; i++)
    if (aps[i].used && now - aps[i].seen < AP_TIMEOUT_MS) snap[n++] = aps[i];
  portEXIT_CRITICAL(&apMux);

  for (int i = 1; i < n; i++) {                       // strongest first
    APInfo t = snap[i]; int j = i - 1;
    while (j >= 0 && snap[j].rssi < t.rssi) { snap[j + 1] = snap[j]; j--; }
    snap[j + 1] = t;
  }
  int m = 0;                                          // merge mesh nodes: same name + same band
  for (int i = 0; i < n; i++) {
    bool dup = false;
    if (snap[i].ssid[0])
      for (int k = 0; k < m; k++)
        if ((snap[k].ch > 14) == (snap[i].ch > 14) && strcmp(snap[k].ssid, snap[i].ssid) == 0) { dup = true; break; }
    if (!dup) snap[m++] = snap[i];
  }
  if (!draw) return m;

  tft.setTextSize(1);
  for (int r = 0; r < LIST_ROWS * LIST_COLS; r++) {
    int x = (r / LIST_ROWS) * COL_W;
    int y = LIST_Y + (r % LIST_ROWS) * ROW_H;
    if (r >= m) {
      if (rowUsed[r]) tft.fillRect(x, y, COL_W, ROW_H, BLACK);
      rowUsed[r] = false;
      continue;
    }
    const APInfo &a = snap[r];
    bool b5 = a.ch > 14;
    uint16_t c = rssiColor(a.rssi, b5);
    char buf[24];

    tft.setTextColor(b5 ? COL_5 : COL_24, BLACK);
    snprintf(buf, sizeof(buf), "%3u", a.ch);
    tft.setCursor(x, y); tft.print(buf);

    tft.setTextColor(c, BLACK);
    snprintf(buf, sizeof(buf), "%4d", a.rssi);
    tft.setCursor(x + 18, y); tft.print(buf);

    bool hidden = a.ssid[0] == 0;
    tft.setTextColor(hidden ? COL_HIDDEN : COL_SSID, BLACK);
    snprintf(buf, sizeof(buf), "%-12.12s", hidden ? "<hidden>" : a.ssid);
    tft.setCursor(x + 48, y); tft.print(buf);

    tft.setTextColor(COL_TEXT, BLACK);
    if (a.width > 20) snprintf(buf, sizeof(buf), "%3u", a.width);
    else              strcpy(buf, "   ");
    tft.setCursor(x + 124, y); tft.print(buf);

    tft.setTextColor(COL_AX, BLACK);
    tft.setCursor(x + 144, y); tft.print(a.ax ? "AX" : "  ");

    int w = 4 + (int)(rssiLevel(a.rssi) * 150);       // strength underline
    tft.drawFastHLine(x + 2, y + 8, w, c);
    tft.drawFastHLine(x + 2 + w, y + 8, 154 - w, BLACK);
    rowUsed[r] = true;
  }
  return m;
}

// ======================= Live packets =======================
const char *frameName(uint8_t fc, uint16_t &col) {
  uint8_t t = (fc >> 2) & 3, s = (fc >> 4) & 0xF;
  if (t == 0) {
    switch (s) {
      case 0:  col = COL_JOIN;   return "ASSOCQ";
      case 1:  col = COL_JOIN;   return "ASSOCR";
      case 2:  col = COL_JOIN;   return "REASSQ";
      case 3:  col = COL_JOIN;   return "REASSR";
      case 4:  col = COL_PROBE;  return "PROBEQ";
      case 5:  col = COL_24;     return "PROBER";
      case 8:  col = COL_24;     return "BEACON";
      case 10: col = COL_DEAUTH; return "DISASC";
      case 11: col = COL_JOIN;   return "AUTH";
      case 12: col = COL_DEAUTH; return "DEAUTH";
      case 13: col = COL_OTHER;  return "ACTION";
      default: col = COL_OTHER;  return "MGMT";
    }
  }
  if (t == 2) {
    col = COL_DATA;
    switch (s) {
      case 4:  return "NULL";
      case 8:  return "QOS";
      case 12: return "QOSNUL";
      default: return "DATA";
    }
  }
  col = COL_OTHER;
  return "CTRL";
}

void drawPacketStatic() {
  tft.setTextSize(1);
  tft.setTextColor(COL_GRID, BLACK);
  tft.setCursor(2,   PKT_HDR_Y); tft.print("CH");
  tft.setCursor(26,  PKT_HDR_Y); tft.print("dBm");
  tft.setCursor(50,  PKT_HDR_Y); tft.print("TYPE");
  tft.setCursor(92,  PKT_HDR_Y); tft.print("FROM");
  tft.setCursor(200, PKT_HDR_Y); tft.print("NETWORK");
  shownCount = 0;
}

void drawPacketRow(int r) {
  GFXcanvas16 &c = *rowCanvas;
  c.fillScreen(BLACK);
  if (r < shownCount) {
    const Pkt &p = shown[r];
    bool b5 = p.ch > 14;
    char buf[24];
    c.setTextSize(1);
    c.setTextWrap(false);

    c.setTextColor(b5 ? COL_5 : COL_24);
    snprintf(buf, sizeof(buf), "%3u", p.ch);
    c.setCursor(0, 1);   c.print(buf);

    c.setTextColor(rssiColor(p.rssi, b5));
    snprintf(buf, sizeof(buf), "%4d", p.rssi);
    c.setCursor(20, 1);  c.print(buf);

    uint16_t tc;
    const char *name = frameName(p.fc, tc);
    c.setTextColor(tc);
    c.setCursor(50, 1);  c.print(name);

    c.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             p.src[0], p.src[1], p.src[2], p.src[3], p.src[4], p.src[5]);
    c.setCursor(92, 1);  c.print(buf);

    c.setTextColor(COL_SSID);
    c.setCursor(200, 1); c.print(p.info);
  }
  tft.drawRGBBitmap(0, PKT_Y + r * ROW_H, c.getBuffer(), 320, ROW_H);
}

void updatePackets() {
  bool changed = false;
  while (true) {
    Pkt e;
    portENTER_CRITICAL(&pktMux);
    uint32_t head = ringHead;
    if (head - ringTail > RING_SIZE) ringTail = head - RING_SIZE;
    bool have = ringTail != head;
    if (have) { e = ring[ringTail % RING_SIZE]; ringTail++; }
    portEXIT_CRITICAL(&pktMux);
    if (!have) break;
    memmove(&shown[1], &shown[0], sizeof(Pkt) * (PKT_ROWS - 1));
    shown[0] = e;
    if (shownCount < PKT_ROWS) shownCount++;
    changed = true;
  }
  if (!changed) return;
  for (int r = 0; r < PKT_ROWS; r++) drawPacketRow(r);
}

// ======================= Header, buttons, LED =======================
void drawHeader(int nets, uint32_t pps) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%2d networks %6lu pkt/s", nets, (unsigned long)pps);
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, BLACK);
  tft.setCursor(2, 2);
  tft.print(buf);
}

void drawRecStatus() {
  const int W = 23, X = 320 - 1 - W * 6, Y = 2;
  char inner[28], buf[28];
  tft.setTextSize(1);
  if (recording) {
    snprintf(inner, sizeof(inner), "REC %6lupk %5.1fMB", (unsigned long)capPkts, capBytes / 1048576.0);
    snprintf(buf, sizeof(buf), "%*s", W, inner);
    tft.setTextColor(COL_DEAUTH, BLACK);
    tft.setCursor(X, Y); tft.print(buf);
    int dotX = 320 - 1 - (int)strlen(inner) * 6 - 6;
    tft.fillCircle(dotX, Y + 3, 3, ((millis() / 1000) % 2) ? COL_DEAUTH : BLACK);
  } else if (millis() < recMsgUntil) {
    snprintf(buf, sizeof(buf), "%*.*s", W, W, recMsg);
    tft.setTextColor(COL_DEAUTH, BLACK);
    tft.setCursor(X, Y); tft.print(buf);
  } else {
    tft.fillRect(X - 10, 0, 320 - X + 10, HDR_H - 1, BLACK);
  }
}

void setLed() {
  if (recMode != REC_OFF)          rgbLedWrite(LED_PIN, 60, 0, 0);    // recording
  else if (pendingMode != recMode) rgbLedWrite(LED_PIN, 40, 20, 0);   // arming
  else                             rgbLedWrite(LED_PIN, 0, 0, 0);
}

void drawButton(int i, const char *label, uint16_t fg, uint16_t bg) {
  int x = 2 + i * (BTN_W + 2);
  tft.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 5, bg);
  tft.drawRoundRect(x, BTN_Y, BTN_W, BTN_H, 5, fg);
  tft.setTextSize(1);
  tft.setTextColor(fg);
  tft.setCursor(x + (BTN_W - (int)strlen(label) * 6) / 2, BTN_Y + 9);
  tft.print(label);
}

void drawButtons() {
  char buf[24];
  bool arming = pendingMode != recMode;
  snprintf(buf, sizeof(buf), arming ? "REC: %s ..." : "REC: %s", REC_NAME[pendingMode]);
  bool redLabel = recMode != REC_OFF || pendingMode != REC_OFF;
  drawButton(0, buf, redLabel ? COL_DEAUTH : COL_TEXT, recMode != REC_OFF ? rgb565(60, 0, 0) : BLACK);

  drawButton(1, packetMode ? "VIEW: PACKETS" : "VIEW: SPECTRUM", COL_24, BLACK);

  if (holdOn) snprintf(buf, sizeof(buf), "HOLD: CH %u", CHANS[holdIdx]);
  else        strcpy(buf, "HOLD CHANNEL");
  drawButton(2, buf, holdOn ? COL_5 : COL_TEXT, holdOn ? rgb565(0, 45, 25) : BLACK);
  setLed();
}

// ======================= Channel hopping =======================
bool tuneTo(uint8_t idx) {
  esp_err_t e = esp_wifi_set_channel(CHANS[idx], WIFI_SECOND_CHAN_NONE);
  if (e != ESP_OK) {
    if (chanOK[idx]) Serial.printf("Channel %u unavailable (%s) - skipping it\n", CHANS[idx], esp_err_to_name(e));
    chanOK[idx] = false;
    return false;
  }
  curIdx = idx;
  return true;
}

void rebuildHop() {
  hopCount = 0;
  if (holdOn) {
    hopList[hopCount++] = holdIdx;
  } else {
    int a = 0, b = NCH;
    if (recMode == REC_24)     b = N24;               // recording one band: stay on it
    else if (recMode == REC_5) a = N24;
    for (int i = a; i < b; i++) if (chanOK[i]) hopList[hopCount++] = i;
  }
  if (hopCount == 0) hopList[hopCount++] = 0;
  hopPos = 0;
  finishDwell(curIdx);
  tuneTo(hopList[0]);
  lastHop = millis();
}

void hop() {
  finishDwell(curIdx);
  if (hopCount <= 1) return;
  for (int tries = 0; tries < hopCount; tries++) {
    hopPos = (hopPos + 1) % hopCount;
    if (chanOK[hopList[hopPos]] && tuneTo(hopList[hopPos])) return;
  }
}

void holdChannel(int idx) {
  if (idx < 0 || !chanOK[idx]) return;
  if (holdOn && holdIdx == idx) holdOn = false;
  else { holdOn = true; holdIdx = idx; }
  rebuildHop();
  drawButtons();
}

// ======================= Recording =======================
void showRecMsg(const char *msg) {
  strncpy(recMsg, msg, sizeof(recMsg) - 1);
  recMsg[sizeof(recMsg) - 1] = 0;
  recMsgUntil = millis() + 4000;
}

void discardRing() {
  size_t sz; void *it;
  while ((it = xRingbufferReceive(capRing, &sz, 0)) != nullptr) vRingbufferReturnItem(capRing, it);
}

bool startRecording(RecMode m) {
  if (!sdOK) sdOK = sdBegin();
  if (!sdOK) { showRecMsg("NO SD CARD"); return false; }

  for (int i = 1; i < 10000; i++) {                   // first number not used by any mode
    bool used = false;
    for (int s = 1; s <= 3 && !used; s++) {
      snprintf(capName, sizeof(capName), "/cap_%04d_%s.pcap", i, REC_SUFFIX[s]);
      used = sd.exists(capName);
    }
    if (!used) { snprintf(capName, sizeof(capName), "/cap_%04d_%s.pcap", i, REC_SUFFIX[m]); break; }
  }
  if (!capFile.open(capName, O_WRONLY | O_CREAT | O_TRUNC)) { showRecMsg("SD WRITE FAILED"); return false; }

  PcapGlobal g;
  capFile.write((const uint8_t *)&g, sizeof(g));
  discardRing();
  capPkts = 0; capBytes = sizeof(g); capDrops = 0;
  lastFlush = millis();
  recording = true;
  Serial.printf("Recording %s to %s\n", REC_NAME[m], capName);
  return true;
}

void drainCapture(bool allItems) {
  if (!capFile) return;
  uint32_t t0 = millis();
  while (allItems || millis() - t0 < 15) {
    size_t sz;
    uint8_t *item = (uint8_t *)xRingbufferReceive(capRing, &sz, 0);
    if (!item) break;

    const CapHdr *h = (const CapHdr *)item;
    uint16_t clen = sz - sizeof(CapHdr);
    uint16_t freq = chanFreq(h->ch);
    bool b5 = h->ch > 14;
    uint8_t rt[RT_LEN] = {
      0, 0, RT_LEN, 0,                    // version, pad, length
      0x2A, 0, 0, 0,                      // present: flags, channel, dBm signal
      0,                                  // flags
      0,                                  // pad (channel field is 2-byte aligned)
      (uint8_t)(freq & 0xFF), (uint8_t)(freq >> 8),
      (uint8_t)(b5 ? 0x00 : 0x80), (uint8_t)(b5 ? 0x01 : 0x00),   // 2 GHz or 5 GHz
      (uint8_t)h->rssi
    };
    PcapRec rec = { h->tsSec, h->tsUsec, (uint32_t)(RT_LEN + clen), (uint32_t)(RT_LEN + h->origLen) };

    size_t w = capFile.write((const uint8_t *)&rec, sizeof(rec));
    w += capFile.write(rt, RT_LEN);
    w += capFile.write(item + sizeof(CapHdr), clen);
    vRingbufferReturnItem(capRing, item);

    if (w != sizeof(rec) + RT_LEN + clen) {           // card pulled or full
      recording = false;
      capFile.close();
      sdOK = false;
      recMode = pendingMode = REC_OFF;
      rebuildHop();
      drawButtons();
      showRecMsg("SD WRITE FAILED");
      return;
    }
    capPkts++;
    capBytes += w;
  }
  if (millis() - lastFlush > 2000) {
    capFile.flush();
    lastFlush = millis();
  }
}

void stopRecording() {
  recording = false;
  delay(5);
  drainCapture(true);
  if (capFile) capFile.close();
  Serial.printf("Saved %s: %lu packets, %lu dropped\n", capName,
                (unsigned long)capPkts, (unsigned long)capDrops);
  char msg[28];
  snprintf(msg, sizeof(msg), "SAVED %s", capName + 1);
  showRecMsg(msg);
}

void applyRec(RecMode m) {
  if (recording) stopRecording();
  recMode = m;
  rebuildHop();
  if (m != REC_OFF && !startRecording(m)) {
    recMode = REC_OFF;
    rebuildHop();
  }
  pendingMode = recMode;
  drawButtons();
  drawRecStatus();
}

void cycleRec() {
  pendingMode = (RecMode)((pendingMode + 1) % 4);
  pendingSince = millis();
  drawButtons();
}

// ======================= Views & input =======================
void setView(bool packets) {
  tft.fillRect(0, HDR_H, 320, BTN_Y - HDR_H - 1, BLACK);
  if (packets) {
    portENTER_CRITICAL(&pktMux);
    ringTail = ringHead;
    portEXIT_CRITICAL(&pktMux);
    drawPacketStatic();
  } else {
    for (int r = 0; r < LIST_ROWS * LIST_COLS; r++) rowUsed[r] = false;
  }
  packetMode = packets;
  drawButtons();
  lastStat = 0;
}

int channelAtTouch(int x, int y) {
  if (x < AX_W) return -1;
  if (y >= P24_Y && y < P24_Y + PANEL_H) {
    int best = -1, bestD = 1000;
    for (int i = 0; i < N24; i++) {
      int d = abs(x - x24(CHANS[i]));
      if (d < bestD) { bestD = d; best = i; }
    }
    return best;
  }
  if (y >= P5_Y && y < P5_Y + PANEL_H) {
    for (int j = 0; j < N5; j++)
      if (x >= slotX(j) - 2 && x < slotX(j) + SLOT_PITCH + 2) return N24 + j;
  }
  return -1;
}

void handleTouch(uint32_t now) {
  if (now - lastTouchPoll < 30) return;
  lastTouchPoll = now;
  bool t = ts.touched();
  if (t && !wasTouched && now - lastTouchAct > 250) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, TS_MINX, TS_MAXX, 0, 319);
    int y = map(p.y, TS_MINY, TS_MAXY, 0, 239);
    if (TS_FLIP_X) x = 319 - x;
    if (TS_FLIP_Y) y = 239 - y;
    x = constrain(x, 0, 319);
    y = constrain(y, 0, 239);
    if (TOUCH_DEBUG) {
      Serial.printf("Touch raw x=%d y=%d z=%d -> screen %d,%d\n", p.x, p.y, p.z, x, y);
      char dbg[28];                                   // also show it on screen
      snprintf(dbg, sizeof(dbg), "T %4d,%4d>%3d,%3d", p.x, p.y, x, y);
      showRecMsg(dbg);
      drawRecStatus();
      tft.drawCircle(x, y, 5, WHITE);                 // where the tap registered
    }

    if (y >= BTN_Y - 8) {                             // button bar (generous hit area)
      int b = x / (BTN_W + 2);
      if (b == 0)      cycleRec();
      else if (b == 1) setView(!packetMode);
      else             holdChannel(holdOn ? holdIdx : curIdx);
    } else if (!packetMode) {
      holdChannel(channelAtTouch(x, y));              // tap a bar to hold that channel
    }
    lastTouchAct = now;
  }
  wasTouched = t;
}

bool bootDown() {
  for (int i = 0; i < nBootPins; i++)
    if (digitalRead(bootPins[i]) == LOW) return true;
  return false;
}

void handleBoot(uint32_t now) {
  bool up = !bootDown();
  if (up != btnUp && now - btnChange > 40) {
    btnChange = now;
    btnUp = up;
    if (!up) {
      pressStart = now; longFired = false;
      for (int i = 0; i < nBootPins; i++)
        if (digitalRead(bootPins[i]) == LOW) Serial.printf("BOOT button is GPIO%u\n", bootPins[i]);
    } else if (!longFired) {
      cycleRec();                                     // short press
    }
  }
  if (!btnUp && !longFired && now - pressStart >= LONG_PRESS_MS) {
    longFired = true;
    setView(!packetMode);                             // long press
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nWi-Fi EQ C5 starting");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  const uint8_t csPins[] = {TFT_CS, TOUCH_CS, SD_CS};  // deselect everything on the shared bus
  for (uint8_t pin : csPins) { pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); }
  rgbLedWrite(LED_PIN, 0, 0, 0);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.invertDisplay(INVERT_COLORS);
  tft.setRotation(ROTATION);
  tft.fillScreen(BLACK);

  ts.begin();                     // uses the shared SPI bus started above
  ts.setRotation(TOUCH_ROTATION);

  panelCanvas = new GFXcanvas16(320, PANEL_H);
  rowCanvas   = new GFXcanvas16(320, ROW_H);
  if (!panelCanvas->getBuffer() || !rowCanvas->getBuffer()) {
    Serial.println("Out of memory for display buffers");
    while (true) delay(1000);
  }
  buildPalette();

  capRing = xRingbufferCreate(CAP_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
  sdOK = sdBegin();
  Serial.println(sdOK ? "SD card ready" : "No SD card (will retry when recording starts)");

  for (uint8_t pin : BOOT_PIN_CANDIDATES) {           // ignore any candidate that reads LOW at rest
    pinMode(pin, INPUT_PULLUP);
    delay(2);
    if (digitalRead(pin) == HIGH) bootPins[nBootPins++] = pin;
  }

  for (int i = 0; i < NCH; i++) { rxMax[i] = -128; rxCnt[i] = 0; chanLevel[i] = 0; chanOK[i] = true; }
  for (int i = 0; i < MAX_APS; i++) aps[i].used = false;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_country_code(COUNTRY, false);
  esp_err_t e = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);   // allow 2.4 and 5 GHz
  if (e != ESP_OK) Serial.printf("Band mode error: %s\n", esp_err_to_name(e));

  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&snifferCb);
  esp_wifi_set_promiscuous(true);

  setView(false);
  rebuildHop();
  lastFrame = lastPktDraw = millis();
}

void loop() {
  uint32_t now = millis();

  handleBoot(now);
  handleTouch(now);
  if (pendingMode != recMode && now - pendingSince >= REC_ARM_MS) applyRec(pendingMode);
  if (recording) drainCapture(false);

  if (now - lastHop >= DWELL_MS) {
    hop();
    lastHop = now;
  }

  if (!packetMode && now - lastFrame >= FRAME_MS) {
    frameSpectrum();
    lastFrame = now;
  }
  if (packetMode && now - lastPktDraw >= PKT_REDRAW_MS) {
    updatePackets();
    lastPktDraw = now;
  }

  if (now - lastStat >= 1000) {
    int nets = updateList(!packetMode);
    drawHeader(nets, pktAccum);
    drawRecStatus();
    pktAccum = 0;
    lastStat = now;
  }
}
