# SassyPlant

# 🌱 Sassy Plant Monitor

An ESP32-C3 soil moisture monitor that sends personality-driven Discord alerts and drives a 256-LED WS2812B matrix with a colour-coded moisture indicator. All settings are configurable from a phone browser — no reflashing needed after the first upload.

---

## Hardware

| Component | Detail |
|---|---|
| Microcontroller | ESP32-C3 Super Mini |
| Moisture sensor | Analog capacitive sensor → GPIO 2 |
| LED matrix | 4× WS2812B-64 chained = 256 LEDs → GPIO 10 |
| Power | 5V via USB; power-inject each matrix panel for full brightness |

---

## Features

- **LED ambient display** — smooth RED → GREEN → BLUE colour blend driven by live moisture level, with a slow sine-wave pulse. Brightness and on/off controllable from phone.
- **Discord webhook alerts** — personality-driven messages in five modes depending on moisture level (critical panic, jealous girlfriend, gentle hints, sassy thriving, drowning complaints). Moisture % appears only in the embed stats fields, not in the message text.
- **Web config UI** — hosted directly on the ESP32 at `http://<device-ip>/`. Change thresholds, calibration values, LED brightness, LED on/off, and the Discord webhook URL from any browser on the same network. All settings persist across reboots via flash storage.
- **Direct band detection** — when moisture changes rapidly (e.g. after watering), the alert timer jumps straight to the real current level rather than stepping through intermediate bands.
- **NTP time sync** — timestamps on Discord embeds in NZST (UTC+12). Adjust `NTP_GMT_OFFSET` for your timezone.

---

## Setup

### 1. Dependencies

Install via Arduino Library Manager:
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)

Built into the ESP32 Arduino core (no install needed):
- `Preferences.h`
- `WebServer.h`

### 2. Board

In Arduino IDE, install the ESP32 board package and select **ESP32C3 Dev Module**.

### 3. Configure WiFi

Open `sassyplant.ino` and set your credentials:

```cpp
#define WIFI_SSID   "your_network_name"
#define WIFI_PASS   "your_password"
```

This is the only thing that requires a reflash to change.

### 4. Flash

Upload to the ESP32-C3. Open the Serial Monitor at 115200 baud — the device IP address will be printed on boot.

### 5. Configure via browser

Open `http://<device-ip>/` on any device on the same network. From here you can set:

- **Discord webhook URL** — paste your webhook from Discord Server Settings → Integrations → Webhooks
- **Dry threshold** — ADC value above which the sensor reads as dry (default 3600)
- **Flood threshold** — ADC value below which the sensor reads as flooded (default 2500)
- **Sensor dry calibration** — raw ADC reading with sensor in open air (default 4000)
- **Sensor wet calibration** — raw ADC reading with sensor fully submerged (default 2400)
- **LED brightness** — slider 0–255
- **LED on/off** — toggle switch

All settings save to flash immediately and survive power cycles.

---

## Discord Alert Modes

| Moisture | Personality | Timing |
|---|---|---|
| < 5% | 🚨 Critical panic | Every 1–20 seconds |
| 5–15% | 💔 Jealous girlfriend | Every 10 min – 2 hrs |
| 15–30% | 👀 Gentle hints | Every 10 min – 2 hrs |
| 30–80% | 😏 Sassy / plant facts | Every 10 min – 2 hrs |
| > 80% | 🌊 Drowning complaints | Every 10 min – 2 hrs |

Timing resets when moisture crosses into a new zone. Inside the 20–80% normal range the timer persists across minor drift.

---

## LED Colour Reference

| Colour | Meaning |
|---|---|
| 🔴 Red | Dry — needs water |
| 🟢 Green | Good — healthy moisture |
| 🔵 Blue | Flooded — too much water |

Colours blend smoothly between these points. The display pulses slowly at all times.

---

## Calibration Tips

1. Put the sensor in dry air → note the ADC value → set as **Dry Calibration**
2. Submerge the sensor in water → note the ADC value → set as **Wet Calibration**
3. Put the sensor in dry soil → note the ADC value → set as **Dry Threshold**
4. Water until just before overwatering → note the ADC value → set as **Flood Threshold**

The moisture percentage is then calculated linearly between the dry and flood thresholds.

---

## License

MIT — do whatever you want with it. If your plant survives, great.
