# Wi-Fi EQ — Dual-Band Edition (ESP32-C5)

A handheld dual-band Wi-Fi scanner for the **RockBase NM-CYD-C5**. It shows the **2.4 GHz and 5 GHz** Wi-Fi bands as two live, music-style graphic equalizers on one screen, with a list of nearby networks that flags **Wi-Fi 6** networks. It also has a scrolling live packet feed and per-band packet capture to microSD in standard `.pcap` format for Wireshark. Everything is controlled from the touchscreen.

Everything is **receive-only**: the device listens passively and never transmits.

> This folder is a separate PlatformIO project. The original single-band version for the ESP32-32E board is in [`../esp32`](../esp32).

---

## Contents

- [What's on screen](#whats-on-screen)
- [Controls](#controls)
- [About the ESP32-C5](#about-the-esp32-c5)
- [Hardware](#hardware)
- [Installation](#installation)
- [Troubleshooting](#troubleshooting)
- [Recording and analyzing captures](#recording-and-analyzing-captures)
- [Understanding 5 GHz channels](#understanding-5-ghz-channels)
- [Configuration](#configuration)
- [Limitations](#limitations)
- [Responsible use](#responsible-use)

---

## What's on screen

```
┌────────────────────────────────────────────────┐
│ 14 networks  1830 pkt/s          ● REC  1.2MB  │  header
│ 2.4  ▁▃▇█▇▃▁    ▁▃▆▆▃▁      ▁▂▅█▅▂▁            │  2.4 GHz equalizer
│ GHz  1  2  3  4  5  6  7  8  9 10 11 12 13     │
│ 5    ▅▃ ▂ ▁ │ ▁ ▁ ▁ ▁ │ ▁ ▁ ▂ ▁ … │ ▇█▆ ▁ ▁      │  5 GHz equalizer
│ GHz  36     │ 52      │ 100  116  132 │ 149 165 │
│ 149 -48 HomeNet      80 AX   6 -61 Neighbor     │  strongest networks
│  36 -55 HomeNet-5G  160 AX  11 -70 <hidden>     │
│ [ REC: OFF ]   [ VIEW: SPECTRUM ]   [ HOLD ]    │  touch buttons
└────────────────────────────────────────────────┘
```

### Spectrum view

- **2.4 GHz panel** (cyan → orange): 37 bars spanning 2400–2484 MHz, labeled with channels 1–13. Each channel's signal is spread across the ~22 MHz it actually occupies, so overlapping channels show up as overlapping humps.
- **5 GHz panel** (green → violet): one bar per 20 MHz channel, 25 channels in all, grouped into sub-bands with gaps between them. See [Understanding 5 GHz channels](#understanding-5-ghz-channels).
- **Bar height** is the strongest signal heard on that channel, in dBm; the left edge has dBm marks. Bars rise quickly and fall slowly, and white peak caps hang for a moment, like a stereo equalizer.
- **Scan marker**: a small bar and a white channel label show where the radio is listening right now.
- **Network list**: the 10 strongest networks across both bands. Each row shows:
  - channel, in cyan for 2.4 GHz or green for 5 GHz
  - signal in dBm, plus a strength bar underneath
  - network name, or `<hidden>`
  - channel width when wider than 20 MHz (40, 80 or 160)
  - **AX** if the network is Wi-Fi 6 (802.11ax)

  Mesh nodes and extenders that broadcast the same name are merged, per band.
- **Header**: networks heard, packets per second, and recording status.

### Live packets view

A scrolling feed of packets from both bands as they're heard, showing channel, signal, frame type, sender MAC address and network name (when the packet carries one). Frame types are color-coded:

| Color | Types | Meaning |
|---|---|---|
| Cyan | `BEACON`, `PROBER` | Access points announcing themselves |
| Yellow | `PROBEQ` | Devices searching for networks (`(any)` = searching for any network) |
| Green | `AUTH`, `ASSOC*`, `REASS*` | Devices joining a network |
| Red | `DEAUTH`, `DISASC` | Devices being disconnected |
| Magenta | `DATA`, `QOS`, `NULL` | Traffic (headers only; contents are encrypted and never decoded) |

---

## Controls

| Control | What it does |
|---|---|
| **REC** button | Each tap moves to the next recording mode: **OFF → 2.4 GHz → 5 GHz → BOTH → OFF**. The mode takes effect **1.5 seconds after your last tap**, so tap twice quickly for 5 GHz, or three times for both. |
| **VIEW** button | Switch between the spectrum view and live packets |
| **HOLD** button | Lock onto the channel currently being scanned; tap again to resume hopping |
| Tap a bar | Hold that specific channel (spectrum view); tap it again to release |
| **BOOT**, short press | Same as REC |
| **BOOT**, hold 1 s | Same as VIEW |

Buttons are gray when their feature is off. **REC** turns red while recording, and **HOLD** turns green and shows the held channel (e.g. `HOLD: CH 149`). The RGB LED glows **red** while recording and **amber** while a new REC mode is waiting to start.

**Why HOLD is useful:** normally the radio splits its time across all 38 channels. Holding one channel listens to it continuously, so you see nearly all of that network's traffic in the packet view, recordings capture far more of it, and its bar becomes a live signal meter for walking around to find dead spots or locate a device.

---

## About the ESP32-C5

The ESP32-C5 is Espressif's first **dual-band** Wi-Fi chip. That's what makes this version possible: the original ESP32 can only receive 2.4 GHz.

| Feature | ESP32-C5 |
|---|---|
| CPU | Single-core 32-bit RISC-V, up to 240 MHz |
| Wi-Fi | Wi-Fi 6 (802.11ax) on **2.4 GHz and 5 GHz**; also 802.11a/b/g/n/ac |
| Bluetooth | Bluetooth LE 5 |
| Other radios | IEEE 802.15.4 (Thread / Zigbee) |
| On-chip RAM | ~384 KB SRAM |
| USB | Native USB serial/JTAG (no separate USB-serial chip needed) |

**Things to know:**

- **Wi-Fi 6 is not 6 GHz.** "Wi-Fi 6" is the 802.11ax protocol, which runs on 2.4 and 5 GHz. The separate **6 GHz** band, called "Wi-Fi 6E", is *not* supported by the C5. The scanner detects Wi-Fi 6 networks on 2.4 and 5 GHz and marks them **AX**.
- **One radio.** The C5 listens to one 20 MHz channel at a time. To cover both bands, it hops through 38 channels, about 45 ms each, so a full sweep takes ~1.7 seconds. Recording a single band, or holding a channel, focuses the radio and misses less.
- **Arduino support** for the C5 arrived in Arduino-ESP32 core 3.3. PlatformIO gets it from the community **pioarduino** platform, because the official `espressif32` platform doesn't support the C5.

---

## Hardware

### RockBase NM-CYD-C5

A "Cheap Yellow Display"-style board built around the ESP32-C5.

| Spec | Value |
|---|---|
| SoC | ESP32-C5, RISC-V, 240 MHz |
| Wi-Fi | Dual-band 2.4 + 5 GHz, Wi-Fi 6 |
| Flash / PSRAM | 16 MB / 8 MB |
| Display | 2.8" 240×320 SPI TFT, ST7789 controller |
| Touch | XPT2046 resistive |
| Storage | microSD slot: FAT32 or exFAT, optional (only needed for recording) |
| Extras | WS2812 RGB LED, BOOT and RESET buttons, 2 × USB-C |

### Pinout used

The display, touch controller and SD card share one SPI bus, each with its own chip-select pin.

| Function | GPIO |
|---|---|
| SPI SCK / MOSI / MISO | 6 / 7 / 2 |
| Display CS / DC | 23 / 24 |
| Display reset | Tied to the board reset |
| Backlight | 25 |
| Touch CS | 1 |
| SD card CS | 10 |
| RGB LED | 27 |
| BOOT button | 28 (GPIO 0 is also checked) |

---

## Installation

### What you need

- The NM-CYD-C5 board and a **USB-C data cable**; some cables carry only power
- [PlatformIO](https://platformio.org/): the CLI (`pip install platformio`) or the VS Code extension
- Optional: a microSD card for recording

Libraries are installed automatically from `platformio.ini`:

| Library | Purpose |
|---|---|
| Adafruit GFX Library | Graphics and text |
| Adafruit ST7735 and ST7789 Library | Display driver |
| XPT2046_Touchscreen | Touch controller |
| SdFat | microSD card access |

### Build and flash

All commands run from **inside the `esp32-c5` folder**. Building the `esp32` folder by mistake builds the *original* version, and uploading that to a C5 fails with `This chip is ESP32-C5, not ESP32`.

```bash
cd esp32-c5
pio run                                            # compile
pio run -t upload --upload-port /dev/ttyACM0       # flash to the board
pio device monitor -b 115200                       # optional: view debug output
```

**The first build is slow.** PlatformIO downloads the pioarduino platform, the C5 toolchain and Arduino core (over a gigabyte), then compiles the core. That can take **over an hour on a Raspberry Pi**, and around 10–20 minutes on a desktop PC. Later builds take a minute or two. Make sure you have at least 3–4 GB of free disk space.

### Finding the board's port

The C5 uses its built-in USB, so on Linux it appears as **`/dev/ttyACM0`**, not `/dev/ttyUSB0`. To find it:

```bash
pio device list
# or
ls /dev/ttyACM* /dev/ttyUSB*
```

On Windows it appears as a `COM` port (check Device Manager). If nothing appears, try the board's other USB-C port or another cable.

On Linux, if you get "permission denied", add yourself to the `dialout` group, then log out and back in:

```bash
sudo usermod -a -G dialout $USER
```

---

## Troubleshooting

Settings are at the top of `src/wifi_eq_c5.cpp`.

| Problem | Fix |
|---|---|
| Upload stuck at `Connecting...` | Hold **BOOT**, tap **RESET**, release BOOT, then upload again |
| `This chip is ESP32-C5, not ESP32` | You built the `esp32` project. `cd esp32-c5` first |
| Build log shows a different environment name (not `nm-cyd-c5`) | The `platformio.ini` in this folder is from another project; restore this one |
| Errors from a file in `src/` other than `wifi_eq_c5.cpp` | PlatformIO compiles everything in `src/`; delete old copies such as `wifi_eq_c5.ino` |
| `Architecture or board not supported` from an SD library | Keep `lib_ignore = SD` in `platformio.ini`. A display library pulls in the generic Arduino SD library, which doesn't support the C5; this project uses SdFat instead |
| Serial monitor shows nothing | Keep `-DARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`, and press RESET with the monitor open |
| Background white, colors inverted | Set `INVERT_COLORS = true` |
| Image upside down | Set `ROTATION = 3` |
| Taps register in the wrong place | Set `TOUCH_DEBUG = true`. A white circle then marks each tap and the header shows the raw touch values. Flip `TS_FLIP_X` / `TS_FLIP_Y` or adjust `TS_MINX/MAXX/MINY/MAXY` until circles land under your finger |
| REC returns to OFF and the header says `NO SD CARD` | Insert a FAT32 or exFAT microSD card |
| Some 5 GHz bars are solid gray | The radio refused those channels for the configured `COUNTRY`; the serial monitor lists them |
| BOOT button does nothing | The serial monitor prints `BOOT button is GPIOnn` when pressed; if nothing prints, the button is on another pin |

---

## Recording and analyzing captures

1. Insert a microSD card (FAT32 or exFAT).
2. Tap **REC** until it shows the band you want, then wait 1.5 seconds. The header shows a blinking red **REC** with the packet count and size.
3. Tap **REC** until it shows **OFF** to stop. The header briefly shows the saved file name.

Files are saved at the top level of the card as `cap_0001_2G.pcap`, `cap_0002_5G.pcap`, `cap_0003_ALL.pcap` and so on. Each recording gets a new number, so nothing is overwritten. Always stop recording before removing the card; data is also saved every 2 seconds in case of power loss.

Open captures in [Wireshark](https://www.wireshark.org/). Each packet includes its frequency and signal strength. Or get text output with `tshark`:

```bash
sudo apt install tshark
tshark -r cap_0001_5G.pcap                                     # one line per packet
tshark -r cap_0001_ALL.pcap -Y "wlan.fc.type_subtype == 0x0c"  # deauth frames only
tshark -r cap_0001_ALL.pcap -T fields \
  -e radiotap.channel.freq -e radiotap.dbm_antsignal -e wlan.sa -e wlan.ssid
```

Timestamps count from when the board powered on, since it has no clock, so Wireshark shows dates in 1970. The timing between packets is accurate.

---

## Understanding 5 GHz channels

Wi-Fi channels are numbered, and each number maps to a frequency:

- **2.4 GHz:** frequency = 2407 + 5 × channel (MHz). Channels are 5 MHz apart but ~22 MHz wide, so neighbors overlap; that's why routers usually use only 1, 6 and 11.
- **5 GHz:** frequency = 5000 + 5 × channel (MHz). Channel 36 is 5180 MHz and channel 165 is 5825 MHz. Channels step by 4 because each is 20 MHz wide, so they sit side by side without overlapping.

The 5 GHz panel is grouped into the sub-bands allowed in the US:

| Sub-band | Channels | Notes |
|---|---|---|
| UNII-1 | 36–48 | Most common for home routers |
| UNII-2A | 52–64 | DFS: shared with weather radar |
| UNII-2C | 100–144 | DFS; the largest block |
| UNII-3 | 149–165 | Common; allows higher power |

DFS channels are shared with radar, so routers must check for radar before using them; many routers avoid them, so these are often quiet. Modern routers often **bond** several 20 MHz channels into one 40, 80 or 160 MHz channel; the width column in the network list shows this. A wide network beacons only on its primary channel, so that's where it shows up most.

Only some 5 GHz channels are labeled under the bars, because there isn't room for all 25. The network list shows exact channel numbers.

---

## Configuration

Tuning constants at the top of `src/wifi_eq_c5.cpp`:

| Constant | Default | Effect |
|---|---|---|
| `COUNTRY` | `"US"` | Regulatory region; decides which 5 GHz channels are allowed |
| `DWELL_MS` | 45 | Time spent on each channel (full sweep ≈ 38 × this) |
| `RSSI_FLOOR` / `RSSI_CEIL` | -95 / -30 | dBm shown as an empty / full bar |
| `ATTACK` / `FALL_PER_FRAME` | 0.55 / 0.03 | How quickly bars rise and fall |
| `CHAN_DECAY` | 0.985 | How much bars sag between sweeps |
| `PEAK_HOLD` / `PEAK_FALL` | 15 / 0.015 | Peak cap hang time and fall speed |
| `AP_TIMEOUT_MS` | 45000 | How long a network stays listed after it's last heard |
| `REC_ARM_MS` | 1500 | Delay before a new REC mode starts |
| `LONG_PRESS_MS` | 1000 | BOOT hold time for VIEW |
| `ROTATION`, `INVERT_COLORS` | 1, false | Display orientation and color inversion |
| `TS_FLIP_X`, `TS_FLIP_Y` | true, true | Touch direction (set for the NM-CYD-C5) |
| `TOUCH_DEBUG` | false | Show tap positions on screen and in the serial monitor |

---

## Limitations

- **No 6 GHz (Wi-Fi 6E).** The ESP32-C5 covers 2.4 and 5 GHz only.
- **One radio, one channel at a time.** Hopping across both bands means each channel is heard only part of the time. Use HOLD or single-band recording for complete captures.
- **20 MHz listening.** Traffic on wide 80/160 MHz channels is mostly seen on the primary channel.
- **Wi-Fi only.** Bluetooth, microwaves and other non-Wi-Fi signals don't appear; the radio reports decoded Wi-Fi packets, not raw RF energy.
- **MAC randomization.** Phones change their MAC address when searching for networks, so one phone can appear under several addresses.
- **Busy environments** can outpace the SD card; dropped packets are reported on the serial monitor when recording stops.

---

## Responsible use

This is a passive, listen-only tool for learning, troubleshooting your own network and surveying your RF environment. Captures can contain MAC addresses and the network names nearby devices are searching for, so treat recordings made in public as sensitive and follow local laws on wireless monitoring.

---

## Project structure

```
esp32-c5/
├── platformio.ini       # pioarduino platform, board, libraries, build flags
├── README.md            # this file
└── src/
    └── wifi_eq_c5.cpp   # the whole application
```
