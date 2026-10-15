# ESP32-C3 Stopwatch & Timer

A compact single-button stopwatch and countdown timer built on an **ESP32-C3 Super Mini**, a **1.3" SH1106 OLED display**, a **TTP223 capacitive touch sensor**, and a **piezo buzzer**. All interaction happens through the touch sensor — no physical buttons, no menus.

The device boots into stopwatch mode. A double tap switches between stopwatch and timer. Wi-Fi credentials are provisioned over USB using the [Improv Wi-Fi](https://www.improv-wifi.com/) standard — no hard-coded SSID, no web config portal.

## Features

### Stopwatch
- **Single tap** — start, pause, resume.
- **Long press (2 s)** — reset to `00:00:00`. The display updates immediately, even while the sensor is still held.
- **Double tap** — switch to timer mode. The stopwatch is reset to zero on the way out, even if it was running or paused.
- Time format adapts: `HH:MM:SS` up to 99 hours, then `H:MM:SS`.

### Timer
- **Long press (2 s)** — enter setup mode. While the sensor is held, the set value increments every 150 ms and cycles `00 → 01 → … → 60 → 00`. The value is saved to flash on release.
- **Single tap** — start the countdown, pause it, or resume it.
- Display shows the remaining minutes as two digits.
- A **3-pixel-high progress bar** along the bottom of the screen spans the full 128 px width and tracks progress through the current 60-second minute. When it reaches the right edge, one minute has elapsed and the bar restarts from zero.
- **Fanfare on completion** — when the countdown reaches `00`, the timer stops, the set value is restored, and a 5-second celebration begins:
  - the screen alternates every second between a fully lit white screen (no digits) and the normal digits-only view;
  - the piezo buzzer plays a **Chinese alarm clock pattern** (three short beeps + a long pause, looping) until the fanfare ends.
  - A single tap cancels the fanfare and silences the buzzer immediately.
- **Double tap** — switch to stopwatch. The countdown stops and the set value is restored.

### Power management
- After 5 minutes of inactivity the device enters **deep sleep** to save power.
- **Wakes on touch** — the TTP223 output is routed to an RTC GPIO, which is the only pin family capable of waking the ESP32-C3 from deep sleep.
- Timer set value, stopwatch elapsed time, and mode are persisted in `Preferences` (`NVS`) and restored on wake.
- A running stopwatch or timer is intentionally **not resumed** after wake — it returns as paused.
- Wi-Fi is disabled before sleeping and re-enabled on every boot so Improv stays functional.

### Wi-Fi provisioning
- Uses **Improv Wi-Fi over USB Serial** — no hard-coded credentials, no captive portal.
- Open `https://www.improv-wifi.com/serial` in Chrome or Edge, plug the board in, and enter your Wi-Fi SSID and password.
- Requires **USB CDC On Boot → Enabled** in the Arduino IDE.

## Hardware

| Component | Notes |
|---|---|
| **ESP32-C3 Super Mini** | Main MCU, Wi-Fi STA |
| **1.3" OLED, SH1106** | 128×64, I²C, address `0x3C` |
| **TTP223 touch sensor** | Active-high digital output |
| **Piezo buzzer** | Passive, driven via LEDC PWM |

### Wiring

| Signal | ESP32-C3 GPIO | Notes |
|---|---|---|
| OLED SDA | GPIO 8 | |
| OLED SCL | GPIO 9 | |
| TTP223 OUT | GPIO 2 | RTC GPIO — required for deep-sleep wake |
| Piezo buzzer | GPIO 7 | Driven via LEDC, ~2700 Hz |

> Pin assignments are defined at the top of the sketch as `SDA_PIN`, `SCL_PIN`, `TOUCH_PIN`, and `BUZZER_PIN`.

**Important:** the touch sensor must be connected to an RTC GPIO (`GPIO0`–`GPIO5`) for deep-sleep wake to work. `GPIO2` is used here. Do not move it to `GPIO6`–`GPIO21` — wake-up will silently fail.

## Software

### Dependencies
- **Adafruit GFX Library**
- **Adafruit SH110X Library**
- **Improv WiFi Library** by Jonathas Barbosa
- **Preferences**, **WiFi**, **Wire** (bundled with the ESP32 Arduino core)

### Build
1. Install the ESP32 board package in Arduino IDE (Boards Manager URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`).
2. Select **ESP32C3 Dev Module**.
3. Set **USB CDC On Boot → Enabled**.
4. Install the Adafruit and Improv libraries via Library Manager.
5. Open the `.ino` file and upload.

## Usage

| Action | Stopwatch | Timer |
|---|---|---|
| Single tap | Start / Pause / Resume | Start / Pause / Resume |
| Long press (2 s) | Reset to zero | Enter setup, hold to set minutes |
| Double tap | Switch to timer (resets stopwatch) | Switch to stopwatch (stops timer) |

During the timer fanfare, a single tap only cancels the fanfare and silences the buzzer. Double tap and long press are ignored while the fanfare is active.

## License

Add your preferred license here (MIT, Apache-2.0, etc.).
