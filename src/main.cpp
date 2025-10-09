#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ===== CONFIGURATION =====
// Buffer configuration
#define BUFFER_SIZE 1000
#define DATA_COLLECTION_INTERVAL_MS 1
#define MQTT_PUBLISH_INTERVAL_MS 1000

// MQTT topic
#define MQTT_TOPIC "power-meter/data"

// Task configuration
#define DATA_COLLECTOR_STACK_SIZE 4096
#define MQTT_PUBLISHER_STACK_SIZE 8192
#define DATA_COLLECTOR_PRIORITY 5
#define MQTT_PUBLISHER_PRIORITY 4

// Power generation range (in watts)
#define POWER_MIN_W 0
#define POWER_MAX_W 1000

// Timeout configurations (in milliseconds)
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define MQTT_CONNECT_TIMEOUT_MS 5000
#define MUTEX_TIMEOUT_MS 100

// WiFi and MQTT Configuration (set via platformio.ini build flags)
#ifndef WIFI_SSID
#define WIFI_SSID "YourWiFiSSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YourWiFiPassword"
#endif

#ifndef MQTT_BROKER
#define MQTT_BROKER "test.mosquitto.org"
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

// Global variables
float dataBuffer[BUFFER_SIZE];
volatile uint16_t bufferIndex = 0;
volatile bool bufferReady = false;

// Mutex for thread-safe buffer access
SemaphoreHandle_t bufferMutex;

// WiFi and MQTT clients
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Task handles
TaskHandle_t dataCollectorTaskHandle;
TaskHandle_t mqttPublisherTaskHandle;

// ===== FUNCTION IMPLEMENTATIONS =====

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32PowerMeter-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" trying again in 5 seconds");
      delay(5000);
    }
  }
}

void setupMQTT() {
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  Serial.print("Connecting to MQTT broker: ");
  Serial.println(MQTT_BROKER);
  reconnectMQTT();
}

void setupWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi connection failed!");
    Serial.println("Please check your WiFi credentials in platformio.ini");
  }
}

float generateRandomPowerData() {
  return random(POWER_MIN_W * 100, POWER_MAX_W * 100) / 100.0;
}

void dataCollectorTask(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  Serial.println("Data Collector Task started");
  for (;;) {
    float powerValue = generateRandomPowerData();
    if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) ==
        pdTRUE) {
      dataBuffer[bufferIndex] = powerValue;
      bufferIndex++;
      if (bufferIndex >= BUFFER_SIZE) {
        bufferReady = true;
        bufferIndex = 0;
        Serial.println("Buffer full - ready for transmission");
      }
      xSemaphoreGive(bufferMutex);
    } else {
      Serial.println(
          "Warning: Could not acquire buffer mutex in data collector");
    }
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(DATA_COLLECTION_INTERVAL_MS));
  }
}

void mqttPublisherTask(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  float localBuffer[BUFFER_SIZE];
  uint16_t localBufferSize = 0;
  Serial.println("MQTT Publisher Task started");
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, reconnecting...");
      setupWiFi();
    }
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();
    if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) ==
        pdTRUE) {
      if (bufferReady || bufferIndex > 0) {
        localBufferSize = bufferIndex;
        memcpy(localBuffer, dataBuffer, localBufferSize * sizeof(float));
        bufferIndex = 0;
        bufferReady = false;
        xSemaphoreGive(bufferMutex);
        if (localBufferSize > 0) {
          JsonDocument doc;
          doc["timestamp"] = millis();
          JsonArray samples = doc.createNestedArray("samples");
          for (uint16_t i = 0; i < localBufferSize; i++) {
            samples.add(localBuffer[i]);
          }
          String jsonString;
          serializeJson(doc, jsonString);
          if (mqttClient.publish(MQTT_TOPIC, jsonString.c_str())) {
            Serial.printf("Published %d samples to MQTT\n", localBufferSize);
          } else {
            Serial.println("Failed to publish to MQTT");
          }
        }
      } else {
        xSemaphoreGive(bufferMutex);
      }
    } else {
      Serial.println(
          "Warning: Could not acquire buffer mutex in MQTT publisher");
    }
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(MQTT_PUBLISH_INTERVAL_MS));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to initialize
  Serial.println("\n========================================");
  Serial.println("Starting ESP32 Power Meter Producer...");
  Serial.printf("ESP32 Chip: Rev %d, Cores: %d\n", ESP.getChipRevision(),
                ESP.getChipCores());
  Serial.printf("CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.println("========================================\n");

  // Create mutex for buffer synchronization
  bufferMutex = xSemaphoreCreateMutex();
  if (bufferMutex == NULL) {
    Serial.println("ERROR: Failed to create mutex!");
    ESP.restart(); // Restart if critical resource fails
  }

  // Initialize WiFi and MQTT
  setupWiFi();
  setupMQTT();

  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(dataCollectorTask,         // Task function
                          "DataCollector",           // Task name
                          DATA_COLLECTOR_STACK_SIZE, // Stack size (bytes)
                          NULL,                      // Parameters
                          DATA_COLLECTOR_PRIORITY,   // Priority (5 = high)
                          &dataCollectorTaskHandle,  // Task handle
                          1                          // Core 1
  );

  xTaskCreatePinnedToCore(mqttPublisherTask,         // Task function
                          "MQTTPublisher",           // Task name
                          MQTT_PUBLISHER_STACK_SIZE, // Stack size (bytes)
                          NULL,                      // Parameters
                          MQTT_PUBLISHER_PRIORITY, // Priority (4 = medium-high)
                          &mqttPublisherTaskHandle, // Task handle
                          0                         // Core 0
  );

  Serial.println("✓ Tasks created successfully!");
  Serial.printf("✓ Data collector: Every %dms on Core 1\n",
                DATA_COLLECTION_INTERVAL_MS);
  Serial.printf("✓ MQTT publisher: Every %dms on Core 0\n",
                MQTT_PUBLISH_INTERVAL_MS);
  Serial.println("✓ System ready for operation\n");
}

void loop() {
  // Empty - all work is done in FreeRTOS tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}
