# Wi-Fi EQ — ESP32 2.4 GHz Spectrum Scanner

A pocket Wi-Fi scanner for the ESP32-32E 2.8" display board that shows the 2.4 GHz band as a live, music-style graphic equalizer — plus a list of nearby networks, a scrolling live packet feed, and packet capture to microSD in standard `.pcap` format for Wireshark.

Everything is **receive-only**: the device listens passively and never transmits.

---

## Features

**Spectrum screen**
- 36 animated LED-segment bars spanning 2400–2484 MHz, with fast attack, slow fall and floating peak caps
- Each Wi-Fi channel's strongest signal is spread across the ~22 MHz it actually occupies, so overlapping channels look the way they really behave
- Labeled axes: signal strength (dBm) on the left, Wi-Fi channels 1–13 along the bottom
- Scan marker showing which channel is being listened to
- Live list of the 12 strongest networks: channel, signal, SSID and a strength bar (mesh nodes/extenders with the same name are merged; hidden networks shown as `<hidden>`)
- Header shows networks heard and packets per second

**Live packets screen**
- Scrolling feed of packets as they arrive: channel, signal, frame type, transmitter MAC address and network name (where the packet carries one)
- Color-coded frame types:

| Color | Types | Meaning |
|---|---|---|
| Cyan | `BEACON`, `PROBER` | Access points announcing themselves |
| Yellow | `PROBEQ` | Devices searching for networks (`(any)` = wildcard search) |
| Green | `AUTH`, `ASSOC*`, `REASS*` | Devices joining a network |
| Red | `DEAUTH`, `DISASC` | Devices being disconnected |
| Magenta | `DATA`, `QOS`, `NULL` | Traffic (headers only — payloads are encrypted and never decoded) |

**Recording**
- Captures every packet heard to `/cap_NNNN.pcap` on the microSD card
- Radiotap headers include channel frequency and signal strength, so Wireshark shows both per packet
- New numbered file per recording — nothing is overwritten
- Blinking red **REC** indicator with packet count and MB written
- Flushes to the card every 2 seconds to limit loss on power cut

---

## Hardware

### Board: ESP32-32E 2.8" display module (240×320)

| Spec | Value |
|---|---|
| Module | ESP32-32E (ESP32, dual-core Xtensa LX6, up to 240 MHz) |
| RAM | 520 KB SRAM — **no PSRAM required** |
| Flash | 4 MB |
| Radio | 2.4 GHz Wi-Fi 802.11 b/g/n (promiscuous mode) |
| Display | 2.8" 240×320 SPI TFT, ILI9341 or ST7789 controller |
| Storage | microSD slot (optional, needed only for recording) |
| Input | BOOT button (GPIO0) |
| Power | USB, 5 V |

The code is written to work without PSRAM: the display is updated by redrawing only the segments that change, so no full-screen framebuffer is needed.

### Pinout used

| Function | GPIO |
|---|---|
| TFT SCK / MOSI / MISO | 14 / 13 / 12 |
| TFT CS / DC / RST | 15 / 2 / tied to EN (-1) |
| TFT backlight | 21 |
| microSD SCK / MISO / MOSI / CS | 18 / 19 / 23 / 5 |
| Button | 0 (BOOT) |

These are the standard pins for this board family. If your board differs, edit the `#define`s at the top of `src/wifi_eq.ino`.

### microSD card
- FAT32 formatted
- Only needed for recording; the scanner runs fine without a card

---

## Installation

### Requirements
- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension)
- USB cable to the board

Libraries are installed automatically by PlatformIO from `platformio.ini`:
- Adafruit GFX Library
- Adafruit ILI9341
- Adafruit ST7735 and ST7789 Library

### Build and flash

```bash
git clone https://github.com/<your-username>/<your-repo>.git
cd <your-repo>
pio run -t upload
```

If PlatformIO can't find the board, specify the port:

```bash
pio run -t upload --upload-port /dev/ttyUSB0     # Linux / Raspberry Pi
pio run -t upload --upload-port COM5             # Windows
```

Optional serial monitor (SD status, recording summaries):

```bash
pio device monitor -b 115200
```

### Display driver
The default build is for the **ILI9341** controller. If your screen shows garbage or nothing, your board probably has an **ST7789** — uncomment these lines in `platformio.ini`:

```ini
build_flags =
  -DLCD_ST7789
```

If the image is upside down, change `ROTATION` from `1` to `3` in the sketch.

---

## Usage

| Action | Result |
|---|---|
| Short press **BOOT** | Switch between Spectrum and Live Packets |
| Hold **BOOT** ~1 s | Start / stop recording to microSD |

The scanner hops channels 1–13 continuously in the background, so switching screens is instant and the network list stays current either way.

### Analyzing recordings

Open `cap_NNNN.pcap` in [Wireshark](https://www.wireshark.org/), or get plain text with `tshark`:

```bash
sudo apt install tshark
tshark -r cap_0001.pcap                         # one line per packet
tshark -r cap_0001.pcap -Y "wlan.fc.type_subtype == 0x0c"   # deauth frames only
tshark -r cap_0001.pcap -T fields -e radiotap.dbm_antsignal -e wlan.sa -e wlan.ssid
```

---

## Configuration

Tuning constants are at the top of `src/wifi_eq.ino`:

| Constant | Default | Effect |
|---|---|---|
| `DWELL_MS` | 70 | Time spent listening on each channel |
| `FIRST_CH` / `LAST_CH` | 1 / 13 | Channel range scanned |
| `RSSI_FLOOR` / `RSSI_CEIL` | -95 / -30 | dBm shown as empty / full |
| `ATTACK` | 0.55 | How quickly bars rise |
| `FALL_PER_FRAME` | 0.022 | How quickly bars fall |
| `CHAN_DECAY` | 0.965 | How hard bars "pump" between sweeps |
| `PEAK_HOLD` / `PEAK_FALL` | 18 / 0.012 | Peak cap hang time and fall speed |
| `AP_TIMEOUT_MS` | 30000 | How long a network stays listed after it's last heard |
| `LONG_PRESS_MS` | 1000 | Hold time to toggle recording |

---

## Limitations

- **2.4 GHz only.** The original ESP32 radio cannot receive 5 GHz or 6 GHz Wi-Fi.
- **Wi-Fi only.** The ESP32 reports decoded Wi-Fi packets, not raw RF energy, so Bluetooth, microwaves, cameras and other non-Wi-Fi sources don't appear on the spectrum.
- **Timestamps** in recordings count from power-on (the board has no real-time clock), so Wireshark shows 1970 dates. Relative timing between packets is accurate.
- **MAC randomization.** Modern phones use random MAC addresses when probing, so one device can appear under several addresses.
- **Very busy environments** can outpace the SD card; dropped packets are reported on the serial monitor when recording stops.

---

## Roadmap ideas

- nRF24L01+ add-on for true 2.4 GHz energy scanning (126 × 1 MHz bins — shows Bluetooth, microwaves, etc.)
- BLE device scanner screen
- Lock to a single channel
- CC1101 add-on for sub-GHz bands (315 / 433 / 915 MHz)

---

## Responsible use

This is a passive, listen-only tool, intended for learning, troubleshooting your own network and surveying your RF environment. Captures can contain MAC addresses and the network names that nearby devices are searching for — treat recordings made in public as sensitive, and follow local laws regarding wireless monitoring.

---

## Project structure

```
.
├── platformio.ini     # board, libraries, build options
├── src/
│   └── wifi_eq.ino    # the whole application
└── README.md
```
