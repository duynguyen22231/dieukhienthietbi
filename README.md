#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include "config.h"

#define DHTTYPE DHT11
DHT dht1(DHT1_PIN, DHTTYPE);
DHT dht2(DHT2_PIN, DHTTYPE);

const char* ntpServers[] = {"time.google.com", "pool.ntp.org", "time.nist.gov", "asia.pool.ntp.org"};
int currentNTPServerIndex = 0;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServers[currentNTPServerIndex], 7 * 3600, 60000);

AsyncWebServer server(80);

String devices[8] = {"maybom", "vantren", "vanduoi", "den1", "quat1", "den2", "quat2", "buzzer"};
int devicePins[8] = {MAYBOM_PIN, VANTREN_PIN, VANDUOI_PIN, DEN1_PIN, QUAT1_PIN, DEN2_PIN, QUAT2_PIN, BUZZER_PIN};
int deviceStatuses[8] = {0, 0, 0, 0, 0, 0, 0, 0};
bool manualOverride[8] = {false, false, false, false, false, false, false, false};
unsigned long lastManualOverride[8] = {0};

unsigned long startMillis = 0;
unsigned long lastSensorUpdate = 0;
unsigned long lastControlCheck = 0;
unsigned long lastScheduleUpdate = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastSyncMillis = 0;
unsigned long lastSyncSeconds = 0;
unsigned long lampOnMillis = 0;
bool lampOnByTemp = false;
bool ntpInProgress = false;
String mcuId = "mcu_001";
String currentDate;

struct TimeBackup {
  unsigned long lastSyncSeconds;
  unsigned long lastSyncMillis;
  char lastSyncDate[11];
};
TimeBackup timeBackup;

struct Garden {
  int garden_id;
  int garden_number;
};
Garden gardens[10];
int gardenCount = 0;

struct Schedule {
  int id;
  String device_name;
  int action;
  unsigned long secondsSinceStart;
  unsigned long end_secondsSinceStart;
  bool is_range;
  bool executed;
  String mcu_id;
  String date;
  int garden_id;
  int garden_number;
};
Schedule schedules[20];
int scheduleCount = 0;

void connectToWiFi();
void checkWiFi();
void syncTime();
void sendSensorData();
void checkControlCommand();
void handleControlRequest(AsyncWebServerRequest *request);
void autoControlDevices();
float readWaterLevel();
void checkSchedules();
void loadSchedules();
void getGardenId();
String getCurrentDate();
int readRainSensor(); // Thêm khai báo hàm mới

void setup() {
  Serial.begin(115200);
  delay(100);

  if (!EEPROM.begin(512)) {
    Serial.println("Lỗi khởi tạo EEPROM!");
    while (true);
  }
  EEPROM.get(0, timeBackup);
  if (timeBackup.lastSyncDate[0] != '\0') {
    lastSyncSeconds = timeBackup.lastSyncSeconds;
    lastSyncMillis = timeBackup.lastSyncMillis;
    currentDate = String(timeBackup.lastSyncDate);
    Serial.println("Khôi phục thời gian từ EEPROM: " + currentDate + " " +
                   String(lastSyncSeconds / 3600) + ":" +
                   String((lastSyncSeconds % 3600) / 60 < 10 ? "0" : "") + String((lastSyncSeconds % 3600) / 60) + ":" +
                   String(lastSyncSeconds % 60 < 10 ? "0" : "") + String(lastSyncSeconds % 60));
  } else {
    currentDate = "1970-01-01";
  }

  for (int i = 0; i < 8; i++) {
    pinMode(devicePins[i], OUTPUT);
    digitalWrite(devicePins[i], HIGH);
  }

  Serial.println("Test còi...");
  digitalWrite(BUZZER_PIN, LOW);
  delay(500);
  digitalWrite(BUZZER_PIN, HIGH);
  Serial.println("Test còi hoàn tất");

  pinMode(SOIL_MOISTURE1_PIN, INPUT);
  pinMode(SOIL_MOISTURE2_PIN, INPUT);
  pinMode(RAIN_SENSOR_PIN, INPUT);
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);

  dht1.begin();
  dht2.begin();

  connectToWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ntpIP;
    if (WiFi.hostByName(ntpServers[0], ntpIP)) {
      Serial.println("DNS giải quyết " + String(ntpServers[0]) + " thành: " + ntpIP.toString());
    } else {
      Serial.println("Lỗi DNS: Không thể giải quyết " + String(ntpServers[0]));
    }
  }
  timeClient.begin();
  lastSyncMillis = millis();
  syncTime();
  getGardenId();
  startMillis = millis();
  Serial.println("startMillis: " + String(startMillis));
  loadSchedules();

  server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request) {
    handleControlRequest(request);
  });

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  timeClient.update();
  checkWiFi();

  unsigned long currentMillis = millis();

  if (currentMillis - lastSensorUpdate >= 10000) {
    lastSensorUpdate = currentMillis;
    sendSensorData();
    autoControlDevices();
  }

  if (currentMillis - lastControlCheck >= 5000) {
    lastControlCheck = currentMillis;
    checkControlCommand();
  }

  if (currentMillis - lastScheduleUpdate >= 60000) {
    lastScheduleUpdate = currentMillis;
    syncTime();
    loadSchedules();
  }

  if (currentMillis - lastScheduleCheck >= 2000) {
    lastScheduleCheck = currentMillis;
    checkSchedules();
  }
}

int readRainSensor() {
  const int NUM_READINGS = 10;
  long sum = 0;
  for (int i = 0; i < NUM_READINGS; i++) {
    sum += analogRead(RAIN_SENSOR_PIN);
    delay(10);
  }
  int rain = sum / NUM_READINGS;
  if (rain < 0 || rain > 4095) rain = 4095;
  return rain;
}

void connectToWiFi() {
  Serial.println("Kết nối WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nĐã kết nối! IP: " + WiFi.localIP().toString());
    Serial.println("RSSI: " + String(WiFi.RSSI()));
  } else {
    Serial.println("\nKết nối WiFi thất bại sau 60 lần thử, RSSI: " + String(WiFi.RSSI()));
    digitalWrite(BUZZER_PIN, LOW);
    delay(1000);
    digitalWrite(BUZZER_PIN, HIGH);
    ESP.restart();
  }
}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi ngắt kết nối, đang kết nối lại...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 60) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nĐã kết nối lại! IP: " + WiFi.localIP().toString());
      Serial.println("RSSI: " + String(WiFi.RSSI()));
      timeClient.begin();
    } else {
      Serial.println("\nKết nối lại WiFi thất bại sau 60 lần thử, RSSI: " + String(WiFi.RSSI()));
      digitalWrite(BUZZER_PIN, LOW);
      delay(500);
      digitalWrite(BUZZER_PIN, HIGH);
    }
  }
}

void syncTime() {
  if (ntpInProgress) return;
  ntpInProgress = true;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Không thể đồng bộ thời gian: WiFi ngắt kết nối");
    ntpInProgress = false;
    return;
  }

  int attempts = 0;
  bool timeSynced = false;

  while (attempts < 5 && !timeSynced && currentNTPServerIndex < 4) {
    timeClient.setPoolServerName(ntpServers[currentNTPServerIndex]);
    if (timeClient.forceUpdate()) {
      timeSynced = true;
      unsigned long currentEpoch = timeClient.getEpochTime();
      unsigned long secondsToday = currentEpoch % 86400;
      startMillis = millis() - (secondsToday * 1000);
      lastSyncMillis = millis();
      lastSyncSeconds = secondsToday;
      currentDate = getCurrentDate();

      timeBackup.lastSyncSeconds = secondsToday;
      timeBackup.lastSyncMillis = millis();
      strncpy(timeBackup.lastSyncDate, currentDate.c_str(), 11);
      EEPROM.put(0, timeBackup);
      if (!EEPROM.commit()) {
        Serial.println("Lỗi lưu EEPROM!");
      }

      Serial.println("Đồng bộ thời gian thành công với " + String(ntpServers[currentNTPServerIndex]) +
                     ": " + currentDate + " " + timeClient.getFormattedTime() + " (GMT+7)");
      Serial.println("startMillis mới: " + String(startMillis));

      loadSchedules();
      checkSchedules();
    } else {
      attempts++;
      Serial.println("Lỗi đồng bộ thời gian với " + String(ntpServers[currentNTPServerIndex]) +
                     ", thử lại lần " + String(attempts));
      delay(2000);
      if (attempts >= 5) {
        currentNTPServerIndex++;
        attempts = 0;
        if (currentNTPServerIndex >= 4) currentNTPServerIndex = 0;
        Serial.println("Chuyển sang máy chủ NTP: " + String(ntpServers[currentNTPServerIndex]));
      }
    }
  }

  if (!timeSynced) {
    Serial.println("Không thể đồng bộ thời gian sau 5 lần thử với tất cả máy chủ NTP");
    unsigned long elapsedMillis = millis() - lastSyncMillis;
    unsigned long elapsedSeconds = elapsedMillis / 1000;
    unsigned long newSeconds = lastSyncSeconds + elapsedSeconds;
    if (newSeconds >= 86400) {
      int days = newSeconds / 86400;
      newSeconds %= 86400;
      int year, month, day;
      sscanf(currentDate.c_str(), "%d-%d-%d", &year, &month, &day);
      day += days;
      int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) daysInMonth[1] = 29;
      if (day > daysInMonth[month - 1]) {
        day -= daysInMonth[month - 1];
        month++;
        if (month > 12) {
          month = 1;
          year++;
        }
      }
      char newDate[11];
      snprintf(newDate, sizeof(newDate), "%04d-%02d-%02d", year, month, day);
      currentDate = String(newDate);
    }
    lastSyncSeconds = newSeconds;
    lastSyncMillis = millis();
    Serial.println("Thời gian cục bộ: " + currentDate + " " +
                   String(lastSyncSeconds / 3600) + ":" +
                   String((lastSyncSeconds % 3600) / 60 < 10 ? "0" : "") + String((lastSyncSeconds % 3600) / 60) + ":" +
                   String(lastSyncSeconds % 60 < 10 ? "0" : "") + String(lastSyncSeconds % 60));
  }
  ntpInProgress = false;
}

String getCurrentDate() {
  time_t now = timeClient.getEpochTime();
  struct tm *timeinfo = localtime(&now);
  char dateBuffer[11];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", timeinfo);
  return String(dateBuffer);
}

void getGardenId() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Không thể lấy danh sách garden_id: WiFi ngắt kết nối");
    gardenCount = 0;
    return;
  }

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(SENSOR_CONTROLLER_URL);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(256);
  doc["action"] = "get_garden_assignments";
  doc["mcu_id"] = mcuId;
  String requestBody;
  serializeJson(doc, requestBody);

  Serial.println("Gửi yêu cầu lấy danh sách garden_id: " + requestBody);

  int httpCode = http.POST(requestBody);
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(1024);
    DeserializationError error = deserializeJson(responseDoc, response);
    if (error) {
      Serial.println("Lỗi phân tích JSON danh sách garden_id: " + String(error.c_str()));
      gardenCount = 0;
    } else if (responseDoc["success"]) {
      gardenCount = 0;
      JsonArray data = responseDoc["data"];
      for (JsonObject assignment : data) {
        if (gardenCount < 10) {
          gardens[gardenCount].garden_id = assignment["garden_id"].as<int>();
          gardens[gardenCount].garden_number = assignment["garden_number"].as<int>();
          Serial.println("Đã lấy garden_id: " + String(gardens[gardenCount].garden_id) +
                         ", garden_number: " + String(gardens[gardenCount].garden_number));
          gardenCount++;
        }
      }
      Serial.println("Tải thành công " + String(gardenCount) + " garden assignments");
    } else {
      Serial.println("Lấy danh sách garden_id thất bại: " + responseDoc["message"].as<String>());
      gardenCount = 0;
    }
  } else {
    Serial.println("Lấy danh sách garden_id thất bại, mã HTTP: " + String(httpCode));
    gardenCount = 0;
  }
  http.end();
}

void loadSchedules() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Không thể tải lịch trình: WiFi ngắt kết nối");
    return;
  }

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(SENSOR_CONTROLLER_URL);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(256);
  doc["action"] = "get_schedules_by_mcu";
  doc["mcu_id"] = mcuId;
  String requestBody;
  serializeJson(doc, requestBody);

  Serial.println("Gửi yêu cầu tải lịch trình: " + requestBody);

  int httpCode = http.POST(requestBody);
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    Serial.println("Phản hồi get_schedules: " + response);
    DynamicJsonDocument responseDoc(2048);
    DeserializationError error = deserializeJson(responseDoc, response);
    if (error) {
      Serial.println("Lỗi phân tích JSON lịch trình: " + String(error.c_str()));
      return;
    }
    if (responseDoc["success"]) {
      scheduleCount = 0;
      JsonArray data = responseDoc["data"];
      for (JsonObject schedule : data) {
        if (scheduleCount < 20) {
          String schedule_date = schedule["date"].as<String>();
          String mcu_id = schedule["mcu_id"].as<String>();
          if (mcu_id == mcuId) {
            String start_time = schedule["time"].as<String>();
            String end_time = schedule["end_time"].as<String>();
            int is_range = schedule["is_range"].as<int>();
            int garden_id = schedule["garden_id"].as<int>();
            int garden_number = 0;
            for (int i = 0; i < gardenCount; i++) {
              if (gardens[i].garden_id == garden_id) {
                garden_number = gardens[i].garden_number;
                break;
              }
            }
            if (garden_number == 0) continue;

            int hour, minute, second;
            sscanf(start_time.c_str(), "%d:%d:%d", &hour, &minute, &second);
            unsigned long secondsSinceStart = hour * 3600 + minute * 60 + second;
            unsigned long end_secondsSinceStart = secondsSinceStart;
            if (is_range && end_time != "") {
              sscanf(end_time.c_str(), "%d:%d:%d", &hour, &minute, &second);
              end_secondsSinceStart = hour * 3600 + minute * 60 + second;
            }

            schedules[scheduleCount].id = schedule["id"].as<int>();
            schedules[scheduleCount].device_name = schedule["device_name"].as<String>();
            schedules[scheduleCount].action = schedule["action"].as<int>();
            schedules[scheduleCount].secondsSinceStart = secondsSinceStart;
            schedules[scheduleCount].end_secondsSinceStart = end_secondsSinceStart;
            schedules[scheduleCount].is_range = is_range;
            schedules[scheduleCount].executed = false;
            schedules[scheduleCount].mcu_id = mcu_id;
            schedules[scheduleCount].date = schedule_date;
            schedules[scheduleCount].garden_id = garden_id;
            schedules[scheduleCount].garden_number = garden_number;

            Serial.println("Đã tải lịch trình " + String(scheduleCount + 1) + ": " +
                           schedules[scheduleCount].device_name + " - " +
                           schedule_date + " " + start_time +
                           (is_range ? " đến " + end_time : "") +
                           " (action: " + schedules[scheduleCount].action +
                           ", garden_id: " + String(garden_id) +
                           ", garden_number: " + String(garden_number) + ")");
            scheduleCount++;
          }
        }
      }
      Serial.println("Tải thành công " + String(scheduleCount) + " lịch trình");
    } else {
      Serial.println("Tải lịch trình thất bại: " + responseDoc["message"].as<String>());
    }
  } else {
    Serial.println("Tải lịch trình thất bại, mã HTTP: " + String(httpCode));
  }
  http.end();
}

void checkSchedules() {
  unsigned long currentSeconds = (millis() - startMillis) / 1000;
  String currentDate = getCurrentDate();

  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].executed && schedules[i].date == currentDate) {
      unsigned long scheduleStart = schedules[i].secondsSinceStart;
      unsigned long scheduleEnd = schedules[i].is_range ? schedules[i].end_secondsSinceStart : scheduleStart + 10;
      if (currentSeconds >= scheduleStart && currentSeconds <= scheduleEnd) {
        for (int j = 0; j < 8; j++) {
          if (devices[j] == schedules[i].device_name &&
              ((schedules[i].garden_number == 1 && (devices[j] == "den1" || devices[j] == "quat1")) ||
               (schedules[i].garden_number == 2 && (devices[j] == "den2" || devices[j] == "quat2")))) {
            manualOverride[j] = false;
            deviceStatuses[j] = schedules[i].action;
            digitalWrite(devicePins[j], schedules[i].action == 1 ? LOW : HIGH);
            Serial.println("Thực thi lịch trình: " + schedules[i].device_name +
                           " -> " + String(schedules[i].action) +
                           " (ID: " + String(schedules[i].id) +
                           ", garden_number: " + String(schedules[i].garden_number) + ")");
            digitalWrite(BUZZER_PIN, LOW);
            delay(200);
            digitalWrite(BUZZER_PIN, HIGH);
            if (!schedules[i].is_range) {
              schedules[i].executed = true;
            }
          }
        }
      } else if (currentSeconds > scheduleEnd) {
        schedules[i].executed = true;
        for (int j = 0; j < 8; j++) {
          if (devices[j] == schedules[i].device_name &&
              ((schedules[i].garden_number == 1 && (devices[j] == "den1" || devices[j] == "quat1")) ||
               (schedules[i].garden_number == 2 && (devices[j] == "den2" || devices[j] == "quat2")))) {
            manualOverride[j] = false;
          }
        }
        Serial.println("Lịch trình đã quá hạn: " + schedules[i].device_name +
                       " (ID: " + String(schedules[i].id) +
                       ", garden_number: " + String(schedules[i].garden_number) + ")");
      }
    }
  }
}

void autoControlDevices() {
  int soil1 = analogRead(SOIL_MOISTURE1_PIN);
  int soil2 = analogRead(SOIL_MOISTURE2_PIN);
  if (soil1 < 0 || soil1 > 4095) soil1 = 4095;
  if (soil2 < 0 || soil2 > 4095) soil2 = 4095;
  float soil1_percent = map(soil1, 4095, 0, 0, 100);
  float soil2_percent = map(soil2, 4095, 0, 0, 100);
  int rain = readRainSensor();
  float waterLevel = readWaterLevel();
  unsigned long currentMillis = millis();

  Serial.println("Vườn 1: raw=" + String(soil1) + ", độ ẩm=" + String(soil1_percent, 1) + "%");
  Serial.println("Vườn 2: raw=" + String(soil2) + ", độ ẩm=" + String(soil2_percent, 1) + "%");
  Serial.println("Mưa: raw=" + String(rain) + ", trạng thái=" + String(rain < 2000 ? "Đang mưa" : "Không mưa"));
  Serial.println("Mực nước: waterLevel=" + String(waterLevel, 1) + " cm");

  bool deviceInSchedule[8] = {false};
  unsigned long elapsedSeconds = (millis() - startMillis) / 1000;
  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].executed && schedules[i].date == currentDate &&
        elapsedSeconds >= schedules[i].secondsSinceStart &&
        (schedules[i].is_range ? elapsedSeconds <= schedules[i].end_secondsSinceStart : elapsedSeconds <= schedules[i].secondsSinceStart + 10)) {
      for (int j = 0; j < 8; j++) {
        if (devices[j] == schedules[i].device_name &&
            ((schedules[i].garden_number == 1 && (devices[j] == "den1" || devices[j] == "quat1")) ||
             (schedules[i].garden_number == 2 && (devices[j] == "den2" || devices[j] == "quat2")))) {
          deviceInSchedule[j] = true;
        }
      }
    }
  }

  for (int i = 0; i < gardenCount; i++) {
    int garden_number = gardens[i].garden_number;
    float temp = (garden_number == 1) ? dht1.readTemperature() : dht2.readTemperature();
    Serial.println("Nhiệt độ vườn " + String(garden_number) + ": " + String(isnan(temp) ? "N/A" : String(temp, 1)) + "°C");

    int lampIndex = (garden_number == 1) ? 3 : 5;
    if (!deviceInSchedule[lampIndex] && !manualOverride[lampIndex]) {
      if (!isnan(temp) && temp < 25 && !lampOnByTemp && deviceStatuses[lampIndex] == 0) {
        deviceStatuses[lampIndex] = 1;
        digitalWrite(devicePins[lampIndex], LOW);
        lampOnByTemp = true;
        lampOnMillis = currentMillis;
        Serial.println("Bật " + devices[lampIndex] + " (vườn " + String(garden_number) + ") do nhiệt độ thấp: " + String(temp, 1) + "°C");
        digitalWrite(BUZZER_PIN, LOW);
        delay(200);
        digitalWrite(BUZZER_PIN, HIGH);
      } else if (lampOnByTemp && (!isnan(temp) && (temp >= 25 || (currentMillis - lampOnMillis >= 15 * 60 * 1000)))) {
        deviceStatuses[lampIndex] = 0;
        digitalWrite(devicePins[lampIndex], HIGH);
        lampOnByTemp = false;
        Serial.println("Tắt " + devices[lampIndex] + " (vườn " + String(garden_number) + ") do nhiệt độ: " + String(temp, 1) + "°C hoặc hết thời gian");
        digitalWrite(BUZZER_PIN, LOW);
        delay(200);
        digitalWrite(BUZZER_PIN, HIGH);
      }
    }

    if (!deviceInSchedule[0] && !deviceInSchedule[1] && !deviceInSchedule[2] && !deviceInSchedule[7] &&
        !manualOverride[0] && !manualOverride[1] && !manualOverride[2] && !manualOverride[7]) {
      if (rain < 2000) {
        deviceStatuses[0] = 0;
        deviceStatuses[1] = 0;
        deviceStatuses[2] = 0;
        deviceStatuses[7] = 0;
        Serial.println("Tắt maybom, van do đang mưa: rain=" + String(rain));
      } else if (waterLevel < 2.0) {
        deviceStatuses[0] = 0;
        deviceStatuses[1] = 0;
        deviceStatuses[2] = 0;
        deviceStatuses[7] = 1;
        Serial.println("Tắt maybom, van, bật còi do mực nước thấp: waterLevel=" + String(waterLevel, 1));
      } else {
        bool shouldWater = (garden_number == 1 && soil1_percent < 60) || (garden_number == 2 && soil2_percent < 75);
        deviceStatuses[0] = shouldWater ? 1 : 0;
        deviceStatuses[1] = (garden_number == 1 && soil1_percent < 60) ? 1 : 0;
        deviceStatuses[2] = (garden_number == 2 && soil2_percent < 75) ? 1 : 0;
        deviceStatuses[7] = 0;
        if (shouldWater) {
          Serial.println("Bật maybom, van do đất khô (vườn " + String(garden_number) + "): soil1_percent=" + String(soil1_percent, 1) + "%, soil2_percent=" + String(soil2_percent, 1) + "%, waterLevel=" + String(waterLevel, 1));
        } else {
          Serial.println("Tắt maybom, van do đất đủ ẩm hoặc không phải vườn điều khiển (vườn " + String(garden_number) + "): soil1_percent=" + String(soil1_percent, 1) + "%, soil2_percent=" + String(soil2_percent, 1) + "%, waterLevel=" + String(waterLevel, 1));
        }
      }
    }
  }

  for (int i = 0; i < 8; i++) {
    digitalWrite(devicePins[i], deviceStatuses[i] == 1 ? LOW : HIGH);
  }

  Serial.println("Điều khiển tự động: maybom=" + String(deviceStatuses[0]) +
                 ", vantren=" + String(deviceStatuses[1]) +
                 ", vanduoi=" + String(deviceStatuses[2]) +
                 ", den1=" + String(deviceStatuses[3]) +
                 ", quat1=" + String(deviceStatuses[4]) +
                 ", den2=" + String(deviceStatuses[5]) +
                 ", quat2=" + String(deviceStatuses[6]) +
                 ", buzzer=" + String(deviceStatuses[7]));
}

void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Không thể gửi dữ liệu cảm biến: WiFi ngắt kết nối");
    return;
  }

  float temp1 = dht1.readTemperature();
  float hum1 = dht1.readHumidity();
  float temp2 = dht2.readTemperature();
  float hum2 = dht2.readHumidity();
  int soil1 = analogRead(SOIL_MOISTURE1_PIN);
  int soil2 = analogRead(SOIL_MOISTURE2_PIN);
  int rain = readRainSensor();
  float waterLevel = readWaterLevel();

  if (soil1 < 0 || soil1 > 4095) {
    Serial.println("Cảnh báo: soil1 bất thường, gán soil1=4095");
    soil1 = 4095;
  }
  if (soil2 < 0 || soil2 > 4095) {
    Serial.println("Cảnh báo: soil2 bất thường, gán soil2=4095");
    soil2 = 4095;
  }
  if (rain < 0 || rain > 4095) {
    Serial.println("Cảnh báo: rain bất thường, gán rain=4095");
    rain = 4095;
  }
  float soil1_percent = map(soil1, 4095, 0, 0, 100);
  float soil2_percent = map(soil2, 4095, 0, 0, 100);
  Serial.println("Raw soil1: " + String(soil1) + ", Soil1_percent: " + String(soil1_percent, 1) + "%");
  Serial.println("Raw soil2: " + String(soil2) + ", Soil2_percent: " + String(soil2_percent, 1) + "%");
  Serial.println("Rain: " + String(rain) + ", Is raining: " + String(rain < 2000 ? "Yes" : "No"));
  Serial.println("Water level: " + String(waterLevel, 1) + " cm");

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(SENSOR_CONTROLLER_URL);
  http.addHeader("Content-Type", "application/json");

  for (int i = 0; i < gardenCount; i++) {
    int garden_number = gardens[i].garden_number;
    DynamicJsonDocument doc(512);
    doc["action"] = "save_sensor_data";
    doc["garden_number"] = garden_number;
    doc["soil_moisture"] = (garden_number == 1) ? soil1_percent : soil2_percent;
    doc["temperature"] = (garden_number == 1) ? (isnan(temp1) ? 0 : temp1) : (isnan(temp2) ? 0 : temp2);
    doc["humidity"] = (garden_number == 1) ? (isnan(hum1) ? 0 : hum1) : (isnan(hum2) ? 0 : hum2);
    doc["water_level_cm"] = waterLevel;
    doc["is_raining"] = rain < 2000 ? 1 : 0;

    String requestBody;
    serializeJson(doc, requestBody);
    Serial.println("Gửi dữ liệu cảm biến cho vườn " + String(garden_number) + ": " + requestBody);

    int attempts = 0;
    int maxAttempts = 3;
    int httpCode = 0;
    String response;
    while (attempts < maxAttempts) {
      httpCode = http.POST(requestBody);
      if (httpCode == HTTP_CODE_OK) {
        response = http.getString();
        Serial.println("Gửi dữ liệu cảm biến thành công cho vườn " + String(garden_number) + ": " + response);
        break;
      } else {
        Serial.println("Gửi dữ liệu cảm biến thất bại, mã HTTP: " + String(httpCode) + ", thử lại lần " + String(attempts + 1));
        attempts++;
        delay(1000);
      }
    }
    if (httpCode != HTTP_CODE_OK) {
      Serial.println("Gửi dữ liệu cảm biến thất bại sau " + String(maxAttempts) + " lần thử cho vườn " + String(garden_number));
    }
  }
  http.end();
}

void checkControlCommand() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Không thể kiểm tra lệnh điều khiển: WiFi ngắt kết nối");
    return;
  }

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(SENSOR_CONTROLLER_URL);
  http.addHeader("Content-Type", "application/json");

  for (int i = 0; i < gardenCount; i++) {
    int garden_number = gardens[i].garden_number;
    DynamicJsonDocument doc(256);
    doc["action"] = "get_control_command";
    doc["garden_number"] = garden_number;
    String requestBody;
    serializeJson(doc, requestBody);

    int attempts = 0;
    int maxAttempts = 3;
    int httpCode = 0;
    while (attempts < maxAttempts) {
      httpCode = http.POST(requestBody);
      if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        DynamicJsonDocument responseDoc(512);
        DeserializationError error = deserializeJson(responseDoc, response);
        if (error) {
          Serial.println("Lỗi phân tích JSON lệnh điều khiển: " + String(error.c_str()));
          break;
        }
        if (responseDoc["success"]) {
          JsonArray data = responseDoc["data"];
          unsigned long elapsedSeconds = (millis() - startMillis) / 1000;
          for (JsonObject command : data) {
            String deviceName = command["device_name"].as<String>();
            int status = command["status"].as<int>();
            bool isScheduled = false;

            for (int j = 0; j < scheduleCount; j++) {
              if (!schedules[j].executed && schedules[j].date == currentDate &&
                  schedules[j].device_name == deviceName &&
                  schedules[j].garden_number == garden_number &&
                  elapsedSeconds >= schedules[j].secondsSinceStart &&
                  (schedules[j].is_range ? elapsedSeconds <= schedules[j].end_secondsSinceStart : elapsedSeconds <= schedules[j].secondsSinceStart + 10)) {
                isScheduled = true;
                Serial.println("Bỏ qua lệnh điều khiển thủ công: Lịch trình đang hoạt động cho " + deviceName +
                               " (garden_number: " + String(garden_number) + ")");
                break;
              }
            }

            if (!isScheduled && ((garden_number == 1 && (deviceName == "den1" || deviceName == "quat1")) ||
                                (garden_number == 2 && (deviceName == "den2" || deviceName == "quat2")))) {
              for (int j = 0; j < 8; j++) {
                if (devices[j] == deviceName) {
                  if (deviceStatuses[j] != status) {
                    deviceStatuses[j] = status;
                    manualOverride[j] = true;
                    digitalWrite(devicePins[j], status == 1 ? LOW : HIGH);
                    Serial.println("Cập nhật thủ công " + deviceName + " (vườn " + String(garden_number) + ") sang trạng thái: " + String(status));
                  }
                }
              }
            }
          }
        } else {
          Serial.println("Kiểm tra lệnh điều khiển thất bại cho vườn " + String(garden_number) + ": " + responseDoc["message"].as<String>());
        }
        break;
      } else {
        Serial.println("Kiểm tra lệnh điều khiển thất bại, mã HTTP: " + String(httpCode) + ", thử lại lần " + String(attempts + 1));
        attempts++;
        delay(1000);
      }
    }
    if (httpCode != HTTP_CODE_OK) {
      Serial.println("Kiểm tra lệnh điều khiển thất bại sau " + String(maxAttempts) + " lần thử cho vườn " + String(garden_number));
    }
  }
  http.end();
}

void handleControlRequest(AsyncWebServerRequest *request) {
  if (!request->hasParam("body", true)) {
    request->send(400, "application/json", "{\"success\":false,\"message\":\"Thiếu body\"}");
    return;
  }

  String body = request->getParam("body", true)->value();
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    request->send(400, "application/json", "{\"success\":false,\"message\":\"Lỗi phân tích JSON\"}");
    return;
  }

  String deviceName = doc["device_name"].as<String>();
  int status = doc["status"].as<int>();
  int requestGardenNumber = doc["garden_number"].as<int>();

  bool validGarden = false;
  for (int i = 0; i < gardenCount; i++) {
    if (gardens[i].garden_number == requestGardenNumber) {
      validGarden = true;
      break;
    }
  }
  if (!validGarden) {
    request->send(400, "application/json", "{\"success\":false,\"message\":\"Số vườn không hợp lệ\"}");
    return;
  }

  unsigned long elapsedSeconds = (millis() - startMillis) / 1000;
  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].executed && schedules[i].date == currentDate &&
        schedules[i].device_name == deviceName && schedules[i].garden_number == requestGardenNumber &&
        elapsedSeconds >= schedules[i].secondsSinceStart &&
        (schedules[i].is_range ? elapsedSeconds <= schedules[i].end_secondsSinceStart : elapsedSeconds <= schedules[i].secondsSinceStart + 10)) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Lịch trình đang hoạt động cho " + deviceName + ", không thể điều khiển thủ công\"}");
      Serial.println("Từ chối điều khiển thủ công: Lịch trình đang hoạt động cho " + deviceName +
                     " (vườn " + String(requestGardenNumber) + ")");
      return;
    }
  }

  if ((requestGardenNumber == 1 && (deviceName == "den1" || deviceName == "quat1")) ||
      (requestGardenNumber == 2 && (deviceName == "den2" || deviceName == "quat2"))) {
    for (int i = 0; i < 8; i++) {
      if (devices[i] == deviceName) {
        deviceStatuses[i] = status;
        manualOverride[i] = true;
        digitalWrite(devicePins[i], status == 1 ? LOW : HIGH);
        DynamicJsonDocument responseDoc(128);
        responseDoc["success"] = true;
        responseDoc["message"] = "Thiết bị " + deviceName + " (vườn " + String(requestGardenNumber) + ") cập nhật trạng thái thủ công: " + String(status);
        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
        Serial.println("Yêu cầu điều khiển thủ công: " + deviceName + " (vườn " + String(requestGardenNumber) + ") đặt thành " + String(status));
        return;
      }
    }
  }

  request->send(400, "application/json", "{\"success\":false,\"message\":\"Tên thiết bị không hợp lệ\"}");
}

float readWaterLevel() {
  long duration;
  float distance;
  float total = 0;
  int samples = 10;
  for (int i = 0; i < samples; i++) {
    digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
    duration = pulseIn(ULTRASONIC_ECHO_PIN, HIGH);
    distance = duration * 0.034 / 2;
    if (distance < 0 || distance > 500) distance = 500;
    total += distance;
    delay(50);
  }
  float avgDistance = total / samples;
  float waterLevel = 30.0 - avgDistance;
  if (waterLevel < 0) waterLevel = 0;
  if (waterLevel > 30.0) waterLevel = 30.0;
  return waterLevel;
}
