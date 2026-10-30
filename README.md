# dryer_cntrl ESP32 Mini-D32 port

This is an ESP32 Mini-D32 port of the original RP2040-based dryer controller project.

The logic is mostly hardware-agnostic: it emulates the dryer encoder and communicates via UART. The main RP2040-specific pieces were:

- the Pico hardware timer (`add_alarm_in_us`, `cancel_alarm`)
- the internal temperature read (`analogReadTemp()`)

These have been replaced with an ESP32-friendly timer implementation and a thermistor read on GPIO 35.

## Hardware notes

This code was built to be compatible with an ESP32 Mini-D32 board.

The original hardware seems to use the following wiring concept:

- Encoder A -> GPIO 27
- Encoder B -> GPIO 25
- Encoder SW -> GPIO 32

Those are kept as the default GPIOs in the project, but you may need to remap them for your exact wiring and board variant.

GPIO 35 reads a 10 kΩ NTC thermistor using the Marlin type 4 beta value
(B = 3950 K), with a 10 kΩ fixed pull-up to match the resistor already
installed. Wire the divider from 3.3 V through the 10 kΩ resistor to a
junction at GPIO 35, then connect the NTC from that junction to GND. Use the
ESP32's 3.3 V supply, not 5 V. Change `thermistorPullupOhms` in `src/main.cpp`
if the fixed resistor value changes. The conversion assumes a 3.3 V supply
and nominal thermistor values, so verify it against a known temperature after
wiring.

## Build and upload with PlatformIO

Open this project in VS Code with the PlatformIO extension, or use the
PlatformIO Core CLI from the project directory:

```sh
pio run
pio run --target upload
pio device monitor
```

The configured environment is `wemos_d1_mini32` and the serial monitor is set
to 115200 baud. Change the board in `platformio.ini` if you are using a
different ESP32 board.

The firmware version is defined by `kFirmwareVersion` in `src/main.cpp` and
shown in the dashboard's **Firmware updates** section. Update it when preparing
a firmware release.

## Wireless firmware updates

After configuring an HTTP username and password, the dashboard offers a
**Firmware updates** section. You can upload a firmware `.bin` from your
computer or enter an HTTPS URL for the ESP32 to download. The ESP32 validates
the firmware image before installing it and restarts when the update completes.
The section displays the version currently running on the device.
HTTPS downloads verify server certificates against the ESP32 certificate
bundle; the device needs internet access to synchronize its clock, and the URL
must provide a content length. The default PlatformIO build artifact is
`.pio/build/wemos_d1_mini32/firmware.bin`.

Firmware installation always requires the configured HTTP username and
password, even when dashboard Basic Auth is disabled. This keeps firmware
installation restricted to credential holders because an update can replace
the device's software, while allowing Basic Auth to be disabled for ordinary
dashboard and API access. Choose a `.bin` built for this board and do not
interrupt power during installation.

PlatformIO network upload is also available using ArduinoOTA, at hostname
`dryer-controller.local`. It uses the configured HTTP password (not the
username), and is enabled after credentials have been created. Find the ESP32's
current IP in the serial output or your router, then run:

```sh
pio run -e wemos_d1_mini32_ota --target upload --upload-port 192.168.1.50
```

Replace the example address with the ESP32's IP. Use the same password as the
web login as the OTA password. PlatformIO's uploader does not prompt for it;
pass it using the `PLATFORMIO_UPLOAD_FLAGS` environment variable:

```sh
export PLATFORMIO_UPLOAD_FLAGS="--auth=YOUR_HTTP_PASSWORD"
pio run -e wemos_d1_mini32_ota --target upload --upload-port 192.168.1.50
unset PLATFORMIO_UPLOAD_FLAGS
```

Keep the password out of tracked project files. Both the web updater and
ArduinoOTA use unencrypted network transfers, and the dashboard uses HTTP, not
HTTPS. Use them only on a trusted local network; do not expose the device
directly to the internet.

## Usage

The command protocol is unchanged from the original project:

- `get temp`
- `get time`
- `get state`
- `set temp 55`
- `set time 6`
- `set state start`
- `set state stop`
- `help`

On first boot, connect to the `DryerController-Setup` Wi-Fi network using the
password `dryer_cntrl_esp32`, then use the captive portal to select your home
Wi-Fi network. Once connected, open the IP address printed to the serial
console at 115200 baud to view the live dryer status page. The page shows the
current state, the GPIO 35 thermistor reading, the ESP32 chip's internal
temperature, target temperature, target time, and remaining time; it refreshes
every five seconds. The chip temperature is the ESP32 die temperature, not the
dryer temperature.

The page can also set the target temperature and time, and start or stop the
dryer. The same controls are available over HTTP for automation:

- `GET /api/status` returns JSON with `state`, `temperature`,
  `chipTemperature`, `targetTemperature`, `targetTime`, and `timeRemaining`.
- `POST /api/command` accepts a plain-text command using the serial syntax,
  such as `set temp 55`, `set time 6`, `set state start`, or `set state stop`.
  Successful commands return `OK`; invalid commands return HTTP 400 and an
  `ERR:` message.

Example Home Assistant `rest_command` configuration (replace the address with
the ESP32's IP address):

```yaml
rest_command:
  dryer_set_temperature:
    url: "http://192.168.1.50/api/command"
    method: POST
    content_type: "text/plain"
    authentication: basic
    username: !secret dryer_username
    password: !secret dryer_password
    payload: "set temp {{ temperature }}"
  dryer_set_time:
    url: "http://192.168.1.50/api/command"
    method: POST
    content_type: "text/plain"
    authentication: basic
    username: !secret dryer_username
    password: !secret dryer_password
    payload: "set time {{ hours }}"
  dryer_start:
    url: "http://192.168.1.50/api/command"
    method: POST
    content_type: "text/plain"
    authentication: basic
    username: !secret dryer_username
    password: !secret dryer_password
    payload: "set state start"
  dryer_stop:
    url: "http://192.168.1.50/api/command"
    method: POST
    content_type: "text/plain"
    authentication: basic
    username: !secret dryer_username
    password: !secret dryer_password
    payload: "set state stop"

To expose the current status in Home Assistant, add a REST sensor:

```yaml
sensor:
  - platform: rest
    name: Dryer Status
    resource: http://192.168.1.50/api/status
    authentication: basic
    username: !secret dryer_username
    password: !secret dryer_password
    value_template: "{{ value_json.state }}"
    json_attributes:
      - temperature
      - chipTemperature
      - targetTemperature
      - targetTime
      - timeRemaining
    scan_interval: 10
```

Add matching `dryer_username` and `dryer_password` values to Home Assistant's
`secrets.yaml`.

For stable automation, reserve a fixed IP for the ESP32 in your router.

HTTP Basic Auth is disabled by default, so the dashboard is available without
login on a fresh install. To enable it, open **HTTP login settings**, set a
username and password, then enable Basic Auth. If auth is enabled without
credentials, the page asks you to create them. You can also change credentials
or disable/enable Basic Auth there. The choice and credentials are stored in
ESP32 non-volatile storage and survive normal firmware uploads. When Basic
Auth is disabled, anyone on the local network can view status and control the
dryer.

The default setup hotspot password is `dryer_cntrl_esp32`, so change it to a
unique value if nearby people could access the device. To change it, copy
`include/secrets.h.example` to `include/secrets.h`, edit
`DRYER_WIFI_SETUP_PASSWORD`, and rebuild. The local `secrets.h` is ignored by
Git. If you forget the HTTP login, erase the ESP32 flash to clear credentials
and configure the device again.

You can also trigger a factory reset by power-cycling the ESP32 10 times
before it has stayed on for 60 seconds. On the 10th boot, it clears the saved
Wi-Fi network, HTTP login, and Basic Auth setting, then restarts. Reconfigure
Wi-Fi and create a new web login if desired. Once the device stays on for 60
seconds, the recovery counter resets. This is a factory reset, not just a
Wi-Fi reset.

HTTP Basic Authentication sends credentials over plain HTTP, not encrypted
transport. Create the initial login only while connected to a trusted network,
and do not expose the device directly to the internet. If Basic Auth is
disabled, the API is unauthenticated; keep the device on a trusted LAN.
