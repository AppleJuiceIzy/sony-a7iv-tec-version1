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

// ------------------------------------------------------------- battery
static const float BATTERY_DIVIDER_RATIO = 2.0f;  // tune against a multimeter
static const float ADC_REF_V = 3.3f;
static const int ADC_MAX = 4095;
static const int BATTERY_ADC_SAMPLES = 10;
static const float BATTERY_V_EMPTY = 3.0f;
static const float BATTERY_V_FULL = 4.2f;
static const float BATTERY_V_USB_THRESHOLD = 2.5f;

// ------------------------------------------------- Peltier duty control
static const float TEMP_OFF = 30.0f;
static const float TEMP_FULL = 45.0f;

// ------------------------------------------ animation temperature bands
static const float T_HOT = 40.0f;
static const float T_COOLING = 34.0f;
static const float T_NEAR = 30.0f;
static const float T_HYST = 1.5f;

static const int PWM_CHANNEL = 0;
static const int PWM_FREQ_HZ = 5000;
static const int PWM_RESOLUTION_BITS = 8;

static const unsigned long SAMPLE_INTERVAL_MS = 500;

static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;

// Chrome overlays, drawn on top of the sprite
static const int TEMP_X = 3;
static const int TEMP_Y = 118;
static const int BAT_ICON_X = 100;
static const int BAT_ICON_Y = 4;
static const int BAT_ICON_W = 22;
static const int BAT_ICON_H = 10;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
Adafruit_SSD1351 display =
    Adafruit_SSD1351(SPRITE_W, SPRITE_H, &SPI, OLED_CS, OLED_DC, OLED_RST);

// Draw the frame in horizontal chunks to keep RAM small but SPI efficient
static const int CHUNK_ROWS = 8;
uint16_t chunkBuf[SPRITE_W * CHUNK_ROWS];

unsigned long lastSampleMs = 0;
unsigned long lastAnimMs = 0;

float lastTempC = NAN;
bool lastTempValid = false;
float lastVbat = 0.0f;
bool lastUsbMode = true;
int lastBatteryPct = 0;

AnimState currentState = ANIM_BOOT;
uint8_t frameIndex = 0;
bool bootComplete = false;

bool forceAnim = false;
AnimState forcedState = ANIM_COOLED;

static char lineBuf[64];
static size_t lineLen = 0;

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

void redraw() {
  drawFrame(forceAnim ? forcedState : currentState, frameIndex);
  if (bootComplete) {
    drawChrome(lastTempC, lastTempValid, lastBatteryPct, lastUsbMode);
  }
}

// ------------------------------------------------------------- control

int computeDuty(float tempC) {
  if (tempC < TEMP_OFF) return 0;
  if (tempC >= TEMP_FULL) return 255;
  float d = (tempC - TEMP_OFF) / (TEMP_FULL - TEMP_OFF) * 255.0f;
  return (int)(d + 0.5f);
}

void setPeltierDuty(int duty) {
  ledcWrite(PWM_CHANNEL, constrain(duty, 0, 255));
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

void readBattery(float &vbatOut, int &rawOut) {
  long sum = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) sum += analogRead(BATTERY_ADC_PIN);
  rawOut = (int)(sum / BATTERY_ADC_SAMPLES);
  vbatOut = rawOut * (ADC_REF_V / ADC_MAX) * BATTERY_DIVIDER_RATIO;
}

int batteryPercent(float vbat) {
  float p = (vbat - BATTERY_V_EMPTY) / (BATTERY_V_FULL - BATTERY_V_EMPTY) * 100.0f;
  return (int)constrain(p, 0.0f, 100.0f);
}

// -------------------------------------------------------------- serial

void handleLine(const char *line) {
  while (*line == ' ') line++;

  if (strcmp(line, "boot") == 0) {
    currentState = ANIM_BOOT;
    frameIndex = 0;
    bootComplete = false;
    forceAnim = false;
    Serial.println(F("Anim: replaying boot"));
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
      Serial.println(F("Anim: state 0=boot 1=cooled 2=near 3=cooling 4=hot, or 'auto'"));
      return;
    }
    forceAnim = true;
    forcedState = (AnimState)n;
    frameIndex = 0;
    Serial.printf("Anim: forced state %d\n", n);
  }
  redraw();
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

  ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PELTIER_PIN, PWM_CHANNEL);
  setPeltierDuty(0);

  sensors.begin();

  SPI.begin(OLED_SCLK, -1, OLED_MOSI);
  display.begin();
  display.fillScreen(COLOR_BLACK);

  int raw;
  readBattery(lastVbat, raw);
  lastUsbMode = (lastVbat < BATTERY_V_USB_THRESHOLD);
  lastBatteryPct = batteryPercent(lastVbat);

  currentState = ANIM_BOOT;
  frameIndex = 0;
  redraw();

  Serial.println(F("A7IV-TEC ready"));
  Serial.println(F("Commands: state 0..4 | state auto | boot"));

  lastSampleMs = lastAnimMs = millis();
}

// ---------------------------------------------------------------- loop

void loop() {
  pollSerial();
  unsigned long now = millis();

  AnimState shown = forceAnim ? forcedState : currentState;
  const AnimClip &clip = animClip(shown);

  if (now - lastAnimMs >= clip.frame_ms) {
    lastAnimMs = now;
    frameIndex++;

    // Boot plays once, then hands over to temperature-driven states
    if (!bootComplete && shown == ANIM_BOOT && frameIndex >= clip.count) {
      bootComplete = true;
      currentState = ANIM_COOLED;
      frameIndex = 0;
    } else if (frameIndex >= clip.count) {
      frameIndex = 0;
    }
    redraw();
  }

  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  bool valid = (tempC != DEVICE_DISCONNECTED_C) && !isnan(tempC);

  int duty = valid ? computeDuty(tempC) : 0;
  setPeltierDuty(duty);

  float vbat;
  int raw;
  readBattery(vbat, raw);

  lastTempC = tempC;
  lastTempValid = valid;
  lastVbat = vbat;
  lastUsbMode = (vbat < BATTERY_V_USB_THRESHOLD);
  lastBatteryPct = batteryPercent(vbat);

  if (valid) {
    Serial.printf("Temp: %.2f C | PWM: %d | ADC: %d | Vbat: %.2f V | state %d\n", tempC, duty,
                  raw, vbat, (int)currentState);
  } else {
    Serial.printf("Temp: --.- C | PWM: %d | ADC: %d | Vbat: %.2f V\n", duty, raw, vbat);
  }

  if (bootComplete) {
    currentState = stateFromTemp(tempC, valid, currentState);
  }
}
