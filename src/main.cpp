#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <AudioFileSourceICYStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>          // *** API added ***
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_spi_flash.h>

// Override library buffer defaults
#define I2S_DMA_BUF_COUNT 16      // Default: 3-4 (try 4-8)
#define I2S_DMA_BUF_LEN 2048     // Default: 512 (try 512-2048)
// ===== WiFi credentials =====
const char* ssid = "MIMA";
const char* password = "nikola18092015";

// ===== OLED setup =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// ===== I2S pins for PCM5102A =====
#define I2S_BCK 27
#define I2S_LRC 26
#define I2S_DIN 25
//
float currentVolume = 0.5f; // Default volume (50%)

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceICYStream *file = nullptr;
AudioOutputI2S *out = nullptr;

AsyncWebServer server(80);
Preferences preferences;

#define MAX_STATIONS 20
#define MAX_NAME_LEN 32
#define MAX_URL_LEN 128

#define VOL_ENCODER_CLK 35
#define VOL_ENCODER_DT  36
#define VOL_ENCODER_SW  39

struct RadioStation {
  char name[MAX_NAME_LEN];
  char url[MAX_URL_LEN];
};

RadioStation stations[MAX_STATIONS];
int numStations = 0;
int currentStationIndex = 0;

String currentTitle = "Loading...";
String stationName = "";
String localIPStr = "";
int wifiBars = 0;
int audioBarState = 0;

unsigned long lastVisualUpdate = 0;
const unsigned long visualInterval = 300;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectDelay = 5000;

SemaphoreHandle_t titleMutex = NULL;
volatile int requestedStationIndex = -1;

// Encoder pins and variables
#define ENCODER_CLK 32
#define ENCODER_DT  33
#define ENCODER_SW  34

volatile int encoderPos = 0;
volatile bool encoderMoved = false;
volatile bool encoderButtonPressed = false;
int lastReportedPos = 0;

volatile unsigned long lastEncoderInterrupt = 0;
volatile unsigned long lastButtonInterrupt = 0;
const unsigned long debounceDelay = 3;   // 3 ms for encoder
const unsigned long buttonDebounceDelay = 50; // 50 ms for button


volatile int volEncoderPos = 0;
volatile bool volEncoderMoved = false;
volatile bool volEncoderButtonPressed = false;
int lastVolReportedPos = 0;

volatile unsigned long lastVolEncoderInterrupt = 0;
volatile unsigned long lastVolButtonInterrupt = 0;
const unsigned long volDebounceDelay = 3;   // 3 ms for encoder
const unsigned long volButtonDebounceDelay = 50; // 50 ms for button
bool isMuted = false;
float lastVolumeBeforeMute = 0.5f;

// Forward declarations
void connectWiFi();
void startStream(int stationIndex);
void showDisplay(const String &line1, const String &line2, const String &line3);
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *str);
void drawWiFiSignal(int x, int y);
//void drawAudioBars(int x, int y);
void audioTask(void *param);
void displayTask(void *param);
void loadStationsFromPrefs();
void saveStationsToPrefs();
int findFirstSupportedStation(); // <-- Add this line




// --- Convert stations array to JSON string for API ---
String stationsToJson() {  // *** API added ***
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < numStations; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = stations[i].name;
    obj["url"] = stations[i].url;
  }

  String output;
  serializeJson(doc, output);
  return output;
}
void IRAM_ATTR handleEncoder() {
  unsigned long now = millis();
  if (now - lastEncoderInterrupt > debounceDelay) {
    static uint8_t lastState = 0;
    uint8_t state = (digitalRead(ENCODER_CLK) << 1) | digitalRead(ENCODER_DT);
    if (state == 0b10 && lastState == 0b00) encoderPos++;
    if (state == 0b01 && lastState == 0b00) encoderPos--;
    lastState = state;
    encoderMoved = true;
    lastEncoderInterrupt = now;
  }
}

void IRAM_ATTR handleEncoderButton() {
  unsigned long now = millis();
  if (now - lastButtonInterrupt > buttonDebounceDelay) {
    if (digitalRead(ENCODER_SW) == LOW) {
      encoderButtonPressed = true;
    }
    lastButtonInterrupt = now;
  }
}

void IRAM_ATTR handleVolEncoder() {
  unsigned long now = millis();
  if (now - lastVolEncoderInterrupt > volDebounceDelay) {
    static uint8_t lastState = 0;
    uint8_t state = (digitalRead(VOL_ENCODER_CLK) << 1) | digitalRead(VOL_ENCODER_DT);
    if (state == 0b10 && lastState == 0b00) volEncoderPos++;
    if (state == 0b01 && lastState == 0b00) volEncoderPos--;
    lastState = state;
    volEncoderMoved = true;
    lastVolEncoderInterrupt = now;
  }
}

void IRAM_ATTR handleVolEncoderButton() {
  unsigned long now = millis();
  if (now - lastVolButtonInterrupt > volButtonDebounceDelay) {
    if (digitalRead(VOL_ENCODER_SW) == LOW) {
      volEncoderButtonPressed = true;
    }
    lastVolButtonInterrupt = now;
  }
}

void printDebugInfo() {
  Serial.println("\n=== ESP32 Debug Info ===");

  Serial.printf("Free Heap: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("Heap Size: %d KB\n", ESP.getHeapSize() / 1024);
  Serial.printf("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Sketch Size: %d KB\n", ESP.getSketchSize() / 1024);

  uint64_t mac = ESP.getEfuseMac();
  Serial.printf("Chip ID: %012llX\n", mac);

  Serial.println("=======================");
}

void printMemoryBar(const char* label, size_t used, size_t total) {
  const int barWidth = 10;
  float percent = (float)used / total * 100.0f;
  int bars = (int)(barWidth * percent / 100.0f);

  Serial.printf("%-12s [", label);
  for (int i = 0; i < bars; i++) Serial.print('=');
  for (int i = bars; i < barWidth; i++) Serial.print(' ');
  Serial.printf("] %5.1f%% (%u KB / %u KB)\n", percent, used / 1024, total / 1024);
}

void printFlashInfo() {
  uint32_t flashChipSize = ESP.getFlashChipSize();
  uint32_t sketchSize = ESP.getSketchSize();
  uint32_t freeSketchSpace = ESP.getFreeSketchSpace();
  uint32_t usedFlash = flashChipSize - freeSketchSpace;

  printMemoryBar("Flash", usedFlash, flashChipSize);
  printMemoryBar("Sketch", sketchSize, flashChipSize);
}

void printRamInfo() {
  size_t heapSize = ESP.getHeapSize();
  size_t freeHeap = ESP.getFreeHeap();
  size_t usedHeap = heapSize - freeHeap;

  printMemoryBar("RAM Heap", usedHeap, heapSize);
  // Optionally, you can add stack and static info if available
}



void setup() {
  // Disable serial debug to save resources
Serial.begin(115200);
delay(1000);  // wait for Serial to be ready
printDebugInfo();
  // Print memory usage info
  Serial.println("=== Memory Usage ===");
  printFlashInfo();
  printRamInfo();
  Serial.println("====================");

  // Push CPU freq to max 240 MHz
  setCpuFrequencyMhz(240);

  delay(100);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // OLED init failed, hang
    while (true) delay(1000);
  }
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), handleEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_DT), handleEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW), handleEncoderButton, FALLING);

    pinMode(VOL_ENCODER_CLK, INPUT_PULLUP);
  pinMode(VOL_ENCODER_DT, INPUT_PULLUP);
  pinMode(VOL_ENCODER_SW, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(VOL_ENCODER_CLK), handleVolEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(VOL_ENCODER_DT), handleVolEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(VOL_ENCODER_SW), handleVolEncoderButton, FALLING);
  
  titleMutex = xSemaphoreCreateMutex();
  showDisplay("Booting...", "", "");

  connectWiFi();

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCK, I2S_LRC, I2S_DIN);
  preferences.begin("radio", false);
  currentVolume = preferences.getFloat("volume", 0.8f);
  out->SetGain(currentVolume);

  loadStationsFromPrefs();
  if (numStations == 0) {
    strncpy(stations[0].name, "Naxi Radio", MAX_NAME_LEN);
    strncpy(stations[0].url, "http://naxi128.streaming.rs:9150", MAX_URL_LEN);
    strncpy(stations[1].name, "Classic FM", MAX_NAME_LEN);
    strncpy(stations[1].url, "http://media-ice.musicradio.com/ClassicFMMP3", MAX_URL_LEN);
    numStations = 2;
  }

  int savedIndex = preferences.getInt("currentIndex", 0);
  if (savedIndex < 0 || savedIndex >= numStations) savedIndex = 0;

  // If saved station is unsupported, auto-select first supported
  if (String(stations[savedIndex].url).startsWith("https://")) {
    int supported = findFirstSupportedStation();
    if (supported >= 0) {
      requestedStationIndex = currentStationIndex = supported;
    } else {
      requestedStationIndex = currentStationIndex = 0;
      showDisplay("No supported", "stations found!", "");
    }
  } else {
    requestedStationIndex = currentStationIndex = savedIndex;
  }

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 4096, NULL, 1, NULL, 1);

  // Serve the web interface (existing HTML + JS)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    xSemaphoreTake(titleMutex, portMAX_DELAY);

   String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32 Radio</title>
  <style>
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen,
        Ubuntu, Cantarell, "Open Sans", "Helvetica Neue", sans-serif;
      margin: 20px auto;
      max-width: 480px;
      background-color: #121212;
      color: #f0f0f0;
      line-height: 1.5;
    }
    h1, h2, h3 {
      margin: 0 0 10px 0;
      font-weight: 600;
    }
    p {
      margin: 6px 0;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 15px;
    }
    th, td {
      padding: 10px 8px;
      border-bottom: 1px solid #333;
      text-align: left;
    }
    th {
      background-color: #222;
      font-weight: 700;
    }
    tr:hover {
      background-color: #2a2a2a;
    }
    input[type="text"],
    input[type="url"] {
      width: 100%;
      padding: 8px;
      margin: 5px 0 12px 0;
      background-color: #222;
      border: 1px solid #444;
      border-radius: 4px;
      color: #eee;
      box-sizing: border-box;
      font-size: 14px;
    }
    input[type="range"] {
      width: 100%;
      margin: 10px 0 20px 0;
    }
    button {
      background-color: #0078d7;
      border: none;
      color: white;
      padding: 8px 14px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 14px;
      margin-right: 6px;
      border-radius: 4px;
      cursor: pointer;
      transition: background-color 0.3s ease;
    }
    button:hover {
      background-color: #005a9e;
    }
    #editForm {
      background-color: #1a1a1a;
      padding: 15px;
      margin-top: 25px;
      border-radius: 6px;
      display: none;
    }
    label {
      font-weight: 600;
      display: block;
      margin-bottom: 6px;
    }
  </style>
  <script>



  function cancelEdit() {
  document.getElementById('editForm').style.display = 'none';
}

    let volumeTimeout = null;
    function setVolumeDebounced(vol) {
      if (volumeTimeout) clearTimeout(volumeTimeout);
      volumeTimeout = setTimeout(() => {
        var xhr = new XMLHttpRequest();
        xhr.open("POST", "/volume", true);
        xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
        xhr.send("vol=" + vol);
      }, 200);
    }
    function editStation(i) {
      document.getElementById('editName').value = document.getElementById('name' + i).innerText;
      document.getElementById('editUrl').value = document.getElementById('url' + i).innerText;
      document.getElementById('editIndex').value = i;
      document.getElementById('editForm').style.display = 'block';
      window.scrollTo(0, document.body.scrollHeight);
    }
  </script>

  <script>
  async function fetchStatus() {
    try {
      let response = await fetch('/status');
      if (response.ok) {
        let data = await response.json();
        document.getElementById('title').innerText = data.title;
        document.getElementById('station').innerText = data.station;
        document.getElementById('ip').innerText = data.ip;
      }
    } catch(e) {
      console.error('Failed to fetch status', e);
    }
  }

  // Update every 3 seconds
  setInterval(fetchStatus, 3000);

  // Also fetch once when page loads
  window.onload = fetchStatus;
</script>
  </script>
</head>
<body>
  <h1>ESP32 Internet Radio</h1>

  <section>
    <h2>Now Playing</h2>
<p><strong>Title:</strong> <span id="title">Loading...</span></p>
<p><strong>Station:</strong> <span id="station">Loading...</span></p>
<p><strong>IP Address:</strong> <span id="ip">Loading...</span></p>

  </section>

  <section>
    <label for="volumeRange">Volume: <span id="volLabel">)rawliteral" + String((int)(currentVolume * 100)) + R"rawliteral(</span>%</label>
    <input type="range" id="volumeRange" min="0" max="100" value=")rawliteral" + String((int)(currentVolume * 100)) + R"rawliteral(" oninput="setVolumeDebounced(this.value); document.getElementById('volLabel').innerText=this.value;" />
  </section>

  <section>
    <h2>Stations</h2>
    <table>
      <thead>
        <tr><th>Name</th><th>URL</th><th>Actions</th></tr>
      </thead>
      <tbody>
)rawliteral";

  for (int i = 0; i < numStations; i++) {
    html += "<tr>";
    html += "<td id='name" + String(i) + "'>" + stations[i].name + "</td>";
    html += "<td id='url" + String(i) + "'>" + stations[i].url + "</td>";
    html += "<td>";
    html += "<a href='/play?index=" + String(i) + "'><button>Play</button></a>";
    html += "<button onclick='editStation(" + String(i) + ")'>Edit</button>";
    html += "<a href='/delete?index=" + String(i) + "'><button>Delete</button></a>";
    html += "</td></tr>";
  }

  html += R"rawliteral(
      </tbody>
    </table>
  </section>

<section id="editForm">
  <h2>Edit Station</h2>
  <form action="/update" method="POST">
    <input type="hidden" id="editIndex" name="index" />
    <label for="editName">Name</label>
    <input type="text" id="editName" name="name" required />
    <label for="editUrl">URL</label>
    <input type="url" id="editUrl" name="url" required />
    <button type="submit">Update Station</button>
    <button type="button" onclick="cancelEdit()">Cancel</button>
  </form>
</section>


  <section>
    <h2>Add New Station</h2>
    <form action="/add" method="POST">
      <label for="name">Name</label>
      <input type="text" id="name" name="name" placeholder="Station Name" required />
      <label for="url">URL</label>
      <input type="url" id="url" name="url" placeholder="Station URL" required />
      <button type="submit">Add Station</button>
    </form>
  </section>

</body>
</html>
)rawliteral";

    xSemaphoreGive(titleMutex);
    request->send(200, "text/html", html);
  });

  // Volume control via web UI (existing)
  server.on("/volume", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("vol", true)) {
      int vol = request->getParam("vol", true)->value().toInt();
      vol = constrain(vol, 0, 100);
      currentVolume = vol / 100.0f;
      preferences.putFloat("volume", currentVolume);
      if (out) out->SetGain(currentVolume);
    }
    request->send(200, "text/plain", "OK");
  });

  // Play station by index (existing)
  server.on("/play", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("index")) {
      int idx = request->getParam("index")->value().toInt();
      if (idx >= 0 && idx < numStations) requestedStationIndex = idx;
    }
    request->redirect("/");
  });

  // Add station (existing)
  server.on("/add", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (numStations < MAX_STATIONS && request->hasParam("name", true) && request->hasParam("url", true)) {
      String name = request->getParam("name", true)->value();
      String url = request->getParam("url", true)->value();
      strncpy(stations[numStations].name, name.c_str(), MAX_NAME_LEN);
      stations[numStations].name[MAX_NAME_LEN - 1] = 0;
      strncpy(stations[numStations].url, url.c_str(), MAX_URL_LEN);
      stations[numStations].url[MAX_URL_LEN - 1] = 0;
      numStations++;
      saveStationsToPrefs();
    }
    request->redirect("/");
  });

  // Update station (existing)
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("index", true) && request->hasParam("name", true) && request->hasParam("url", true)) {
      int idx = request->getParam("index", true)->value().toInt();
      if (idx >= 0 && idx < numStations) {
        strncpy(stations[idx].name, request->getParam("name", true)->value().c_str(), MAX_NAME_LEN);
        stations[idx].name[MAX_NAME_LEN - 1] = 0;
        strncpy(stations[idx].url, request->getParam("url", true)->value().c_str(), MAX_URL_LEN);
        stations[idx].url[MAX_URL_LEN - 1] = 0;
        saveStationsToPrefs();
      }
    }
    request->redirect("/");
  });

  // Delete station (existing)
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("index")) {
      int idx = request->getParam("index")->value().toInt();
      if (idx >= 0 && idx < numStations) {
        for (int i = idx; i < numStations - 1; i++) {
          stations[i] = stations[i + 1];
        }
        numStations--;
        saveStationsToPrefs();
      }
    }
    request->redirect("/");
  });

  // --- REST API: Get all stations ---
  server.on("/stations", HTTP_GET, [](AsyncWebServerRequest *request){  // *** API added ***
    String json = stationsToJson();
    request->send(200, "application/json", json);
  });

  // --- REST API: Add new station ---
  server.on("/stations", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    static String body;
    if(index == 0) body = "";
    body += String((char*)data).substring(0, len);

    if(index + len == total){
      DynamicJsonDocument doc(512);
      DeserializationError error = deserializeJson(doc, body);
      if(error){
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      String name = doc["name"] | "";
      String url = doc["url"] | "";

      if(name.isEmpty() || url.isEmpty()){
        request->send(400, "application/json", "{\"error\":\"Missing name or url\"}");
        return;
      }

      if(numStations >= MAX_STATIONS){
        request->send(400, "application/json", "{\"error\":\"Station list full\"}");
        return;
      }

      RadioStation newStation;
      strncpy(newStation.name, name.c_str(), MAX_NAME_LEN);
      newStation.name[MAX_NAME_LEN-1] = 0;
      strncpy(newStation.url, url.c_str(), MAX_URL_LEN);
      newStation.url[MAX_URL_LEN-1] = 0;
      stations[numStations++] = newStation;

      saveStationsToPrefs();

      request->send(200, "application/json", "{\"status\":\"station added\"}");
    }
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
  xSemaphoreTake(titleMutex, portMAX_DELAY);
  DynamicJsonDocument doc(512);
  doc["title"] = currentTitle;
  doc["station"] = stationName;
  doc["ip"] = localIPStr;
  String response;
  serializeJson(doc, response);
  xSemaphoreGive(titleMutex);
  request->send(200, "application/json", response);
});


  // --- REST API: Control playback ---
  server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    static String body;
    if(index == 0) body = "";
    body += String((char*)data).substring(0, len);

    if(index + len == total){
      DynamicJsonDocument doc(512);
      DeserializationError error = deserializeJson(doc, body);
      if(error){
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      String action = doc["action"] | "";
      int station = doc["station"] | -1;
      float volume = doc["volume"] | -1.0;

      if(action == "play" && station >= 0 && station < numStations){
        requestedStationIndex = station;
        request->send(200, "application/json", "{\"status\":\"playing\"}");
      }
      else if(action == "pause"){
        if(mp3 && mp3->isRunning()) mp3->stop();
        request->send(200, "application/json", "{\"status\":\"paused\"}");
      }
      else if(action == "next"){
        requestedStationIndex = (currentStationIndex + 1) % numStations;
        request->send(200, "application/json", "{\"status\":\"next\"}");
      }
      else if(action == "prev"){
        requestedStationIndex = (currentStationIndex - 1 + numStations) % numStations;
        request->send(200, "application/json", "{\"status\":\"previous\"}");
      }
      else if(action == "volume" && volume >= 0.0 && volume <= 1.0){
        currentVolume = volume;
        preferences.putFloat("volume", currentVolume);
        if (out) out->SetGain(currentVolume);
        request->send(200, "application/json", "{\"status\":\"volume set\"}");
      }
      else{
        request->send(400, "application/json", "{\"error\":\"Invalid action or parameters\"}");
      }
    }
  });
  server.on("/nowplaying", HTTP_GET, [](AsyncWebServerRequest *request) {
  xSemaphoreTake(titleMutex, portMAX_DELAY);

  DynamicJsonDocument doc(512);
  doc["title"] = currentTitle;
  doc["station"] = stationName;
  doc["ip"] = localIPStr;
  doc["volume"] = (int)(currentVolume * 100);

  String json;
  serializeJson(doc, json);

  xSemaphoreGive(titleMutex);

  request->send(200, "application/json", json);
});



  server.begin();
}

void loop() {
  // All work is done in tasks
 static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 5000) {
    unsigned long uptimeSec = millis() / 1000;
    unsigned int h = uptimeSec / 3600;
    unsigned int m = (uptimeSec % 3600) / 60;
    unsigned int s = uptimeSec % 60;
    Serial.printf("Uptime: %02u:%02u:%02u\n", h, m, s);
    lastDebug = millis();
  }
  // Rotary encoder station change
  if (encoderMoved) {
    encoderMoved = false;
    if (encoderPos > lastReportedPos) {
      // Next station
      requestedStationIndex = (currentStationIndex + 1) % numStations;
      lastReportedPos = encoderPos;
    } else if (encoderPos < lastReportedPos) {
      // Previous station
      requestedStationIndex = (currentStationIndex - 1 + numStations) % numStations;
      lastReportedPos = encoderPos;
    }
  }
  // --- Volume encoder: increase/decrease volume ---
  const float VOLUME_STEP = 0.05f; // 5% per detent

if (volEncoderMoved) {
  volEncoderMoved = false;
  bool updated = false;
  if (volEncoderPos > lastVolReportedPos) {
    // Increase volume
    if (!isMuted && currentVolume < 1.0f) {
      currentVolume += VOLUME_STEP;
      if (currentVolume > 1.0f) currentVolume = 1.0f;
      preferences.putFloat("volume", currentVolume);
      if (out) out->SetGain(currentVolume);
      lastVolReportedPos = volEncoderPos;
      updated = true;
    }
  } else if (volEncoderPos < lastVolReportedPos) {
    // Decrease volume
    if (!isMuted && currentVolume > 0.0f) {
      currentVolume -= VOLUME_STEP;
      if (currentVolume < 0.0f) currentVolume = 0.0f;
      preferences.putFloat("volume", currentVolume);
      if (out) out->SetGain(currentVolume);
      lastVolReportedPos = volEncoderPos;
      updated = true;
    }
  }
  if (updated) showDisplay(currentTitle, stationName, localIPStr);
}

// --- Volume encoder button: mute/unmute ---
if (volEncoderButtonPressed) {
  volEncoderButtonPressed = false;
  if (!isMuted) {
    lastVolumeBeforeMute = currentVolume;
    currentVolume = 0.0f;
    isMuted = true;
    if (mp3 && mp3->isRunning()) mp3->stop(); // Stop audio
  } else {
    currentVolume = lastVolumeBeforeMute;
    isMuted = false;
    requestedStationIndex = currentStationIndex; // Restart audio
  }
  preferences.putFloat("volume", currentVolume);
  if (out) out->SetGain(currentVolume);
  showDisplay(currentTitle, stationName, localIPStr);
}

  // --- Handle requested station change ---
  // Encoder button: pause/play
  if (encoderButtonPressed) {
    encoderButtonPressed = false;
    if (mp3 && mp3->isRunning()) {
      mp3->stop();
    } else {
      requestedStationIndex = currentStationIndex;
    }
  }
}



void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  localIPStr = WiFi.localIP().toString();
  showDisplay("WiFi Connected", WiFi.SSID(), localIPStr);
}

void startStream(int stationIndex) {
  out->SetGain(currentVolume);

  String url = stations[stationIndex].url;
  if (url.startsWith("https://")) {
    showDisplay("HTTPS not supported", stations[stationIndex].name, "");
    Serial.printf("[StartStream] HTTPS not supported: %s\n", url.c_str());
    int nextIndex = findFirstSupportedStation();
    if (nextIndex != -1 && nextIndex != stationIndex) {
      requestedStationIndex = nextIndex;
    }
    return;
  }

  if (mp3) {
    mp3->stop();
    delete mp3;
    mp3 = nullptr;
  }
  if (file) {
    delete file;
    file = nullptr;
  }
  Serial.printf("[StartStream] Playing station %d: %s\n", stationIndex, stations[stationIndex].name);
  Serial.printf("URL: %s\n", stations[stationIndex].url);

  currentStationIndex = stationIndex;
  stationName = stations[stationIndex].name;

  file = new AudioFileSourceICYStream(stations[stationIndex].url);
  file->RegisterMetadataCB(MDCallback, nullptr);

  mp3 = new AudioGeneratorMP3();
  if (!mp3->begin(file, out)) {
    delete mp3;
    mp3 = nullptr;
    showDisplay("Stream error", stationName, "");
    return;
  }

  xSemaphoreTake(titleMutex, portMAX_DELAY);
  currentTitle = "Streaming...";
  xSemaphoreGive(titleMutex);
  preferences.putInt("currentIndex", currentStationIndex);
}



void MDCallback(void *cbData, const char *type, bool isUnicode, const char *str) {
  Serial.print("[Metadata Type] ");
  Serial.println(type);

  if (str && strlen(str) > 0) {
    Serial.print("[Metadata] ");
    Serial.println(str);
  }

  if (strcmp(type, "StreamTitle") == 0) {
    xSemaphoreTake(titleMutex, portMAX_DELAY);
    currentTitle = String(str);
    xSemaphoreGive(titleMutex);
  }

  // Additional: parse full ICY metadata string if available (type == "ICY")
  // Some implementations send full raw ICY metadata as type "ICY"
  if (strcmp(type, "ICY") == 0 && str && strlen(str) > 0) {
    Serial.println("[ICY Raw Metadata]");
    Serial.println(str);

    // Parse key=value pairs separated by ';'
    String metadataStr = String(str);
    int start = 0;
    while (true) {
      int sep = metadataStr.indexOf(';', start);
      String token = (sep == -1) ? metadataStr.substring(start) : metadataStr.substring(start, sep);
      token.trim();
      if (token.length() == 0) break;

      int eqPos = token.indexOf('=');
      if (eqPos > 0) {
        String key = token.substring(0, eqPos);
        String val = token.substring(eqPos + 1);

        // Remove quotes if present
        if (val.startsWith("'") && val.endsWith("'")) {
          val = val.substring(1, val.length() - 1);
        }

        Serial.printf("  Key: %s, Value: %s\n", key.c_str(), val.c_str());

        // You can update your variables here if you want to keep them
        if (key == "StreamTitle") {
          xSemaphoreTake(titleMutex, portMAX_DELAY);
          currentTitle = val;
          xSemaphoreGive(titleMutex);
        }
        else if (key == "StreamUrl") {
          Serial.println("[ICY Logo or Stream URL detected]");
          // You could save or fetch this URL if you want to show logo later
        }
      }
      if (sep == -1) break;
      start = sep + 1;
    }
  }
}


void showDisplay(const String &station, const String &meta, const String &ip) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Line 1: Station info
  display.setCursor(0, 0);
  display.println(station);

  // Line 2: (empty space)

  // Line 3: Metadata
  display.setCursor(0, 16);
  display.println(meta);

  // Line 4: (empty space)

  // Line 5: IP + WiFi bars
  String ipText = "IP: " + ip;
  display.setCursor(0, 32);
  display.print(ipText);

  // Measure width of IP text to position WiFi bars
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(ipText, 0, 32, &x1, &y1, &w, &h);

  // Draw WiFi signal bars
  drawWiFiSignal(w + 6, 38);

  // Line 6: Volume bar
  display.setCursor(0, 48);
  display.print("Vol: ");
  int barWidth = 80;
  int filled = (int)(barWidth * currentVolume);





























  
  display.drawRect(36, 48, barWidth, 10, SSD1306_WHITE);
  display.fillRect(36, 48, filled, 10, SSD1306_WHITE);

  if (isMuted) {
    display.setCursor(120, 48);
    display.print("MUTE");
  }

  display.display();
}





void drawWiFiSignal(int x, int y) {
  long rssi = WiFi.RSSI();
  int bars = (rssi > -60) ? 4 : (rssi > -70) ? 3 : (rssi > -80) ? 2 : (rssi > -90) ? 1 : 0;

  for (int i = 0; i < 4; i++) {
    int h = (i + 1) * 3;
    if (i < bars)
      display.fillRect(x + i * 4, y - h, 3, h, SSD1306_WHITE);
    else
      display.drawRect(x + i * 4, y - h, 3, h, SSD1306_WHITE);
  }
}


/* void drawAudioBars(int x, int y) {
  for (int i = 0; i < 4; i++) {
    int height = ((i + audioBarState) % 4 + 1) * 2;
    display.fillRect(x + i * 4, y - height, 3, height, SSD1306_WHITE);
  }
}
*/


void audioTask(void *param) {
  while (true) {
    if (requestedStationIndex != -1) {
      int newIndex = requestedStationIndex;
      requestedStationIndex = -1;
      startStream(newIndex);
    }

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      connectWiFi();
      requestedStationIndex = currentStationIndex;
    }

    if (mp3 && mp3->isRunning()) {
      if (!mp3->loop()) {
        mp3->stop();
        requestedStationIndex = currentStationIndex; // retry same station
        vTaskDelay(pdMS_TO_TICKS(100)); // Shorter delay
      }
    } else if (millis() - lastReconnectAttempt > reconnectDelay) {
      requestedStationIndex = currentStationIndex; // reconnect
      lastReconnectAttempt = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(2)); // Keep this as short as possible
  }
}

void displayTask(void *param) {
  while (true) {
    audioBarState = (audioBarState + 1) % 4;
    xSemaphoreTake(titleMutex, portMAX_DELAY);
    String titleCopy = currentTitle;
    String stationCopy = stationName;
    String ipCopy = localIPStr;
    xSemaphoreGive(titleMutex);

    showDisplay(titleCopy, stationCopy, ipCopy);
    vTaskDelay(pdMS_TO_TICKS(visualInterval));
  }
}

void saveStationsToPrefs() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < numStations; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = stations[i].name;
    obj["url"] = stations[i].url;
  }
  String json;
  serializeJson(doc, json);
  preferences.putString("stations", json);
  preferences.putInt("currentIndex", currentStationIndex);
  preferences.putFloat("volume", currentVolume);
}

void printTaskStats() {
    const int maxTasks = 10; // adjust as needed
    TaskStatus_t taskStatusArray[maxTasks];
    UBaseType_t taskCount;
    uint32_t totalRunTime;

    // Get number of tasks and their info
    taskCount = uxTaskGetSystemState(taskStatusArray, maxTasks, &totalRunTime);

    Serial.printf("Task\t\tRuntime\t\tCPU%%\n");
    for (int i = 0; i < taskCount; i++) {
        float cpuPercent = 0;
        if (totalRunTime > 0) {
            cpuPercent = (taskStatusArray[i].ulRunTimeCounter * 100.0f) / totalRunTime;
        }
        Serial.printf("%-16s %10lu %6.2f%%\n",
            taskStatusArray[i].pcTaskName,
            taskStatusArray[i].ulRunTimeCounter,
            cpuPercent);
    }
}


void loadStationsFromPrefs() {
  String json = preferences.getString("stations", "");
  if (json.length() > 0) {
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, json);
    if (!err) {
      JsonArray arr = doc.as<JsonArray>();
      numStations = 0;
      for (JsonObject obj : arr) {
        if (numStations >= MAX_STATIONS) break;
        strncpy(stations[numStations].name, obj["name"] | "", MAX_NAME_LEN);
        stations[numStations].name[MAX_NAME_LEN - 1] = 0;
        strncpy(stations[numStations].url, obj["url"] | "", MAX_URL_LEN);
        stations[numStations].url[MAX_URL_LEN - 1] = 0;
        numStations++;
      }
    }
  }
}

void printFreeHeap() {
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
}

int findFirstSupportedStation() {
  for (int i = 0; i < numStations; i++) {
    String url = stations[i].url;
    if (!url.startsWith("https://")) {
      return i;
    }
  }
  return -1; // No supported station found
}

