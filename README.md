# dryer_cntrl ESP32 Mini-D32 port

This is an ESP32 Mini-D32 port of the original RP2040-based dryer controller project.

The logic is mostly hardware-agnostic: it emulates the dryer encoder and communicates via UART. The main RP2040-specific pieces were:

- the Pico hardware timer (`add_alarm_in_us`, `cancel_alarm`)
- the internal temperature read (`analogReadTemp()`)

These have been replaced with an ESP32-friendly timer implementation and a generic ADC-based temperature read.

## Hardware notes

This code was built to be compatible with an ESP32 Mini-D32 board.

The original hardware seems to use the following wiring concept:

- Encoder A -> GPIO 2
- Encoder B -> GPIO 3
- Encoder SW -> GPIO 4

Those are kept as the default GPIOs in the project, but you may need to remap them for your exact wiring and board variant.

The ADC temperature read uses GPIO 35 by default. If your board or sensor arrangement differs, change `TEMP_PIN` in the sketch.

## Build

Use the ESP32 Arduino core in Arduino IDE and select a board such as:

- ESP32 Dev Module
- ESP32 Mini-D32 (if available in your board package)

Then compile and upload the sketch.

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

The board will speak over the USB serial console at 115200 baud.
