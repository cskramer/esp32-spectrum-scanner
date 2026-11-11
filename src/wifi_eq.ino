/*
  wifi_eq.ino  —  2.4 GHz Wi-Fi spectrum "equalizer" + SSID list + live packet view
  Board: ESP32-32E 2.8" (240x320), Adafruit GFX + ILI9341 (or ST7789 with -DLCD_ST7789),
  same libraries and build flags as the Kiruna clock.

  BOOT button:  short press = switch screens,  hold ~1 s = start/stop recording to microSD.

  SPECTRUM
  - The ESP32 listens passively (promiscuous mode) and hops channels 1-13.
  - Each channel's strongest signal is spread across the ~22 MHz it occupies and drawn
    as 36 LED-segment bars (height = signal in dBm) with fast attack, slow fall and
    peak caps. Axes: dBm on the left, Wi-Fi channel along the bottom.
  - The 12 strongest networks are listed underneath with channel, signal and a strength bar.

  LIVE PACKETS
  - A scrolling feed of packets as they are heard: channel, signal, frame type,
    transmitting device's MAC address, and network name where the packet carries one.
  - Only packet headers are shown; data contents are encrypted and are never decoded.

  RECORDING
  - Every packet heard is written to /cap_NNNN.pcap on the microSD card (FAT32), with a
    radiotap header carrying channel and signal strength. Open it in Wireshark, or get
    text with:  tshark -r cap_0001.pcap
  - Timestamps are time since power-on (the board has no clock), so Wireshark shows 1970
    dates; relative times between packets are accurate.
  - A red REC indicator in the header shows packets and MB written.

  Receive-only: nothing is transmitted.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <FS.h>
#include <SD.h>
#include <esp_timer.h>
#include "freertos/ringbuf.h"

// ------------------- Pins (check against your clock sketch) -------------------
#define TFT_SCLK 14
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1     // reset is tied to EN on these boards
#define BACKLIGHT_PIN 21
#define BUTTON_PIN 0    // BOOT button

#define SD_SCLK 18      // microSD slot (separate SPI bus from the display)
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS    5

SPIClass hspi(HSPI);
SPIClass sdspi(VSPI);

#ifdef LCD_ST7789
  #include <Adafruit_ST7789.h>
  Adafruit_ST7789 tft(&hspi, TFT_CS, TFT_DC, TFT_RST);
#else
  #include <Adafruit_ILI9341.h>
  Adafruit_ILI9341 tft(&hspi, TFT_DC, TFT_CS, TFT_RST);
#endif

#define BLACK 0x0000
#define WHITE 0xFFFF

// ------------------- Tuning -------------------
const uint8_t  ROTATION   = 1;  // try 3 if the image is upside down
const uint16_t DWELL_MS   = 70; // time listening on each channel
const uint16_t FRAME_MS   = 33; // ~30 fps animation
const uint16_t PKT_REDRAW_MS = 100; // live packet view refresh
const uint8_t  FIRST_CH   = 1;
const uint8_t  LAST_CH    = 13;

const float RSSI_FLOOR    = -95.0f;  // dBm shown as empty
const float RSSI_CEIL     = -30.0f;  // dBm shown as full
const float ATTACK        = 0.55f;   // how fast bars jump up (0-1)
const float FALL_PER_FRAME= 0.022f;  // how fast bars drop
const float CHAN_DECAY    = 0.965f;  // channel level decay between visits (makes bars "pump")
const uint8_t PEAK_HOLD   = 18;      // frames a peak cap hangs before falling
const float PEAK_FALL     = 0.012f;
const uint32_t AP_TIMEOUT_MS = 30000; // drop networks not heard for this long

// ------------------- Spectrum geometry -------------------
const int METER_X = 28;                             // left margin for the dBm axis
const int NBARS = 36, BAR_PITCH = 8, BAR_W = 6;     // 36 * 8 = 288 px wide
const int SEGS  = 24, SEG_PITCH = 5, SEG_H = 4;     // 24 segments, 120 px tall
const int METER_BOTTOM = 152;
const float F_START = 2400.0f;                      // MHz at left edge
const float BIN_W   = 84.0f / NBARS;                // MHz per bar (2400-2484)

const int LIST_Y = 176, ROW_H = 10, LIST_ROWS = 6, LIST_COLS = 2, COL_W = 160;
const int SSID_CHARS = 19;

// ------------------- Packet view geometry -------------------
const int PKT_HDR_Y = 30, PKT_Y = 41, PKT_ROWS = 19;
const int INFO_CHARS = 19;

uint16_t litCol[SEGS], dimCol[SEGS];
uint16_t COL_ACCENT, COL_TEXT, COL_GRID, COL_SSID, COL_HIDDEN;
uint16_t COL_PROBE, COL_JOIN, COL_DEAUTH, COL_DATA, COL_OTHER;

// ------------------- Sniffer data -------------------
volatile int8_t   rxMaxRssi[15];
volatile uint16_t rxPkts[15];
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

struct APInfo {
  uint8_t  bssid[6];
  char     ssid[33];
  int8_t   rssi;
  uint8_t  ch;
  uint32_t seen;
  bool     used;
};
const int MAX_APS = 48;
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

// ------------------- Recording -------------------
const uint16_t LONG_PRESS_MS  = 1000;
const uint16_t CAP_SNAPLEN    = 2324;    // max 802.11 frame body we keep
const size_t   CAP_RING_BYTES = 32768;   // buffer between radio and SD card
const uint8_t  RT_LEN         = 15;      // radiotap header length

struct CapHdr {            // what the sniffer hands to the SD writer
  uint32_t tsSec;
  uint32_t tsUsec;
  uint16_t origLen;
  uint8_t  ch;
  int8_t   rssi;
};

struct __attribute__((packed)) PcapGlobal {
  uint32_t magic    = 0xa1b2c3d4;
  uint16_t vMajor   = 2;
  uint16_t vMinor   = 4;
  int32_t  thiszone = 0;
  uint32_t sigfigs  = 0;
  uint32_t snaplen  = 65535;
  uint32_t linktype = 127;   // 802.11 with radiotap header
};

struct __attribute__((packed)) PcapRec {
  uint32_t tsSec, tsUsec, inclLen, origLen;
};

RingbufHandle_t capRing = nullptr;
volatile bool     recording = false;
volatile uint32_t capDrops  = 0;
File     capFile;
bool     sdOK = false;
char     capName[20] = "";
uint32_t capPkts = 0, capBytes = 0, lastFlush = 0;
char     recMsg[24] = "";
uint32_t recMsgUntil = 0;

// ------------------- Display state -------------------
float   chanLevel[15];
float   disp[NBARS], peakVal[NBARS];
uint8_t peakHold[NBARS];
int     lastLit[NBARS], lastPeak[NBARS];
bool    rowUsed[LIST_ROWS * LIST_COLS];

Pkt shown[PKT_ROWS];
int shownCount = 0;
GFXcanvas16 *lineCanvas = nullptr;   // one text line, rendered in RAM then blitted

uint8_t  curCh = FIRST_CH;
int      lastMarkerX = -1;
uint32_t lastHop = 0, lastFrame = 0, lastStat = 0, lastPktDraw = 0, pktAccum = 0;
bool     lastBtn = HIGH;
uint32_t lastBtnChange = 0, pressStart = 0;
bool     longFired = false;

// ============================================================
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void recordAP(const uint8_t *bssid, const char *ssid, int8_t rssi, uint8_t ch) {
  uint32_t now = millis();
  portENTER_CRITICAL(&apMux);
  int slot = -1;
  for (int i = 0; i < MAX_APS; i++)
    if (aps[i].used && memcmp(aps[i].bssid, bssid, 6) == 0) { slot = i; break; }

  if (slot >= 0) {
    aps[slot].rssi = (aps[slot].rssi * 3 + rssi) / 4;   // light smoothing
  } else {
    for (int i = 0; i < MAX_APS; i++) if (!aps[i].used) { slot = i; break; }
    if (slot < 0) {                                       // table full: replace stalest
      slot = 0;
      for (int i = 1; i < MAX_APS; i++) if (aps[i].seen < aps[slot].seen) slot = i;
    }
    memcpy(aps[slot].bssid, bssid, 6);
    aps[slot].rssi = rssi;
    aps[slot].used = true;
  }
  strncpy(aps[slot].ssid, ssid, 32);
  aps[slot].ssid[32] = 0;
  aps[slot].ch   = ch;
  aps[slot].seen = now;
  portEXIT_CRITICAL(&apMux);
}

// Walk the tagged parameters of a management frame for the SSID and DS channel
void parseIEs(const uint8_t *f, int pos, int len, char *ssid, uint8_t &apCh) {
  while (pos + 2 <= len) {
    uint8_t id = f[pos], l = f[pos + 1];
    if (pos + 2 + l > len) break;
    if (id == 0 && l <= 32) {
      bool allZero = true;
      for (int k = 0; k < l; k++) {
        uint8_t c = f[pos + 2 + k];
        if (c) allZero = false;
        ssid[k] = (c >= 32 && c < 127) ? (char)c : '?';
      }
      ssid[allZero ? 0 : l] = 0;                         // zero-filled name = hidden network
    } else if (id == 3 && l == 1) {
      apCh = f[pos + 2];                                 // the AP's real channel
    }
    pos += 2 + l;
  }
}

void snifferCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  uint8_t ch  = p->rx_ctrl.channel;
  int8_t rssi = p->rx_ctrl.rssi;
  if (ch < 1 || ch > 14) return;

  portENTER_CRITICAL(&rxMux);
  if (rssi > rxMaxRssi[ch]) rxMaxRssi[ch] = rssi;
  rxPkts[ch]++;
  portEXIT_CRITICAL(&rxMux);

  const uint8_t *f = p->payload;
  int len = (int)p->rx_ctrl.sig_len - 4;                 // strip FCS

  if (recording && capRing && len > 0) {
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
      capDrops++;                                        // SD card couldn't keep up
    }
  }

  if (len < 24) return;

  uint8_t fc = f[0];
  uint8_t ftype = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
  bool beaconLike = (ftype == 0) && (sub == 8 || sub == 5);   // beacon / probe response
  bool probeReq   = (ftype == 0) && (sub == 4);

  char ssid[33] = {0};
  uint8_t apCh = ch;
  if (beaconLike && len > 36) {
    parseIEs(f, 36, len, ssid, apCh);                    // 24-byte header + 12 fixed bytes
    if (apCh >= 1 && apCh <= 14) recordAP(f + 16, ssid, rssi, apCh);
  } else if (probeReq) {
    parseIEs(f, 24, len, ssid, apCh);
  }

  if (packetMode) {
    portENTER_CRITICAL(&pktMux);
    Pkt &e = ring[ringHead % RING_SIZE];
    e.ch = ch; e.rssi = rssi; e.fc = fc;
    memcpy(e.src, f + 10, 6);                            // transmitter address
    if (beaconLike)    strncpy(e.info, ssid[0] ? ssid : "<hidden>", INFO_CHARS);
    else if (probeReq) strncpy(e.info, ssid[0] ? ssid : "(any)", INFO_CHARS);
    else               e.info[0] = 0;
    e.info[INFO_CHARS] = 0;
    ringHead++;
    portEXIT_CRITICAL(&pktMux);
  }
}

// Gradient: cyan -> violet -> magenta -> orange (bottom to top)
void gradRGB(float t, uint8_t &r, uint8_t &g, uint8_t &b) {
  static const int st[4][3] = {{0, 230, 255}, {130, 70, 255}, {255, 0, 170}, {255, 150, 0}};
  float x = t * 3.0f;
  int i = (int)x; if (i > 2) i = 2;
  float f = x - i;
  r = st[i][0] + (st[i + 1][0] - st[i][0]) * f;
  g = st[i][1] + (st[i + 1][1] - st[i][1]) * f;
  b = st[i][2] + (st[i + 1][2] - st[i][2]) * f;
}

void buildPalette() {
  for (int s = 0; s < SEGS; s++) {
    uint8_t r, g, b;
    gradRGB((float)s / (SEGS - 1), r, g, b);
    litCol[s] = rgb565(r, g, b);
    dimCol[s] = rgb565(r * 0.13f, g * 0.13f, b * 0.13f);  // "unlit LED" ghost
  }
  COL_ACCENT = rgb565(0, 230, 255);
  COL_TEXT   = rgb565(150, 150, 170);
  COL_GRID   = rgb565(60, 60, 75);
  COL_SSID   = rgb565(225, 225, 235);
  COL_HIDDEN = rgb565(100, 100, 115);
  COL_PROBE  = rgb565(255, 200, 0);
  COL_JOIN   = rgb565(0, 255, 120);
  COL_DEAUTH = rgb565(255, 60, 60);
  COL_DATA   = rgb565(255, 0, 170);
  COL_OTHER  = rgb565(130, 110, 255);
}

float chanCenter(uint8_t c) { return 2407.0f + 5.0f * c; }
int   freqToX(float f)      { return METER_X + (int)((f - F_START) / BIN_W * BAR_PITCH); }
float rssiLevel(float rssi) { return constrain((rssi - RSSI_FLOOR) / (RSSI_CEIL - RSSI_FLOOR), 0.0f, 1.0f); }
uint16_t rssiColor(int rssi){ return litCol[(int)(rssiLevel(rssi) * (SEGS - 1))]; }

// Wi-Fi channel occupies ~22 MHz: flat top, smooth skirts
float chanShape(float d) {
  d = fabsf(d);
  if (d <= 8.0f)  return 1.0f;
  if (d >= 12.0f) return 0.0f;
  float t = (d - 8.0f) / 4.0f;
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// ======================= SPECTRUM SCREEN =======================
void drawSeg(int bar, int s, uint16_t col) {
  int x = METER_X + bar * BAR_PITCH + (BAR_PITCH - BAR_W) / 2;
  int y = METER_BOTTOM - s * SEG_PITCH - SEG_H;
  tft.fillRect(x, y, BAR_W, SEG_H, col);
}

void drawBar(int i) {
  int n = constrain((int)lroundf(disp[i] * SEGS), 0, SEGS);
  int p = (peakVal[i] > 0.04f) ? constrain((int)(peakVal[i] * SEGS), 0, SEGS - 1) : -1;
  int o = lastLit[i], op = lastPeak[i];

  for (int s = o; s < n; s++) drawSeg(i, s, litCol[s]);   // light new segments
  for (int s = n; s < o; s++) drawSeg(i, s, dimCol[s]);   // dim dropped segments

  if (op != p && op >= 0) drawSeg(i, op, op < n ? litCol[op] : dimCol[op]);  // erase old cap
  if (p >= 0 && (p != op || n != o)) drawSeg(i, p, WHITE);                    // draw cap

  lastLit[i] = n;
  lastPeak[i] = p;
}

void drawTitle(const char *title) {
  tft.fillScreen(BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(COL_ACCENT, BLACK);
  tft.setCursor(6, 4);
  tft.print(title);
  tft.drawFastHLine(0, 26, 320, COL_GRID);
}

void drawSpectrumStatic() {
  drawTitle("WI-FI SPECTRUM");

  for (int i = 0; i < NBARS; i++) {
    lastLit[i] = 0; lastPeak[i] = -1;
    for (int s = 0; s < SEGS; s++) drawSeg(i, s, dimCol[s]);
  }

  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, BLACK);

  // Y axis: signal strength in dBm
  tft.setCursor(2, 29);
  tft.print("dBm");
  for (int dbm = -90; dbm <= -40; dbm += 10) {
    int y = METER_BOTTOM - (int)(rssiLevel(dbm) * SEGS * SEG_PITCH);
    tft.drawFastHLine(METER_X - 5, y, 3, COL_GRID);
    tft.setCursor(2, y - 3);
    tft.print(dbm);
  }

  // X axis: Wi-Fi channels
  tft.setCursor(4, 158);
  tft.print("CH");
  for (uint8_t c = FIRST_CH; c <= LAST_CH; c++) {
    int x = freqToX(chanCenter(c));
    tft.drawFastVLine(x, 154, 3, COL_GRID);
    String label(c);
    tft.setCursor(x - (label.length() * 6) / 2 + 1, 158);  // 6 px per char at size 1
    tft.print(label);
  }
  tft.drawFastHLine(0, 173, 320, COL_GRID);

  for (int r = 0; r < LIST_ROWS * LIST_COLS; r++) rowUsed[r] = false;
  lastMarkerX = -1;
}

void drawScanMarker() {
  int x = freqToX(chanCenter(curCh));
  if (lastMarkerX >= 0) tft.fillRect(lastMarkerX - 3, 167, 7, 3, BLACK);
  tft.fillRect(x - 3, 167, 7, 3, COL_ACCENT);
  lastMarkerX = x;
}

void drawStats(int nets, uint32_t pps) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%2d nets %5lu pkt/s", nets, (unsigned long)pps);  // fixed width
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, BLACK);
  tft.setCursor(320 - 6 - (int)strlen(buf) * 6, 4);
  tft.print(buf);
}

// Second header line: red REC indicator, or a short status message
void drawRecStatus() {
  const int y = 15;
  char buf[28];
  tft.setTextSize(1);
  if (recording) {
    snprintf(buf, sizeof(buf), "REC %6lu pk %5.1fMB", (unsigned long)capPkts, capBytes / 1048576.0);
    int x = 320 - 6 - (int)strlen(buf) * 6;
    tft.setTextColor(COL_DEAUTH, BLACK);
    tft.setCursor(x, y);
    tft.print(buf);
    tft.fillCircle(x - 8, y + 3, 3, ((millis() / 1000) % 2) ? COL_DEAUTH : BLACK);  // blinking dot
  } else if (millis() < recMsgUntil) {
    snprintf(buf, sizeof(buf), "%22s", recMsg);
    tft.setTextColor(COL_DEAUTH, BLACK);
    tft.setCursor(320 - 6 - 22 * 6, y);
    tft.print(buf);
  } else {
    tft.fillRect(176, y - 1, 144, 10, BLACK);
  }
}

// Builds the network list (drawing it if asked); returns how many distinct networks are heard
int updateList(bool draw) {
  static APInfo snap[MAX_APS];
  int n = 0;
  uint32_t now = millis();
  portENTER_CRITICAL(&apMux);
  for (int i = 0; i < MAX_APS; i++)
    if (aps[i].used && now - aps[i].seen < AP_TIMEOUT_MS) snap[n++] = aps[i];
  portEXIT_CRITICAL(&apMux);

  // sort strongest first
  for (int i = 1; i < n; i++) {
    APInfo t = snap[i]; int j = i - 1;
    while (j >= 0 && snap[j].rssi < t.rssi) { snap[j + 1] = snap[j]; j--; }
    snap[j + 1] = t;
  }

  // merge mesh nodes / extenders broadcasting the same name (keep strongest)
  int m = 0;
  for (int i = 0; i < n; i++) {
    bool dup = false;
    if (snap[i].ssid[0])
      for (int k = 0; k < m; k++) if (strcmp(snap[k].ssid, snap[i].ssid) == 0) { dup = true; break; }
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
    float lvl = rssiLevel(a.rssi);
    uint16_t c = rssiColor(a.rssi);
    char buf[24];

    tft.setTextColor(COL_ACCENT, BLACK);
    snprintf(buf, sizeof(buf), "%2u", a.ch);
    tft.setCursor(x + 2, y);  tft.print(buf);

    tft.setTextColor(c, BLACK);
    snprintf(buf, sizeof(buf), "%4d", a.rssi);
    tft.setCursor(x + 16, y); tft.print(buf);

    bool hidden = a.ssid[0] == 0;
    tft.setTextColor(hidden ? COL_HIDDEN : COL_SSID, BLACK);
    snprintf(buf, sizeof(buf), "%-*.*s", SSID_CHARS, SSID_CHARS, hidden ? "<hidden>" : a.ssid);
    tft.setCursor(x + 44, y); tft.print(buf);

    int w = 4 + (int)(lvl * 148);                          // strength underline
    tft.drawFastHLine(x + 2, y + 8, w, c);
    tft.drawFastHLine(x + 2 + w, y + 8, 152 - w, BLACK);
    rowUsed[r] = true;
  }
  return m;
}

void finishDwell(uint8_t ch) {
  int8_t mx; uint16_t n;
  portENTER_CRITICAL(&rxMux);
  mx = rxMaxRssi[ch]; n = rxPkts[ch];
  rxMaxRssi[ch] = -128; rxPkts[ch] = 0;
  portEXIT_CRITICAL(&rxMux);

  pktAccum += n;
  if (n == 0) return;  // nothing heard: let it decay naturally

  float lvl = rssiLevel(mx);   // bar height = strongest signal, so the dBm axis is accurate
  if (lvl > chanLevel[ch]) chanLevel[ch] = lvl;
  else                     chanLevel[ch] = 0.5f * (chanLevel[ch] + lvl);
}

void frame() {
  for (uint8_t c = FIRST_CH; c <= LAST_CH; c++) chanLevel[c] *= CHAN_DECAY;

  for (int i = 0; i < NBARS; i++) {
    float f = F_START + (i + 0.5f) * BIN_W;
    float tgt = 0;
    for (uint8_t c = FIRST_CH; c <= LAST_CH; c++) {
      float v = chanLevel[c] * chanShape(f - chanCenter(c));
      if (v > tgt) tgt = v;
    }

    if (tgt > disp[i]) disp[i] += (tgt - disp[i]) * ATTACK;
    else               disp[i] = max(tgt, disp[i] - FALL_PER_FRAME);

    if (disp[i] >= peakVal[i]) { peakVal[i] = disp[i]; peakHold[i] = PEAK_HOLD; }
    else if (peakHold[i])      peakHold[i]--;
    else                       peakVal[i] = max(0.0f, peakVal[i] - PEAK_FALL);

    drawBar(i);
  }
}

// ======================= LIVE PACKET SCREEN =======================
const char *frameName(uint8_t fc, uint16_t &col) {
  uint8_t t = (fc >> 2) & 3, s = (fc >> 4) & 0xF;
  if (t == 0) {
    switch (s) {
      case 0:  col = COL_JOIN;   return "ASSOCQ";
      case 1:  col = COL_JOIN;   return "ASSOCR";
      case 2:  col = COL_JOIN;   return "REASSQ";
      case 3:  col = COL_JOIN;   return "REASSR";
      case 4:  col = COL_PROBE;  return "PROBEQ";
      case 5:  col = COL_ACCENT; return "PROBER";
      case 8:  col = COL_ACCENT; return "BEACON";
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
  drawTitle("LIVE PACKETS");
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
  GFXcanvas16 &c = *lineCanvas;
  c.fillScreen(BLACK);
  if (r < shownCount) {
    const Pkt &p = shown[r];
    char buf[24];
    c.setTextSize(1);
    c.setTextWrap(false);

    c.setTextColor(COL_ACCENT);
    snprintf(buf, sizeof(buf), "%2u", p.ch);
    c.setCursor(2, 1);   c.print(buf);

    c.setTextColor(rssiColor(p.rssi));
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
  // pull everything new out of the ring buffer (newest goes to the top)
  bool changed = false;
  while (true) {
    Pkt e;
    portENTER_CRITICAL(&pktMux);
    uint32_t head = ringHead;
    if (head - ringTail > RING_SIZE) ringTail = head - RING_SIZE;   // fell behind: skip oldest
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

// ======================= RECORDING =======================
void showRecMsg(const char *msg) {
  strncpy(recMsg, msg, sizeof(recMsg) - 1);
  recMsg[sizeof(recMsg) - 1] = 0;
  recMsgUntil = millis() + 4000;
}

void discardRing() {
  size_t sz;
  void *it;
  while ((it = xRingbufferReceive(capRing, &sz, 0)) != nullptr) vRingbufferReturnItem(capRing, it);
}

bool startRecording() {
  if (!sdOK) {
    SD.end();
    sdOK = SD.begin(SD_CS, sdspi, 20000000);
  }
  if (!sdOK) { showRecMsg("NO SD CARD"); return false; }

  for (int i = 1; i < 10000; i++) {
    snprintf(capName, sizeof(capName), "/cap_%04d.pcap", i);
    if (!SD.exists(capName)) break;
  }
  capFile = SD.open(capName, FILE_WRITE);
  if (!capFile) { sdOK = false; showRecMsg("SD WRITE FAILED"); return false; }

  PcapGlobal g;
  capFile.write((const uint8_t *)&g, sizeof(g));
  discardRing();                     // drop anything left from a previous run
  capPkts = 0; capBytes = sizeof(g); capDrops = 0;
  lastFlush = millis();
  recording = true;
  Serial.printf("Recording to %s\n", capName);
  return true;
}

// Write buffered packets to the card. allItems=false limits time spent so the display stays smooth.
void drainCapture(bool allItems) {
  if (!capFile) return;
  uint32_t t0 = millis();
  while (allItems || millis() - t0 < 15) {
    size_t sz;
    uint8_t *item = (uint8_t *)xRingbufferReceive(capRing, &sz, 0);
    if (!item) break;

    const CapHdr *h = (const CapHdr *)item;
    uint16_t clen = sz - sizeof(CapHdr);
    uint16_t freq = (h->ch == 14) ? 2484 : 2407 + 5 * h->ch;

    uint8_t rt[RT_LEN] = {
      0, 0, RT_LEN, 0,                      // version, pad, length
      0x2A, 0, 0, 0,                        // present: flags, channel, dBm signal
      0,                                    // flags
      0,                                    // pad (channel field is 2-byte aligned)
      (uint8_t)(freq & 0xFF), (uint8_t)(freq >> 8),
      0x80, 0x00,                           // channel flags: 2 GHz
      (uint8_t)h->rssi                      // signal, dBm
    };
    PcapRec rec = { h->tsSec, h->tsUsec, (uint32_t)(RT_LEN + clen), (uint32_t)(RT_LEN + h->origLen) };

    size_t w = capFile.write((const uint8_t *)&rec, sizeof(rec));
    w += capFile.write(rt, RT_LEN);
    w += capFile.write(item + sizeof(CapHdr), clen);
    vRingbufferReturnItem(capRing, item);

    if (w != sizeof(rec) + RT_LEN + clen) {          // card pulled or full
      recording = false;
      capFile.close();
      sdOK = false;
      showRecMsg("SD WRITE FAILED");
      return;
    }
    capPkts++;
    capBytes += w;
  }

  if (millis() - lastFlush > 2000) {                 // limit loss if power is cut
    capFile.flush();
    lastFlush = millis();
  }
}

void stopRecording() {
  recording = false;
  delay(5);                          // let any in-flight packet finish
  drainCapture(true);
  if (capFile) capFile.close();
  Serial.printf("Saved %s: %lu packets, %lu dropped\n", capName,
                (unsigned long)capPkts, (unsigned long)capDrops);
  char msg[24];
  snprintf(msg, sizeof(msg), "SAVED %s", capName + 1);
  showRecMsg(msg);
}

void toggleRecording() {
  if (recording) stopRecording();
  else           startRecording();
  drawRecStatus();
}

// ======================= MODE SWITCHING =======================
void setMode(bool packets) {
  if (packets) {
    portENTER_CRITICAL(&pktMux);
    ringTail = ringHead;            // start the feed fresh
    portEXIT_CRITICAL(&pktMux);
    drawPacketStatic();
  } else {
    drawSpectrumStatic();
    drawScanMarker();
  }
  packetMode = packets;
  lastStat = 0;                     // refresh header stats right away
}

// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  hspi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
#ifdef LCD_ST7789
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
#else
  tft.begin(40000000);
#endif
  tft.setRotation(ROTATION);

  lineCanvas = new GFXcanvas16(320, ROW_H);
  buildPalette();

  capRing = xRingbufferCreate(CAP_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
  sdspi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  sdOK = SD.begin(SD_CS, sdspi, 20000000);
  Serial.println(sdOK ? "SD card ready" : "No SD card (will retry when recording starts)");

  for (int c = 0; c < 15; c++) { rxMaxRssi[c] = -128; rxPkts[c] = 0; chanLevel[c] = 0; }
  for (int i = 0; i < NBARS; i++) { disp[i] = peakVal[i] = 0; peakHold[i] = 0; }
  for (int i = 0; i < MAX_APS; i++) aps[i].used = false;

  setMode(false);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&snifferCb);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(curCh, WIFI_SECOND_CHAN_NONE);

  lastHop = lastFrame = lastPktDraw = millis();
}

void loop() {
  uint32_t now = millis();

  // BOOT button: short press = switch screens, hold = start/stop recording
  bool b = digitalRead(BUTTON_PIN);
  if (b != lastBtn && now - lastBtnChange > 40) {
    lastBtnChange = now;
    lastBtn = b;
    if (b == LOW)        { pressStart = now; longFired = false; }
    else if (!longFired) setMode(!packetMode);
  }
  if (lastBtn == LOW && !longFired && now - pressStart >= LONG_PRESS_MS) {
    longFired = true;
    toggleRecording();
  }

  if (recording) drainCapture(false);

  if (now - lastHop >= DWELL_MS) {
    finishDwell(curCh);
    curCh = (curCh >= LAST_CH) ? FIRST_CH : curCh + 1;
    esp_wifi_set_channel(curCh, WIFI_SECOND_CHAN_NONE);
    if (!packetMode) drawScanMarker();
    lastHop = now;
  }

  if (!packetMode && now - lastFrame >= FRAME_MS) {
    frame();
    lastFrame = now;
  }

  if (packetMode && now - lastPktDraw >= PKT_REDRAW_MS) {
    updatePackets();
    lastPktDraw = now;
  }

  if (now - lastStat >= 1000) {
    int nets = updateList(!packetMode);
    drawStats(nets, pktAccum);
    drawRecStatus();
    pktAccum = 0;
    lastStat = now;
  }
}
