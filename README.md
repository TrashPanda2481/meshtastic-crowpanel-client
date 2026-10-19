# CrowPanel Meshtastic Client

A standalone **Meshtastic reader + canned-message sender** that runs on an
[Elecrow CrowPanel 5.79" e-paper](https://www.elecrow.com/) display and talks to
a Meshtastic node over your WiFi LAN — "the phone app, on e-paper."

It's built to be an always-on household comms post: anyone can walk up, read the
latest mesh status and messages, and fire off a canned reply through the node —
no phone required. E-paper is a deliberate choice: zero-glare, low-power, and
perfect for a static readout that updates occasionally rather than a scrolling
firehose.

> This is a **client/reader**. It does not contain a LoRa radio. It connects to
> a separate Meshtastic node (e.g. a Heltec Vision Master E290) over TCP/WiFi and
> renders what that node hears. All radio/mesh configuration lives on the node.

## Features

- **Messages** — live text from the mesh, filtered to the active channel.
- **Nodes** — everyone the node currently knows, with a selectable channel list
  (pick a channel and it becomes the active channel for viewing + sending).
- **Send** — a menu of canned messages with a **two-step confirm**, including a
  literal `!!!` emergency code. No keyboard needed.
- **Weather** — indoor temp/humidity from an optional DHT11, plus outdoor
  conditions from [Open-Meteo](https://open-meteo.com/) (no API key).
- **Flashless UI** — a tabbed interface (`MSGS` / `NODES` / `SEND` / `WX`) using
  a two-pass partial refresh, so the panel updates without the jarring full
  black/white flash and without accumulating ghosting.

## Hardware

| Part | Notes |
|------|-------|
| **Elecrow CrowPanel 5.79" e-paper** | ESP32-S3-WROOM-1-N8R8 (8 MB flash, 8 MB OPI PSRAM), SSD1683 dual-controller panel (792×272 visible). |
| **A Meshtastic node on your WiFi** | Any Meshtastic device with WiFi + its TCP API reachable on your LAN (default port `4403`). |
| **DHT11 temp/humidity sensor** *(optional)* | For the indoor weather readout. Data pin on **GPIO38**. Omit it and the indoor panel simply shows no reading. |

### Onboard buttons

The CrowPanel's buttons drive the whole UI (active-low):

| Button | GPIO | Action |
|--------|------|--------|
| MENU | 2 | Cycle tabs / move right |
| EXIT | 1 | Back / up a level |
| UP   | 6 | Scroll up |
| DOWN | 4 | Scroll down |
| OK   | 5 | Open / select / confirm send |

## How it works

The ESP32-S3 joins your WiFi, opens a TCP connection to the node's API
(`MESH_NODE_IP:4403`), and performs the Meshtastic `want_config` handshake. It
then reads the framed `FromRadio` protobuf stream (2 start bytes + 2-byte
length) and decodes it on-device with [nanopb](https://jpa.kapsi.fi/nanopb/):

- `my_info` / `node_info` → the node list
- `channel` → the channel list
- `packet` (TEXT_MESSAGE_APP) → messages
- `config_complete` → first full render

Sending encodes a `ToRadio` packet (a TEXT_MESSAGE_APP addressed to the active
channel) and writes it back over the same socket.

## Build & flash

This is a [PlatformIO](https://platformio.org/) project.

1. **Clone and configure secrets:**
   ```bash
   git clone <your-repo-url>
   cd meshtastic-crowpanel-client
   cp include/secrets.h.example include/secrets.h
   ```
2. **Edit `include/secrets.h`** with your WiFi, your node's LAN address, and your
   location for the weather lookup (see below). This file is gitignored.
3. **Build & upload** (CrowPanel connected over USB):
   ```bash
   pio run -e crowpanel-579 -t upload
   ```
4. **Watch the serial log** (optional):
   ```bash
   pio device monitor -b 115200
   ```

PlatformIO auto-installs the two external dependencies (`DHTesp`,
`ArduinoJson`); the display driver, protobuf runtime, and Meshtastic protobufs
are vendored under `lib/` (see below).

## Configuration

Everything machine- or location-specific lives in `include/secrets.h` (never
committed). Copy `include/secrets.h.example` and fill in:

```c
#define WIFI_SSID   "your-2.4GHz-ssid"   // ESP32-S3 is 2.4 GHz only
#define WIFI_PASS   "your-wifi-password"

#define MESH_NODE_IP    "192.168.1.252"  // your node's LAN IP (pin it via DHCP reservation)
#define MESH_NODE_PORT  4403             // Meshtastic TCP API port

#define WX_LAT   "34.05"                 // your location for the outdoor weather readout
#define WX_LON   "-118.24"               // decimal degrees
```

Canned messages, the number of partial refreshes between flashless clears
(`CLEAN_EVERY`), and the button pins are defined near the top of
[`src/main.cpp`](src/main.cpp) if you want to tune them.

### On the node side

Because this is a client, what you can see and send is governed by the node:

- The node must be on your WiFi with its **TCP API** reachable (default).
- The panel shows whatever **channels** the node has; select one to view/send.
- To see public / internet (MQTT) traffic, that's configured **on the node**,
  not here — the panel is preset-agnostic and just renders what the node has.

## Usage

- **MENU** cycles the tabs. In a list, **UP/DOWN** scroll and **OK** opens a
  detail view; **EXIT** backs out.
- **NODES → your local node** exposes the channel list — **OK** on a channel
  makes it the active channel and jumps to `MSGS`. Messages shown and messages
  sent both follow the active channel.
- **SEND** is a two-step confirm: pick a canned message with **OK**, then **OK**
  again to actually transmit. `!!!` is the send-help code (sent literally).

## Project layout

```
src/main.cpp                 the firmware
include/secrets.h.example    template for your local secrets.h
platformio.ini               board + build config (env: crowpanel-579)
lib/EPD579/                   Elecrow's SSD1683 e-paper driver (vendored)
lib/nanopb/                   nanopb protobuf runtime (vendored)
lib/meshtastic_pb/            Meshtastic protobufs, generated to nanopb (vendored)
tools/mock_node.py            a PC-side fake Meshtastic node for offline testing
```

### Testing without a node

`tools/mock_node.py` stands up a fake Meshtastic node on `localhost:4403` that
serves a config dump, emits periodic texts, and decodes anything the panel
sends — handy for UI work when the real node isn't around. Point
`MESH_NODE_IP` at your PC's LAN address to use it.

## Credits & licenses

This project vendors third-party code, each under its own license — see the
respective `lib/` subfolders:

- **EPD579** — e-paper driver from Elecrow's CrowPanel examples.
- **nanopb** — © Petteri Aimonen (zlib license).
- **meshtastic_pb** — generated from the [Meshtastic protobuf
  definitions](https://github.com/meshtastic/protobufs).

Meshtastic® is a registered trademark of Meshtastic LLC; this is an independent
hobby project and is not affiliated with or endorsed by Meshtastic LLC.

This project's own code is released under the [MIT License](LICENSE). The
vendored third-party code under `lib/` keeps its own license (see above).
