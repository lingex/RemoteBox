# RemoteBox

RemoteBox is a small ESP32-C3 based one-button remote. It sends a JSON command
over ESP-NOW, shows status on a 128x64 LCD, and can change its ESP-NOW channel
directly on the device. In practice, it is a handheld remote for
[lingex/IRStation](https://github.com/lingex/IRStation).

## Features

- Sends the `power` command over ESP-NOW broadcast.
- Adds a per-command `uid` so repeated packets can be deduplicated by the receiver.
- Sends each command three times with the same payload to improve reliability.
- Includes a CRC32-style `chk` field covering the whole JSON packet except
  `chk` itself.
- Shows battery/status information on an ST7567/U8g2 LCD.
- Supports USB-powered standby mode and battery-triggered send-and-power-off mode.
- Allows channel selection from `CH 1` to `CH 13` with the single button.

## Hardware

The firmware targets:

- Board: `esp32-c3-devkitm-1`
- Framework: Arduino via PlatformIO
- Filesystem: LittleFS
- Display: ST7567 128x64 LCD driven by U8g2

Important pins are defined in `src/main.cpp`:

| Function | GPIO |
| --- | ---: |
| Power hold | 1 |
| Button | 8 |
| USB detect | 10 |
| LCD SCK | 6 |
| LCD SDA | 7 |
| LCD DC | 5 |
| LCD RST | 4 |
| LCD backlight | 9 |
| Battery divider enable | 0 |
| Battery ADC | 3 |

## Configuration

Default configuration lives in `data/config.json` and is also recreated on the
device if missing or invalid:

```json
{
  "id": "remote-001",
  "name": "RemoteBox",
  "to": "IRStation-01",
  "cmd": "power",
  "data": {},
  "channel": 11,
  "backlightBrightness": 50
}
```

Fields:

- `id`: Sender ID included in every command packet.
- `name`: Friendly device name shown on the LCD.
- `to`: Target device name or ID included in every command packet.
- `cmd`: Command value included in every command packet.
- `data`: Extra JSON parameters included in every command packet as `dat`.
  Keep it small because ESP-NOW v1 payloads are limited to 250 bytes.
- `channel`: ESP-NOW channel, valid range `1-13`.
- `backlightBrightness`: LCD backlight brightness percentage, `0-100`.

## Button Operation

### Normal/Battery Mode

When powered by the button without USB:

- Short press/release: send the configured `cmd`, show the result, then power off.
- Long press for 2 seconds: enter channel settings instead of sending.

### USB Mode

When USB is connected:

- Short press/release: send the configured `cmd`.
- Long press for 2 seconds: enter channel settings.
- The screen stays available while USB is present and dims after idle time.

### Channel Settings

In channel settings:

- Short press: switch to the next channel.
- Channels cycle as `1 -> 2 -> ... -> 13 -> 1`.
- Long press for 2 seconds: save the selected channel and exit.
- 5 seconds idle: cancel changes and exit.
- Saving only updates local config; it does not send a test command.

## ESP-NOW Packet

Each command is serialized as JSON before sending:

```json
{
  "id": "remote-001",
  "uid": "00001234-0001-A7F2",
  "to": "IRStation-01",
  "cmd": "power",
  "dat": {},
  "bat": 86,
  "chk": "1A2B3C4D"
}
```

Notes:

- The same JSON payload is sent three times for one button action.
- All three repeats share the same `uid` and `chk`.
- The receiver should deduplicate by `id + uid` and only execute a repeated
  command once.
- `chk` is generated from a canonical JSON representation:

  - remove the top-level `chk` field
  - sort object keys alphabetically, recursively
  - keep array item order unchanged
  - serialize as compact JSON
  - calculate CRC32 over that string

For example, the checksum source is shaped like:

```json
{"bat":86,"cmd":"power","dat":{},"id":"remote-001","to":"IRStation-01","uid":"00001234-0001-A7F2"}
```

## Channel Notes

The sender and receiver should normally use the same ESP-NOW channel.
Adjacent 2.4 GHz Wi-Fi channels overlap, so very nearby devices may sometimes
receive packets on neighboring channels such as `CH10`, `CH11`, and `CH12`.
Do not rely on that behavior for real use. Use wider tests such as `CH1`,
`CH6`, and `CH11` to confirm actual channel matching.

## Known Issues

1. `VIN-SW1-IO8` voltage is too high and may damage the IO pin. Increase `R20`
   to `1K`, and add a `3V3` Zener diode in parallel with `R22`.
2. ESP startup is relatively slow, so `SW1` must be held for at least several
   hundred milliseconds before the firmware can lock power. Add a capacitor in
   parallel with `R21`; `1uF` was tested and works well enough.

## Build

Install PlatformIO, then run:

```powershell
pio run
```

Upload firmware:

```powershell
pio run -t upload
```

Upload LittleFS data:

```powershell
pio run -t uploadfs
```

Open serial monitor:

```powershell
pio device monitor
```

## Project Layout

```text
.
|-- data/
|   `-- config.json
|-- doc/
|   |-- BOM / schematic / PCB references
|   `-- LCD notes and images
|-- src/
|   `-- main.cpp
|-- partitions.csv
`-- platformio.ini
```
