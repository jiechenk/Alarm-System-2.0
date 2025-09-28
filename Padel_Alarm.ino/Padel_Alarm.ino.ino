#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// Watchdog timeout (30 seconds)
#define WDT_TIMEOUT 30

// Pin definitions for 4-channel relay (safer GPIO pins)
#define RELAY_1 4   // Field 1 - Safe GPIO
#define RELAY_2 5   // Field 2 - Safe GPIO
#define RELAY_3 18  // Field 3 - Safe GPIO
#define RELAY_4 19  // Field 4 - Safe GPIO

// WiFi AP settings
const char* AP_SSID = "FADEL_TIMER";
const char* AP_PASSWORD = "padel123";
const int AP_CHANNEL = 6;  // Less congested channel
const int AP_MAX_CONNECTIONS = 4;

WebServer server(80);
Preferences preferences;

// Timer data structure for each field
struct TimerData {
  unsigned long startTime;
  unsigned long duration;      // in seconds
  bool isRunning;
  bool isFinished;
  bool warningTriggered;
  String fieldName;
};

TimerData fields[4] = {
  {0, 0, false, false, false, "Field 1"},
  {0, 0, false, false, false, "Field 2"},
  {0, 0, false, false, false, "Field 3"},
  {0, 0, false, false, false, "Field 4"}
};

// Alarm configuration structure
struct AlarmConfig {
  int testDuration;        // Test alarm duration (seconds)
  int warningDuration;     // Warning alarm duration (seconds)
  int timeupDuration;      // Time up alarm duration (seconds)
  int warningTime;         // Warning time before end (seconds)
  int repeatCount;         // Number of alarm repetitions
  int repeatInterval;      // Interval between repetitions (seconds)
  bool enableWarning;      // Enable warning alarm
  bool enableRepeating;    // Enable repeating alarm
};

AlarmConfig alarmConfig = {3, 2, 8, 300, 3, 2, true, true}; // Default config

// Relay pins array
const int relayPins[4] = {RELAY_1, RELAY_2, RELAY_3, RELAY_4};

// Alarm state tracking
struct AlarmState {
  bool active;
  unsigned long startTime;
  int type; // 0=test, 1=warning, 2=timeup
  int currentRepeat;
  bool relayOn;
  unsigned long lastToggle;
};

AlarmState alarmStates[4] = {
  {false, 0, 0, 0, false, 0},
  {false, 0, 0, 0, false, 0},
  {false, 0, 0, 0, false, 0},
  {false, 0, 0, 0, false, 0}
};

// Memory monitoring
unsigned long lastMemoryCheck = 0;
const unsigned long MEMORY_CHECK_INTERVAL = 60000; // Check every minute

// Function declarations
void loadAlarmConfig();
void saveAlarmConfig();
void startAlarmSequence(int fieldIndex, int durationType);
void processAlarms();
void checkTimers();
void printMemoryStats();
bool validateFieldIndex(int index);
bool validateAlarmType(int type);
String getTimeString(unsigned long seconds);
String getStatusClass(int fieldIndex);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== FUTSAL TIMER SYSTEM v2.0 ===");
  Serial.println("Initializing system...");
  
  // Initialize Watchdog Timer
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  
  if (esp_task_wdt_init(&wdt_config) == ESP_OK) {
    esp_task_wdt_add(NULL);
    Serial.println("✓ Watchdog timer initialized");
  } else {
    Serial.println("✗ Watchdog timer failed to initialize");
  }
  
  // Initialize LittleFS
  if (LittleFS.begin(true)) {
    Serial.println("✓ LittleFS mounted successfully");
    
    // List files in LittleFS for debugging
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    Serial.println("Files in LittleFS:");
    while (file) {
      Serial.printf("  - %s (%d bytes)\n", file.name(), file.size());
      file = root.openNextFile();
    }
  } else {
    Serial.println("✗ LittleFS mount failed - using inline HTML");
  }
  
  // Load alarm configuration from preferences
  loadAlarmConfig();
  Serial.println("✓ Alarm configuration loaded");
  
  // Initialize relay pins
  for (int i = 0; i < 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // OFF (HIGH for active LOW relay)
  }
  Serial.println("✓ Relay pins initialized (GPIO: 4,5,18,19)");
  
  // Test all relays at startup
  Serial.println("Testing all relays...");
  for (int i = 0; i < 4; i++) {
    Serial.printf("  Testing relay %d (GPIO %d)...", i + 1, relayPins[i]);
    digitalWrite(relayPins[i], LOW);   // ON
    delay(200);
    digitalWrite(relayPins[i], HIGH);  // OFF
    delay(100);
    Serial.println(" OK");
  }
  Serial.println("✓ Relay test completed");
  
  // Setup WiFi Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAPdisconnect(true);
  delay(100);
  
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),    // IP
    IPAddress(192, 168, 4, 1),    // Gateway
    IPAddress(255, 255, 255, 0)   // Subnet
  );
  
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONNECTIONS);
  
  if (apStarted) {
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("✓ WiFi AP started successfully\n");
    Serial.printf("  SSID: %s\n", AP_SSID);
    Serial.printf("  Password: %s\n", AP_PASSWORD);
    Serial.printf("  IP Address: %s\n", apIP.toString().c_str());
    Serial.printf("  Channel: %d\n", AP_CHANNEL);
  } else {
    Serial.println("✗ WiFi AP failed to start");
  }
  
  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/start", HTTP_GET, handleStart);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/alarm", HTTP_GET, handleAlarm);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save-config", HTTP_POST, handleSaveConfig);
  server.on("/info", HTTP_GET, handleSystemInfo);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("✓ Web server started on port 80");
  
  // Print initial memory statistics
  printMemoryStats();
  
  Serial.println("=== SYSTEM READY ===");
  Serial.println("Connect to WiFi: " + String(AP_SSID));
  Serial.println("Password: " + String(AP_PASSWORD));
  Serial.println("Open browser: http://192.168.4.1");
  Serial.println("========================");
}

void loop() {
  // Reset watchdog timer
  esp_task_wdt_reset();
  
  // Handle web server clients
  server.handleClient();
  
  // Check and update timers
  checkTimers();
  
  // Process non-blocking alarms
  processAlarms();
  
  // Periodic memory check
  unsigned long currentTime = millis();
  if (currentTime - lastMemoryCheck > MEMORY_CHECK_INTERVAL) {
    lastMemoryCheck = currentTime;
    
    // Check memory usage
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 10000) { // Less than 10KB free
      Serial.printf("⚠ Low memory warning: %d bytes free\n", freeHeap);
    }
    
    // Check WiFi status
    int connectedClients = WiFi.softAPgetStationNum();
    Serial.printf("Status - Connected devices: %d, Free heap: %d bytes\n", 
                  connectedClients, freeHeap);
  }
  
  // Small delay to prevent excessive CPU usage
  delay(10);
}

bool validateFieldIndex(int index) {
  return (index >= 0 && index < 4);
}

bool validateAlarmType(int type) {
  return (type >= 0 && type <= 2);
}

void loadAlarmConfig() {
  preferences.begin("alarm_config", true); // Read-only
  
  alarmConfig.testDuration = preferences.getInt("testDur", 3);
  alarmConfig.warningDuration = preferences.getInt("warnDur", 2);
  alarmConfig.timeupDuration = preferences.getInt("timeupDur", 8);
  alarmConfig.warningTime = preferences.getInt("warnTime", 300);
  alarmConfig.repeatCount = preferences.getInt("repeatCnt", 3);
  alarmConfig.repeatInterval = preferences.getInt("repeatInt", 2);
  alarmConfig.enableWarning = preferences.getBool("enableWarn", true);
  alarmConfig.enableRepeating = preferences.getBool("enableRep", true);
  
  preferences.end();
  
  // Validate loaded values
  alarmConfig.testDuration = constrain(alarmConfig.testDuration, 1, 30);
  alarmConfig.warningDuration = constrain(alarmConfig.warningDuration, 1, 30);
  alarmConfig.timeupDuration = constrain(alarmConfig.timeupDuration, 1, 60);
  alarmConfig.warningTime = constrain(alarmConfig.warningTime, 30, 1800);
  alarmConfig.repeatCount = constrain(alarmConfig.repeatCount, 1, 10);
  alarmConfig.repeatInterval = constrain(alarmConfig.repeatInterval, 1, 10);
}

void saveAlarmConfig() {
  preferences.begin("alarm_config", false); // Read-write
  
  preferences.putInt("testDur", alarmConfig.testDuration);
  preferences.putInt("warnDur", alarmConfig.warningDuration);
  preferences.putInt("timeupDur", alarmConfig.timeupDuration);
  preferences.putInt("warnTime", alarmConfig.warningTime);
  preferences.putInt("repeatCnt", alarmConfig.repeatCount);
  preferences.putInt("repeatInt", alarmConfig.repeatInterval);
  preferences.putBool("enableWarn", alarmConfig.enableWarning);
  preferences.putBool("enableRep", alarmConfig.enableRepeating);
  
  preferences.end();
  
  Serial.println("Alarm configuration saved to preferences");
}

void startAlarmSequence(int fieldIndex, int durationType) {
  // Enhanced validation
  if (!validateFieldIndex(fieldIndex)) {
    Serial.printf("Error: Invalid field index %d (must be 0-3)\n", fieldIndex);
    return;
  }
  
  if (!validateAlarmType(durationType)) {
    Serial.printf("Error: Invalid alarm type %d (must be 0-2)\n", durationType);
    return;
  }
  
  // Stop any existing alarm for this field
  alarmStates[fieldIndex].active = false;
  digitalWrite(relayPins[fieldIndex], HIGH); // Turn OFF relay
  
  // Configure new alarm
  alarmStates[fieldIndex].active = true;
  alarmStates[fieldIndex].startTime = millis();
  alarmStates[fieldIndex].type = durationType;
  alarmStates[fieldIndex].currentRepeat = 0;
  alarmStates[fieldIndex].relayOn = false;
  alarmStates[fieldIndex].lastToggle = millis();
  
  const char* alarmTypes[] = {"Test", "Warning", "Time Up"};
  Serial.printf("Alarm started: %s - %s (GPIO %d)\n", 
                alarmTypes[durationType], 
                fields[fieldIndex].fieldName.c_str(),
                relayPins[fieldIndex]);
}

void processAlarms() {
  for (int i = 0; i < 4; i++) {
    if (!alarmStates[i].active) continue;
    
    unsigned long currentTime = millis();
    int duration, maxRepeats, interval;
    
    // Get alarm parameters based on type
    switch (alarmStates[i].type) {
      case 0: // Test alarm
        duration = alarmConfig.testDuration * 1000;
        maxRepeats = 1;
        interval = 0;
        break;
        
      case 1: // Warning alarm
        duration = alarmConfig.warningDuration * 1000;
        maxRepeats = alarmConfig.enableRepeating ? 2 : 1;
        interval = alarmConfig.repeatInterval * 1000;
        break;
        
      case 2: // Time up alarm
        duration = alarmConfig.timeupDuration * 1000;
        maxRepeats = alarmConfig.enableRepeating ? alarmConfig.repeatCount : 1;
        interval = alarmConfig.repeatInterval * 1000;
        break;
        
      default:
        Serial.printf("Error: Unknown alarm type %d for field %d\n", alarmStates[i].type, i);
        alarmStates[i].active = false;
        continue;
    }
    
    unsigned long elapsed = currentTime - alarmStates[i].startTime;
    unsigned long totalDuration = maxRepeats * duration + (maxRepeats - 1) * interval;
    
    // Check if alarm sequence is complete
    if (elapsed >= totalDuration) {
      alarmStates[i].active = false;
      digitalWrite(relayPins[i], HIGH); // Turn OFF relay
      Serial.printf("Alarm sequence completed - %s\n", fields[i].fieldName.c_str());
      continue;
    }
    
    // Calculate current position in alarm sequence
    unsigned long cycleTime = duration + interval;
    int currentCycle = elapsed / cycleTime;
    unsigned long cycleElapsed = elapsed % cycleTime;
    
    if (currentCycle < maxRepeats) {
      if (cycleElapsed < duration) {
        // Should be ON
        if (!alarmStates[i].relayOn) {
          digitalWrite(relayPins[i], LOW); // Turn ON relay
          alarmStates[i].relayOn = true;
        }
      } else {
        // Should be OFF (in interval between repeats)
        if (alarmStates[i].relayOn) {
          digitalWrite(relayPins[i], HIGH); // Turn OFF relay
          alarmStates[i].relayOn = false;
        }
      }
    }
  }
}

void checkTimers() {
  unsigned long currentTime = millis();
  
  // Handle timer overflow (every ~49 days)
  static unsigned long lastOverflowCheck = 0;
  if (currentTime < lastOverflowCheck) {
    Serial.println("Timer overflow detected - adjusting all running timers");
    for (int i = 0; i < 4; i++) {
      if (fields[i].isRunning) {
        fields[i].startTime = currentTime;
      }
    }
  }
  lastOverflowCheck = currentTime;
  
  for (int i = 0; i < 4; i++) {
    if (fields[i].isRunning && !fields[i].isFinished) {
      unsigned long elapsed = (currentTime - fields[i].startTime) / 1000; // Convert to seconds
      
      // Safety check for invalid elapsed time
      if (elapsed > fields[i].duration + 60) { // Allow 60 seconds grace period
        Serial.printf("Invalid timer detected for %s - auto stopping\n", fields[i].fieldName.c_str());
        fields[i].isRunning = false;
        fields[i].isFinished = true;
        continue;
      }
      
      unsigned long remaining = (elapsed >= fields[i].duration) ? 0 : fields[i].duration - elapsed;
      
      // Check for warning alarm trigger
      if (alarmConfig.enableWarning && 
          !fields[i].warningTriggered && 
          remaining <= alarmConfig.warningTime && 
          remaining > 0) {
        
        fields[i].warningTriggered = true;
        Serial.printf("Warning triggered - %s: %lu seconds remaining\n", 
                     fields[i].fieldName.c_str(), remaining);
        startAlarmSequence(i, 1); // Warning alarm
      }
      
      // Check for time up
      if (elapsed >= fields[i].duration) {
        fields[i].isRunning = false;
        fields[i].isFinished = true;
        
        Serial.printf("Time up! %s - Activating time up alarm\n", fields[i].fieldName.c_str());
        startAlarmSequence(i, 2); // Time up alarm
      }
    }
  }
}

String getTimeString(unsigned long seconds) {
  int hours = seconds / 3600;
  int minutes = (seconds % 3600) / 60;
  int secs = seconds % 60;
  
  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", hours, minutes, secs);
  return String(timeStr);
}

String getStatusClass(int fieldIndex) {
  if (fields[fieldIndex].isFinished) {
    return "finished";
  } else if (fields[fieldIndex].isRunning) {
    unsigned long currentTime = millis();
    unsigned long elapsed = (currentTime - fields[fieldIndex].startTime) / 1000;
    unsigned long remaining = (elapsed >= fields[fieldIndex].duration) ? 0 : fields[fieldIndex].duration - elapsed;
    
    if (alarmConfig.enableWarning && remaining <= alarmConfig.warningTime && remaining > 0) {
      return "warning";
    } else {
      return "running";
    }
  } else {
    return "idle";
  }
}

void printMemoryStats() {
  Serial.println("=== Memory Statistics ===");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Heap size: %d bytes\n", ESP.getHeapSize());
  Serial.printf("Min free heap: %d bytes\n", ESP.getMinFreeHeap());
  Serial.printf("Max alloc heap: %d bytes\n", ESP.getMaxAllocHeap());
  Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  Serial.println("========================");
}

// Web Server Handlers
void handleRoot() {
  if (LittleFS.exists("/index.html")) {
    File file = LittleFS.open("/index.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
    } else {
      server.send(500, "text/plain", "Failed to open index.html");
    }
  } else {
    // Fallback inline HTML
    String html = "<!DOCTYPE html><html><head><title>Futsal Timer</title></head><body>";
    html += "<h1>FUTSAL TIMER SYSTEM v2.0</h1>";
    html += "<p>HTML files not found in LittleFS. Please upload web files using data upload tool.</p>";
    html += "<h2>Available API Endpoints:</h2>";
    html += "<ul>";
    html += "<li>GET /status - Get current timer status</li>";
    html += "<li>GET /start?lap=0&durasi=3600 - Start timer (lap: 0-3, durasi: seconds)</li>";
    html += "<li>GET /stop?lap=0 - Stop timer (lap: 0-3)</li>";
    html += "<li>GET /alarm?lap=0 - Test alarm (lap: 0-3)</li>";
    html += "<li>GET /config - Get alarm configuration</li>";
    html += "<li>POST /save-config - Save alarm configuration</li>";
    html += "<li>GET /info - System information</li>";
    html += "</ul>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  }
}

void handleSettings() {
  if (LittleFS.exists("/settings.html")) {
    File file = LittleFS.open("/settings.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
    } else {
      server.send(500, "text/plain", "Failed to open settings.html");
    }
  } else {
    server.send(404, "text/plain", "Settings page not found - please upload web files");
  }
}

void handleStart() {
  if (!server.hasArg("lap") || !server.hasArg("durasi")) {
    server.send(400, "text/plain", "Missing parameters: lap and durasi required");
    return;
  }
  
  int fieldIndex = server.arg("lap").toInt();
  unsigned long duration = server.arg("durasi").toInt();
  
  // Input validation
  if (!validateFieldIndex(fieldIndex)) {
    server.send(400, "text/plain", "Invalid field index (must be 0-3)");
    return;
  }
  
  if (duration <= 0 || duration > 14400) { // Max 4 hours
    server.send(400, "text/plain", "Invalid duration (must be 1-14400 seconds)");
    return;
  }
  
  // Start the timer
  fields[fieldIndex].startTime = millis();
  fields[fieldIndex].duration = duration;
  fields[fieldIndex].isRunning = true;
  fields[fieldIndex].isFinished = false;
  fields[fieldIndex].warningTriggered = false;
  
  Serial.printf("Timer started - %s, Duration: %lu seconds (%s)\n", 
                fields[fieldIndex].fieldName.c_str(), 
                duration, 
                getTimeString(duration).c_str());
  
  server.send(200, "text/plain", "Timer started successfully");
}

void handleStop() {
  if (!server.hasArg("lap")) {
    server.send(400, "text/plain", "Missing parameter: lap required");
    return;
  }
  
  int fieldIndex = server.arg("lap").toInt();
  
  if (!validateFieldIndex(fieldIndex)) {
    server.send(400, "text/plain", "Invalid field index (must be 0-3)");
    return;
  }
  
  // Stop the timer and any active alarms
  fields[fieldIndex].isRunning = false;
  fields[fieldIndex].isFinished = false;
  fields[fieldIndex].duration = 0;
  fields[fieldIndex].warningTriggered = false;
  
  // Stop any active alarms for this field
  alarmStates[fieldIndex].active = false;
  digitalWrite(relayPins[fieldIndex], HIGH); // Turn OFF relay
  
  Serial.printf("Timer stopped - %s\n", fields[fieldIndex].fieldName.c_str());
  server.send(200, "text/plain", "Timer stopped successfully");
}

void handleAlarm() {
  if (!server.hasArg("lap")) {
    server.send(400, "text/plain", "Missing parameter: lap required");
    return;
  }
  
  int fieldIndex = server.arg("lap").toInt();
  
  if (!validateFieldIndex(fieldIndex)) {
    server.send(400, "text/plain", "Invalid field index (must be 0-3)");
    return;
  }
  
  startAlarmSequence(fieldIndex, 0); // Test alarm (type 0)
  Serial.printf("Manual test alarm activated - %s\n", fields[fieldIndex].fieldName.c_str());
  server.send(200, "text/plain", "Test alarm activated successfully");
}

void handleStatus() {
  StaticJsonDocument<1024> doc;
  unsigned long currentTime = millis();
  
  for (int i = 0; i < 4; i++) {
    String fieldKey = "lapangan" + String(i + 1);
    JsonObject fieldObj = doc.createNestedObject(fieldKey);
    
    if (fields[i].isFinished) {
      fieldObj["status"] = "Waktu Habis!";
      fieldObj["statusClass"] = "finished";
      fieldObj["timeRemaining"] = "00:00:00";
    }
    else if (fields[i].isRunning) {
      unsigned long elapsed = (currentTime - fields[i].startTime) / 1000;
      long remaining = fields[i].duration - elapsed;
      
      if (remaining <= 0) {
        remaining = 0;
      }
      
      // Check if in warning period
      if (alarmConfig.enableWarning && remaining <= alarmConfig.warningTime && remaining > 0) {
        fieldObj["status"] = "Warning! Sisa " + String(remaining/60) + " menit";
        fieldObj["statusClass"] = "warning";
      } else {
        fieldObj["status"] = "Sedang Jalan";
        fieldObj["statusClass"] = "running";
      }
      
      fieldObj["timeRemaining"] = getTimeString(remaining);
    }
    else {
      fieldObj["status"] = "Idle";
      fieldObj["statusClass"] = "idle";
      fieldObj["timeRemaining"] = "--:--:--";
    }
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleConfig() {
  StaticJsonDocument<512> doc;
  
  doc["testDuration"] = alarmConfig.testDuration;
  doc["warningDuration"] = alarmConfig.warningDuration;
  doc["timeupDuration"] = alarmConfig.timeupDuration;
  doc["warningTime"] = alarmConfig.warningTime;
  doc["repeatCount"] = alarmConfig.repeatCount;
  doc["repeatInterval"] = alarmConfig.repeatInterval;
  doc["enableWarning"] = alarmConfig.enableWarning;
  doc["enableRepeating"] = alarmConfig.enableRepeating;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSaveConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No JSON data received");
    return;
  }
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON data");
    Serial.printf("JSON parsing error: %s\n", error.c_str());
    return;
  }
  
  // Update alarm configuration with validation
  alarmConfig.testDuration = constrain(doc["testDuration"] | 3, 1, 30);
  alarmConfig.warningDuration = constrain(doc["warningDuration"] | 2, 1, 30);
  alarmConfig.timeupDuration = constrain(doc["timeupDuration"] | 8, 1, 60);
  alarmConfig.warningTime = constrain(doc["warningTime"] | 300, 30, 1800);
  alarmConfig.repeatCount = constrain(doc["repeatCount"] | 3, 1, 10);
  alarmConfig.repeatInterval = constrain(doc["repeatInterval"] | 2, 1, 10);
  alarmConfig.enableWarning = doc["enableWarning"] | true;
  alarmConfig.enableRepeating = doc["enableRepeating"] | true;
  
  // Save to preferences
  saveAlarmConfig();
  
  Serial.println("Alarm configuration updated and saved");
  server.send(200, "text/plain", "Configuration saved successfully");
}

void handleSystemInfo() {
  StaticJsonDocument<1024> doc;
  
  doc["version"] = "2.0";
  doc["uptime"] = millis() / 1000;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["heapSize"] = ESP.getHeapSize();
  doc["minFreeHeap"] = ESP.getMinFreeHeap();
  doc["chipModel"] = ESP.getChipModel();
  doc["chipRevision"] = ESP.getChipRevision();
  doc["cpuFreq"] = ESP.getCpuFreqMHz();
  doc["flashSize"] = ESP.getFlashChipSize();
  doc["connectedClients"] = WiFi.softAPgetStationNum();
  doc["apSSID"] = AP_SSID;
  doc["apIP"] = WiFi.softAPIP().toString();
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleNotFound() {
  // Try to serve static files from LittleFS
  String path = server.uri();
  if (path.endsWith("/")) {
    path += "index.html";
  }
  
  String contentType = "text/plain";
  if (path.endsWith(".html")) contentType = "text/html";
  else if (path.endsWith(".css")) contentType = "text/css";
  else if (path.endsWith(".js")) contentType = "application/javascript";
  else if (path.endsWith(".ico")) contentType = "image/x-icon";
  else if (path.endsWith(".png")) contentType = "image/png";
  else if (path.endsWith(".jpg")) contentType = "image/jpeg";
  
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    if (file) {
      server.streamFile(file, contentType);
      file.close();
      return;
    }
  }
  
  // File not found
  String message = "404 - File Not Found\n\n";
  message += "URI: " + server.uri() + "\n";
  message += "Method: " + String((server.method() == HTTP_GET) ? "GET" : "POST") + "\n";
  message += "Available endpoints:\n";
  message += "  GET / - Main page\n";
  message += "  GET /settings - Settings page\n";
  message += "  GET /status - Timer status\n";
  message += "  GET /info - System information\n";
  
  server.send(404, "text/plain", message);
  Serial.printf("404 Error: %s (Method: %s)\n", 
               server.uri().c_str(), 
               (server.method() == HTTP_GET) ? "GET" : "POST");
}
