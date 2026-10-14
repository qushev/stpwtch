# ESP32-C3 Stopwatch & Timer

A compact, single-button stopwatch and countdown timer built on an **ESP32-C3 Super Mini**, a **1.3-inch SH1106 OLED display**, and a **TTP223 capacitive touch sensor**. All interaction happens through the touch sensor — no physical buttons, no menus.

The device boots into stopwatch mode. A double tap switches between stopwatch and timer.

## Features

### Stopwatch
- **Single tap** — start, pause, resume.
- **Long press (2 s)** — reset to `00:00:00`. The display updates immediately, even while the sensor is still held.
- Time format adapts: `HH:MM:SS` for up to 99 hours, then `H:MM:SS`.

### Timer
- **Long press (2 s)** — enter setup mode. While the sensor is held, the set value increments every 150 ms and cycles `00 → 01 → … → 60 → 00`. The value is saved to flash (`Preferences`) on release.
- **Single tap** — start the countdown, pause it, or resume it.
- Display shows the remaining minutes as two digits.
- A **3-pixel-high progress bar** along the bottom of the screen spans the full 128 px width and tracks progress through the current 60-second minute. When it reaches the right edge, one minute has elapsed and the bar restarts from zero.
- Switching back to stopwatch stops the countdown and restores the set value.

### Other behavior
- Double tap switches between stopwatch and timer (blocked while the stopwatch is running).
- The OLED turns off after 5 minutes of inactivity. Touching the sensor wakes it up.
- The stopwatch and timer keep running internally while the display is off.
- Settings persist across power cycles via `Preferences`.

## Hardware

| Component | Notes |
|---|---|
| **ESP32-C3 Super Mini** | Main MCU |
| **1.3" OLED, SH1106** | 128×64, I²C, address `0x3C` |
| **TTP223 touch sensor** | Active-high digital output |

### Wiring

| Signal | ESP32-C3 GPIO |
|---|---|
| OLED SDA | GPIO 8 |
| OLED SCL | GPIO 9 |
| TTP223 OUT | GPIO 7 |

> Pin assignments are defined at the top of the sketch as `SDA_PIN`, `SCL_PIN`, and `TOUCH_PIN`.

## Software

### Dependencies
- **Adafruit GFX Library**
- **Adafruit SH110X Library**
- **Preferences** (bundled with the ESP32 Arduino core)

### Build
1. Install the ESP32 board package in Arduino IDE (Boards Manager URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`).
2. Select **ESP32C3 Dev Module**.
3. Install the two Adafruit libraries via Library Manager.
4. Open the `.ino` file and upload.

## Usage

| Action | Stopwatch | Timer |
|---|---|---|
| Single tap | Start / Pause / Resume | Start / Pause / Resume |
| Long press (2 s) | Reset to zero | Enter setup, hold to set minutes |
| Double tap | Switch to timer | Switch to stopwatch |

## License

Add your preferred license here (MIT, Apache-2.0, etc.).
