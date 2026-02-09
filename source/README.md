# BLE echo between laptop and ESP32

This repo contains:
- `low/src/main.cpp` - BLE peripheral for ESP32 that logs incoming control commands (PlatformIO).
- `laptop/ble_client.py` - BLE client that sends differential drive commands from WASD.

## ESP32 (PlatformIO)

1. Install PlatformIO (VS Code extension or CLI).
2. Build and upload:

```
pio run -t upload
```

3. Open serial monitor:

```
pio device monitor
```

You should see `BLE echo service started`.

The device advertises as `esp32dev` and exposes Nordic UART Service UUIDs.

### Control protocol

Single command is 4 bytes:
- byte0: left motor power (int8, -127..127)
- byte1: right motor power (int8, -127..127)
- byte2..3: duration in ms (uint16, little-endian)

## Laptop (Python)

1. Create and activate a virtual environment if desired.
2. Install dependencies:

```
pip install -r laptop/requirements.txt
```

3. Run the client (GUI window, WASD, 50 ms polling):

```
python laptop/ble_client.py --name esp32dev
```

If name discovery does not work, pass the device address:

```
python laptop/ble_client.py --address AA:BB:CC:DD:EE:FF
```
