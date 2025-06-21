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

// ===== WiFi credentials =====
const char* ssid = "MIMA";
const char* password = "nikola18092015";

// ===== OLED setup =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
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

// Forward declarations
void connectWiFi();
void startStream(int stationIndex);
void showDisplay(const String &line1, const String &line2, const String &line3);
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *str);
void drawWiFiSignal(int x, int y);
void drawAudioBars(int x, int y);
void audioTask(void *param);
void displayTask(void *param);
void loadStationsFromPrefs();
void saveStationsToPrefs();


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


void setup() {
  // Disable serial debug to save resources
  Serial.begin(115200); // <-- disabled intentionally

  // Push CPU freq to max 240 MHz
  setCpuFrequencyMhz(240);

  delay(100);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // OLED init failed, hang
    while (true) delay(1000);
  }

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

  requestedStationIndex = currentStationIndex = preferences.getInt("currentIndex", 0);

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 1, NULL, 0);
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
    html += "<a href='/delete?index=" + String(i) + "'><button>Delete</button></a>";
    html += "<button onclick='editStation(" + String(i) + ")'>Edit</button>";
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

  if (mp3) {
    mp3->stop();
    delete mp3;
    mp3 = nullptr;
  }
  if (file) {
    delete file;
    file = nullptr;
  }

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
  if (strcmp(type, "StreamTitle") == 0) {
    xSemaphoreTake(titleMutex, portMAX_DELAY);
    currentTitle = String(str);
    xSemaphoreGive(titleMutex);
  }
}

void showDisplay(const String &line1, const String &line2, const String &line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  display.setCursor(0, 10);
  display.println(line2);
  display.setCursor(0, 20);
  display.println(line3);
  drawWiFiSignal(SCREEN_WIDTH - 20, 25);
  drawAudioBars(SCREEN_WIDTH - 35, 31);
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

void drawAudioBars(int x, int y) {
  for (int i = 0; i < 4; i++) {
    int height = ((i + audioBarState) % 4 + 1) * 2;
    display.fillRect(x + i * 4, y - height, 3, height, SSD1306_WHITE);
  }
}

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
        vTaskDelay(pdMS_TO_TICKS(1000));
        requestedStationIndex = currentStationIndex; // retry same station
      }
    } else if (millis() - lastReconnectAttempt > reconnectDelay) {
      requestedStationIndex = currentStationIndex; // reconnect
      lastReconnectAttempt = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
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
