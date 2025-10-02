#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

// WiFi Access Point credentials
const char* ssid = "IEKM ESP32 PulseGen";
const char* password = "12345678";

// Server on port 80
AsyncWebServer server(80);

// Pulse parameters (ms)
volatile int pulse_width = 100;    // milliseconds
volatile int pulse_period = 200;   // milliseconds
volatile int n_pulses = 10;        // number of pulses
volatile int gen_delay_ms = 50;    // generator delay relative to myopacer (ms)

// Per-pin control flags
volatile bool pulsing_myop = false;
volatile bool pulsing_gen = false;
volatile bool stop_myop = false;
volatile bool stop_gen = false;

// Output pins
const int out_myopacer_pin = 25;
const int out_generator_pin = 26;

// FreeRTOS task handles
TaskHandle_t myopTaskHandle = NULL;
TaskHandle_t genTaskHandle = NULL;

// Logging buffer
String logBuffer;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// Logging function
void addLog(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  char line[320];
  snprintf(line, sizeof(line), "%s\n", buf);

  portENTER_CRITICAL(&logMux);
  logBuffer += String(line);
  if (logBuffer.length() > 8192) {
    logBuffer = logBuffer.substring(logBuffer.length() - 4096);
  };

  portEXIT_CRITICAL(&logMux);
};

// Parameter struct for task
struct PulseParams {
  int pin;
  int width;
  int period;
  int n_pulses;
  int startDelay;           // ms initial delay before first pulse (0 for immediate)
  volatile bool *stopFlag;  // pointer to the pin-specific stop flag
  volatile bool *runningFlag; // pointer to the pin-specific running flag
};

// Reusable pulse task
void pulseTask(void *pvParameters) {
  PulseParams *p = (PulseParams*) pvParameters;
  if (p -> startDelay > 0) {
    addLog("Pin %d: waiting initial delay %d ms", p -> pin, p -> startDelay);
    vTaskDelay(pdMS_TO_TICKS(p -> startDelay));
  }

  *(p -> runningFlag) = true;

  for (int i = 0; i < p -> n_pulses; ++i) {
    if (*(p -> stopFlag)) break;

    digitalWrite(p -> pin, HIGH);
    vTaskDelay(pdMS_TO_TICKS(p -> width));
    digitalWrite(p -> pin, LOW);

    int lowDelay = p -> period - p -> width;

    if (lowDelay > 0) vTaskDelay(pdMS_TO_TICKS(lowDelay));
  }

  *(p -> runningFlag) = false;
  *(p -> stopFlag) = false;

  delete p;
  vTaskDelete(NULL);
}

// HTML UI
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>IEKM ESP32 Pulse Generator</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>body{font-family:Arial; text-align:center;} textarea{width:90%;}</style>
</head>
<body>
  <h2>IEKM ESP32 Pulse Generator</h2>
  <div>
    <label>Pulse Width (ms):</label><br>
    <input id="width" type="number" value="100"><br><br>
    <label>Pulse Period (ms):</label><br>
    <input id="period" type="number" value="200"><br><br>
    <label>Number of Pulses:</label><br>
    <input id="npulses" type="number" value="10"><br><br>
    <label>Generator - Myopacer Delay (ms):</label><br>
    <input id="gdelay" type="number" value="50"><br><br>
    <button onclick="setParams()">Set Parameters</button>
  </div>
<br>
<button onclick="start()">Start Pulsing</button>
<button onclick="stopAll()">Stop Pulsing</button>
<h3>Debug Log</h3>
<textarea id="log" rows="12" readonly></textarea>
<script>
  function appendLocal(text) {
    const ta = document.getElementById('log');
    ta.value += text + '\n';
    ta.scrollTop = ta.scrollHeight;
  }

  function setParams() {
    const w = document.getElementById('width').value;
    const p = document.getElementById('period').value;
    const n = document.getElementById('npulses').value;
    const d = document.getElementById('gdelay').value;
    fetch(`/set?width=${w}&period=${p}&npulses=${n}&delay=${d}`).then(r => r.text());
  }

  function start() {
    fetch('/start').then(r => r.text())
  }

  function stopAll() {
    fetch('/stop').then(r => r.text())
  }
  
  function fetchLog() {
    fetch('/log').then(r => r.text()).then(txt => {
      document.getElementById('log').value = txt;
      document.getElementById('log').scrollTop = document.getElementById('log').scrollHeight;
    });
  }
  setInterval(fetchLog, 1000);
  window.onload = fetchLog;
</script>
</body>
</html>
)rawliteral";

void setup() {
  // start serial for debugging
  Serial.begin(115200);

  // set pins
  pinMode(out_myopacer_pin, OUTPUT);
  digitalWrite(out_myopacer_pin, LOW);
  pinMode(out_generator_pin, OUTPUT);
  digitalWrite(out_generator_pin, LOW);

  // create WiFi access point
  WiFi.softAP(ssid, password);
  Serial.println((String)"Started WiFi AP at " + WiFi.softAPIP().toString().c_str());
  addLog("AP started. IP: %s", WiFi.softAPIP().toString().c_str());

  // start server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request -> send(200, "text/html", index_html);
  });

  // commands
  server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request -> hasParam("width")) {
      int w = request -> getParam("width") -> value().toInt();
      if (w > 0) pulse_width = w;
      else {
        request -> send(400, "text/plain", "Invalid width");
        return;
      }
    };

    if (request -> hasParam("period")) {
      int p = request -> getParam("period") -> value().toInt();
      if (p > 0) pulse_period = p;
      else {
        request -> send(400, "text/plain", "Invalid period");
        return;
      }
    };

    if (request -> hasParam("npulses")) {
      int n = request -> getParam("npulses") -> value().toInt();
      if (n > 0) n_pulses = n;
      else {
        request -> send(400, "text/plain", "Invalid number of pulses");
        return;
      }
    };

    if (request -> hasParam("delay")) {
      int d = request -> getParam("delay") -> value().toInt();
      if (d > 0) gen_delay_ms = d;
      else {
        request -> send(400, "text/plain", "Invalid delay");
        return;
      }
    };

    // check for condition pulse_width >= pulse_period
    if (pulse_width >= pulse_period) {
      request -> send(400, "text/plain", "Error: pulse width must be less than pulse period.");
      addLog("Rejected params: width (%d ms) must be smaller than period (%d ms).", pulse_width, pulse_period);
      return;
    };

    // write to log
    addLog("Params updated:: width = %d ms, period = %d ms, n pulses = %d, delay = %d ms.", pulse_width, pulse_period, n_pulses, gen_delay_ms);

    // done with getting parameters
    request -> send(200, "text/plain", "Parameters updated.");
  });

  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!pulsing_myop) {
      stop_myop = false;
      PulseParams *myo_pm = new PulseParams{out_myopacer_pin, pulse_width, pulse_period, n_pulses, 0, &stop_myop, &pulsing_myop};

      if (xTaskCreatePinnedToCore(pulseTask, "MyopTask", 4096, myo_pm, 1, &myopTaskHandle, 1) == pdPASS) {
        request -> send(200, "text/plain", "Started Myopacer pulsing.");
      }
      
      else {
        addLog("Failed to create Myopacer Task");
        request -> send(500, "text/plain", "Failed to start Myopacer.");
        delete myo_pm;
        return;
      }
    };

    if (!pulsing_gen) {
      stop_gen = false;
      PulseParams *gen_pm = new PulseParams{out_generator_pin, pulse_width, pulse_period, n_pulses, gen_delay_ms, &stop_gen, &pulsing_gen};

      if (xTaskCreatePinnedToCore(pulseTask, "GenTask", 4096, gen_pm, 1, &genTaskHandle, 1) == pdPASS) {
        request -> send(200, "text/plain", "Started Generator pulsing.");
      }
      
      else {
        addLog("Failed to create Generator Task");
        request -> send(500, "text/plain", "Failed to start Generator.");
        delete gen_pm;
        return;
      }
    };
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request) {
    stop_myop = true;
    stop_gen = true;

    addLog("Stop requested for all pulse tasks");
    request -> send(200, "text/plain", "Stopping pulsing.");
  });

  server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    portENTER_CRITICAL(&logMux);
    String out = logBuffer;
    portEXIT_CRITICAL(&logMux);
    request -> send(200, "text/plain", out);
  });

  // start server
  server.begin();
  addLog("HTTP server started.");
}

void loop() {}