# CROSS-firmware

Firmware for the CROSS hardware platform by Project Seven.

ESP32-based device. RGB LED strip (common anode or WS2812). Polls the [SchedKit](https://schedkit.net) Alerts API every 30s — fires the LED on new alerts, acks them after pulsing.

## Releases

Compiled `.bin` files are published automatically via GitHub Actions on every version tag (`v*`).

Flash with Arduino IDE, esptool, or OTA:
```
esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x10000 cross.bin
```

## Building locally

Requires [arduino-cli](https://arduino.github.io/arduino-cli/):

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "ArduinoJson" "FastLED"
arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir ./build cross/
```

## OTA Updates

The firmware checks GitHub releases once every 24h. Tag a release `vX.Y` with `cross.bin` attached and all deployed devices will update automatically within 24h.

## Configuration

Key constants in `cross/cross.ino`:
- `SCHEDKIT_BASE_URL` — SchedKit API endpoint
- `SCHEDKIT_API_KEY` — SchedKit user API key
- `ALERT_POLL_MS` — Alert poll interval (default 30s)
- `LED_TYPE_COMMON_ANODE` / `LED_TYPE_ADDRESSABLE` — Select LED type

## License

Project Seven Co. Ltd.
