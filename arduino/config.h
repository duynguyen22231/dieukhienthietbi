#ifndef CONFIG_H
#define CONFIG_H

// Thông tin WiFi
const char* WIFI_SSID = "maychu";          // Tên WiFi
const char* WIFI_PASS = "12345678";        // Mật khẩu WiFi

// URL API của server
const char* SENSOR_CONTROLLER_URL = "http://192.168.1.123/SmartGarden/backend-api/routes/sensor.php"; // URL lưu dữ liệu cảm biến và lấy lệnh điều khiển

// Định nghĩa chân relay và còi
#define MAYBOM_PIN 13
#define VANTREN_PIN 16
#define VANDUOI_PIN 14
#define QUAT1_PIN 27
#define QUAT2_PIN 26
#define DEN1_PIN 25
#define DEN2_PIN 17
#define BUZZER_PIN 19

// Định nghĩa chân cảm biến
#define DHT1_PIN 15
#define DHT2_PIN 4
#define SOIL_MOISTURE1_PIN 34
#define SOIL_MOISTURE2_PIN 33
#define RAIN_SENSOR_PIN 32
#define ULTRASONIC_TRIG_PIN 5
#define ULTRASONIC_ECHO_PIN 18

#endif