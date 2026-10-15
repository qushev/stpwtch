#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ImprovWiFiLibrary.h>
#include "esp_sleep.h"

// --- Пины ---
#define SDA_PIN 8
#define SCL_PIN 9
#define TOUCH_PIN 2   // RTC GPIO для пробуждения из deep sleep
#define BUZZER_PIN 7  // Пьезо-излучатель

// --- Тайминги ---
#define IDLE_TIMEOUT_MS (5UL * 60UL * 1000UL)
#define TAP_MIN_MS 50UL
#define DOUBLE_TAP_MS 300UL
#define LONG_PRESS_MS 2000UL
#define HOLD_REPEAT_MS 150UL
#define MAX_TIMER_MINUTES 60
#define FANFARE_DURATION_MS 5000UL

// --- Бузер ---
#define BUZZER_FREQ 2700  // резонансная частота типичного пьезо-излучателя

Adafruit_SH1106G display(128, 64, &Wire, -1);
Preferences preferences;
ImprovWiFi improvSerial(&Serial);

// --- Состояния ---
enum Mode { MODE_STOPWATCH, MODE_TIMER };
Mode currentMode = MODE_STOPWATCH;

enum SwState { SW_STOPPED, SW_RUNNING, SW_PAUSED };
SwState swState = SW_STOPPED;
uint64_t swElapsedMs = 0;
uint32_t swLastUpdate = 0;

enum TimerState { T_IDLE, T_SETUP, T_RUNNING, T_PAUSED };
TimerState timerState = T_IDLE;
uint16_t timerMinutes = 1;
uint64_t timerRemainingMs = 0;
uint32_t timerLastUpdate = 0;

bool fanfareActive = false;
uint32_t fanfareStartMs = 0;

// --- Бузер ---
bool buzzerActive = false;
uint8_t buzzerStep = 0;
uint32_t buzzerStepStartMs = 0;

// Паттерн китайского будильника:
//   ON 150 ms, OFF 100 ms, ON 150 ms, OFF 100 ms, ON 150 ms, OFF 500 ms, повтор.
const uint16_t buzzerPatternMs[] = {150, 100, 150, 100, 150, 500};
const uint8_t BUZZER_PATTERN_LEN = 6;

// --- Сенсор ---
enum TouchEvent { EVENT_NONE, EVENT_SINGLE_TAP, EVENT_DOUBLE_TAP };
TouchEvent currentEvent = EVENT_NONE;

bool lastTouchState = false;
uint32_t touchStartTime = 0;
bool touchLongActive = false;
uint32_t touchHoldTickTime = 0;
bool pendingSingleTap = false;
uint32_t lastTapTime = 0;
uint32_t lastActivityTime = 0;

bool displayOn = true;
bool wokeFromSleep = false;

// --- Прототипы ---
void renderDisplay();
void turnDisplayOff();
void turnDisplayOn();
void onLongPressStart();
void onHoldTick();
void onLongPressEnd();
void goToDeepSleep();

// ============================================================
//  Improv Wi-Fi callbacks
// ============================================================
void onImprovConnected(const char* ssid, const char* password) {
  Serial.printf("Improv: Connected to %s\n", ssid);
}

void onImprovError(ImprovTypes::Error err) {
  Serial.printf("Improv error: %d\n", err);
}

// ============================================================
//  Buzzer
// ============================================================
void buzzerToneOn() {
  ledcWriteTone(BUZZER_PIN, BUZZER_FREQ);
}

void buzzerToneOff() {
  ledcWrite(BUZZER_PIN, 0);
}

void startBuzzer() {
  buzzerActive = true;
  buzzerStep = 0;
  buzzerStepStartMs = millis();
  buzzerToneOn();
}

void stopBuzzer() {
  buzzerActive = false;
  buzzerToneOff();
}

void updateBuzzer() {
  if (!buzzerActive) return;
  uint32_t now = millis();
  if ((uint32_t)(now - buzzerStepStartMs) >= buzzerPatternMs[buzzerStep]) {
    buzzerStep = (buzzerStep + 1) % BUZZER_PATTERN_LEN;
    buzzerStepStartMs = now;
    if (buzzerStep % 2 == 0) buzzerToneOn();
    else buzzerToneOff();
  }
}

// ============================================================
//  Display
// ============================================================
void drawCenteredText(const char* text, uint8_t size, int16_t y) {
  display.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((128 - (int)w) / 2, y);
  display.print(text);
}

void renderStopwatch() {
  char buf[20];
  uint64_t totalSeconds = swElapsedMs / 1000ULL;
  uint32_t hours = totalSeconds / 3600ULL;
  uint8_t minutes = (totalSeconds / 60ULL) % 60;
  uint8_t seconds = totalSeconds % 60;
  if (hours <= 99) snprintf(buf, sizeof(buf), "%02lu:%02u:%02u",
                           (unsigned long)hours, minutes, seconds);
  else snprintf(buf, sizeof(buf), "%lu:%02u:%02u",
                (unsigned long)hours, minutes, seconds);
  drawCenteredText(buf, 2, 22);
}

void renderTimer() {
  char buf[20];
  uint16_t shownMin;
  if (timerState == T_IDLE || timerState == T_SETUP) {
    shownMin = timerMinutes;
  } else {
    shownMin = (timerRemainingMs + 59999ULL) / 60000ULL;
  }
  if (shownMin > MAX_TIMER_MINUTES) shownMin = MAX_TIMER_MINUTES;
  snprintf(buf, sizeof(buf), "%02u", (unsigned)shownMin);
  drawCenteredText(buf, 5, 10);

  int16_t frac = 0;
  if (timerState == T_RUNNING || timerState == T_PAUSED) {
    uint32_t remInMin = (uint32_t)(timerRemainingMs % 60000ULL);
    uint32_t secsRem = (remInMin + 999UL) / 1000UL;
    if (secsRem == 0) secsRem = 60;
    uint32_t secsElapsed = 60UL - secsRem;
    frac = (int16_t)((secsElapsed * 128UL) / 60UL);
    if (frac > 128) frac = 128;
  }
  if (frac > 0) display.fillRect(0, 61, frac, 3, SH110X_WHITE);
}

void renderDisplay() {
  display.clearDisplay();

  if (fanfareActive) {
    uint32_t elapsed = millis() - fanfareStartMs;
    bool whitePhase = ((elapsed / 1000UL) % 2UL) == 0;
    if (whitePhase) {
      display.fillRect(0, 0, 128, 64, SH110X_WHITE);
    } else {
      char buf[20];
      snprintf(buf, sizeof(buf), "%02u", (unsigned)timerMinutes);
      drawCenteredText(buf, 5, 10);
    }
    display.display();
    return;
  }

  if (currentMode == MODE_STOPWATCH) {
    renderStopwatch();
  } else {
    renderTimer();
  }
  display.display();
}

void turnDisplayOff() {
  if (!displayOn) return;
  display.clearDisplay();
  display.display();
  display.oled_command(SH110X_DISPLAYOFF);
  displayOn = false;
}

void turnDisplayOn() {
  if (displayOn) return;
  display.oled_command(SH110X_DISPLAYON);
  displayOn = true;
  lastActivityTime = millis();
  renderDisplay();
}

// ============================================================
//  Long press handlers
// ============================================================
void onLongPressStart() {
  lastActivityTime = millis();
  if (currentMode == MODE_STOPWATCH) {
    swElapsedMs = 0;
    swState = SW_STOPPED;
    swLastUpdate = millis();
  } else {
    timerState = T_SETUP;
    timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
    touchHoldTickTime = millis();
  }
}

void onHoldTick() {
  if (currentMode != MODE_TIMER || timerState != T_SETUP) return;
  timerMinutes++;
  if (timerMinutes > MAX_TIMER_MINUTES) timerMinutes = 0;
  lastActivityTime = millis();
}

void onLongPressEnd() {
  if (currentMode != MODE_TIMER || timerState != T_SETUP) return;
  timerState = T_IDLE;
  timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
  preferences.putUShort("tmins", timerMinutes);
  lastActivityTime = millis();
}

// ============================================================
//  Updates
// ============================================================
void updateStopwatch() {
  uint32_t now = millis();
  if (swState == SW_RUNNING) {
    swElapsedMs += (uint32_t)(now - swLastUpdate);
  }
  swLastUpdate = now;
}

void updateTimer() {
  uint32_t now = millis();
  if (timerState == T_RUNNING) {
    uint32_t delta = (uint32_t)(now - timerLastUpdate);
    if ((uint64_t)delta >= timerRemainingMs) {
      timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
      timerState = T_IDLE;
      fanfareActive = true;
      fanfareStartMs = millis();
      startBuzzer();
    } else {
      timerRemainingMs -= delta;
    }
  }
  timerLastUpdate = now;
}

void updateFanfare() {
  if (!fanfareActive) return;
  if ((uint32_t)(millis() - fanfareStartMs) >= FANFARE_DURATION_MS) {
    fanfareActive = false;
    stopBuzzer();
  }
}

// ============================================================
//  Touch processing
// ============================================================
void processTouch() {
  bool touched = digitalRead(TOUCH_PIN) == HIGH;
  uint32_t now = millis();

  if (touched && !lastTouchState) {
    touchStartTime = now;
    touchLongActive = false;
    lastTouchState = true;
  } else if (touched && lastTouchState) {
    if (!touchLongActive && (uint32_t)(now - touchStartTime) >= LONG_PRESS_MS) {
      touchLongActive = true;
      touchHoldTickTime = now;
      onLongPressStart();
    } else if (touchLongActive) {
      if ((uint32_t)(now - touchHoldTickTime) >= HOLD_REPEAT_MS) {
        touchHoldTickTime = now;
        onHoldTick();
      }
    }
  } else if (!touched && lastTouchState) {
    lastTouchState = false;
    uint32_t duration = now - touchStartTime;
    if (touchLongActive) {
      onLongPressEnd();
      touchLongActive = false;
    } else if (duration >= TAP_MIN_MS) {
      if (pendingSingleTap && (uint32_t)(now - lastTapTime) <= DOUBLE_TAP_MS) {
        pendingSingleTap = false;
        currentEvent = EVENT_DOUBLE_TAP;
      } else {
        pendingSingleTap = true;
        lastTapTime = now;
      }
    }
  }

  if (pendingSingleTap && (uint32_t)(now - lastTapTime) > DOUBLE_TAP_MS) {
    pendingSingleTap = false;
    currentEvent = EVENT_SINGLE_TAP;
  }
}

void handleEvents() {
  if (currentEvent == EVENT_NONE) return;
  TouchEvent event = currentEvent;
  currentEvent = EVENT_NONE;
  lastActivityTime = millis();

  if (fanfareActive) {
    if (event == EVENT_SINGLE_TAP) {
      fanfareActive = false;
      stopBuzzer();
      renderDisplay();
    }
    return;
  }

  if (event == EVENT_DOUBLE_TAP) {
    if (currentMode == MODE_STOPWATCH) {
      updateStopwatch();
      swElapsedMs = 0;
      swState = SW_STOPPED;
      swLastUpdate = millis();
      currentMode = MODE_TIMER;
    } else {
      timerState = T_IDLE;
      timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
      currentMode = MODE_STOPWATCH;
    }
    return;
  }

  if (currentMode == MODE_STOPWATCH) {
    if (event == EVENT_SINGLE_TAP) {
      if (swState == SW_STOPPED) {
        swLastUpdate = millis();
        swState = SW_RUNNING;
      } else if (swState == SW_RUNNING) {
        updateStopwatch();
        swState = SW_PAUSED;
      } else if (swState == SW_PAUSED) {
        swLastUpdate = millis();
        swState = SW_RUNNING;
      }
    }
  } else {
    if (event == EVENT_SINGLE_TAP) {
      if (timerState == T_IDLE) {
        if (timerMinutes > 0) {
          timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
          timerLastUpdate = millis();
          timerState = T_RUNNING;
        }
      } else if (timerState == T_RUNNING) {
        updateTimer();
        if (timerState == T_RUNNING) timerState = T_PAUSED;
      } else if (timerState == T_PAUSED) {
        timerLastUpdate = millis();
        timerState = T_RUNNING;
      }
    }
  }
}

// ============================================================
//  Deep sleep
// ============================================================
void goToDeepSleep() {
  Serial.println("Entering deep sleep...");

  stopBuzzer();

  preferences.putULong64("swms", swElapsedMs);
  preferences.putUChar("mode", (uint8_t)currentMode);
  preferences.putUChar("swstate", (uint8_t)swState);
  preferences.putUShort("tmins", timerMinutes);
  preferences.putULong64("tleft", timerRemainingMs);
  preferences.putUChar("tstate", (uint8_t)timerState);

  turnDisplayOff();

  pinMode(TOUCH_PIN, INPUT);
  if (digitalRead(TOUCH_PIN) == HIGH) {
    Serial.println("Touch active, aborting sleep.");
    turnDisplayOn();
    return;
  }

  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  esp_deep_sleep_start();
}

// ============================================================
//  Setup
// ============================================================
void restoreState() {
  swElapsedMs = preferences.getULong64("swms", 0);
  uint8_t savedMode = preferences.getUChar("mode", MODE_STOPWATCH);
  currentMode = (savedMode <= MODE_TIMER) ? (Mode)savedMode : MODE_STOPWATCH;
  uint8_t savedSwState = preferences.getUChar("swstate", SW_STOPPED);
  swState = (savedSwState <= SW_PAUSED) ? (SwState)savedSwState : SW_STOPPED;
  timerMinutes = preferences.getUShort("tmins", 1);
  if (timerMinutes < 1 || timerMinutes > MAX_TIMER_MINUTES) timerMinutes = 1;
  timerRemainingMs = preferences.getULong64("tleft", (uint64_t)timerMinutes * 60000ULL);
  uint8_t savedTState = preferences.getUChar("tstate", T_IDLE);
  timerState = (savedTState <= T_PAUSED) ? (TimerState)savedTState : T_IDLE;

  if (swState == SW_RUNNING) swState = SW_PAUSED;
  if (timerState == T_RUNNING) timerState = T_PAUSED;

  swLastUpdate = millis();
  timerLastUpdate = millis();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  wokeFromSleep = (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO);
  if (wokeFromSleep) {
    Serial.println("Woke up from GPIO (touch)");
  } else {
    Serial.println("Power-on or reset");
  }

  improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32_C3,
                             "Timer-Device-9a4c2b",
                             "1.0.0",
                             "ESP32 Timer");
  improvSerial.onImprovConnected(onImprovConnected);
  improvSerial.onImprovError(onImprovError);

  // Инициализация бузера
  ledcAttach(BUZZER_PIN, BUZZER_FREQ, 8);
  ledcWrite(BUZZER_PIN, 0);

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(0x3C, true)) {
    while (true) delay(100);
  }
  display.setTextColor(SH110X_WHITE);
  display.clearDisplay();
  display.display();

  preferences.begin("timerwifi", false);
  restoreState();

  // Wi-Fi: включаем режим станции всегда, чтобы Improv мог работать.
  // После пробуждения из глубокого сна модуль был выключен, поэтому
  // без этой строки Improv вернёт "Wi-Fi turned off".
  WiFi.mode(WIFI_STA);
  WiFi.begin();  // попытка подключиться к сохранённой сети

  displayOn = true;
  lastActivityTime = millis();
  renderDisplay();
}

// ============================================================
//  Loop
// ============================================================
void loop() {
  improvSerial.handleSerial();

  processTouch();
  updateStopwatch();
  updateTimer();
  updateFanfare();
  updateBuzzer();
  handleEvents();

  if (displayOn) {
    renderDisplay();
  }

  bool stopwatchActive = (swState == SW_RUNNING) || (swState == SW_PAUSED);
  bool timerActive = (timerState == T_RUNNING) || (timerState == T_PAUSED) ||
                     (timerState == T_SETUP) || fanfareActive;
  if (!stopwatchActive && !timerActive &&
      (uint32_t)(millis() - lastActivityTime) >= IDLE_TIMEOUT_MS) {
    goToDeepSleep();
  }

  delay(5);
}