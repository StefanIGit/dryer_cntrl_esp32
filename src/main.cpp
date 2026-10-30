#include <math.h>
#include <stdarg.h>
#include <time.h>

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "HWTimer.h"
#include "help.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#endif

#ifndef DRYER_WIFI_SETUP_PASSWORD
  #define DRYER_WIFI_SETUP_PASSWORD "dryer_cntrl_esp32"
#endif

constexpr bool kDebugEnabled = false;
constexpr char kFirmwareVersion[] = "0.5.0";
constexpr uint32_t kSerialBaudRate = 115200;
constexpr uint16_t kWebServerPort = 80;
constexpr char kWifiSetupSsid[] = "DryerController-Setup";
constexpr char kOtaHostname[] = "dryer-controller";
constexpr char kWifiSetupPassword[] = DRYER_WIFI_SETUP_PASSWORD;
static_assert(sizeof(kWifiSetupPassword) >= 9, "Wi-Fi setup password must be at least 8 characters");
constexpr char kCredentialsNamespace[] = "dryer-auth";
constexpr char kCredentialsKey[] = "credentials";
constexpr char kAuthEnabledKey[] = "authEnabled";
constexpr char kRecoveryNamespace[] = "recovery";
constexpr char kPowerCycleCountKey[] = "powerCycles";
constexpr uint8_t kFactoryResetPowerCycleCount = 10;
constexpr uint32_t kFactoryResetWindowMs = 60000;
constexpr time_t kMinimumTlsVerificationEpoch = 1704067200;
constexpr size_t kMaxUsernameLength = 32;
constexpr size_t kMinPasswordLength = 8;
constexpr size_t kMaxPasswordLength = 64;
constexpr size_t kDebugBufferSize = 255;
constexpr uint16_t kStartupDelayMs = 1000;
constexpr uint32_t kTlsClockSyncTimeoutMs = 10000;
constexpr uint8_t kEncoderTransitionDelayMs = 1;
constexpr uint16_t kEncoderPhaseDelayUs = 500;
constexpr uint8_t kEncoderButtonPressMs = 50;

constexpr uint8_t kTemperaturePin = 35;
constexpr uint8_t kEncoderPinA = 27;
constexpr uint8_t kEncoderPinB = 25;
constexpr uint8_t kEncoderPinSwitch = 32;

constexpr float kThermistorPullupOhms = 10000.0f;
constexpr float kThermistorResistanceAt25C = 10000.0f;
constexpr float kThermistorBeta = 3950.0f;
constexpr float kThermistorSupplyMillivolts = 3300.0f;
constexpr float kThermistorReferenceKelvin = 298.15f;
constexpr float kKelvinOffset = 273.15f;

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
const String kAckError = "ERR";
const String kAck = "OK";

/* Max values */
constexpr uint8_t kMinTemperature = 45; // Degrees Celsius
constexpr uint8_t kMaxTemperature = 65; // Degrees Celsius
constexpr uint8_t kMaxTimeHours = 24; // Hours

/* Global params */
static bool stateRunning = false;
static uint8_t tempToSet = 0;
static uint8_t timeToSet = 0;
static String httpUsername;
static String httpPassword;
static bool httpAuthEnabled = true;
static bool powerCycleResetPending = false;
static bool otaStarted = false;
static bool firmwareUploadStarted = false;
static bool firmwareUploadAuthorized = false;
static bool firmwareUploadFailed = false;
static bool firmwareUploadComplete = false;
static String firmwareUploadError;

/* Timer */
HWTimer timer;
WebServer webServer(kWebServerPort);

class DefaultBundleWiFiClientSecure : public WiFiClientSecure {
public:
  void useDefaultCACertBundle() {
    _use_ca_bundle = true;
  }
};

void debugPrintf(const char* fmt, ...);
void handleStatusPage();
void handleStatusApi();
void handleCommandApi();
void handleCredentialsApi();
void handleAuthApi();
void handleFirmwareUploadComplete();
void handleFirmwareUpload();
void handleFirmwareUrlUpdate();
void startWifiAndWebServer();
void startOtaUpdates();
bool authenticateHttpRequest();
bool authenticateFirmwareRequest();
bool downloadFirmwareFromUrl(const String& url, String& error);
void sendCredentialSetupPage();
bool saveHttpCredentials(const String& username, const String& password);
bool saveHttpAuthEnabled(bool enabled);
bool recordPowerCycleAndResetIfNeeded(WiFiManager& wifiManager);
void clearPowerCycleCountAfterWindow();
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
void setPins(uint8_t a, uint8_t b);

float analogReadTemp() {
  const uint32_t voltageMillivolts = analogReadMilliVolts(kTemperaturePin);
  if (voltageMillivolts == 0 || voltageMillivolts >= kThermistorSupplyMillivolts)
    return NAN;

  const float thermistorResistance = kThermistorPullupOhms * voltageMillivolts
      / (kThermistorSupplyMillivolts - voltageMillivolts);
  const float inverseTemperatureKelvin = 1.0f / kThermistorReferenceKelvin
      + logf(thermistorResistance / kThermistorResistanceAt25C) / kThermistorBeta;
  return 1.0f / inverseTemperatureKelvin - kKelvinOffset;
}

/**
 * @brief Arduino setup() function. Runs one.
 */
void setup() {
  pinMode(kEncoderPinA, INPUT);
  pinMode(kEncoderPinB, INPUT);
  pinMode(kEncoderPinSwitch, INPUT);

  Serial.begin(kSerialBaudRate);
  delay(kStartupDelayMs);

  analogReadResolution(12);
  startWifiAndWebServer();
  printHelp();
  Serial.println(kAck);
}

/**
 * @brief Arduino loop() function. Runs constantly.
 */
void loop() {
  static String input;

  timer.update();
  webServer.handleClient();
  if (otaStarted)
    ArduinoOTA.handle();
  clearPowerCycleCountAfterWindow();

  if (!Serial.available())
    return;

  const char ch = Serial.read();
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
      if (!input.isEmpty())
        input.remove(input.length() - 1);
      return;
  }

  input += ch;
}

void startWifiAndWebServer() {
  WiFiManager wifiManager;
  if (recordPowerCycleAndResetIfNeeded(wifiManager))
    return;

  if (!wifiManager.autoConnect(kWifiSetupSsid, kWifiSetupPassword)) {
    Serial.println("ERR: Wi-Fi setup failed; restarting.");
    delay(1000);
    ESP.restart();
    return;
  }

  Preferences preferences;
  if (preferences.begin(kCredentialsNamespace, false)) {
    const String storedCredentials = preferences.getString(kCredentialsKey, "");
    httpAuthEnabled = preferences.getBool(kAuthEnabledKey, false);
    preferences.end();
    const int separator = storedCredentials.indexOf('\n');
    if (separator > 0) {
      httpUsername = storedCredentials.substring(0, separator);
      httpPassword = storedCredentials.substring(separator + 1);
    } else {
      Serial.println("No HTTP login configured; create one on the web page.");
    }
  } else {
    Serial.println("ERR: Could not open HTTP credential storage; web login setup may fail.");
  }

  webServer.on("/", HTTP_GET, handleStatusPage);
  webServer.on("/api/status", HTTP_GET, handleStatusApi);
  webServer.on("/api/command", HTTP_POST, handleCommandApi);
  webServer.on("/api/credentials", HTTP_POST, handleCredentialsApi);
  webServer.on("/api/auth", HTTP_POST, handleAuthApi);
  webServer.on("/api/firmware/upload", HTTP_POST, handleFirmwareUploadComplete,
               handleFirmwareUpload);
  webServer.on("/api/firmware/url", HTTP_POST, handleFirmwareUrlUpdate);
  webServer.begin();
  startOtaUpdates();

  Serial.printf("Wi-Fi connected. Status page: http://%s/\r\n",
                WiFi.localIP().toString().c_str());
}

void startOtaUpdates() {
  if (httpUsername.isEmpty() || httpPassword.isEmpty()) {
    Serial.println("OTA disabled until an HTTP login is configured.");
    return;
  }

  ArduinoOTA.setHostname(kOtaHostname);
  ArduinoOTA.setPassword(httpPassword.c_str());
  ArduinoOTA.onStart([]() {
    Serial.println("OTA update started.");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\r\nOTA update complete.");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA update failed (error %u).\r\n", error);
  });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.printf("OTA ready at %s.local; password is the configured HTTP password.\r\n",
                kOtaHostname);
}

bool recordPowerCycleAndResetIfNeeded(WiFiManager& wifiManager) {
  Preferences recovery;
  if (!recovery.begin(kRecoveryNamespace, false)) {
    Serial.println("ERR: Could not open power-cycle recovery storage.");
    return false;
  }

  uint8_t powerCycles = recovery.getUChar(kPowerCycleCountKey, 0);
  if (powerCycles < kFactoryResetPowerCycleCount)
    ++powerCycles;
  if (!recovery.putUChar(kPowerCycleCountKey, powerCycles)) {
    Serial.println("ERR: Could not save power-cycle count.");
    recovery.end();
    return false;
  }
  recovery.end();
  powerCycleResetPending = true;

  Serial.printf("Power cycle recovery count: %u/%u\r\n",
                powerCycles, kFactoryResetPowerCycleCount);
  if (powerCycles < kFactoryResetPowerCycleCount)
    return false;

  Preferences credentials;
  if (!credentials.begin(kCredentialsNamespace, false)) {
    Serial.println("ERR: Factory reset could not clear HTTP credentials.");
    return false;
  }
  const bool credentialsCleared = credentials.clear();
  credentials.end();
  if (!credentialsCleared) {
    Serial.println("ERR: Factory reset could not clear HTTP credentials.");
    return false;
  }

  if (!recovery.begin(kRecoveryNamespace, false)) {
    Serial.println("ERR: Factory reset could not clear its power-cycle count.");
    return false;
  }
  const bool countCleared = recovery.putUChar(kPowerCycleCountKey, 0);
  recovery.end();
  if (!countCleared) {
    Serial.println("ERR: Factory reset could not clear its power-cycle count.");
    return false;
  }

  Serial.println("Factory reset: clearing saved Wi-Fi and HTTP settings.");
  wifiManager.resetSettings();
  delay(kStartupDelayMs);
  ESP.restart();
  return true;
}

void clearPowerCycleCountAfterWindow() {
  if (!powerCycleResetPending || millis() < kFactoryResetWindowMs)
    return;

  Preferences recovery;
  if (!recovery.begin(kRecoveryNamespace, false)) {
    Serial.println("ERR: Could not clear power-cycle recovery count.");
    return;
  }
  const bool cleared = recovery.putUChar(kPowerCycleCountKey, 0);
  recovery.end();
  if (!cleared) {
    Serial.println("ERR: Could not clear power-cycle recovery count.");
    return;
  }

  powerCycleResetPending = false;
}

void handleStatusPage() {
  if (httpAuthEnabled && (httpUsername.isEmpty() || httpPassword.isEmpty())) {
    sendCredentialSetupPage();
    return;
  }

  if (!authenticateHttpRequest())
    return;

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
  input{font:inherit;padding:.45rem;color:var(--text);background:var(--card);border:1px solid var(--border);border-radius:6px}
  input[type="checkbox"]{width:auto}
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
    <details>
      <summary>HTTP login settings</summary>
      <form id="credentialsForm">
        <label>New username
          <input id="usernameInput" name="username" maxlength="32" autocomplete="username" required>
        </label>
        <label>New password (at least 8 characters)
          <input id="passwordInput" name="password" type="password" minlength="8" maxlength="64"
                 autocomplete="new-password" required>
        </label>
        <label><input id="showPassword" type="checkbox"> Show password</label>
        <button type="submit">Save login</button>
      </form>
      <p>Saving new credentials will require signing in again.</p>
      <p>HTTP Basic Auth: <strong id="authState">Loading…</strong></p>
      <button type="button" id="authToggleButton">Loading…</button>
    </details>
    <details>
      <summary>Firmware updates</summary>
      <p>Current firmware version: <strong id="firmwareVersion">Loading…</strong></p>
      <p>Updates require the configured HTTP username and password, even if dashboard Basic Auth is disabled. Use a firmware .bin built for this device.</p>
      <form id="firmwareUploadForm">
        <label>Upload firmware file
          <input id="firmwareFile" type="file" accept=".bin,application/octet-stream" required>
        </label>
        <button type="submit">Upload and install</button>
      </form>
      <form id="firmwareUrlForm">
        <label>Or install from an HTTPS URL
          <input id="firmwareUrl" type="url" maxlength="512" placeholder="https://example.com/firmware.bin" required>
        </label>
        <button type="submit">Download and install</button>
      </form>
      <progress id="firmwareProgress" max="100" value="0" hidden></progress>
      <p id="firmwareResult" role="status"></p>
    </details>
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

    document.getElementById('showPassword').addEventListener('change', event => {
      document.getElementById('passwordInput').type = event.currentTarget.checked ? 'text' : 'password';
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

    document.getElementById('credentialsForm').addEventListener('submit', async event => {
      event.preventDefault();
      const result = document.getElementById('result');
      const body = new URLSearchParams({
        username: document.getElementById('usernameInput').value,
        password: document.getElementById('passwordInput').value
      });
      try {
        const response = await fetch('/api/credentials', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body
        });
        result.textContent = await response.text();
        if (response.ok)
          result.textContent += ' Reload the page and sign in with the new credentials.';
      } catch (error) {
        result.textContent = 'Could not save credentials; check the connection.';
      }
    });

    document.getElementById('firmwareUploadForm').addEventListener('submit', event => {
      event.preventDefault();
      const file = document.getElementById('firmwareFile').files[0];
      const result = document.getElementById('firmwareResult');
      const progress = document.getElementById('firmwareProgress');
      if (!file) {
        result.textContent = 'Choose a firmware .bin file first.';
        return;
      }

      const formData = new FormData();
      formData.append('firmware', file);
      progress.hidden = false;
      progress.value = 0;
      result.textContent = 'Uploading firmware…';
      const request = new XMLHttpRequest();
      request.open('POST', '/api/firmware/upload');
      request.upload.addEventListener('progress', uploadEvent => {
        if (uploadEvent.lengthComputable)
          progress.value = Math.round(uploadEvent.loaded * 100 / uploadEvent.total);
      });
      request.addEventListener('load', () => {
        result.textContent = request.responseText || 'Firmware upload failed.';
        if (request.status >= 200 && request.status < 300)
          setTimeout(() => location.reload(), 5000);
      });
      request.addEventListener('error', () => {
        result.textContent = 'Firmware upload failed; check the connection.';
      });
      request.send(formData);
    });

    document.getElementById('firmwareUrlForm').addEventListener('submit', async event => {
      event.preventDefault();
      const result = document.getElementById('firmwareResult');
      result.textContent = 'Downloading and installing firmware…';
      try {
        const response = await fetch('/api/firmware/url', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: new URLSearchParams({url: document.getElementById('firmwareUrl').value})
        });
        result.textContent = await response.text();
        if (response.ok)
          setTimeout(() => location.reload(), 5000);
      } catch (error) {
        result.textContent = 'Firmware download failed; check the connection.';
      }
    });

    function updateAuthControl(enabled, credentialsConfigured) {
      const button = document.getElementById('authToggleButton');
      button.dataset.enabled = String(enabled);
      button.dataset.credentialsConfigured = String(credentialsConfigured);
      button.textContent = enabled ? 'Disable HTTP Basic Auth'
        : credentialsConfigured ? 'Enable HTTP Basic Auth' : 'Set up login to enable Basic Auth';
      document.getElementById('authState').textContent = enabled ? 'Enabled' : 'Disabled';
    }

    document.getElementById('authToggleButton').addEventListener('click', async () => {
      const button = document.getElementById('authToggleButton');
      if (button.dataset.credentialsConfigured !== 'true') {
        const settings = document.querySelector('details');
        settings.open = true;
        document.getElementById('usernameInput').focus();
        document.getElementById('result').textContent =
          'Set a username and password before enabling HTTP Basic Auth.';
        return;
      }
      const enable = button.dataset.enabled !== 'true';
      try {
        const response = await fetch('/api/auth', {
          method: 'POST',
          headers: {'Content-Type': 'text/plain'},
          body: enable ? 'enabled' : 'disabled'
        });
        const message = await response.text();
        document.getElementById('result').textContent = message;
        if (response.ok) {
          updateAuthControl(enable);
          if (enable)
            document.getElementById('result').textContent += ' Further requests will require login.';
        }
      } catch (error) {
        document.getElementById('result').textContent = 'Could not change authentication setting.';
      }
    });

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
        document.getElementById('firmwareVersion').textContent = status.firmwareVersion;
        updateAuthControl(status.httpAuthEnabled, status.httpCredentialsConfigured);
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

void sendCredentialSetupPage() {
  static const char page[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Set dryer login</title>
  <style>
  :root{color-scheme:dark;--page:#111820;--card:#1c2732;--text:#edf3f8;--muted:#b3c0cc;--border:#536273;--accent:#79baff}
  body{font:16px system-ui,sans-serif;max-width:32rem;margin:3rem auto;padding:0 1rem;background:var(--page);color:var(--text)}
  main{background:var(--card);border-radius:12px;padding:1.5rem}
  label{display:block;margin:1rem 0;color:var(--muted)}
  input,button{font:inherit;padding:.55rem;color:var(--text);background:var(--card);border:1px solid var(--border);border-radius:6px}
  input{display:block;width:calc(100% - 1.2rem);margin-top:.35rem}
  input[type="checkbox"]{display:inline;width:auto;margin-right:.4rem}
  button{cursor:pointer}button:hover{border-color:var(--accent)}
  :focus-visible{outline:2px solid var(--accent);outline-offset:2px}
  </style>
</head>
<body>
  <main>
    <h1>Set dryer login</h1>
    <p>Create a username and password to protect the dashboard and control API.</p>
    <form id="setupForm">
      <label>Username (1–32 printable characters, no spaces or colons)
        <input name="username" maxlength="32" autocomplete="username" required>
      </label>
      <label>Password (8–64 characters)
        <input name="password" type="password" minlength="8" maxlength="64"
               autocomplete="new-password" required>
      </label>
      <label><input id="showSetupPassword" type="checkbox"> Show password</label>
      <button type="submit">Save login</button>
    </form>
    <p id="result" role="status"></p>
  </main>
  <script>
    document.getElementById('showSetupPassword').addEventListener('change', event => {
      document.querySelector('input[name="password"]').type =
        event.currentTarget.checked ? 'text' : 'password';
    });

    document.getElementById('setupForm').addEventListener('submit', async event => {
      event.preventDefault();
      const result = document.getElementById('result');
      const body = new URLSearchParams(new FormData(event.currentTarget));
      try {
        const response = await fetch('/api/credentials', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body
        });
        result.textContent = await response.text();
        if (response.ok) {
          result.textContent += ' Reloading; sign in with the credentials you just created.';
          setTimeout(() => location.reload(), 1500);
        }
      } catch (error) {
        result.textContent = 'Could not save credentials; check the connection.';
      }
    });
  </script>
</body>
</html>
)html";
  webServer.send(200, "text/html; charset=utf-8", page);
}

void handleStatusApi() {
  if (!authenticateHttpRequest())
    return;

  String json;
  json.reserve(256);
  json = "{\"state\":\"";
  json += stateRunning ? "running" : "stopped";
  json += "\",\"firmwareVersion\":\"";
  json += kFirmwareVersion;
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
  json += "\",\"httpAuthEnabled\":";
  json += httpAuthEnabled ? "true" : "false";
  json += ",\"httpCredentialsConfigured\":";
  json += !httpUsername.isEmpty() && !httpPassword.isEmpty() ? "true" : "false";
  json += "}";

  webServer.send(200, "application/json; charset=utf-8", json);
}

void handleCommandApi() {
  if (!authenticateHttpRequest())
    return;

  String command = webServer.arg("plain");
  command.trim();
  if (command.isEmpty() || command.length() > 64) {
    webServer.send(400, "text/plain; charset=utf-8", "ERR:Command must be 1-64 characters.");
    return;
  }

  const String response = inputHandler(command);
  const bool failed = response.startsWith(kAckError);
  webServer.send(failed ? 400 : 200, "text/plain; charset=utf-8", response);
}

void handleCredentialsApi() {
  const bool isSetup = httpUsername.isEmpty() || httpPassword.isEmpty();
  if (!isSetup && !authenticateHttpRequest())
    return;

  const String username = webServer.arg("username");
  const String password = webServer.arg("password");
  bool validUsername = !username.isEmpty() && username.length() <= kMaxUsernameLength;
  for (size_t i = 0; validUsername && i < username.length(); i++) {
    validUsername = username[i] >= 0x21 && username[i] <= 0x7e && username[i] != ':';
  }
  if (!validUsername) {
    webServer.send(400, "text/plain; charset=utf-8",
                   "Username must be 1-32 printable ASCII characters without spaces or colons.");
    return;
  }
  if (password.length() < kMinPasswordLength || password.length() > kMaxPasswordLength) {
    webServer.send(400, "text/plain; charset=utf-8", "Password must be 8-64 characters.");
    return;
  }
  for (size_t i = 0; i < password.length(); i++) {
    if (password[i] < 0x20 || password[i] > 0x7e) {
      webServer.send(400, "text/plain; charset=utf-8", "Password must use printable ASCII characters.");
      return;
    }
  }

  if (!saveHttpCredentials(username, password)) {
    webServer.send(500, "text/plain; charset=utf-8", "Could not save credentials to NVS.");
    return;
  }

  webServer.send(200, "text/plain; charset=utf-8", "Credentials saved.");
}

void handleAuthApi() {
  if (!authenticateHttpRequest())
    return;

  String setting = webServer.arg("plain");
  setting.trim();
  if (setting != "enabled" && setting != "disabled") {
    webServer.send(400, "text/plain; charset=utf-8", "Expected 'enabled' or 'disabled'.");
    return;
  }

  const bool enabled = setting == "enabled";
  if (enabled && (httpUsername.isEmpty() || httpPassword.isEmpty())) {
    webServer.send(400, "text/plain; charset=utf-8", "Set an HTTP username and password first.");
    return;
  }
  if (!saveHttpAuthEnabled(enabled)) {
    webServer.send(500, "text/plain; charset=utf-8", "Could not save auth setting to NVS.");
    return;
  }

  webServer.send(200, "text/plain; charset=utf-8",
                 enabled ? "HTTP Basic Auth enabled." : "HTTP Basic Auth disabled.");
}

void handleFirmwareUpload() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    firmwareUploadStarted = true;
    firmwareUploadAuthorized = authenticateFirmwareRequest();
    firmwareUploadFailed = !firmwareUploadAuthorized;
    firmwareUploadComplete = false;
    firmwareUploadError = "";
    if (!firmwareUploadAuthorized)
      return;

    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".bin")) {
      firmwareUploadFailed = true;
      firmwareUploadError = "Choose a firmware .bin file.";
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      firmwareUploadFailed = true;
      firmwareUploadError = Update.errorString();
    }
    return;
  }

  if (!firmwareUploadAuthorized)
    return;

  if (upload.status == UPLOAD_FILE_WRITE && !firmwareUploadFailed) {
    const size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      firmwareUploadFailed = true;
      firmwareUploadError = Update.errorString();
      if (firmwareUploadError.isEmpty())
        firmwareUploadError = "Could not write the firmware image.";
      Update.abort();
    }
  } else if (upload.status == UPLOAD_FILE_END && !firmwareUploadFailed) {
    firmwareUploadComplete = Update.end(true);
    if (!firmwareUploadComplete) {
      firmwareUploadFailed = true;
      firmwareUploadError = Update.errorString();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    firmwareUploadFailed = true;
    firmwareUploadError = "Firmware upload was interrupted.";
  }
}

void handleFirmwareUploadComplete() {
  if (!firmwareUploadStarted) {
    if (!authenticateFirmwareRequest())
      return;
    webServer.send(400, "text/plain; charset=utf-8", "Choose a firmware .bin file.");
    return;
  }
  if (!firmwareUploadAuthorized) {
    firmwareUploadStarted = false;
    return;
  }
  if (firmwareUploadFailed || !firmwareUploadComplete) {
    const String message = firmwareUploadError.isEmpty()
        ? "Firmware upload failed."
        : firmwareUploadError;
    webServer.send(400, "text/plain; charset=utf-8", message);
    firmwareUploadStarted = false;
    firmwareUploadAuthorized = false;
    firmwareUploadFailed = false;
    firmwareUploadComplete = false;
    return;
  }

  webServer.send(200, "text/plain; charset=utf-8",
                 "Firmware uploaded successfully. The device is restarting.");
  firmwareUploadStarted = false;
  delay(1000);
  ESP.restart();
}

void handleFirmwareUrlUpdate() {
  if (!authenticateFirmwareRequest())
    return;

  String url = webServer.arg("url");
  url.trim();
  if (url.length() > 512 || !url.startsWith("https://")) {
    webServer.send(400, "text/plain; charset=utf-8",
                   "Enter an HTTPS firmware URL no longer than 512 characters.");
    return;
  }

  String error;
  if (!downloadFirmwareFromUrl(url, error)) {
    webServer.send(400, "text/plain; charset=utf-8", error);
    return;
  }

  webServer.send(200, "text/plain; charset=utf-8",
                 "Firmware downloaded successfully. The device is restarting.");
  delay(1000);
  ESP.restart();
}

bool downloadFirmwareFromUrl(const String& url, String& error) {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  const uint32_t syncStartedAt = millis();
  while (time(nullptr) < kMinimumTlsVerificationEpoch
         && millis() - syncStartedAt < kTlsClockSyncTimeoutMs)
    delay(250);
  if (time(nullptr) < kMinimumTlsVerificationEpoch) {
    error = "Could not synchronize the clock for HTTPS certificate verification.";
    return false;
  }

  DefaultBundleWiFiClientSecure client;
  client.useDefaultCACertBundle();

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    error = "Could not start the HTTPS firmware download.";
    return false;
  }

  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    error = "Firmware download failed with HTTP status " + String(responseCode) + ".";
    http.end();
    return false;
  }

  const int firmwareSize = http.getSize();
  if (firmwareSize <= 0) {
    error = "The firmware server must provide a content length.";
    http.end();
    return false;
  }
  if (!Update.begin(static_cast<size_t>(firmwareSize))) {
    error = String("Could not start firmware update: ") + Update.errorString();
    http.end();
    return false;
  }

  const size_t written = Update.writeStream(http.getStream());
  if (written != static_cast<size_t>(firmwareSize)) {
    error = String("Firmware download was incomplete: ") + Update.errorString();
    Update.abort();
    http.end();
    return false;
  }

  const bool updated = Update.end();
  if (!updated) {
    error = String("Firmware image was rejected: ") + Update.errorString();
    Update.abort();
  }
  http.end();
  return updated;
}

bool saveHttpCredentials(const String& username, const String& password) {
  Preferences preferences;
  if (!preferences.begin(kCredentialsNamespace, false))
    return false;

  String credentials;
  credentials.reserve(username.length() + password.length() + 1);
  credentials = username;
  credentials += '\n';
  credentials += password;
  const size_t bytesWritten = preferences.putString(kCredentialsKey, credentials);
  const bool saved = bytesWritten > 0
      && preferences.getString(kCredentialsKey, "") == credentials;
  preferences.end();
  if (saved) {
    httpUsername = username;
    httpPassword = password;
    if (otaStarted)
      ArduinoOTA.setPassword(httpPassword.c_str());
    else
      startOtaUpdates();
  }
  return saved;
}

bool saveHttpAuthEnabled(bool enabled) {
  Preferences preferences;
  if (!preferences.begin(kCredentialsNamespace, false))
    return false;

  const bool saved = preferences.putBool(kAuthEnabledKey, enabled);
  preferences.end();
  if (saved)
    httpAuthEnabled = enabled;
  return saved;
}

bool authenticateHttpRequest() {
  if (!httpAuthEnabled)
    return true;
  if (httpUsername.isEmpty() || httpPassword.isEmpty()) {
    webServer.send(503, "text/plain; charset=utf-8", "Create HTTP credentials at / before using the API.");
    return false;
  }
  if (webServer.authenticate(httpUsername.c_str(), httpPassword.c_str()))
    return true;

  webServer.requestAuthentication(BASIC_AUTH, "Dryer Controller");
  return false;
}

bool authenticateFirmwareRequest() {
  if (httpUsername.isEmpty() || httpPassword.isEmpty()) {
    webServer.send(503, "text/plain; charset=utf-8",
                   "Configure an HTTP username and password before installing firmware.");
    return false;
  }
  if (webServer.authenticate(httpUsername.c_str(), httpPassword.c_str()))
    return true;

  webServer.requestAuthentication(BASIC_AUTH, "Firmware Update");
  return false;
}

/**
 * @brief Print message on Serial only when debugging is enabled.
 *
 * @param fmt message.
 * @param ... additional parameters.
 */
void debugPrintf(const char* fmt, ...) {
  if (kDebugEnabled) {
    char buffer[kDebugBufferSize];
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
        return kAckError + ":\"" + input + "\" is not a valid mode.";
      }
      return kAck;
    case MODE_HELP:
      printHelp();
      return kAck;
  }
  return kAckError + ":Invalid command.";
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
          return kAck + ":" + String(temperature, 1);
        return kAckError + ":Temperature sensor reading is out of range.";
      }
    case PARAM_TIME:
      return kAck + ":" + timer.getTimeLeft();
    case PARAM_STATE:
      return kAck + ":" + (stateRunning ? "running" : "stopped");
    case PARAM_UNKNOWN:
      return kAckError + ":\"" + paramStr + "\" is not a valid param.";
  }
  return kAckError + ":Invalid parameter.";
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
      return kAckError + ":\"" + paramStr + "\" is not a valid param.";
  }
  return kAckError + ":Invalid parameter.";
}

/**
 * @brief Set up target temperature and apply immediately if already running.
 *
 * @param tempStr temperature string, will be parsed to integer.
 */
String setTemp(const String& tempStr) {
  if (tempStr.isEmpty())
    return kAckError + ":Temperature is required.";
  long parsedTemp = 0;
  for (size_t i = 0; i < tempStr.length(); i++) {
    if (!isDigit(tempStr[i]))
      return kAckError + ":Temperature must be a whole number.";
    parsedTemp = parsedTemp * 10 + (tempStr[i] - '0');
    if (parsedTemp > kMaxTemperature)
      return kAckError + ":Allowed values range from " + String(kMinTemperature) + " to " + String(kMaxTemperature) + ".";
  }
  if (parsedTemp < kMinTemperature || parsedTemp > kMaxTemperature)
    return kAckError + ":Allowed values range from " + String(kMinTemperature) + " to " + String(kMaxTemperature) + ".";
  const uint8_t temp = static_cast<uint8_t>(parsedTemp);

  debugPrintf("Setting up the temperature.\r\n");
  tempToSet = temp;

  if (stateRunning) {
    debugPrintf("Changing temperature to: %d\r\n", tempToSet);
    dial(PARAM_TEMP, tempToSet);
    return kAck;
  }

  debugPrintf("Temperature to set: %d\r\n", tempToSet);
  return kAck;
}

/**
 * @brief Set up target time and apply immediately if already running.
 *
 * @param timeStr time string, will be parsed to integer.
 */
String setTime(const String& timeStr) {
  if (timeStr.isEmpty())
    return kAckError + ":Time is required.";
  long parsedTime = 0;
  for (size_t i = 0; i < timeStr.length(); i++) {
    if (!isDigit(timeStr[i]))
      return kAckError + ":Time must be a whole number.";
    parsedTime = parsedTime * 10 + (timeStr[i] - '0');
    if (parsedTime > kMaxTimeHours)
      return kAckError + ":Allowed values range from 0 to " + String(kMaxTimeHours) + ".";
  }
  const uint8_t time = static_cast<uint8_t>(parsedTime);

  debugPrintf("Setting up the time.\r\n");
  timeToSet = time;

  debugPrintf("Time to set: %dh\r\n", timeToSet);

  if (stateRunning) {
    debugPrintf("Changing time to: %dh\r\n", timeToSet);
    dial(PARAM_TIME, timeToSet);
  }

  return kAck;
}

/**
 * @brief Set the state. Start or stop the job.
 *
 * @param stateStr state string, either "start" or "stop".
 */
String setStatus(const String& stateStr) {
  if (stateStr.equalsIgnoreCase("start")) {
    if (!timeToSet) {
      return kAckError + ":Cannot start, time not set.";
    }

    dial(PARAM_TEMP, tempToSet);
    dial(PARAM_TIME, timeToSet);

    timer.start(timeToSet, resetStatus);

    stateRunning = true;

    debugPrintf("Started!\r\n");
    return kAck;
  }
  if (stateStr.equalsIgnoreCase("stop")) {
    dial(PARAM_TIME, 0);

    timer.stop();

    stateRunning = false;

    debugPrintf("Stopped!\r\n");
    return kAck;
  }

  return kAckError + ":\"" + stateStr + "\" is not a valid state.";
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
      minValue = kMinTemperature;
      maxValue = kMaxTemperature;
      debugPrintf("Dialing in temperature.\r\n");
      break;
    case PARAM_TIME:
      settingSteps = 2;
      minValue = 0;
      maxValue = kMaxTimeHours;
      debugPrintf("Dialing in time.\r\n");
      break;
    default:
      return;
  }

  debugPrintf("Value to dial in: %d\r\n", value);

  // Select which param to set
  for (int i = 0; i < settingSteps; i++) {
    rotateLeft();
    delay(kEncoderTransitionDelayMs);
  }

  // Confirm
  pressButton();
  delay(kEncoderTransitionDelayMs);

  // Reset to minimal value
  for (int i = 0; i < maxValue - minValue; i++) {
    rotateRight();
    delay(kEncoderTransitionDelayMs);
  }

  // Dial new value
  for (int i = minValue; i < value; i++) {
    rotateLeft();
    delay(kEncoderTransitionDelayMs);
  }

  // Confirm settings
  pressButton();
  delay(kEncoderTransitionDelayMs);
}

/**
 * @brief Emulates encoder single step to the right.
 */
void rotateRight() {
  pinMode(kEncoderPinA, OUTPUT);
  pinMode(kEncoderPinB, OUTPUT);
  delay(kEncoderTransitionDelayMs);

  setPins(LOW, LOW);
  delayMicroseconds(kEncoderPhaseDelayUs);
  setPins(HIGH, LOW);
  delayMicroseconds(kEncoderPhaseDelayUs);
  setPins(HIGH, HIGH);
  delay(kEncoderTransitionDelayMs);

  pinMode(kEncoderPinA, INPUT);
  pinMode(kEncoderPinB, INPUT);
}

/**
 * @brief Emulates encoder single step to the left.
 */
void rotateLeft() {
  pinMode(kEncoderPinA, OUTPUT);
  pinMode(kEncoderPinB, OUTPUT);
  delay(kEncoderTransitionDelayMs);

  setPins(LOW, LOW);
  delayMicroseconds(kEncoderPhaseDelayUs);
  setPins(LOW, HIGH);
  delayMicroseconds(kEncoderPhaseDelayUs);
  setPins(HIGH, HIGH);
  delay(kEncoderTransitionDelayMs);

  pinMode(kEncoderPinA, INPUT);
  pinMode(kEncoderPinB, INPUT);
}

/**
 * @brief Emulates encoder button press.
 */
void pressButton() {
  pinMode(kEncoderPinSwitch, OUTPUT);
  digitalWrite(kEncoderPinSwitch, HIGH);
  delay(kEncoderButtonPressMs);

  pinMode(kEncoderPinSwitch, INPUT);
}

/**
 * @brief Set the encoder pins direction.
 *
 * @param a encoder pin A.
 * @param b encoder pin B.
 */
void setPins(uint8_t a, uint8_t b) {
  digitalWrite(kEncoderPinA, a);
  digitalWrite(kEncoderPinB, b);
}
