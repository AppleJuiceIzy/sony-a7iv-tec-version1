# A7IV-TEC

<!-- One-line pitch: what it is and why it exists -->
<!-- Hero image or GIF: the module mounted on the camera with the screen animating -->

![status](badge) ![license](MIT) ![platform](ESP32 / PlatformIO)

## Status
<!-- Be honest: what works, what's in progress, what's known-broken -->
- [x] Temperature sensing (DS18B20)
- [x] Proportional Peltier control with 80% duty cap
- [x] Animated display, stats page, manual cooling
- [ ] Hot-side sensor + thermal cutoff
- [ ] Enclosure v1

## How it works
<!-- 3–5 sentences, no jargon. Sensor reads camera surface → ESP32 decides → MOSFET drives Peltier → screen shows state -->
<!-- Block diagram image -->

### Thermal stack
<!-- Camera → copper shim → TEC cold face → TEC hot face → heatsink → fan. One image + one sentence each -->

### Control logic
<!-- Temperature bands table, hysteresis, HOT dwell, duty cap and why (15V rail, 12V module) -->

| State | Temp range | Animation | Peltier |
|---|---|---|---|

## Hardware

### Bill of materials
| Part | Spec | Qty | Approx cost | Link |
|---|---|---|---|---|
<!-- ESP32-WROOM-32, SSD1351 1.5" OLED, TEC1-12703, IRLB8721, DS18B20, ZY12PDN PD trigger,
     Adafruit 2500mAh LiPo + MicroLipo, 30mm 5V fan, 40x40 heatsink, copper shims, resistors, button -->

<!-- Total cost line -->

### Tools needed
<!-- Soldering iron, multimeter, USB-C PD charger with 12V or 15V profile, etc. -->

### Wiring
<!-- Schematic or wiring diagram image -->

| Component | Pin | ESP32 / Rail |
|---|---|---|

<!-- Callouts: common ground, low-side MOSFET orientation, DS18B20 pinout, pull-up placement -->

> ⚠️ Safety: <!-- LiPo handling, never run the TEC without a heatsink, duty cap, polarity -->

## Firmware

### Requirements
<!-- PlatformIO (CLI or IDE), CP210x driver on macOS, Python 3 + Pillow for the sprite tool -->

### Build and flash
```bash
git clone ...
cd firmware
pio run --target upload
pio device monitor --baud 115200
```
<!-- Note on upload_port in platformio.ini -->

### Configuration
<!-- The constants people will actually change: DUTY_MAX, TEMP_OFF/FULL, state thresholds,
     BATTERY_DIVIDER_RATIO (+ how to calibrate), MANUAL_COOL_DUTY, timeout -->

### Controls
| Input | Action |
|---|---|
| Single press | Toggle stats page |
| Double press | Manual cooling (10 min timeout) |

### Serial commands
| Command | Effect |
|---|---|
<!-- state 0..5, state auto, boot, page, cool -->

## Custom sprites
<!-- How to swap in your own art: 128x128 PNGs → art/ → run tools/png2sprites.py → upload -->
<!-- Frame naming convention and the CLIPS table -->

## Repository structure
