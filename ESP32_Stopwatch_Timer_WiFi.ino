#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define SDA_PIN 8
#define SCL_PIN 9
#define TOUCH_PIN 7

#define IDLE_TIMEOUT_MS (5UL * 60UL * 1000UL)
#define TAP_MIN_MS 50UL
#define DOUBLE_TAP_MS 300UL
#define LONG_PRESS_MS 2000UL
#define HOLD_REPEAT_MS 150UL
#define MAX_TIMER_MINUTES 60

// Setup AP credentials: connect to this network to configure the home Wi-Fi.
const char* AP_SSID = "Timer-Setup";
const char* AP_PASSWORD = "configure123";

Adafruit_SH1106G display(128, 64, &Wire, -1);
WebServer server(80);
Preferences preferences;

enum Mode { MODE_STOPWATCH, MODE_TIMER };
Mode currentMode = MODE_STOPWATCH;

// Stopwatch: stopped -> running -> paused -> running -> ... ; long press resets.
enum SwState { SW_STOPPED, SW_RUNNING, SW_PAUSED };
SwState swState = SW_STOPPED;
uint64_t swElapsedMs = 0;
uint32_t swLastUpdate = 0;

// Timer: idle (ready to start) / setup (holding to set minutes) / running / paused.
enum TimerState { T_IDLE, T_SETUP, T_RUNNING, T_PAUSED };
TimerState timerState = T_IDLE;
uint16_t timerMinutes = 1;          // set value: 0..60
uint64_t timerRemainingMs = 0;
uint32_t timerLastUpdate = 0;

enum TouchEvent { EVENT_NONE, EVENT_SINGLE_TAP, EVENT_DOUBLE_TAP };
TouchEvent currentEvent = EVENT_NONE;

bool lastTouchState = false;
uint32_t touchStartTime = 0;
bool touchLongActive = false;
uint32_t touchHoldTickTime = 0;
bool pendingSingleTap = false;
uint32_t lastTapTime = 0;
uint32_t lastActivityTime = 0;
bool wifiConfigured = false;
bool webActivity = false;
bool displayOn = true;

// Forward declarations
void renderDisplay();
void turnDisplayOn();
void turnDisplayOff();
void onLongPressStart();
void onHoldTick();
void onLongPressEnd();

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

void drawCenteredText(const char* text, uint8_t size, int16_t y) {
  display.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((128 - (int)w) / 2, y);
  display.print(text);
}

void renderDisplay() {
  display.clearDisplay();
  char buf[20];

  if (currentMode == MODE_STOPWATCH) {
    uint64_t totalSeconds = swElapsedMs / 1000ULL;
    uint32_t hours = totalSeconds / 3600ULL;
    uint8_t minutes = (totalSeconds / 60ULL) % 60;
    uint8_t seconds = totalSeconds % 60;
    if (hours <= 99) snprintf(buf, sizeof(buf), "%02lu:%02u:%02u",
                             (unsigned long)hours, minutes, seconds);
    else snprintf(buf, sizeof(buf), "%lu:%02u:%02u",
                  (unsigned long)hours, minutes, seconds);
    drawCenteredText(buf, 2, 22);
  } else {
    // Timer mode: two digits, only minutes.
    uint16_t shownMin;
    if (timerState == T_IDLE || timerState == T_SETUP) {
      shownMin = timerMinutes;
    } else {
      shownMin = (timerRemainingMs + 59999ULL) / 60000ULL; // round up
    }
    if (shownMin > MAX_TIMER_MINUTES) shownMin = MAX_TIMER_MINUTES;
    snprintf(buf, sizeof(buf), "%02u", (unsigned)shownMin);
    drawCenteredText(buf, 5, 10);

    // Bottom progress bar: 3 px tall, 128 px wide.
    // Represents progress within the current minute (60 s).
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

void onLongPressStart() {
  lastActivityTime = millis();
  if (currentMode == MODE_STOPWATCH) {
    // Reset stopwatch immediately, while the sensor is still held.
    swElapsedMs = 0;
    swState = SW_STOPPED;
    swLastUpdate = millis();
  } else {
    // Timer: stop whatever is running and enter setup mode.
    // Value keeps its current set value; user increments from here.
    timerState = T_SETUP;
    timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
    touchHoldTickTime = millis();
  }
}

void onHoldTick() {
  if (currentMode != MODE_TIMER || timerState != T_SETUP) return;
  timerMinutes++;
  if (timerMinutes > MAX_TIMER_MINUTES) timerMinutes = 0; // cycle 60 -> 00
  lastActivityTime = millis();
}

void onLongPressEnd() {
  if (currentMode != MODE_TIMER || timerState != T_SETUP) return;
  timerState = T_IDLE;
  timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
  preferences.putUShort("tmins", timerMinutes); // save set value
  lastActivityTime = millis();
}

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
      // Countdown finished: return to idle with the set value.
      timerState = T_IDLE;
      timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
    } else {
      timerRemainingMs -= delta;
    }
  }
  timerLastUpdate = now;
}

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

  if (event == EVENT_DOUBLE_TAP) {
    // Switching is allowed as long as the stopwatch isn't actively running.
    if (swState != SW_RUNNING) {
      if (currentMode == MODE_TIMER) {
        // Leaving timer mode stops the countdown and restores the set value.
        timerState = T_IDLE;
        timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
      }
      currentMode = (currentMode == MODE_STOPWATCH) ? MODE_TIMER : MODE_STOPWATCH;
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
    // Timer mode
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

String makeConfigPage(const String& message = "") {
  String ssid = preferences.getString("ssid", "");
  String page;
  page.reserve(1800);
  page += F("<!doctype html><html lang='ru'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<meta charset='utf-8'><title>Wi-Fi setup</title><style>");
  page += F("body{font:16px sans-serif;max-width:480px;margin:30px auto;padding:0 16px;background:#f5f5f5}");
  page += F("form{background:white;padding:20px;border-radius:12px}input,button{box-sizing:border-box;width:100%;padding:12px;margin:8px 0;font-size:16px}");
  page += F("</style><h2>Настройка Wi-Fi</h2><form method='POST' action='/save'>");
  page += F("<label>Имя сети (SSID)</label><input name='ssid' maxlength='32' required value='");
  page += htmlEscape(ssid);
  page += F("'><label>Пароль Wi-Fi</label><input name='pass' type='password' maxlength='63' autocomplete='new-password'>");
  page += F("<p>Оставьте пароль пустым, чтобы не менять сохранённый пароль.</p>");
  page += F("<button type='submit'>Сохранить и подключиться</button></form>");
  if (message.length()) {
    page += F("<p>");
    page += htmlEscape(message);
    page += F("</p>");
  }
  page += F("</html>");
  return page;
}

void handleRoot() {
  webActivity = true;
  server.send(200, "text/html; charset=utf-8", makeConfigPage());
}

void handleSave() {
  webActivity = true;
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) {
    server.send(400, "text/html; charset=utf-8", makeConfigPage("Проверьте имя сети и пароль."));
    return;
  }

  preferences.putString("ssid", ssid);
  if (pass.length() > 0) preferences.putString("pass", pass);
  wifiConfigured = true;

  String response = F("<!doctype html><html lang='ru'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><p>Настройки сохранены. ESP32 подключается к сети. Если страница перестанет открываться, подключитесь к точке доступа Timer-Setup снова.</p></html>");
  server.send(200, "text/html; charset=utf-8", response);

  String savedPass = preferences.getString("pass", "");
  WiFi.begin(ssid.c_str(), savedPass.c_str());
}

void startWebAndWiFi() {
  preferences.begin("timerwifi", false);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  wifiConfigured = !ssid.isEmpty();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (wifiConfigured) WiFi.begin(ssid.c_str(), pass.c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    webActivity = true;
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void restoreState() {
  // Stopwatch is not persisted across reboots.
  swElapsedMs = 0;
  swState = SW_STOPPED;
  swLastUpdate = millis();

  // Timer set value is persisted.
  timerMinutes = preferences.getUShort("tmins", 1);
  if (timerMinutes > MAX_TIMER_MINUTES) timerMinutes = 1;
  timerRemainingMs = (uint64_t)timerMinutes * 60000ULL;
  timerState = T_IDLE;
  timerLastUpdate = millis();

  currentMode = MODE_STOPWATCH;
}

void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);
  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(0x3C, true)) {
    while (true) delay(100);
  }
  display.setTextColor(SH110X_WHITE);
  display.clearDisplay();
  display.display();

  preferences.begin("timerwifi", false);
  restoreState();
  startWebAndWiFi();

  displayOn = true;
  lastActivityTime = millis();
  renderDisplay();
}

void loop() {
  webActivity = false;
  server.handleClient();

  // Wake the display on touch or web activity. The touch is NOT consumed, so
  // a long press that started while the screen was off still triggers its
  // action after 2 seconds.
  if (!displayOn) {
    bool touched = digitalRead(TOUCH_PIN) == HIGH;
    if (webActivity || (touched && !lastTouchState)) {
      turnDisplayOn();
      pendingSingleTap = false;
      currentEvent = EVENT_NONE;
      // fall through and let processTouch() handle the new press
    } else {
      lastTouchState = touched;
      updateStopwatch();
      updateTimer();
      if (webActivity) lastActivityTime = millis();
      delay(5);
      return;
    }
  }

  processTouch();
  updateStopwatch();
  updateTimer();
  handleEvents();

  if (webActivity) lastActivityTime = millis();

  renderDisplay();

  bool stopwatchActive = (swState == SW_RUNNING) || (swState == SW_PAUSED);
  bool timerActive = (timerState == T_RUNNING) || (timerState == T_PAUSED) ||
                     (timerState == T_SETUP);
  if (!stopwatchActive && !timerActive &&
      (uint32_t)(millis() - lastActivityTime) >= IDLE_TIMEOUT_MS) {
    turnDisplayOff();
  }

  delay(5);
}