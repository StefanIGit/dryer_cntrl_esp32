#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>

#include "HWTimer.h"
#include "help.h"

#define NAME "Filament Dryer Automator"
#define VERSION "0.0.1"
#define DEBUG false

#define TEMP_PIN 35

constexpr float thermistorPullupOhms = 10000.0f;
constexpr float thermistorResistanceAt25C = 10000.0f;
constexpr float thermistorBeta = 3950.0f;
constexpr float thermistorSupplyMillivolts = 3300.0f;

/* Available modes */
enum Mode {
  MODE_GET,
  MODE_SET,
  MODE_HELP,
  MODE_UNKNOWN,
};

/* Available params */
enum Param {
  PARAM_TEMP,
  PARAM_TIME,
  PARAM_STATE,
  PARAM_UNKNOWN
};

/* Status messages */
const String ackErr = "ERR";
const String ack = "OK";

/* GPIO definitions */
const uint8_t pinA = 2; // Encoder A
const uint8_t pinB = 3; // Encoder B
const uint8_t pinSW = 4; // Switch

/* Max values */
const uint8_t minTemp = 45; // Degrees Celsius
const uint8_t maxTemp = 65; // Degrees Celsius
const uint8_t maxTime = 24; // Hours

/* Global params */
static bool stateRunning = false;
static uint8_t tempToSet = 0;
static uint8_t timeToSet = 0;

/* Timer */
HWTimer timer;
WebServer webServer(80);

void debugPrintf(const char* fmt, ...);
void handleStatusPage();
void handleStatusApi();
void handleCommandApi();
void startWifiAndWebServer();
String inputHandler(const String& input);
String modeGetHandler(const String& paramStr);
String modeSetHandler(const String& paramStr, const String& valueStr);
String setTemp(const String& tempStr);
String setTime(const String& timeStr);
String setStatus(const String& stateStr);
void resetStatus();
void dial(Param param, uint8_t value);
void rotateRight();
void rotateLeft();
void pressButton();
void setPins(int a, int b);

float analogReadTemp() {
  const uint32_t voltageMillivolts = analogReadMilliVolts(TEMP_PIN);
  if (voltageMillivolts == 0 || voltageMillivolts >= thermistorSupplyMillivolts)
    return NAN;

  const float thermistorResistance = thermistorPullupOhms * voltageMillivolts
      / (thermistorSupplyMillivolts - voltageMillivolts);
  const float inverseTemperatureKelvin = 1.0f / 298.15f
      + logf(thermistorResistance / thermistorResistanceAt25C) / thermistorBeta;
  return 1.0f / inverseTemperatureKelvin - 273.15f;
}

/**
 * @brief Arduino setup() function. Runs one.
 */
void setup() {
  pinMode(pinA, INPUT);
  pinMode(pinB, INPUT);
  pinMode(pinSW, INPUT);

  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  startWifiAndWebServer();
  printHelp();
  Serial.println(ack);
}

/**
 * @brief Arduino loop() function. Runs constantly.
 */
void loop() {
  static String input;
  static char ch;

  timer.update();
  webServer.handleClient();

  if (!Serial.available())
    return;

  ch = Serial.read();
  Serial.write(ch);

  switch (ch) {
    case '\r':
      Serial.println();
      if (Serial.peek() == '\n')
        Serial.read();
    case '\n':
      if (Serial.peek() == '\r')
        Serial.read();

      input.trim();
      debugPrintf("Received input: %s\r\n", input.c_str());
      Serial.println(inputHandler(input));

      input = "";
      return;
    case '\b':
      Serial.print(" \b");
      input.remove(input.length() - 1);
      return;
  }

  input += ch;
}

void startWifiAndWebServer() {
  WiFiManager wifiManager;
  if (!wifiManager.autoConnect("DryerController-Setup")) {
    Serial.println("ERR: Wi-Fi setup failed; restarting.");
    delay(1000);
    ESP.restart();
    return;
  }

  webServer.on("/", HTTP_GET, handleStatusPage);
  webServer.on("/api/status", HTTP_GET, handleStatusApi);
  webServer.on("/api/command", HTTP_POST, handleCommandApi);
  webServer.begin();

  Serial.printf("Wi-Fi connected. Status page: http://%s/\r\n",
                WiFi.localIP().toString().c_str());
}

void handleStatusPage() {
  static const char page[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Filament Dryer Status</title>
  <style>
  :root{color-scheme:dark;--page:#111820;--card:#1c2732;--text:#edf3f8;--muted:#b3c0cc;--border:#536273;--accent:#79baff}
  :root[data-theme="light"]{color-scheme:light;--page:#f4f6f8;--card:#fff;--text:#18212b;--muted:#536273;--border:#cbd3dc;--accent:#1769aa}
  body{font:16px system-ui,sans-serif;max-width:36rem;margin:3rem auto;padding:0 1rem;background:var(--page);color:var(--text)}
  main{background:var(--card);border-radius:12px;padding:1.5rem;box-shadow:0 2px 12px #17212b18}
  h1{margin-top:0}.status{font-size:1.4rem;font-weight:700}
  dl{display:grid;grid-template-columns:1fr auto;gap:.8rem;margin-bottom:0}
  dt{color:var(--muted)}dd{margin:0;font-weight:600}
  fieldset{border:0;padding:0;margin:1.5rem 0}
  label{display:block;margin:.8rem 0}
  select{font:inherit;padding:.45rem;width:10rem;color:var(--text);background:var(--card);border:1px solid var(--border);border-radius:6px}
  button{font:inherit;padding:.55rem .9rem;margin:.25rem;cursor:pointer;color:var(--text);background:var(--card);border:1px solid var(--border);border-radius:6px}
  button:hover{border-color:var(--accent)}
  :focus-visible{outline:2px solid var(--accent);outline-offset:2px}
  #result{min-height:1.5em}
  </style>
</head>
<body>
  <main>
    <h1>Filament Dryer</h1>
    <button type="button" id="themeButton" aria-pressed="false">Use light mode</button>
    <p class="status" id="state">Loading…</p>
    <dl>
      <dt>GPIO 35 thermistor</dt><dd><span id="temperature">--</span> °C</dd>
      <dt>ESP32 chip temperature</dt><dd><span id="chipTemperature">--</span> °C</dd>
      <dt>Target temperature</dt><dd><span id="targetTemperature">--</span> °C</dd>
      <dt>Target time</dt><dd><span id="targetTime">--</span> h</dd>
      <dt>Time remaining</dt><dd id="timeRemaining">--</dd>
    </dl>
    <fieldset>
      <legend>Settings</legend>
      <form id="temperatureForm">
        <label>Target temperature (45–65 °C)
          <select id="temperatureInput" aria-label="Target temperature"></select>
        </label>
        <button type="submit">Set temperature</button>
      </form>
      <form id="timeForm">
        <label>Target time (0–24 hours)
          <select id="timeInput" aria-label="Target time"></select>
        </label>
        <button type="submit">Set time</button>
      </form>
      <button type="button" id="startButton">Start</button>
      <button type="button" id="stopButton">Stop</button>
      <p id="result" role="status"></p>
    </fieldset>
  </main>
  <script>
    const themeButton = document.getElementById('themeButton');
    function setTheme(theme) {
      document.documentElement.dataset.theme = theme;
      const isDark = theme === 'dark';
      themeButton.textContent = isDark ? 'Use light mode' : 'Use dark mode';
      themeButton.setAttribute('aria-pressed', String(isDark));
    }

    setTheme('dark');
    themeButton.addEventListener('click', () => {
      setTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark');
    });

    function populateSelect(select, min, max, suffix) {
      for (let value = min; value <= max; value++) {
        const option = document.createElement('option');
        option.value = value;
        option.textContent = value + suffix;
        select.appendChild(option);
      }

      select.addEventListener('wheel', event => {
        event.preventDefault();
        const nextIndex = select.selectedIndex + (event.deltaY > 0 ? 1 : -1);
        select.selectedIndex = Math.max(0, Math.min(select.options.length - 1, nextIndex));
      }, {passive: false});
    }

    const temperatureInput = document.getElementById('temperatureInput');
    const timeInput = document.getElementById('timeInput');
    populateSelect(temperatureInput, 45, 65, ' °C');
    populateSelect(timeInput, 0, 24, ' hours');

    async function sendCommand(command) {
      const result = document.getElementById('result');
      result.textContent = 'Sending…';
      try {
        const response = await fetch('/api/command', {
          method: 'POST',
          headers: {'Content-Type': 'text/plain'},
          body: command
        });
        const message = await response.text();
        result.textContent = message;
        if (!response.ok) return;
        await refreshStatus();
      } catch (error) {
        result.textContent = 'Command failed; check the connection.';
      }
    }

    document.getElementById('temperatureForm').addEventListener('submit', event => {
      event.preventDefault();
      sendCommand('set temp ' + document.getElementById('temperatureInput').value);
    });
    document.getElementById('timeForm').addEventListener('submit', event => {
      event.preventDefault();
      sendCommand('set time ' + document.getElementById('timeInput').value);
    });
    document.getElementById('startButton').addEventListener('click', () => sendCommand('set state start'));
    document.getElementById('stopButton').addEventListener('click', () => sendCommand('set state stop'));

    async function refreshStatus() {
      try {
        const response = await fetch('/api/status', {cache: 'no-store'});
        if (!response.ok) throw new Error('Status request failed');
        const status = await response.json();
        document.getElementById('state').textContent = status.state;
        document.getElementById('temperature').textContent =
          Number.isFinite(status.temperature) ? status.temperature.toFixed(1) : 'Sensor error';
        document.getElementById('chipTemperature').textContent = status.chipTemperature.toFixed(1);
        document.getElementById('targetTemperature').textContent =
          status.targetTemperature || 'Not set';
        document.getElementById('targetTime').textContent = status.targetTime;
        document.getElementById('timeRemaining').textContent = status.timeRemaining;
        if (status.targetTemperature >= 45 && status.targetTemperature <= 65
            && document.activeElement !== temperatureInput && !temperatureInput.matches(':hover'))
          temperatureInput.value = status.targetTemperature;
        if (status.targetTime >= 0 && status.targetTime <= 24
            && document.activeElement !== timeInput && !timeInput.matches(':hover'))
          timeInput.value = status.targetTime;
      } catch (error) {
        document.getElementById('state').textContent = 'Status unavailable';
      }
    }
    refreshStatus();
    setInterval(refreshStatus, 5000);
  </script>
</body>
</html>
)html";

  webServer.send(200, "text/html; charset=utf-8", page);
}

void handleStatusApi() {
  String json;
  json.reserve(128);
  json = "{\"state\":\"";
  json += stateRunning ? "running" : "stopped";
  json += "\",\"temperature\":";
  const float temperature = analogReadTemp();
  json += isfinite(temperature) ? String(temperature, 1) : "null";
  json += ",\"chipTemperature\":";
  json += String(temperatureRead(), 1);
  json += ",\"targetTemperature\":";
  json += String(tempToSet);
  json += ",\"targetTime\":";
  json += String(timeToSet);
  json += ",\"timeRemaining\":\"";
  json += timer.getTimeLeft();
  json += "\"}";

  webServer.send(200, "application/json; charset=utf-8", json);
}

void handleCommandApi() {
  String command = webServer.arg("plain");
  command.trim();
  if (command.isEmpty() || command.length() > 64) {
    webServer.send(400, "text/plain; charset=utf-8", "ERR:Command must be 1-64 characters.");
    return;
  }

  const String response = inputHandler(command);
  const bool failed = response.startsWith(ackErr);
  webServer.send(failed ? 400 : 200, "text/plain; charset=utf-8", response);
}

/**
 * @brief Print message on Serial only if DEBUG is enabled.
 *
 * @param fmt message.
 * @param ... additional parameters.
 */
void debugPrintf(const char* fmt, ...) {
  const uint8_t buff_size = 255;

  if (DEBUG) {
    char buffer[buff_size];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    Serial.print("<DEB>");
    Serial.print(buffer);
  }
}

/**
 * @brief Converts parameter string to enum value.
 *
 * @param paramStr  parameter string.
 * @return          enum value of the parameter.
 */
Param strToParam(const String& paramStr) {
  debugPrintf("param: %s\r\n", paramStr.c_str());
  if (paramStr.equalsIgnoreCase("temp"))
    return PARAM_TEMP;
  if (paramStr.equalsIgnoreCase("time"))
    return PARAM_TIME;
  if (paramStr.equalsIgnoreCase("state"))
    return PARAM_STATE;
  return PARAM_UNKNOWN;
}

/**
 * @brief Converts mode string to enum value.
 *
 * @param modeStr mode string.
 * @return        enum value of the mode.
 */
Mode strToMode(const String& modeStr) {
  debugPrintf("mode: %s\r\n", modeStr.c_str());
  if (modeStr.equalsIgnoreCase("get"))
    return MODE_GET;
  if (modeStr.equalsIgnoreCase("set"))
    return MODE_SET;
  if (modeStr.equalsIgnoreCase("help"))
    return MODE_HELP;
  return MODE_UNKNOWN;
}

/**
 * @brief Handles input from Serial.
 *
 * @param input input string.
 */
String inputHandler(const String& input) {
  int sepIndex = 0;
  String mode, param, value;

  sepIndex = input.indexOf(' ');
  if (sepIndex < 0) {
    mode = input;
    param = "";
    value = "";
  } else {
    mode = input.substring(0, sepIndex);
    param = input.substring(++sepIndex);
    sepIndex = param.indexOf(' ');
    if (sepIndex >= 0) {
      value = param.substring(sepIndex + 1);
      param = param.substring(0, sepIndex);
    } else {
      value = "";
    }
  }

  switch (strToMode(mode)) {
    case MODE_GET:
      return modeGetHandler(param);
    case MODE_SET:
      return modeSetHandler(param, value);
    case MODE_UNKNOWN:
      if (input != "") {
        return ackErr + ":\"" + input + "\" is not a valid mode.";
      }
      return ack;
    case MODE_HELP:
      printHelp();
      return ack;
  }
  return ackErr + ":Invalid command.";
}

/**
 * @brief Handles operations in "get" mode.
 *
 * @param paramStr parameter string.
 */
String modeGetHandler(const String& paramStr) {
  switch (strToParam(paramStr)) {
    case PARAM_TEMP:
      {
        const float temperature = analogReadTemp();
        if (isfinite(temperature))
          return ack + ":" + String(temperature, 1);
        return ackErr + ":Temperature sensor reading is out of range.";
      }
    case PARAM_TIME:
      return ack + ":" + timer.getTimeLeft();
    case PARAM_STATE:
      return ack + ":" + (stateRunning ? "running" : "stopped");
    case PARAM_UNKNOWN:
      return ackErr + ":\"" + paramStr + "\" is not a valid param.";
  }
  return ackErr + ":Invalid parameter.";
}

/**
 * @brief Handles operations in "set" mode.
 *
 * @param paramStr  parameter string.
 * @param valueStr  parameter value.
 */
String modeSetHandler(const String& paramStr, const String& valueStr) {
  switch (strToParam(paramStr)) {
    case PARAM_TEMP:
      return setTemp(valueStr);
    case PARAM_TIME:
      return setTime(valueStr);
    case PARAM_STATE:
      return setStatus(valueStr);
    case PARAM_UNKNOWN:
      return ackErr + ":\"" + paramStr + "\" is not a valid param.";
  }
  return ackErr + ":Invalid parameter.";
}

/**
 * @brief Set up target temperature and apply immediately if already running.
 *
 * @param tempStr temperature string, will be parsed to integer.
 */
String setTemp(const String& tempStr) {
  if (tempStr.isEmpty())
    return ackErr + ":Temperature is required.";
  long parsedTemp = 0;
  for (size_t i = 0; i < tempStr.length(); i++) {
    if (!isDigit(tempStr[i]))
      return ackErr + ":Temperature must be a whole number.";
    parsedTemp = parsedTemp * 10 + (tempStr[i] - '0');
    if (parsedTemp > maxTemp)
      return ackErr + ":Allowed values range from " + String(minTemp) + " to " + String(maxTemp) + ".";
  }
  if (parsedTemp < minTemp || parsedTemp > maxTemp)
    return ackErr + ":Allowed values range from " + String(minTemp) + " to " + String(maxTemp) + ".";
  const uint8_t temp = static_cast<uint8_t>(parsedTemp);

  debugPrintf("Setting up the temperature.\r\n");
  tempToSet = temp;

  if (stateRunning) {
    debugPrintf("Changing temperature to: %d\r\n", tempToSet);
    dial(PARAM_TEMP, tempToSet);
    return ack;
  }

  debugPrintf("Temperature to set: %d\r\n", tempToSet);
  return ack;
}

/**
 * @brief Set up target time and apply immediately if already running.
 *
 * @param timeStr time string, will be parsed to integer.
 */
String setTime(const String& timeStr) {
  if (timeStr.isEmpty())
    return ackErr + ":Time is required.";
  long parsedTime = 0;
  for (size_t i = 0; i < timeStr.length(); i++) {
    if (!isDigit(timeStr[i]))
      return ackErr + ":Time must be a whole number.";
    parsedTime = parsedTime * 10 + (timeStr[i] - '0');
    if (parsedTime > maxTime)
      return ackErr + ":Allowed values range from 0 to " + String(maxTime) + ".";
  }
  const uint8_t time = static_cast<uint8_t>(parsedTime);

  debugPrintf("Setting up the time.\r\n");
  timeToSet = time;

  debugPrintf("Time to set: %dh\r\n", timeToSet);

  if (stateRunning) {
    debugPrintf("Changing time to: %dh\r\n", tempToSet);
    dial(PARAM_TIME, timeToSet);
  }

  return ack;
}

/**
 * @brief Set the state. Start or stop the job.
 *
 * @param stateStr state string, either "start" or "stop".
 */
String setStatus(const String& stateStr) {
  if (stateStr.equalsIgnoreCase("start")) {
    if (!timeToSet) {
      return ackErr + ":Cannot start, time not set.";
    }

    dial(PARAM_TEMP, tempToSet);
    dial(PARAM_TIME, timeToSet);

    timer.start(timeToSet, resetStatus);

    stateRunning = true;

    debugPrintf("Started!\r\n");
    return ack;
  }
  if (stateStr.equalsIgnoreCase("stop")) {
    dial(PARAM_TIME, 0);

    timer.stop();

    stateRunning = false;

    debugPrintf("Stopped!\r\n");
    return ack;
  }

  return ackErr + ":\"" + stateStr + "\" is not a valid state.";
}

/**
 * @brief A callback function for timer, reset state do "stopped".
 */
void resetStatus() {
  stateRunning = false;
}

/**
 * @brief Sets up target values by emulating encoder movement. Dials in the settings.
 *
 * @param param parameter to set, either temperature or time.
 * @param value value to set to.
 */
void dial(Param param, uint8_t value) {
  uint8_t settingSteps;
  uint8_t minValue, maxValue;

  switch (param) {
    case PARAM_TEMP:
      settingSteps = 1;
      minValue = minTemp;
      maxValue = maxTemp;
      debugPrintf("Dialing in temperature.\r\n");
      break;
    case PARAM_TIME:
      settingSteps = 2;
      minValue = 0;
      maxValue = maxTime;
      debugPrintf("Dialing in time.\r\n");
      break;
    default:
      return;
  }

  debugPrintf("Value to dial in: %d\r\n", value);

  // Select which param to set
  for (int i = 0; i < settingSteps; i++) {
    rotateLeft();
    delay(1);
  }

  // Confirm
  pressButton();
  delay(1);

  // Reset to minimal value
  for (int i = 0; i < maxValue - minValue; i++) {
    rotateRight();
    delay(1);
  }

  // Dial new value
  for (int i = minValue; i < value; i++) {
    rotateLeft();
    delay(1);
  }

  // Confirm settings
  pressButton();
  delay(1);
}

/**
 * @brief Emulates encoder single step to the right.
 */
void rotateRight() {
  pinMode(pinA, OUTPUT);
  pinMode(pinB, OUTPUT);
  delay(1);

  setPins(LOW, LOW);
  delayMicroseconds(500);
  setPins(HIGH, LOW);
  delayMicroseconds(500);
  setPins(HIGH, HIGH);
  delay(1);

  pinMode(pinA, INPUT);
  pinMode(pinB, INPUT);
}

/**
 * @brief Emulates encoder single step to the left.
 */
void rotateLeft() {
  pinMode(pinA, OUTPUT);
  pinMode(pinB, OUTPUT);
  delay(1);

  setPins(LOW, LOW);
  delayMicroseconds(500);
  setPins(LOW, HIGH);
  delayMicroseconds(500);
  setPins(HIGH, HIGH);
  delay(1);

  pinMode(pinA, INPUT);
  pinMode(pinB, INPUT);
}

/**
 * @brief Emulates encoder button press.
 */
void pressButton() {
  pinMode(pinSW, OUTPUT);
  digitalWrite(pinSW, HIGH);
  delay(50);

  pinMode(pinSW, INPUT);
}

/**
 * @brief Set the encoder pins direction.
 *
 * @param a encoder pin A.
 * @param b encoder pin B.
 */
void setPins(int a, int b) {
  digitalWrite(pinA, a);
  digitalWrite(pinB, b);
}
