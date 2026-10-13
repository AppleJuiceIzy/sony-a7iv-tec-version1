#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include "sprites.h"

// ---------------------------------------------------------------- pins
static const int ONE_WIRE_BUS = 4;
static const int OLED_CS = 5;
static const int OLED_DC = 17;
static const int OLED_RST = 16;
static const int OLED_SCLK = 18;
static const int OLED_MOSI = 23;
static const int PELTIER_PIN = 25;
static const int BATTERY_ADC_PIN = 34;
static const int BTN_PAGE_PIN = 27;

// ------------------------------------------------------------- battery
// Calibration knob: measure pack voltage with a multimeter and adjust this ratio.
static const float BATTERY_DIVIDER_RATIO = 2.064f;
static const float ADC_REF_V = 3.3f;
static const int ADC_MAX = 4095;
static const int BATTERY_ADC_SAMPLES = 32;
static const float BATTERY_EMA_ALPHA = 0.2f;
static const float BATTERY_V_EMPTY = 3.0f;
static const float BATTERY_V_FULL = 4.2f;
static const float BATTERY_V_USB_THRESHOLD = 2.5f;
static const int BATTERY_PCT_REDRAW_DELTA = 2;

// ------------------------------------------------- Peltier duty control
static const float TEMP_OFF = 30.0f;
static const float TEMP_FULL = 45.0f;
static const int DUTY_MAX = 204;  // 80% of 255 — 15V PD supply, 12V TEC

// ------------------------------------------ animation temperature bands
static const float T_HOT = 40.0f;
static const float T_COOLING = 34.0f;
static const float T_NEAR = 30.0f;
static const float T_HYST = 1.5f;
static const unsigned long HOT_DWELL_MS = 6000;

static const int PWM_CHANNEL = 0;
static const int PWM_FREQ_HZ = 5000;
static const int PWM_RESOLUTION_BITS = 8;

static const unsigned long SAMPLE_INTERVAL_MS = 500;
static const unsigned long BTN_DEBOUNCE_MS = 50;
static const unsigned long BTN_DOUBLE_WINDOW_MS = 350;

// ----------------------------------------------------------- logo intro
// The logo scales up slightly as it holds, redrawn on its own cadence so the
// zoom stays smooth without baking extra frames into sprites.h.
static const unsigned long LOGO_MIN_HOLD_MS = 2000;
static const unsigned long LOGO_STEP_MS = 40;
static const float LOGO_SCALE_START = 1.00f;
static const float LOGO_SCALE_END = 1.20f;

// ------------------------------------------------- manual cooling override
// Manual cooling runs the TEC flat out, still bounded by the DUTY_MAX cap.
static const int MANUAL_COOL_DUTY = DUTY_MAX;
static const unsigned long MANUAL_COOL_TIMEOUT_MS = 600000UL;  // 10 minutes

static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;
static const uint16_t COLOR_CYAN = 0x07FF;    // manual cooling indicator
static const uint16_t COLOR_MAC_BG = 0xEF7D;  // classic off-white
static const uint16_t COLOR_MAC_FG = 0x2124;  // near-black text/border

// Chrome overlays, drawn on top of the sprite
static const int TEMP_X = 3;
static const int TEMP_Y = 118;
static const int BAT_ICON_X = 100;
static const int BAT_ICON_Y = 4;
static const int BAT_ICON_W = 22;
static const int BAT_ICON_H = 10;
static const int MAN_X = 3;
static const int MAN_Y = 4;
static const int TITLE_BAR_H = 14;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
Adafruit_SSD1351 display =
    Adafruit_SSD1351(SPRITE_W, SPRITE_H, &SPI, OLED_CS, OLED_DC, OLED_RST);

// Draw the frame in horizontal chunks to keep RAM small but SPI efficient
static const int CHUNK_ROWS = 8;
uint16_t chunkBuf[SPRITE_W * CHUNK_ROWS];

unsigned long lastSampleMs = 0;
unsigned long lastAnimMs = 0;
unsigned long lastBtnMs = 0;
unsigned long hotEnteredMs = 0;
unsigned long logoStartMs = 0;

// Press arming: a single press only acts once the double-press window expires
unsigned long pendingPressMs = 0;
uint8_t pendingPresses = 0;

bool manualCool = false;
unsigned long manualCoolStartMs = 0;

float lastTempC = NAN;
bool lastTempValid = false;
int lastDuty = 0;
float lastVbat = 0.0f;
bool lastUsbMode = true;
int lastBatteryPct = 0;

float chromeDrawnTemp = NAN;
bool chromeDrawnValid = false;
int chromeDrawnBatPct = -999;
bool chromeDrawnUsb = true;
bool chromeDrawnManual = false;

float vbatEma = 0.0f;
bool vbatEmaInit = false;

AnimState currentState = ANIM_LOGO;
uint8_t frameIndex = 0;
bool logoComplete = false;
bool bootComplete = false;

bool forceAnim = false;
AnimState forcedState = ANIM_COOLED;

bool showStats = false;
bool btnWasPressed = false;

// Last values drawn on the stats page (whole-page redraw on any change)
float statsDrawnTemp = NAN;
bool statsDrawnValid = false;
int statsDrawnDutyPct = -1;
float statsDrawnVbat = -1.0f;
int statsDrawnBatPct = -1;
bool statsDrawnUsb = false;
bool statsDrawnManual = false;
long statsDrawnManualSecs = -1;
bool statsPageDrawn = false;

static char lineBuf[64];
static size_t lineLen = 0;

// ------------------------------------------------------------ helpers

unsigned long manualCoolRemainingMs() {
  if (!manualCool) return 0;
  unsigned long elapsed = millis() - manualCoolStartMs;
  return elapsed >= MANUAL_COOL_TIMEOUT_MS ? 0 : MANUAL_COOL_TIMEOUT_MS - elapsed;
}

int dutyPercent(int duty) {
  if (DUTY_MAX <= 0) return 0;
  return (duty * 100) / DUTY_MAX;
}

// ------------------------------------------------------------ rendering

void drawFrame(AnimState state, uint8_t idx) {
  const uint16_t *frame = animFrame(state, idx);

  for (int y0 = 0; y0 < SPRITE_H; y0 += CHUNK_ROWS) {
    int rows = min(CHUNK_ROWS, SPRITE_H - y0);
    for (int r = 0; r < rows; r++) {
      const uint16_t *src = frame + (y0 + r) * SPRITE_W;
      uint16_t *dst = chunkBuf + r * SPRITE_W;
      for (int x = 0; x < SPRITE_W; x++) {
        dst[x] = pgm_read_word(&src[x]);
      }
    }
    display.drawRGBBitmap(0, y0, chunkBuf, SPRITE_W, rows);
  }
}

// Bilinear blend of four RGB565 samples; fx/fy are Q8 fractions, weights sum to 1<<16.
uint16_t blend565(uint16_t c00, uint16_t c10, uint16_t c01, uint16_t c11, int fx, int fy) {
  uint32_t w00 = (uint32_t)(256 - fx) * (256 - fy);
  uint32_t w10 = (uint32_t)fx * (256 - fy);
  uint32_t w01 = (uint32_t)(256 - fx) * fy;
  uint32_t w11 = (uint32_t)fx * fy;

  uint32_t r = ((c00 >> 11) & 0x1F) * w00 + ((c10 >> 11) & 0x1F) * w10 +
               ((c01 >> 11) & 0x1F) * w01 + ((c11 >> 11) & 0x1F) * w11;
  uint32_t g = ((c00 >> 5) & 0x3F) * w00 + ((c10 >> 5) & 0x3F) * w10 +
               ((c01 >> 5) & 0x3F) * w01 + ((c11 >> 5) & 0x3F) * w11;
  uint32_t b = (c00 & 0x1F) * w00 + (c10 & 0x1F) * w10 + (c01 & 0x1F) * w01 +
               (c11 & 0x1F) * w11;

  return (uint16_t)(((r >> 16) << 11) | ((g >> 16) << 5) | (b >> 16));
}

// Bilinear zoom about the centre so sub-pixel steps stay smooth instead of
// snapping a whole pixel at a time; scale > 1 crops the outer edges.
void drawFrameScaled(const uint16_t *frame, float scale) {
  const float centre = SPRITE_W / 2.0f;
  const float inv = 1.0f / scale;

  static int32_t xSrc[SPRITE_W];  // Q8 source column per destination column
  for (int x = 0; x < SPRITE_W; x++) {
    float sx = constrain(centre + (x - centre) * inv, 0.0f, (float)(SPRITE_W - 1));
    xSrc[x] = (int32_t)(sx * 256.0f);
  }

  // Cache the two source rows in play so each is pulled out of flash once
  static uint16_t rowTop[SPRITE_W], rowBot[SPRITE_W];
  int cachedTop = -1, cachedBot = -1;

  for (int y0 = 0; y0 < SPRITE_H; y0 += CHUNK_ROWS) {
    int rows = min(CHUNK_ROWS, SPRITE_H - y0);
    for (int r = 0; r < rows; r++) {
      float syf = constrain(centre + ((y0 + r) - centre) * inv, 0.0f, (float)(SPRITE_H - 1));
      int32_t ySrc = (int32_t)(syf * 256.0f);
      int yi = ySrc >> 8;
      int yf = ySrc & 0xFF;
      int yi2 = min(yi + 1, SPRITE_H - 1);

      if (cachedTop != yi) {
        if (cachedBot == yi) {
          memcpy(rowTop, rowBot, sizeof(rowTop));
        } else {
          const uint16_t *src = frame + yi * SPRITE_W;
          for (int x = 0; x < SPRITE_W; x++) rowTop[x] = pgm_read_word(&src[x]);
        }
        cachedTop = yi;
        cachedBot = -1;
      }
      if (cachedBot != yi2) {
        const uint16_t *src = frame + yi2 * SPRITE_W;
        for (int x = 0; x < SPRITE_W; x++) rowBot[x] = pgm_read_word(&src[x]);
        cachedBot = yi2;
      }

      uint16_t *dst = chunkBuf + r * SPRITE_W;
      for (int x = 0; x < SPRITE_W; x++) {
        int xi = xSrc[x] >> 8;
        int xf = xSrc[x] & 0xFF;
        int xi2 = min(xi + 1, SPRITE_W - 1);
        dst[x] = blend565(rowTop[xi], rowTop[xi2], rowBot[xi], rowBot[xi2], xf, yf);
      }
    }
    display.drawRGBBitmap(0, y0, chunkBuf, SPRITE_W, rows);
  }
}

unsigned long logoHoldMs() {
  unsigned long clipMs = animClip(ANIM_LOGO).frame_ms;
  return clipMs > LOGO_MIN_HOLD_MS ? clipMs : LOGO_MIN_HOLD_MS;
}

float logoScale() {
  unsigned long hold = logoHoldMs();
  unsigned long elapsed = millis() - logoStartMs;
  float t = elapsed >= hold ? 1.0f : (float)elapsed / (float)hold;
  t = t * t * (3.0f - 2.0f * t);  // smoothstep, so it eases in and out
  return LOGO_SCALE_START + (LOGO_SCALE_END - LOGO_SCALE_START) * t;
}

// The logo gets the scaling path; every other state blits its frame as-is.
void drawCurrentFrame() {
  AnimState shown = forceAnim ? forcedState : currentState;
  if (shown == ANIM_LOGO) {
    drawFrameScaled(animFrame(ANIM_LOGO, 0), logoScale());
  } else {
    drawFrame(shown, frameIndex);
  }
}

void drawChrome(float tempC, bool valid, int pct, bool usbMode) {
  char buf[16];
  if (valid) {
    snprintf(buf, sizeof(buf), "%.1fC", tempC);
  } else {
    snprintf(buf, sizeof(buf), "--.-C");
  }
  display.setTextSize(1);
  display.setTextColor(COLOR_WHITE);
  display.setCursor(TEMP_X, TEMP_Y);
  display.print(buf);

  if (manualCool) {
    display.setTextColor(COLOR_CYAN);
    display.setCursor(MAN_X, MAN_Y);
    display.print(F("MAN"));
    display.setTextColor(COLOR_WHITE);
  }

  display.drawRect(BAT_ICON_X, BAT_ICON_Y, BAT_ICON_W, BAT_ICON_H, COLOR_WHITE);
  display.fillRect(BAT_ICON_X + BAT_ICON_W, BAT_ICON_Y + 3, 2, 4, COLOR_WHITE);

  if (usbMode) {
    display.drawLine(BAT_ICON_X + 2, BAT_ICON_Y + 2, BAT_ICON_X + BAT_ICON_W - 3,
                     BAT_ICON_Y + BAT_ICON_H - 3, COLOR_WHITE);
    display.drawLine(BAT_ICON_X + 2, BAT_ICON_Y + BAT_ICON_H - 3, BAT_ICON_X + BAT_ICON_W - 3,
                     BAT_ICON_Y + 2, COLOR_WHITE);
    return;
  }

  int innerW = BAT_ICON_W - 4;
  int fillW = (innerW * pct) / 100;
  if (fillW > 0) {
    uint16_t c = 0x07E0;
    if (pct <= 20) {
      c = 0xF800;
    } else if (pct <= 50) {
      c = 0xFFE0;
    }
    display.fillRect(BAT_ICON_X + 2, BAT_ICON_Y + 2, fillW, BAT_ICON_H - 4, c);
  }
}

void advanceBatteryChromeIfNeeded() {
  if (chromeDrawnUsb != lastUsbMode ||
      abs(lastBatteryPct - chromeDrawnBatPct) >= BATTERY_PCT_REDRAW_DELTA) {
    chromeDrawnBatPct = lastBatteryPct;
    chromeDrawnUsb = lastUsbMode;
  }
}

void redraw() {
  if (showStats) return;
  advanceBatteryChromeIfNeeded();
  drawCurrentFrame();
  if (bootComplete) {
    drawChrome(lastTempC, lastTempValid, chromeDrawnBatPct, chromeDrawnUsb);
    chromeDrawnTemp = lastTempC;
    chromeDrawnValid = lastTempValid;
    chromeDrawnManual = manualCool;
  }
}

void drawStatsPage() {
  display.fillScreen(COLOR_MAC_BG);
  display.drawRect(0, 0, SPRITE_W, SPRITE_H, COLOR_MAC_FG);

  display.fillRect(1, 1, SPRITE_W - 2, TITLE_BAR_H, COLOR_MAC_FG);
  display.setTextSize(1);
  display.setTextColor(COLOR_MAC_BG);
  display.setCursor(4, 4);
  display.print(F("A7IV-TEC"));

  display.setTextColor(COLOR_MAC_FG);
  int y = TITLE_BAR_H + 10;
  char line[40];

  if (lastTempValid) {
    snprintf(line, sizeof(line), "Temp   %.1f C", lastTempC);
  } else {
    snprintf(line, sizeof(line), "Temp   --.- C");
  }
  display.setCursor(8, y);
  display.print(line);
  y += 14;

  snprintf(line, sizeof(line), "Duty   %d%%", dutyPercent(lastDuty));
  display.setCursor(8, y);
  display.print(line);
  y += 14;

  if (lastUsbMode) {
    snprintf(line, sizeof(line), "Batt   --.- V");
  } else {
    snprintf(line, sizeof(line), "Batt   %.2f V", lastVbat);
  }
  display.setCursor(8, y);
  display.print(line);
  y += 14;

  if (lastUsbMode) {
    snprintf(line, sizeof(line), "Charge --");
  } else {
    snprintf(line, sizeof(line), "Charge %d%%", lastBatteryPct);
  }
  display.setCursor(8, y);
  display.print(line);
  y += 14;

  display.setCursor(8, y);
  display.print(F("Power  "));
  display.print(lastUsbMode ? F("USB") : F("BATTERY"));
  y += 14;

  // "Manual cool: ON (mm:ss left)" overruns 128 px at size 1, so wrap the countdown
  display.setCursor(8, y);
  long manualSecs = -1;
  if (manualCool) {
    display.print(F("Manual cool: FULL"));
    unsigned long left = manualCoolRemainingMs();
    manualSecs = (long)(left / 1000UL);
    snprintf(line, sizeof(line), "(%02ld:%02ld left)", manualSecs / 60, manualSecs % 60);
    display.setCursor(14, y + 12);
    display.print(line);
  } else {
    display.print(F("Manual cool: OFF"));
  }

  statsDrawnManual = manualCool;
  statsDrawnManualSecs = manualSecs;
  statsDrawnTemp = lastTempC;
  statsDrawnValid = lastTempValid;
  statsDrawnDutyPct = dutyPercent(lastDuty);
  statsDrawnVbat = lastVbat;
  statsDrawnBatPct = lastBatteryPct;
  statsDrawnUsb = lastUsbMode;
  statsPageDrawn = true;
}

bool statsNeedsRedraw() {
  if (!statsPageDrawn) return true;
  if (statsDrawnValid != lastTempValid) return true;
  if (lastTempValid && fabsf(statsDrawnTemp - lastTempC) > 0.05f) return true;
  if (statsDrawnDutyPct != dutyPercent(lastDuty)) return true;
  if (statsDrawnUsb != lastUsbMode) return true;
  if (statsDrawnManual != manualCool) return true;
  if (manualCool && statsDrawnManualSecs != (long)(manualCoolRemainingMs() / 1000UL)) return true;
  if (!lastUsbMode) {
    if (fabsf(statsDrawnVbat - lastVbat) > 0.01f) return true;
    if (statsDrawnBatPct != lastBatteryPct) return true;
  }
  return false;
}

void setShowStats(bool on) {
  if (showStats == on) return;
  showStats = on;
  if (showStats) {
    statsPageDrawn = false;
    drawStatsPage();
    Serial.println(F("UI: stats"));
  } else {
    statsPageDrawn = false;
    redraw();
    Serial.println(F("UI: anim"));
  }
}

void toggleStatsPage() { setShowStats(!showStats); }

// Defined further down with the rest of the control code
int targetDuty(float tempC, bool valid);
void setPeltierDuty(int duty);

void setManualCool(bool on) {
  if (manualCool == on) return;
  manualCool = on;
  manualCoolStartMs = millis();

  // Drive the TEC straight away rather than waiting for the next sample tick
  lastDuty = targetDuty(lastTempC, lastTempValid);
  setPeltierDuty(lastDuty);

  if (manualCool) {
    Serial.printf("Manual cool: FULL (PWM %d)\n", lastDuty);
  } else {
    Serial.printf("Manual cool: OFF (PWM %d)\n", lastDuty);
  }

  if (showStats) {
    drawStatsPage();
  } else {
    redraw();
  }
}

void toggleManualCool() { setManualCool(!manualCool); }

void pollButton() {
  unsigned long now = millis();
  bool pressed = (digitalRead(BTN_PAGE_PIN) == LOW);

  if (pressed && !btnWasPressed && now - lastBtnMs >= BTN_DEBOUNCE_MS) {
    lastBtnMs = now;
    if (pendingPresses == 0) {
      pendingPresses = 1;
      pendingPressMs = now;
    } else {
      pendingPresses = 0;  // second press inside the window: never touches the stats page
      toggleManualCool();
    }
  }
  btnWasPressed = pressed;

  if (pendingPresses == 1 && now - pendingPressMs >= BTN_DOUBLE_WINDOW_MS) {
    pendingPresses = 0;
    toggleStatsPage();
  }
}

bool animChromeNeedsUpdate() {
  if (!bootComplete) return false;
  if (chromeDrawnValid != lastTempValid) return true;
  if (lastTempValid && fabsf(chromeDrawnTemp - lastTempC) > 0.05f) return true;
  if (chromeDrawnUsb != lastUsbMode) return true;
  if (chromeDrawnManual != manualCool) return true;
  if (abs(lastBatteryPct - chromeDrawnBatPct) >= BATTERY_PCT_REDRAW_DELTA) return true;
  return false;
}

void refreshAnimChrome() {
  advanceBatteryChromeIfNeeded();
  drawCurrentFrame();
  drawChrome(lastTempC, lastTempValid, chromeDrawnBatPct, chromeDrawnUsb);
  chromeDrawnTemp = lastTempC;
  chromeDrawnValid = lastTempValid;
  chromeDrawnManual = manualCool;
}

// ------------------------------------------------------------- control

int computeDuty(float tempC) {
  if (tempC < TEMP_OFF) return 0;
  if (tempC >= TEMP_FULL) return DUTY_MAX;
  float d = (tempC - TEMP_OFF) / (TEMP_FULL - TEMP_OFF) * (float)DUTY_MAX;
  return (int)constrain(d + 0.5f, 0.0f, (float)DUTY_MAX);
}

void setPeltierDuty(int duty) {
  ledcWrite(PWM_CHANNEL, constrain(duty, 0, DUTY_MAX));
}

// Manual cooling forces full power and never commands less than the auto curve
int targetDuty(float tempC, bool valid) {
  int duty = valid ? computeDuty(tempC) : 0;
  if (manualCool) duty = max(duty, MANUAL_COOL_DUTY);
  return duty;
}

AnimState stateFromTemp(float t, bool valid, AnimState cur) {
  if (!valid) return ANIM_COOLED;

  if (t >= T_HOT) return ANIM_HOT;
  if (cur == ANIM_HOT && t > T_HOT - T_HYST) return ANIM_HOT;

  if (t >= T_COOLING) return ANIM_COOLING;
  if (cur == ANIM_COOLING && t > T_COOLING - T_HYST) return ANIM_COOLING;

  if (t >= T_NEAR) return ANIM_NEAR;
  if (cur == ANIM_NEAR && t > T_NEAR - T_HYST) return ANIM_NEAR;

  return ANIM_COOLED;
}

void applyHotDwell(AnimState proposed, unsigned long now) {
  if (forceAnim) return;

  if (proposed == ANIM_HOT && currentState != ANIM_HOT) {
    hotEnteredMs = now;
    currentState = ANIM_HOT;
    return;
  }

  if (currentState == ANIM_HOT && proposed != ANIM_HOT) {
    if (now - hotEnteredMs < HOT_DWELL_MS) {
      return;
    }
  }

  currentState = proposed;
}

void readBattery(float &vbatOut, int &rawOut) {
  long sum = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) sum += analogRead(BATTERY_ADC_PIN);
  rawOut = (int)(sum / BATTERY_ADC_SAMPLES);
  float instant = rawOut * (ADC_REF_V / ADC_MAX) * BATTERY_DIVIDER_RATIO;

  if (!vbatEmaInit) {
    vbatEma = instant;
    vbatEmaInit = true;
  } else {
    vbatEma = BATTERY_EMA_ALPHA * instant + (1.0f - BATTERY_EMA_ALPHA) * vbatEma;
  }
  vbatOut = vbatEma;
}

int batteryPercent(float vbat) {
  float p = (vbat - BATTERY_V_EMPTY) / (BATTERY_V_FULL - BATTERY_V_EMPTY) * 100.0f;
  return (int)constrain(p, 0.0f, 100.0f);
}

// -------------------------------------------------------------- serial

void handleLine(const char *line) {
  while (*line == ' ') line++;

  if (strcmp(line, "page") == 0) {
    toggleStatsPage();
    return;
  }

  if (strcmp(line, "cool") == 0) {
    toggleManualCool();
    return;
  }

  if (strcmp(line, "boot") == 0) {
    currentState = ANIM_LOGO;
    frameIndex = 0;
    logoComplete = false;
    bootComplete = false;
    forceAnim = false;
    logoStartMs = millis();
    Serial.println(F("Anim: replaying intro"));
    if (!showStats) redraw();
    return;
  }

  if (strncmp(line, "state", 5) != 0) return;
  const char *arg = line + 5;
  while (*arg == ' ') arg++;

  if (strcmp(arg, "auto") == 0) {
    forceAnim = false;
    Serial.println(F("Anim: auto"));
  } else {
    int n = atoi(arg);
    if (n < 0 || n >= ANIM_STATE_COUNT) {
      Serial.println(
          F("Anim: state 0=logo 1=boot 2=cooled 3=near 4=cooling 5=hot, or 'auto'"));
      return;
    }
    forceAnim = true;
    forcedState = (AnimState)n;
    frameIndex = 0;
    if (forcedState == ANIM_LOGO) logoStartMs = millis();  // replay the scale-in
    Serial.printf("Anim: forced state %d\n", n);
  }
  if (!showStats) redraw();
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) {
        lineBuf[lineLen] = 0;
        handleLine(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen + 1 < sizeof(lineBuf)) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;
    }
  }
}

// --------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  delay(100);

  analogReadResolution(12);
  pinMode(BATTERY_ADC_PIN, INPUT);
  pinMode(BTN_PAGE_PIN, INPUT_PULLUP);

  ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PELTIER_PIN, PWM_CHANNEL);
  setPeltierDuty(0);

  sensors.begin();

  SPI.begin(OLED_SCLK, -1, OLED_MOSI);
  display.begin();
  display.setRotation(2);  // panel mounted upside down; SSD1351 remaps in hardware
  display.fillScreen(COLOR_BLACK);

  int raw;
  readBattery(lastVbat, raw);
  lastUsbMode = (lastVbat < BATTERY_V_USB_THRESHOLD);
  lastBatteryPct = batteryPercent(lastVbat);
  chromeDrawnBatPct = lastBatteryPct;
  chromeDrawnUsb = lastUsbMode;

  currentState = ANIM_LOGO;
  frameIndex = 0;
  logoStartMs = millis();
  redraw();

  Serial.println(F("A7IV-TEC ready"));
  Serial.println(F("Commands: state 0..5 | state auto | boot | page | cool"));

  lastSampleMs = lastAnimMs = lastBtnMs = millis();
}

// ---------------------------------------------------------------- loop

void loop() {
  pollSerial();
  pollButton();
  unsigned long now = millis();

  if (manualCool && now - manualCoolStartMs >= MANUAL_COOL_TIMEOUT_MS) {
    Serial.println(F("Manual cool: timeout"));
    setManualCool(false);
  }

  AnimState shown = forceAnim ? forcedState : currentState;
  const AnimClip &clip = animClip(shown);

  // The logo redraws on its own cadence to animate the scale-in
  unsigned long frameInterval = (shown == ANIM_LOGO) ? LOGO_STEP_MS : clip.frame_ms;

  if (!showStats && now - lastAnimMs >= frameInterval) {
    lastAnimMs = now;

    if (shown == ANIM_LOGO) {
      if (!logoComplete && now - logoStartMs >= logoHoldMs()) {
        logoComplete = true;
        currentState = ANIM_BOOT;
        frameIndex = 0;
      }
    } else {
      frameIndex++;

      if (!bootComplete && shown == ANIM_BOOT && frameIndex >= clip.count) {
        bootComplete = true;
        currentState = ANIM_COOLED;
        frameIndex = 0;
      } else if (frameIndex >= clip.count) {
        frameIndex = 0;
      }
    }
    redraw();
  }

  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  bool valid = (tempC != DEVICE_DISCONNECTED_C) && !isnan(tempC);

  int duty = targetDuty(tempC, valid);
  setPeltierDuty(duty);

  float vbat;
  int raw;
  readBattery(vbat, raw);
  bool usbMode = (vbat < BATTERY_V_USB_THRESHOLD);
  int batPct = batteryPercent(vbat);

  lastTempC = tempC;
  lastTempValid = valid;
  lastDuty = duty;
  lastVbat = vbat;
  lastUsbMode = usbMode;
  lastBatteryPct = batPct;

  if (valid) {
    Serial.printf("Temp: %.2f C | PWM: %d | ADC: %d | Vbat: %.2f V | state %d\n", tempC, duty,
                  raw, vbat, (int)currentState);
  } else {
    Serial.printf("Temp: --.- C | PWM: %d | ADC: %d | Vbat: %.2f V\n", duty, raw, vbat);
  }

  if (bootComplete && !forceAnim) {
    AnimState proposed = stateFromTemp(tempC, valid, currentState);
    applyHotDwell(proposed, now);
  }

  if (showStats) {
    if (statsNeedsRedraw()) drawStatsPage();
  } else if (animChromeNeedsUpdate()) {
    refreshAnimChrome();
  }
}
