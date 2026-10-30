# dryer_cntrl ESP32 Mini-D32 port

This is an ESP32 Mini-D32 port of the original RP2040-based dryer controller project.

The logic is mostly hardware-agnostic: it emulates the dryer encoder and communicates via UART. The main RP2040-specific pieces were:

- the Pico hardware timer (`add_alarm_in_us`, `cancel_alarm`)
- the internal temperature read (`analogReadTemp()`)

These have been replaced with an ESP32-friendly timer implementation and a thermistor read on GPIO 35.

## Hardware notes

This code was built to be compatible with an ESP32 Mini-D32 board.

The original hardware seems to use the following wiring concept:

- Encoder A -> GPIO 2
- Encoder B -> GPIO 3
- Encoder SW -> GPIO 4

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

On first boot, connect to the `DryerController-Setup` Wi-Fi network and use
the captive portal to select your home Wi-Fi network. Once connected, open the
IP address printed to the serial console at 115200 baud to view the live dryer
status page. The page shows the current state, the GPIO 35 thermistor reading,
the ESP32 chip's internal temperature, target temperature, target time, and
remaining time; it refreshes every five seconds. The chip temperature is the
ESP32 die temperature, not the dryer temperature.

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
    payload: "set temp {{ temperature }}"
  dryer_set_time:
    url: "http://192.168.1.50/api/command"
    method: POST
    content_type: "text/plain"
    payload: "set time {{ hours }}"
  dryer_start:
    url: "http://192.168.1.50/api/command"
    method: POST
    content_type: "text/plain"
    payload: "set state start"
  dryer_stop:
    url: "http://192.168.1.50/api/command"
    method: POST
    content_type: "text/plain"
    payload: "set state stop"

To expose the current status in Home Assistant, add a REST sensor:

```yaml
sensor:
  - platform: rest
    name: Dryer Status
    resource: http://192.168.1.50/api/status
    value_template: "{{ value_json.state }}"
    json_attributes:
      - temperature
      - chipTemperature
      - targetTemperature
      - targetTime
      - timeRemaining
    scan_interval: 10
```

For stable automation, reserve a fixed IP for the ESP32 in your router. The
API has no authentication and uses plain HTTP: anyone on the same network who
can reach it can control the dryer. Keep it on a trusted LAN and do not expose
it directly to the internet.
