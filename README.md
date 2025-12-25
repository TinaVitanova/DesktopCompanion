# ESP32 WiFi Desktop Companion 🌤️

[![ESP32](https://img.shields.io/badge/ESP32-WiFi%20Weather-blue?style=flat-square&logo=espressif)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-green?style=flat-square&logo=esp-idf)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/)

**ESP32 firmware for a small OLED weather station** that shows **time, temperature and HTTP‑provided status/icons**, with two buttons for UI control and WiFi retry, plus infrastructure ready for deep sleep control on a second button.

> BTN1 changes what you see on the OLED, BTN2 triggers actions like WiFi retry, weather fetch, or icon/text toggle.

## ✨ Features

- **SSD1306 I2C OLED** display (128×64) showing:
  - Current time (from NTP)  
  - Latest fetched temperature value  
  - HTTP text or matching icon (Sun / Coffee / Snowflake)
- **Two-button UI**:
  - **Button 1 (GPIO4 → GND, internal pull‑up)**: cycles display modes  
    - Mode 0: Time  
    - Mode 1: Temperature  
    - Mode 2: HTTP text / icons
  - **Button 2 (GPIO33 → VCC, internal pull‑down)**: context action  
    - Mode 0: requests "sleep / WiFi retry" via `retry_wifi_task`  
    - Mode 1: notifies `weather_task` to fetch weather  
    - Mode 2: toggles between text and icon view
- **WiFi + NTP integration** via `connect_ap_sta()` and a weather HTTP client
- **Simple task structure**:
  - `weather_task` waits on notifications and calls `fetch_weather()`
  - `wifi_retry_task` (external) handles reconnection / retries when BTN2 in mode 0
  - Two button tasks for debounced polling at 50 ms
- **No‑WiFi indicator**: overlay "no WiFi" bitmap in all relevant screens when disconnected

## 🧱 Hardware Connections

### ESP32 Core

| Function      | ESP32 GPIO | Notes                       |
|---------------|-----------:|-----------------------------|
| **BTN1**      | **GPIO4**  | Button → GND, pull‑up only  |
| **BTN2**      | **GPIO33** | Button → 3.3V, pull‑down    |
| **I2C SCL**   | **GPIO22** | SSD1306 SCL                 |
| **I2C SDA**   | **GPIO21** | SSD1306 SDA                 |

### SSD1306 OLED (I2C)

| OLED Pin | ESP32 Pin |
|----------|-----------|
| VCC      | 3.3V      |
| GND      | GND       |
| SCL      | GPIO22    |
| SDA      | GPIO21    |

Buttons are **active-low / active-high by design**:  
- BTN1: idle = HIGH (due to pull‑up), pressed = LOW  
- BTN2: idle = LOW (due to pull‑down), pressed = HIGH  

## 📺 Display Modes

The firmware maintains a **`current_mode`** variable:

| Mode | Content            | BTN2 Action                                  |
|------|--------------------|----------------------------------------------|
| 0    | Time (HH:MM:SS)    | Request sleep / WiFi retry via notification |
| 1    | Temperature + city | Notify `weather_task` to refetch weather    |
| 2    | HTTP text / icons  | Toggle between multiline text and icons     |

### Time Mode (0)

- Uses `time()` + `localtime_r()` to format **HH:MM:SS**
- Only renders when **`wifi_and_time_synced == true`**
- If WiFi is down, draws a **no‑WiFi icon overlay** in the top right

### Temperature Mode (1)

- Displays `latest_temp` as `"Temp: XX.XC"`  
- Shows a hardcoded location label (`"Skopje"`) under the temperature

### HTTP Text / Icon Mode (2)

- If `showIcons == true`:  
  - Renders `http_echo_value` as multi‑line text (max 4 lines, 15 chars/line)
- If `showIcons == false`:  
  - Matches `http_echo_value` against keywords and shows:
    - `"Sun"` → `sun_bitmap`
    - `"Coffee"` → `coffee_cup_bitmap`
    - `"Snowflake"` → `snowflake_bitmap`
    - Else → `"Missing icon..."` text

## 🧵 Tasks & Button Handling

### Button 1 Task (`button_1_task`)

- Config:
  - `GPIO4`, input, internal pull‑up
- Logic:
  1. Poll every 50 ms
  2. Detect falling edge (HIGH → LOW) with 300 ms debounce
  3. Increment `current_mode = (current_mode + 1) % DISPLAY_MODES`
  4. Call `update_display()` to redraw current screen

### Button 2 Task (`button_2_task`)

- Config:
  - `GPIO33`, input, internal pull‑down (button to VCC)
- Logic:
  1. Poll every 50 ms
  2. Detect rising edge (LOW → HIGH) with 300 ms debounce
  3. Switch on `current_mode`:
     - Mode 0: print *"BTN2: Sleep requested!"*, reset `retry_wifi_num`, notify `retry_task_handle`
     - Mode 1: notify `weather_task_handle` (fetch new weather)
     - Mode 2: toggle `showIcons` and refresh via `toggle_view()`

### Weather Task (`weather_task`)

- Blocks on `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`
- On notification → calls `fetch_weather()` to update `latest_temp` and `http_echo_value`

## 🌐 WiFi & Time

- `connect_ap_sta()` handles:
  - Initial WiFi connection in **station mode**
  - NTP time synchronization (sets `wifi_and_time_synced` and `wifi_connected`)
- `wifi_retry_task` (external source) reacts to BTN2 requests in mode 0:
  - Retries WiFi connection
  - Clears / updates `wifi_connected` and possibly `retry_wifi_num`
- OLED boot:
  - Shows **"Loading…"** and, if not connected, also the **no‑WiFi icon**

## 🏗️ Project Structure (Suggested)

desktopCompanion/
├── main/                    # Primary component
│   ├── main.c              # Your main code (this file)
│   ├── wifi_handler.c      # WiFi + retry task
│   ├── wifi_handler.h
│   ├── icons.c             # Bitmap data
│   ├── icons.h             # Bitmap externs
│   ├── shared.h            # Global variables
│   └── CMakeLists.txt      # Component build
├── components/
│   └── ssd1306/            # OLED driver (if external)
│       ├── ssd1306.c
│       └── CMakeLists.txt
├── CMakeLists.txt          # Project root
├── sdkconfig               # Config (gitignored)
└── build/                  # Build output (gitignored)


## ⚙️ Build & Flash

Configure target and project (from project root)
idf.py set-target esp32
idf.py menuconfig

Build
idf.py build

Flash & monitor
idf.py flash monitor
