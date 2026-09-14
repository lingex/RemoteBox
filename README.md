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
- Connects to Wi-Fi only in USB mode and only when both Wi-Fi fields are set.
- Provides a status/configuration web page at `http://remotebox.local/`.
- Advertises HTTP and ESPOTA through mDNS and accepts PlatformIO ESPOTA updates.
- Synchronizes the saved ESP-NOW channel to the connected access point channel.

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
  "wifi": {
    "ssid": "",
    "password": ""
  },
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

- `wifi.ssid`: Wi-Fi SSID. A non-empty SSID also enables the `AUTO` channel
  selection item.
- `wifi.password`: Wi-Fi password. Network services are disabled unless both
  `wifi.ssid` and `wifi.password` are non-empty.
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
- With complete Wi-Fi credentials, the device connects to Wi-Fi and starts the
  web console, mDNS, and ESPOTA. These services stop as soon as USB is removed.
- The standby screen shows IP address, synchronized channel, device name,
  command, and target. With incomplete credentials it shows `Wifi disabled`.

## Web Console and ESPOTA

After Wi-Fi connects in USB mode, open either:

- `http://remotebox.local/`
- the IP address shown on the LCD

The page shows device, network, battery, ESP-NOW, memory, and filesystem status.
It can load, format, validate, and save `/config.json`. The browser checks JSON
syntax first; the firmware then validates required fields, types, lengths,
ranges, `data`, and the resulting ESP-NOW packet size. Saving uses a temporary
file and backup replacement to reduce the risk of a partial write.

When Wi-Fi credentials are changed from the page, the HTTP response is sent
before the device reconnects with the new values. The web console intentionally
has no authentication, so use it only on a trusted network.

### Channel Settings

In channel settings:

- Short press: switch to the next channel.
- Without `wifi.ssid`, channels cycle as `1 -> 2 -> ... -> 13 -> 1`.
- With `wifi.ssid`, an `AUTO` item appears after channel 13. Saving `AUTO`
  scans for the configured SSID, selects the channel of the strongest matching
  access point, and saves the detected channel number. A failed scan leaves the
  existing channel unchanged.
- Long press for 2 seconds: save the selected channel and exit.
- 5 seconds idle: cancel changes and exit.
- Saving only updates local config; it does not send a test command.

## ESP-NOW Packet

Each command is serialized as JSON before sending:

```json
{
  "id": "remote-001",
  "uid": "1234A7F2",
  "to": "IRStation-01",
  "cmd": "power",
  "dat": {},
  "bat": 86,
  "chk": "1A2B3C4D"
}
```

Notes:

- `uid` is a random 32-bit value encoded as eight uppercase hexadecimal
  characters (8 bytes in the JSON string).
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
{"bat":86,"cmd":"power","dat":{},"id":"remote-001","to":"IRStation-01","uid":"1234A7F2"}
```

## Channel Notes

The sender and receiver should normally use the same ESP-NOW channel.
Whenever Wi-Fi connects successfully in USB mode, the access point's actual
channel is authoritative. If it differs from `config.json`, the firmware updates
and saves the local `channel` automatically before starting network services.
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

The partition table now contains two OTA application slots. Flash this build by
USB at least once before using ESPOTA; the existing LittleFS offset and size are
kept unchanged.

Upload LittleFS data:

```powershell
pio run -t uploadfs
```

Open serial monitor:

```powershell
pio device monitor
```

Upload later firmware builds through ESPOTA (device must be connected to USB
power and Wi-Fi):

```powershell
pio run -e remote_box_ota -t upload
```

## Project Layout

```text
.
|-- data/
|   `-- config.json
|-- include/
|   `-- WebPage.h
|-- doc/
|   |-- BOM / schematic / PCB references
|   `-- LCD notes and images
|-- src/
|   `-- main.cpp
|-- partitions.csv
`-- platformio.ini
```
