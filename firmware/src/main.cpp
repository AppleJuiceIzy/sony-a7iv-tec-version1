#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include "sprites.h"

// Pins
static const int ONE_WIRE_BUS = 4;
static const int OLED_CS = 5;
static const int OLED_DC = 17;
static const int OLED_RST = 16;
static const int OLED_SCLK = 18;
static const int OLED_MOSI = 23;
static const int PELTIER_PIN = 25;
static const int BATTERY_ADC_PIN = 34;

// Battery divider: equal resistors => Vbat = Vadc * 2. Change if your board differs.
static const float BATTERY_DIVIDER_RATIO = 2.0f;
static const float ADC_REF_V = 3.3f;
static const int ADC_MAX = 4095;
static const int BATTERY_ADC_SAMPLES = 10;
static const float BATTERY_V_EMPTY = 3.0f;
static const float BATTERY_V_FULL = 4.2f;
static const float BATTERY_V_USB_THRESHOLD = 2.5f;

// OLED
static const int SCREEN_WIDTH = 128;
static const int SCREEN_HEIGHT = 128;
static const int SPRITE_X = (SCREEN_WIDTH - SPRITE_W) / 2;   // 40
static const int SPRITE_Y = (SCREEN_HEIGHT - SPRITE_H) / 2;  // 40

// Control thresholds (°C) — Peltier PWM (unchanged)
static const float TEMP_OFF = 30.0f;
static const float TEMP_FULL = 45.0f;

// Animation temperature thresholds (°C)
static const float ANIM_IDLE_MAX = 30.0f;
static const float ANIM_WARMING_MAX = 42.0f;

// PWM (8-bit LEDC)
static const int PWM_CHANNEL = 0;
static const int PWM_FREQ_HZ = 5000;
static const int PWM_RESOLUTION_BITS = 8;

// Timing
static const unsigned long SAMPLE_INTERVAL_MS = 500;
static const unsigned long ANIM_FRAME_INTERVAL_MS = 250;  // ~4 fps

// Colors (RGB565)
static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;

// Battery icon layout (top-right)
static const int BAT_ICON_X = 96;
static const int BAT_ICON_Y = 4;
static const int BAT_ICON_W = 22;
static const int BAT_ICON_H = 10;
static const int BAT_CHROME_X = 90;
static const int BAT_CHROME_Y = 0;
static const int BAT_CHROME_W = 38;
static const int BAT_CHROME_H = 18;

// Temp chrome (bottom-left)
static const int TEMP_CHROME_X = 2;
static const int TEMP_CHROME_Y = 116;
static const int TEMP_CHROME_W = 56;
static const int TEMP_CHROME_H = 12;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
Adafruit_SSD1351 display =
    Adafruit_SSD1351(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_CS, OLED_DC, OLED_RST);

unsigned long lastSampleMs = 0;
unsigned long lastAnimMs = 0;

float lastTempC = NAN;
bool lastTempValid = false;
int lastAdcRaw = 0;
float lastVbat = 0.0f;
bool lastUsbMode = true;
int lastBatteryPct = 0;

uint8_t animFrameIndex = 0;
AnimState currentAnimState = ANIM_IDLE;
bool forceAnim = false;
AnimState forcedAnimState = ANIM_IDLE;

static char serialLineBuf[64];
static size_t serialLineLen = 0;

uint16_t spriteLineBuf[SPRITE_W];

int computeDuty(float tempC) {
  if (tempC < TEMP_OFF) {
    return 0;
  }
  if (tempC >= TEMP_FULL) {
    return 255;
  }
  float duty = (tempC - TEMP_OFF) / (TEMP_FULL - TEMP_OFF) * 255.0f;
  if (duty < 0.0f) {
    return 0;
  }
  if (duty > 255.0f) {
    return 255;
  }
  return static_cast<int>(duty + 0.5f);
}

void setPeltierDuty(int duty) {
  if (duty < 0) {
    duty = 0;
  }
  if (duty > 255) {
    duty = 255;
  }
  ledcWrite(PWM_CHANNEL, duty);
}

AnimState animStateFromTemp(float tempC, bool valid) {
  if (!valid) {
    return ANIM_IDLE;
  }
  if (tempC < ANIM_IDLE_MAX) {
    return ANIM_IDLE;
  }
  if (tempC <= ANIM_WARMING_MAX) {
    return ANIM_WARMING;
  }
  return ANIM_COOLING;
}

AnimState activeAnimState() {
  return forceAnim ? forcedAnimState : currentAnimState;
}

void readBattery(int &adcRawOut, float &vbatOut) {
  long sum = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    sum += analogRead(BATTERY_ADC_PIN);
  }
  adcRawOut = static_cast<int>(sum / BATTERY_ADC_SAMPLES);
  float vadc = static_cast<float>(adcRawOut) * (ADC_REF_V / static_cast<float>(ADC_MAX));
  vbatOut = vadc * BATTERY_DIVIDER_RATIO;
}

int batteryPercent(float vbat) {
  float pct = (vbat - BATTERY_V_EMPTY) / (BATTERY_V_FULL - BATTERY_V_EMPTY) * 100.0f;
  if (pct < 0.0f) {
    return 0;
  }
  if (pct > 100.0f) {
    return 100;
  }
  return static_cast<int>(pct + 0.5f);
}

void drawSpriteFrame(AnimState state, uint8_t frameIndex) {
  const uint8_t *frame = spriteFrame(state, frameIndex);
  const uint16_t *palette = spritePalette(state);

  for (int y = 0; y < SPRITE_H; y++) {
    for (int x = 0; x < SPRITE_W; x++) {
      uint8_t idx = pgm_read_byte(&frame[y * SPRITE_W + x]);
      if (idx == 0 || idx > 4) {
        spriteLineBuf[x] = COLOR_BLACK;
      } else {
        spriteLineBuf[x] = palette[idx];
      }
    }
    display.drawRGBBitmap(SPRITE_X, SPRITE_Y + y, spriteLineBuf, SPRITE_W, 1);
  }
}

void drawTempChrome(float tempC, bool valid) {
  display.fillRect(TEMP_CHROME_X, TEMP_CHROME_Y, TEMP_CHROME_W, TEMP_CHROME_H, COLOR_BLACK);

  char tempBuf[16];
  if (valid) {
    snprintf(tempBuf, sizeof(tempBuf), "%.1fC", tempC);
  } else {
    snprintf(tempBuf, sizeof(tempBuf), "--.-C");
  }

  display.setTextSize(1);
  display.setTextColor(COLOR_WHITE);
  display.setCursor(TEMP_CHROME_X, TEMP_CHROME_Y + 2);
  display.print(tempBuf);
}

void drawBatteryChrome(float /*vbat*/, int pct, bool usbMode) {
  display.fillRect(BAT_CHROME_X, BAT_CHROME_Y, BAT_CHROME_W, BAT_CHROME_H, COLOR_BLACK);

  // Battery body + terminal nub
  display.drawRect(BAT_ICON_X, BAT_ICON_Y, BAT_ICON_W, BAT_ICON_H, COLOR_WHITE);
  display.fillRect(BAT_ICON_X + BAT_ICON_W, BAT_ICON_Y + 3, 2, 4, COLOR_WHITE);

  if (usbMode) {
    display.drawLine(BAT_ICON_X + 2, BAT_ICON_Y + 2, BAT_ICON_X + BAT_ICON_W - 3,
                     BAT_ICON_Y + BAT_ICON_H - 3, COLOR_WHITE);
    display.drawLine(BAT_ICON_X + 2, BAT_ICON_Y + BAT_ICON_H - 3, BAT_ICON_X + BAT_ICON_W - 3,
                     BAT_ICON_Y + 2, COLOR_WHITE);
    return;
  }

  const int innerX = BAT_ICON_X + 2;
  const int innerY = BAT_ICON_Y + 2;
  const int innerW = BAT_ICON_W - 4;
  const int innerH = BAT_ICON_H - 4;
  int fillW = (innerW * pct) / 100;
  if (fillW > 0) {
    uint16_t fillColor = 0x07E0;  // green
    if (pct <= 20) {
      fillColor = 0xF800;  // red
    } else if (pct <= 50) {
      fillColor = 0xFFE0;  // yellow
    }
    display.fillRect(innerX, innerY, fillW, innerH, fillColor);
  }

  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
  int16_t x1, y1;
  uint16_t tw, th;
  display.setTextSize(1);
  display.getTextBounds(pctBuf, 0, 0, &x1, &y1, &tw, &th);
  display.setTextColor(COLOR_WHITE);
  display.setCursor(SCREEN_WIDTH - static_cast<int16_t>(tw) - 2, BAT_ICON_Y + BAT_ICON_H + 1);
  display.print(pctBuf);
}

void handleSerialLine(const char *line) {
  // Trim leading spaces
  while (*line == ' ' || *line == '\t') {
    line++;
  }

  if (strncmp(line, "state", 5) != 0) {
    return;
  }

  const char *arg = line + 5;
  while (*arg == ' ' || *arg == '\t') {
    arg++;
  }

  if (strcmp(arg, "0") == 0) {
    forceAnim = true;
    forcedAnimState = ANIM_IDLE;
    Serial.println(F("Anim: forced IDLE"));
  } else if (strcmp(arg, "1") == 0) {
    forceAnim = true;
    forcedAnimState = ANIM_WARMING;
    Serial.println(F("Anim: forced WARMING"));
  } else if (strcmp(arg, "2") == 0) {
    forceAnim = true;
    forcedAnimState = ANIM_COOLING;
    Serial.println(F("Anim: forced COOLING"));
  } else if (strcmp(arg, "auto") == 0) {
    forceAnim = false;
    Serial.println(F("Anim: auto (temperature-driven)"));
  } else {
    Serial.println(F("Anim: use 'state 0|1|2|auto'"));
    return;
  }

  drawSpriteFrame(activeAnimState(), animFrameIndex);
}

void pollSerialCommands() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (serialLineLen > 0) {
        serialLineBuf[serialLineLen] = '\0';
        handleSerialLine(serialLineBuf);
        serialLineLen = 0;
      }
      continue;
    }
    if (serialLineLen + 1 < sizeof(serialLineBuf)) {
      serialLineBuf[serialLineLen++] = c;
    } else {
      serialLineLen = 0;  // overflow: reset
    }
  }
}

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

  readBattery(lastAdcRaw, lastVbat);
  lastUsbMode = (lastVbat < BATTERY_V_USB_THRESHOLD);
  lastBatteryPct = batteryPercent(lastVbat);

  drawSpriteFrame(activeAnimState(), animFrameIndex);
  drawTempChrome(lastTempC, lastTempValid);
  drawBatteryChrome(lastVbat, lastBatteryPct, lastUsbMode);

  Serial.println(F("ESP32 DS18B20 + SSD1351 + Peltier ready"));
  Serial.println(F("Commands: state 0 | state 1 | state 2 | state auto"));

  unsigned long now = millis();
  lastSampleMs = now;
  lastAnimMs = now;
}

void loop() {
  pollSerialCommands();

  unsigned long now = millis();

  // Animation frame advance (~4 fps) — redraw sprite region only
  if (now - lastAnimMs >= ANIM_FRAME_INTERVAL_MS) {
    lastAnimMs = now;
    animFrameIndex ^= 1;
    drawSpriteFrame(activeAnimState(), animFrameIndex);
  }

  // Sensor / PWM / battery sample tick
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSampleMs = now;

  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  bool valid = (tempC != DEVICE_DISCONNECTED_C) && !isnan(tempC);

  int duty = 0;
  if (valid) {
    duty = computeDuty(tempC);
  } else {
    Serial.println(F("WARN: DS18B20 disconnected or invalid reading"));
  }

  setPeltierDuty(duty);

  int adcRaw = 0;
  float vbat = 0.0f;
  readBattery(adcRaw, vbat);
  bool usbMode = (vbat < BATTERY_V_USB_THRESHOLD);
  int pct = batteryPercent(vbat);

  if (valid) {
    Serial.printf("Temp: %.2f C | PWM: %d | ADC: %d | Vbat: %.2f V\n", tempC, duty, adcRaw,
                  vbat);
  } else {
    Serial.printf("Temp: --.- C | PWM: %d | ADC: %d | Vbat: %.2f V\n", duty, adcRaw, vbat);
  }

  // Update auto animation state from temperature
  AnimState newState = animStateFromTemp(tempC, valid);
  bool stateChanged = (newState != currentAnimState);
  currentAnimState = newState;
  if (stateChanged && !forceAnim) {
    drawSpriteFrame(activeAnimState(), animFrameIndex);
  }

  // Chrome updates only when values change
  bool tempChanged =
      (valid != lastTempValid) || (valid && fabsf(tempC - lastTempC) > 0.05f);
  if (tempChanged) {
    drawTempChrome(tempC, valid);
    lastTempC = tempC;
    lastTempValid = valid;
  }

  bool batChanged = (adcRaw != lastAdcRaw) || (usbMode != lastUsbMode) || (pct != lastBatteryPct) ||
                    (fabsf(vbat - lastVbat) > 0.01f);
  if (batChanged) {
    drawBatteryChrome(vbat, pct, usbMode);
    lastAdcRaw = adcRaw;
    lastVbat = vbat;
    lastUsbMode = usbMode;
    lastBatteryPct = pct;
  }
}
