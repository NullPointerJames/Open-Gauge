# OpenGauge

A configurable CAN bus gauge for the **LilyGo T-Circle-S3** — a round 466×466 AMOLED display driven by an ESP32-S3. Connect it to your ECU's CAN bus and get a clean, touch-switchable gauge cluster with a browser-based configuration interface.

---

## Hardware

| Component | Detail |
|-----------|--------|
| Board | LilyGo T-Circle-S3 |
| MCU | ESP32-S3 (8 MB OPI PSRAM, 16 MB flash) |
| Display | 466×466 round AMOLED, SH8601 controller, QSPI |
| Touch | CST816S capacitive, I2C |
| CAN | External CAN transceiver on GPIO 43 (TX) / 44 (RX) by default |

---

## Features

- **5 configurable gauge faces**, cycled by tapping the screen
- **5 face layouts**: RPM Arc, Single value, Dual, Quad (2×2), and Needle Dial
- **36 data fields** assignable to any face slot (RPM, coolant, oil temp/pressure, TPS, MAP, lambda×2, battery, speed, gear, boost, IAT, fuel pressure/temp/consumption, EGT×12, and more)
- **3 CAN protocols**: Link ECU G4+/G4X, Haltech Elite/Nexus, OBD2 (ISO 15765-4)
- **Browser configuration UI** served directly from the ESP32 over WiFi (no app required)
- **Custom dial images** — upload your own PNG background and needle via the web UI
- **Peak hold** on dial faces — marks and labels the highest value seen above threshold
- **User-configurable colour scheme** — normal, warning, and danger colours
- **Configurable alert thresholds** — RPM redline, coolant temp, oil pressure

---

## Gauge Faces

### RPM Arc
Large sweeping arc spanning the full circle, RPM value centred, with two configurable info cells in the lower dead zone (any field you choose).

### Single / Dual / Quad
Clean numeric readouts of one, two, or four fields simultaneously, with automatic colour changes when values cross warning thresholds.

### Needle Dial
Analogue-style needle gauge with:
- Auto-scaled tick marks (≈6 major ticks with labels, minor subdivisions)
- Warning zone arc overlay in danger colour above the threshold angle
- Peak hold marker — a white tick that persists at the highest value seen while above threshold, with configurable hold-time after dropping back
- Optional custom PNG background and needle images uploaded via the web UI

---

## CAN Protocol Support

| Protocol | Bus speed | Notes |
|----------|-----------|-------|
| **Link ECU G4+ / G4X** | 1 Mbit/s (configurable) | Little-endian CAN stream, base ID 0x518 (configurable) |
| **Haltech Elite / Nexus** | 1 Mbit/s (configurable) | Big-endian Protocol V2.35.0, base ID 0x360 (configurable), 12 frame types |
| **OBD2** | 500 kbit/s (configurable) | ISO 15765-4, polled mode (0x7DF request / 0x7E8 response) |

Selective decoding: the Haltech decoder only processes the CAN frames needed by the currently active gauge faces, reducing CPU load.

---

## Web Configuration UI

Connect to the **OpenGauge** WiFi access point, then open `http://192.168.4.1` in any browser.

### Configuration tab
- CAN protocol and bus speed selection
- Protocol-specific base CAN ID
- TX/RX GPIO pin assignment
- RPM redline, coolant warn temp, oil pressure warn threshold
- Full colour scheme editor (normal/warning/danger/text/arc-background)
- WiFi AP SSID and password

### Live Data tab
Real-time polling of all decoded fields at 2 Hz — useful for verifying CAN reception before mounting.

### Gauge Faces tab
- Enable/disable each of the 5 faces
- Select layout type per face
- Assign data fields to each slot
- Configure dial scale (min/max), warn threshold, and peak hold time
- Upload custom dial background and needle PNG images (max 512 KB each)

---

## Project Structure

```
OpenGauge/
├── main/                         # App entry point, main loop (30 fps)
├── components/
│   ├── config_manager/           # NVS-backed app_config_t
│   ├── can_manager/              # ESP32 TWAI driver + RX task
│   ├── protocol_decoder/         # Link ECU / Haltech / OBD2 → gauge_data_t
│   ├── display_manager/          # QSPI init, LVGL port, touch, gauge UI
│   └── wifi_manager/             # SoftAP + SPIFFS HTTP server + REST API
└── data/                         # Web UI (index.html, style.css, app.js) → SPIFFS
```

---

## REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/config` | Read full configuration as JSON |
| POST | `/api/config` | Write configuration, restart CAN |
| GET | `/api/status` | Live gauge data snapshot |
| GET | `/api/faces` | Read face configuration |
| POST | `/api/faces` | Write face configuration, rebuild display |
| GET | `/api/images` | Query whether custom images are stored |
| POST | `/api/upload/background` | Upload custom dial background PNG (raw body) |
| POST | `/api/upload/needle` | Upload custom needle PNG (raw body) |
| DELETE | `/api/images/background` | Remove background image |
| DELETE | `/api/images/needle` | Remove needle image |

---

## Building

Requires **ESP-IDF ≥ 5.1** (tested on 5.5.2).

```bash
# One-time setup
idf.py set-target esp32s3
idf.py update-dependencies   # fetches SH8601, CST816S, esp_lvgl_port

# Build and flash
idf.py build
idf.py -p COM<N> flash monitor
```

Dependencies are declared in `main/idf_component.yml` and `components/display_manager/idf_component.yml` and are fetched automatically by the IDF Component Manager:

- `espressif/esp_lvgl_port >= 2.3.0`
- `espressif/esp_lcd_sh8601 >= 1.0.0`
- `espressif/esp_lcd_touch_cst816s >= 1.0.0`

### LVGL PNG decoder

To enable custom dial images, set in `sdkconfig` (or `menuconfig`):

```
CONFIG_LV_USE_LODEPNG=y
```

---

## Custom Dial Images

The Needle Dial face supports user-supplied PNG images for the background and needle:

- **Background**: 466×466 px PNG replaces the arc track and tick marks entirely
- **Needle**: any size PNG — the needle must point straight **down** (6 o'clock) at 0° rotation; OpenGauge rotates it to the correct angle each frame

Upload via the **Gauge Faces → Custom Dial Images** section of the web UI, or POST directly to `/api/upload/background` and `/api/upload/needle`.

---

## Pin Wiring Summary

| Signal | GPIO |
|--------|------|
| LCD CS | 10 |
| LCD CLK | 12 |
| LCD D0–D3 | 11, 13, 14, 15 |
| LCD RST | 17 |
| LCD EN (power) | 16 |
| Touch SDA | 7 |
| Touch SCL | 6 |
| Touch INT | 9 |
| CAN TX | 43 (default, configurable) |
| CAN RX | 44 (default, configurable) |

---

## License

MIT
