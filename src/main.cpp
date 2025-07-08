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
#include <esp_pm.h>               // Power management
#include <esp_wifi.h>             // WiFi power management

// Override library buffer defaults - reduced for power savings
#define I2S_DMA_BUF_COUNT 8       // Reduced from 16
#define I2S_DMA_BUF_LEN 1024      // Reduced from 2048
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
void connectWiFiSafe();              // Add this
void printPowerStatus();             // Add this
void startStream(int stationIndex);
void showDisplay(const String &line1, const String &line2, const String &line3);
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *str);
void drawWiFiSignal(int x, int y);
//void drawAudioBars(int x, int y);
void audioTask(void *param);
void displayTask(void *param);
void loadStationsFromPrefs();
void saveStationsToPrefs();
int findFirstSupportedStation();

void moveStationUp(int idx);
void moveStationDown(int idx);
void moveStationFirst(int idx);
void moveStationLast(int idx);


// --- Convert stations array to JSON string for API ---
String stationsToJson() {  // *** API added ***
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < numStations; i++) {
    JsonObject obj = arr.add<JsonObject>();
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
    if (state == 0b10 && lastState == 0b00) encoderPos = encoderPos + 1;
    if (state == 0b01 && lastState == 0b00) encoderPos = encoderPos - 1;
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
    if (state == 0b10 && lastState == 0b00) volEncoderPos = volEncoderPos + 1;
    if (state == 0b01 && lastState == 0b00) volEncoderPos = volEncoderPos - 1;
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
  // Start with lower power consumption
  setCpuFrequencyMhz(80);  // Start at lower frequency
  
  // Initialize power management early
  esp_pm_config_t pm_config;
  pm_config.max_freq_mhz = 80;
  pm_config.min_freq_mhz = 10;
  pm_config.light_sleep_enable = true;
  esp_pm_configure(&pm_config);
  
  Serial.begin(115200);
  delay(1000);  // Allow power to stabilize
  
  Serial.println("=== ESP32 Radio Starting ===");
  Serial.println("Power-optimized boot sequence");
  
  // Initialize display first (lower power)
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed!");
    while (true) delay(1000);
  }
  showDisplay("Booting...", "Power stabilizing", "");
  
  // Allow additional power stabilization
  delay(2000);
  
  printDebugInfo();
  // Print memory usage info
  Serial.println("=== Memory Usage ===");
  printFlashInfo();
  printRamInfo();
  Serial.println("====================");

  // Initialize encoders
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
  
  // Initialize WiFi with lower power settings first
  showDisplay("Connecting WiFi", "Lower power mode", "");
  connectWiFiSafe();
  
  // Only increase CPU frequency after WiFi is stable
  showDisplay("WiFi Connected", "Optimizing performance", "");
  delay(1000);
  
  // Update power management for performance mode
  pm_config.max_freq_mhz = 240;
  pm_config.min_freq_mhz = 80;
  pm_config.light_sleep_enable = false;  // Disable for audio performance
  esp_pm_configure(&pm_config);
  
  setCpuFrequencyMhz(240);  // Now increase to full speed
  delay(500); // Allow CPU freq change to stabilize

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
  <title>ESP32 Radio Station</title>
  <style>
    * {
      margin: 0;
      padding: 0;
      box-sizing: border-box;
    }
    
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen,
        Ubuntu, Cantarell, "Open Sans", "Helvetica Neue", sans-serif;
      background: linear-gradient(135deg, #0f0f23 0%, #1a1a2e 100%);
      color: #e2e8f0;
      min-height: 100vh;
      line-height: 1.6;
    }
    
    .container {
      max-width: 800px;
      margin: 0 auto;
      padding: 20px;
    }
    
    .header {
      text-align: center;
      margin-bottom: 2rem;
      padding: 2rem 0;
      background: rgba(255, 255, 255, 0.05);
      border-radius: 20px;
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255, 255, 255, 0.1);
    }
    
    .header h1 {
      font-size: 2.5rem;
      font-weight: 700;
      margin-bottom: 0.5rem;
      background: linear-gradient(45deg, #667eea 0%, #764ba2 100%);
      background-clip: text;
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
    }
    
    .header p {
      font-size: 1.1rem;
      opacity: 0.8;
    }
    
    .section {
      background: rgba(255, 255, 255, 0.05);
      border-radius: 16px;
      padding: 1.5rem;
      margin-bottom: 1.5rem;
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255, 255, 255, 0.1);
      transition: all 0.3s ease;
    }
    
    .section:hover {
      transform: translateY(-2px);
      box-shadow: 0 8px 25px rgba(0, 0, 0, 0.3);
    }
    
    .section h2 {
      font-size: 1.4rem;
      margin-bottom: 1rem;
      color: #94a3b8;
      display: flex;
      align-items: center;
      gap: 0.5rem;
    }
    
    .section h2::before {
      content: "";
      width: 4px;
      height: 20px;
      background: linear-gradient(45deg, #667eea, #764ba2);
      border-radius: 2px;
    }
    
    .now-playing {
      background: linear-gradient(135deg, rgba(102, 126, 234, 0.1) 0%, rgba(118, 75, 162, 0.1) 100%);
    }
    
    .status-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
      gap: 1rem;
      margin-bottom: 1rem;
    }
    
    .status-item {
      background: rgba(255, 255, 255, 0.05);
      padding: 1rem;
      border-radius: 12px;
      border: 1px solid rgba(255, 255, 255, 0.1);
    }
    
    .status-label {
      font-size: 0.875rem;
      color: #94a3b8;
      margin-bottom: 0.25rem;
    }
    
    .status-value {
      font-size: 1.1rem;
      font-weight: 600;
      color: #e2e8f0;
    }
    
    .volume-control {
      margin: 1.5rem 0;
    }
    
    .volume-label {
      font-size: 1rem;
      margin-bottom: 0.75rem;
      color: #94a3b8;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    
    .volume-value {
      font-weight: 600;
      color: #667eea;
      font-size: 1.1rem;
    }
    
    input[type="range"] {
      width: 100%;
      height: 8px;
      border-radius: 4px;
      background: rgba(255, 255, 255, 0.1);
      outline: none;
      -webkit-appearance: none;
      appearance: none;
    }
    
    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: linear-gradient(45deg, #667eea, #764ba2);
      cursor: pointer;
      box-shadow: 0 2px 6px rgba(0, 0, 0, 0.3);
    }
    
    input[type="range"]::-moz-range-thumb {
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: linear-gradient(45deg, #667eea, #764ba2);
      cursor: pointer;
      border: none;
      box-shadow: 0 2px 6px rgba(0, 0, 0, 0.3);
    }
    
    .stations-grid {
      display: grid;
      gap: 1rem;
    }
    
    .station-card {
      background: rgba(255, 255, 255, 0.05);
      border-radius: 12px;
      padding: 1.25rem;
      border: 1px solid rgba(255, 255, 255, 0.1);
      transition: all 0.3s ease;
      position: relative;
      overflow: hidden;
    }
    
    .station-card::before {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 3px;
      background: linear-gradient(90deg, #667eea, #764ba2);
      transform: scaleX(0);
      transition: transform 0.3s ease;
    }
    
    .station-card:hover::before {
      transform: scaleX(1);
    }
    
    .station-card:hover {
      transform: translateY(-2px);
      background: rgba(255, 255, 255, 0.08);
      box-shadow: 0 8px 25px rgba(0, 0, 0, 0.3);
    }
    
    .station-header {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      margin-bottom: 1rem;
    }
    
    .station-info {
      flex: 1;
    }
    
    .station-number {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      width: 32px;
      height: 32px;
      background: linear-gradient(45deg, #667eea, #764ba2);
      color: white;
      border-radius: 50%;
      font-weight: 600;
      font-size: 0.875rem;
      margin-bottom: 0.5rem;
    }
    
    .station-name {
      font-size: 1.25rem;
      font-weight: 600;
      color: #e2e8f0;
      margin-bottom: 0.25rem;
    }
    
    .station-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 0.5rem;
      margin-top: 1rem;
    }
    
    .btn {
      padding: 0.5rem 1rem;
      border: none;
      border-radius: 8px;
      font-size: 0.875rem;
      font-weight: 500;
      cursor: pointer;
      transition: all 0.3s ease;
      text-decoration: none;
      display: inline-flex;
      align-items: center;
      gap: 0.5rem;
    }
    
    .btn-primary {
      background: linear-gradient(45deg, #667eea, #764ba2);
      color: white;
    }
    
    .btn-primary:hover {
      transform: translateY(-1px);
      box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
    }
    
    .btn-secondary {
      background: rgba(255, 255, 255, 0.1);
      color: #94a3b8;
      border: 1px solid rgba(255, 255, 255, 0.2);
    }
    
    .btn-secondary:hover {
      background: rgba(255, 255, 255, 0.2);
      color: #e2e8f0;
    }
    
    .btn-danger {
      background: rgba(239, 68, 68, 0.2);
      color: #fca5a5;
      border: 1px solid rgba(239, 68, 68, 0.3);
    }
    
    .btn-danger:hover {
      background: rgba(239, 68, 68, 0.3);
      color: #ffffff;
    }
    
    input[type="text"],
    input[type="url"] {
      width: 100%;
      padding: 0.75rem;
      margin: 0.5rem 0;
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.2);
      border-radius: 8px;
      color: #e2e8f0;
      font-size: 1rem;
      transition: all 0.3s ease;
    }
    
    input[type="text"]:focus,
    input[type="url"]:focus {
      outline: none;
      border-color: #667eea;
      background: rgba(255, 255, 255, 0.08);
      box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
    }
    
    label {
      font-weight: 600;
      display: block;
      margin-bottom: 0.5rem;
      color: #94a3b8;
    }
    
    .form-section {
      margin-top: 1.5rem;
    }
    
    .form-row {
      display: flex;
      gap: 1rem;
      margin-bottom: 1rem;
    }
    
    .form-row > div {
      flex: 1;
    }
    
    #editForm {
      background: rgba(255, 255, 255, 0.05);
      padding: 1.5rem;
      margin-top: 1.5rem;
      border-radius: 16px;
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255, 255, 255, 0.1);
      display: none;
    }
    
    .backup-actions {
      display: flex;
      gap: 1rem;
      flex-wrap: wrap;
    }
    
    @media (max-width: 768px) {
      .container {
        padding: 15px;
      }
      
      .header h1 {
        font-size: 2rem;
      }
      
      .status-grid {
        grid-template-columns: 1fr;
      }
      
      .station-actions {
        justify-content: center;
      }
      
      .form-row {
        flex-direction: column;
      }
      
      .backup-actions {
        justify-content: center;
      }
    }
  </style>
  <script>
async function exportStations() {
  let response = await fetch('/stations');
  if (response.ok) {
    let data = await response.text();
    let blob = new Blob([data], {type: "application/json"});
    let url = URL.createObjectURL(blob);
    let a = document.createElement('a');
    a.href = url;
    a.download = "stations_backup.json";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  } else {
    alert("Failed to export stations!");
  }
}

function importStations(input) {
  let file = input.files[0];
  if (!file) return;
  let reader = new FileReader();
  reader.onload = async function(e) {
    try {
      let json = e.target.result;
      let response = await fetch('/stations', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: json
      });
      if (response.ok) {
        alert("Stations imported! Rebooting...");
        location.reload();
      } else {
        alert("Import failed: " + await response.text());
      }
    } catch (err) {
      alert("Import error: " + err);
    }
  };
  reader.readAsText(file);
}
</script>
<script>
function submitEditForm(event) {
  event.preventDefault();
  const form = document.getElementById('editStationForm');
  const formData = new FormData(form);
  fetch('/update', {
    method: 'POST',
    body: new URLSearchParams(formData)
  })
  .then(response => response.json())
  .then(data => {
    if (data.status === "updated") {
      location.reload();
    } else {
      alert("Update failed: " + (data.error || "Unknown error"));
    }
  })
  .catch(err => alert("Update error: " + err));
  return false;
}
</script>
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
      document.getElementById('editUrl').value = document.getElementById('hiddenUrl' + i).innerText;
      document.getElementById('editIndex').value = i;
      document.getElementById('editForm').style.display = 'block';
      document.getElementById('editForm').scrollIntoView({ behavior: 'smooth' });
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
        
        // Update volume slider and label if volume changed from encoder
        if (data.volume !== undefined) {
          let volumeSlider = document.getElementById('volumeRange');
          let volumeLabel = document.getElementById('volLabel');
          if (volumeSlider && volumeLabel) {
            volumeSlider.value = data.volume;
            volumeLabel.innerText = data.volume;
          }
        }
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
  <div class="container">
    <div class="header">
      <h1>🎵 ESP32 Radio</h1>
      <p>Internet Radio Station Controller</p>
    </div>

    <div class="section now-playing">
      <h2>Now Playing</h2>
      <div class="status-grid">
        <div class="status-item">
          <div class="status-label">Current Track</div>
          <div class="status-value" id="title">Loading...</div>
        </div>
        <div class="status-item">
          <div class="status-label">Station</div>
          <div class="status-value" id="station">Loading...</div>
        </div>
        <div class="status-item">
          <div class="status-label">IP Address</div>
          <div class="status-value" id="ip">Loading...</div>
        </div>
      </div>
    </div>

    <div class="section">
      <h2>Volume Control</h2>
      <div class="volume-control">
        <div class="volume-label">
          <span>Volume</span>
          <span class="volume-value"><span id="volLabel">)rawliteral" + String((int)(currentVolume * 100)) + R"rawliteral(</span>%</span>
        </div>
        <input type="range" id="volumeRange" min="0" max="100" value=")rawliteral" + String((int)(currentVolume * 100)) + R"rawliteral(" oninput="setVolumeDebounced(this.value); document.getElementById('volLabel').innerText=this.value;" />
      </div>
    </div>

    <div class="section">
      <h2>Radio Stations</h2>
      <div class="stations-grid">)rawliteral";

  for (int i = 0; i < numStations; i++) {
    html += R"rawliteral(
        <div class="station-card">
          <div class="station-header">
            <div class="station-info">
              <div class="station-number">)rawliteral" + String(i + 1) + R"rawliteral(</div>
              <div class="station-name" id="name)rawliteral" + String(i) + R"rawliteral(">)rawliteral" + stations[i].name + R"rawliteral(</div>
            </div>
          </div>
          <div class="station-actions">
            <a href="/play?index=)rawliteral" + String(i) + R"rawliteral(" class="btn btn-primary">▶ Play</a>
            <button onclick="editStation()rawliteral" + String(i) + R"rawliteral()" class="btn btn-secondary">✏ Edit</button>
            <a href="/delete?index=)rawliteral" + String(i) + R"rawliteral(" class="btn btn-danger">🗑 Delete</a>
            <a href="/moveup?index=)rawliteral" + String(i) + R"rawliteral(" class="btn btn-secondary">↑</a>
            <a href="/movedown?index=)rawliteral" + String(i) + R"rawliteral(" class="btn btn-secondary">↓</a>
            <a href="/movefirst?index=)rawliteral" + String(i) + R"rawliteral(" class="btn btn-secondary">⤴</a>
            <a href="/movelast?index=)rawliteral" + String(i) + R"rawliteral(" class="btn btn-secondary">⤵</a>
          </div>
          <div style="display: none;" id="url)rawliteral" + String(i) + R"rawliteral(">)rawliteral" + stations[i].url + R"rawliteral(</div>
        </div>)rawliteral";
  }

  html += R"rawliteral(
      </div>
    </div>

    <div class="section" id="editForm">
      <h2>Edit Station</h2>
      <form id="editStationForm" action="/update" method="POST" onsubmit="return submitEditForm(event)">
        <input type="hidden" id="editIndex" name="index" />
        <div class="form-row">
          <div>
            <label for="editName">Station Name</label>
            <input type="text" id="editName" name="name" required placeholder="Enter station name" />
          </div>
        </div>
        <div class="form-row">
          <div>
            <label for="editUrl">Stream URL</label>
            <input type="url" id="editUrl" name="url" required placeholder="http://..." />
          </div>
        </div>
        <div class="station-actions">
          <button type="submit" class="btn btn-primary">💾 Update Station</button>
          <button type="button" onclick="cancelEdit()" class="btn btn-secondary">❌ Cancel</button>
        </div>
      </form>
    </div>

    <div class="section">
      <h2>Add New Station</h2>
      <form action="/add" method="POST">
        <div class="form-row">
          <div>
            <label for="name">Station Name</label>
            <input type="text" id="name" name="name" placeholder="Enter station name" required />
          </div>
        </div>
        <div class="form-row">
          <div>
            <label for="url">Stream URL</label>
            <input type="url" id="url" name="url" placeholder="http://stream.example.com:8000" required />
          </div>
        </div>
        <div class="station-actions">
          <button type="submit" class="btn btn-primary">➕ Add Station</button>
        </div>
      </form>
    </div>

    <div class="section">
      <h2>Backup & Restore</h2>
      <div class="backup-actions">
        <button onclick="exportStations()" class="btn btn-secondary">📥 Export Stations</button>
        <input type="file" id="importFile" style="display:none" accept=".json" onchange="importStations(this)">
        <button onclick="document.getElementById('importFile').click()" class="btn btn-secondary">📤 Import Stations</button>
      </div>
    </div>
  </div>

  <div style="display: none;" id="hiddenUrls">)rawliteral";

  // Add hidden URL data for JavaScript access
  for (int i = 0; i < numStations; i++) {
    html += "<div id='hiddenUrl" + String(i) + "'>" + stations[i].url + "</div>";
  }

  html += R"rawliteral(
  </div>
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
        request->send(200, "application/json", "{\"status\":\"updated\"}");
        return;
      }
    }
    request->send(400, "application/json", "{\"error\":\"Invalid parameters\"}");
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
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, body);
      if(error){
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      // If it's an array, import all stations
      if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        int count = 0;
        for (JsonObject obj : arr) {
          if (!obj["name"].is<const char*>() || !obj["url"].is<const char*>()) continue;
          if (count >= MAX_STATIONS) break;
          strncpy(stations[count].name, obj["name"] | "", MAX_NAME_LEN);
          stations[count].name[MAX_NAME_LEN - 1] = 0;
          strncpy(stations[count].url, obj["url"] | "", MAX_URL_LEN);
          stations[count].url[MAX_URL_LEN - 1] = 0;
          count++;
        }
        numStations = count;
        saveStationsToPrefs();
        request->send(200, "application/json", "{\"status\":\"imported\"}");
        return;
      }

      // Fallback: single station object (legacy)
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
  JsonDocument doc;
  doc["title"] = currentTitle;
  doc["station"] = stationName;
  doc["ip"] = localIPStr;
  doc["volume"] = (int)(currentVolume * 100);  // Add volume percentage
  doc["muted"] = isMuted;                      // Add mute status
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
      JsonDocument doc;
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

  JsonDocument doc;
  doc["title"] = currentTitle;
  doc["station"] = stationName;
  doc["ip"] = localIPStr;
  doc["volume"] = (int)(currentVolume * 100);

  String json;
  serializeJson(doc, json);

  xSemaphoreGive(titleMutex);

  request->send(200, "application/json", json);
});



  server.on("/moveup", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("index")) {
    int idx = request->getParam("index")->value().toInt();
    moveStationUp(idx);
  }
  request->redirect("/");
});

server.on("/movedown", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("index")) {
    int idx = request->getParam("index")->value().toInt();
    moveStationDown(idx);
  }
  request->redirect("/");
});

server.on("/movefirst", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("index")) {
    int idx = request->getParam("index")->value().toInt();
    moveStationFirst(idx);
  }
  request->redirect("/");
});

server.on("/movelast", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("index")) {
    int idx = request->getParam("index")->value().toInt();
    moveStationLast(idx);
  }
  request->redirect("/");
});


  server.begin();
}

void loop() {
  // All work is done in tasks
  static unsigned long lastDebug = 0;
  static unsigned long lastPowerCheck = 0;
  
  if (millis() - lastDebug > 10000) {  // Every 10 seconds
    unsigned long uptimeSec = millis() / 1000;
    unsigned int h = uptimeSec / 3600;
    unsigned int m = (uptimeSec % 3600) / 60;
    unsigned int s = uptimeSec % 60;
    Serial.printf("Uptime: %02u:%02u:%02u\n", h, m, s);
    lastDebug = millis();
  }
  
  // Print power status every 30 seconds for debugging
  if (millis() - lastPowerCheck > 30000) {
    printPowerStatus();
    lastPowerCheck = millis();
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

// Add a safer WiFi connection function
void connectWiFiSafe() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // Enable sleep mode initially for lower power
  WiFi.setTxPower(WIFI_POWER_11dBm);  // Start with lower TX power
  
  showDisplay("WiFi Starting", "Low power mode", "");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  const int maxAttempts = 60; // 30 seconds timeout
  
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    vTaskDelay(pdMS_TO_TICKS(500));
    attempts++;
    
    // Show progress
    if (attempts % 6 == 0) {  // Every 3 seconds
      showDisplay("WiFi Connecting", "Attempt " + String(attempts/2), ssid);
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    // Now optimize for performance
    WiFi.setSleep(false);  // Disable sleep for better performance
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Increase TX power
    localIPStr = WiFi.localIP().toString();
    Serial.printf("WiFi Connected! IP: %s\n", localIPStr.c_str());
    Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
    showDisplay("WiFi Connected", WiFi.SSID(), localIPStr);
    delay(1000); // Allow settings to stabilize
  } else {
    Serial.println("WiFi connection failed! Check credentials and signal strength.");
    showDisplay("WiFi Failed", "Check credentials", "Retrying...");
    delay(5000);
    ESP.restart(); // Restart if WiFi fails
  }
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
}*/


void audioTask(void *param) {
  while (true) {
    if (requestedStationIndex != -1) {
      int newIndex = requestedStationIndex;
      requestedStationIndex = -1;
      Serial.printf("[AudioTask] Switching to station %d\n", newIndex);
      startStream(newIndex);
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[AudioTask] WiFi disconnected, reconnecting...");
      WiFi.disconnect();
      connectWiFi();
      requestedStationIndex = currentStationIndex;
    }

    if (mp3 && mp3->isRunning()) {
      if (!mp3->loop()) {
        Serial.println("[AudioTask] Stream ended, restarting...");
        mp3->stop();
        vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds before retry
        requestedStationIndex = currentStationIndex; // retry same station
      }
    } else if (millis() - lastReconnectAttempt > reconnectDelay) {
      Serial.println("[AudioTask] No audio running, attempting reconnect...");
      requestedStationIndex = currentStationIndex; // reconnect
      lastReconnectAttempt = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // Increased from 2ms to reduce CPU load
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
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < numStations; i++) {
    JsonObject obj = arr.add<JsonObject>();
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
    JsonDocument doc;
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

void moveStationUp(int idx) {
  if (idx > 0 && idx < numStations) {
    RadioStation tmp = stations[idx];
    stations[idx] = stations[idx - 1];
    stations[idx - 1] = tmp;
    saveStationsToPrefs();
  }
}

void moveStationDown(int idx) {
  if (idx >= 0 && idx < numStations - 1) {
    RadioStation tmp = stations[idx];
    stations[idx] = stations[idx + 1];
    stations[idx + 1] = tmp;
    saveStationsToPrefs();
  }
}

void moveStationFirst(int idx) {
  if (idx > 0 && idx < numStations) {
    RadioStation tmp = stations[idx];
    for (int i = idx; i > 0; i--) {
      stations[i] = stations[i - 1];
    }
    stations[0] = tmp;
    saveStationsToPrefs();
  }
}

void moveStationLast(int idx) {
  if (idx >= 0 && idx < numStations - 1) {
    RadioStation tmp = stations[idx];
    for (int i = idx; i < numStations - 1; i++) {
      stations[i] = stations[i + 1];
    }
    stations[numStations - 1] = tmp;
    saveStationsToPrefs();
  }
}

// Add power monitoring function
void printPowerStatus() {
  Serial.println("\n=== Power Status ===");
  Serial.printf("CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Free Heap: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("WiFi Power Save: %s\n", WiFi.getSleep() ? "ON" : "OFF");
  
  // Get WiFi power level - simplified approach
  Serial.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
  }
  
  // Get current consumption estimate
  int totalCurrent = 50; // Base ESP32 current
  totalCurrent += ESP.getCpuFreqMHz() / 10; // Rough CPU current estimate
  if (!WiFi.getSleep()) totalCurrent += 120; // WiFi active
  if (mp3 && mp3->isRunning()) totalCurrent += 80; // Audio processing
  
  Serial.printf("Estimated Current: ~%d mA\n", totalCurrent);
  Serial.println("=====================\n");
}



