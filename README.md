# Wi-Fi EQ

**See Wi-Fi like music.** Wi-Fi EQ turns a cheap ESP32 display board into a handheld Wi-Fi scanner. It shows the airwaves as a live, animated graphic equalizer, lists the networks around you, shows packets as they fly by, and can record traffic to microSD for analysis in Wireshark.

Everything is **receive-only**: the device listens passively and never transmits.

---

## Two versions

This repository holds two separate PlatformIO projects, one per board. Pick the folder that matches your hardware.

| | [`esp32/`](esp32) — Original | [`esp32-c5/`](esp32-c5) — Dual-Band |
|---|---|---|
| **Board** | ESP32-32E 2.8" display (240×320) | RockBase NM-CYD-C5 2.8" display (240×320) |
| **Chip** | ESP32 (Xtensa dual-core) | ESP32-C5 (RISC-V) |
| **Wi-Fi bands** | 2.4 GHz | 2.4 GHz **and** 5 GHz |
| **Wi-Fi 6 detection** | — | ✓ networks marked **AX** |
| **Spectrum display** | One 2.4 GHz equalizer | 2.4 GHz and 5 GHz equalizers on one screen |
| **Network list** | 12 strongest networks | 10 strongest across both bands, with channel width |
| **Live packet view** | ✓ | ✓ |
| **Record to microSD (.pcap)** | ✓ | ✓ per band: 2.4, 5 or both |
| **Controls** | BOOT button | Touchscreen buttons + BOOT button |
| **Hold one channel** | — | ✓ tap a bar or the HOLD button |
| **PlatformIO platform** | `espressif32` | `pioarduino` (Arduino core 3.3+) |

Each folder has its own README with full hardware details, installation steps, controls and troubleshooting:

- **[esp32/README.md](esp32/README.md)**: original 2.4 GHz version
- **[esp32-c5/README.md](esp32-c5/README.md)**: dual-band ESP32-C5 version

---

## Quick start

1. Install [PlatformIO](https://platformio.org/): the CLI (`pip install platformio`) or the VS Code extension.
2. Clone this repository:
   ```bash
   git clone https://github.com/<your-username>/<your-repo>.git
   cd <your-repo>
   ```
3. Go into the folder for **your** board, then build and flash:
   ```bash
   cd esp32        # original ESP32 board
   # or
   cd esp32-c5     # NM-CYD-C5 (ESP32-C5)

   pio run -t upload
   ```
   Always run `pio` from inside a board folder. Each one has its own `platformio.ini`, and flashing one board's build onto the other fails.

Required libraries download automatically on the first build. For the ESP32-C5, that first build also downloads a large toolchain and can take a while; see its README.

---

## What it shows

- **Spectrum:** LED-style bars rise and fall with signal strength on each Wi-Fi channel, with floating peak caps, like a stereo equalizer.
- **Networks:** the strongest nearby networks, with channel, signal strength and name.
- **Live packets:** a color-coded feed of beacons, devices searching for networks, joins, disconnects and data traffic (headers only; contents are encrypted and never decoded).
- **Recording:** captures to microSD in standard `.pcap` format, with channel and signal strength per packet, ready for [Wireshark](https://www.wireshark.org/) or `tshark`.

---

## Repository layout

```
.
├── README.md              # this file
├── .gitignore             # ignores build output and packet captures
├── esp32/                 # original ESP32, 2.4 GHz only
│   ├── platformio.ini
│   ├── README.md
│   └── src/wifi_eq.ino
└── esp32-c5/              # ESP32-C5, 2.4 + 5 GHz, touch controls
    ├── platformio.ini
    ├── README.md
    └── src/wifi_eq_c5.cpp
```

---

## Ideas for the future

- nRF24L01+ add-on for raw 2.4 GHz energy scanning (shows Bluetooth, microwaves and other non-Wi-Fi signals)
- Bluetooth LE device scanner screen
- CC1101 add-on for sub-GHz bands (315 / 433 / 915 MHz)

---

## Responsible use

Wi-Fi EQ is a passive, listen-only tool for learning, troubleshooting your own network and surveying your RF environment. Packet captures can contain MAC addresses and the network names nearby devices are searching for, so treat recordings made in public as sensitive and follow local laws on wireless monitoring. Captures (`*.pcap`) are kept out of this repository by `.gitignore`.
