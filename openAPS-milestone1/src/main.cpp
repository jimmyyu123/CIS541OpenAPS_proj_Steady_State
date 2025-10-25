// written by Shraavasti (Shraav) Bhat 10/10/2026
#include <Arduino.h>
#include <vector>
#include <ArduinoMqttClient.h>
#include <WiFiNINA.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include "arduino_secrets.h"
#include "OpenAPS.h"

// WiFi and MQTT configuration
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
OpenAPS openaps; // main OpenAPS controller instance

// RTOS Task initialization
TaskHandle_t TaskMQTTHandle = NULL;
TaskHandle_t TaskOpenAPSHandle = NULL;

// Global variables
volatile float current_BG = 0.0;
volatile long current_time = 0;
volatile bool newBGData = false;
volatile bool newInsulinTreatment = false;
volatile bool attributeReceived = false;

SemaphoreHandle_t xDataSemaphore;   // protects shared BG data



//functions
void TaskMQTT(void *pvParameters);
void TaskOpenAPS(void *pvParameters);
void onMqttMessage(int messageSize);

void onMqttMessage(int messageSize) {
    // TODO: Implement MQTT message callback
    // Handle attribute updates and CGM data
    // Update openAPS, current_BG, current_time, and flags as needed
  String topic = mqttClient.messageTopic();
  String payload = "";

  // Read the entire incoming payload byte by byte
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  // Serial.print("Message arrived on topic: ");
  // Serial.println(topic);
  // Serial.print("Payload: ");
  // Serial.println(payload);

  // 1. Handle vp-attributes (Patient Profile)
  if (topic.endsWith("vp-attributes") || topic.indexOf("vp-attributes/response/") >= 0) {
    if (!attributeReceived) {
      Serial.println("Processing Patient Profile...");

      int bolusPos = payload.indexOf("\"bolus_insulins\"");
      if (bolusPos != -1) {
          int arrStart = payload.indexOf("[", bolusPos);
          int arrEnd   = payload.indexOf("]", arrStart);
          if (arrStart != -1 && arrEnd != -1) {
              String bolusArray = payload.substring(arrStart + 1, arrEnd);
              unsigned int pos = 0;

              while (pos < bolusArray.length()) {
                  // Serial.print("[DEBUG] Current pos = ");
                  // Serial.println(pos);
                  // Serial.print("[DEBUG] bolusArray substring (remaining): ");
                  // Serial.println(bolusArray.substring(pos));

                  int timePos = bolusArray.indexOf("\"time\":", pos);
                  if (timePos == -1) break;
                  int dosePos = bolusArray.indexOf("\"dose\":", pos);
                  int durPos  = bolusArray.indexOf("\"duration\":", pos);

                  long t  = bolusArray.substring(timePos + 7, bolusArray.indexOf(",", timePos)).toInt();
                  float d = bolusArray.substring(dosePos + 7, bolusArray.indexOf(",", dosePos)).toFloat();
                  int dur = bolusArray.substring(durPos + 11, bolusArray.indexOf("}", durPos)).toInt();

                  // Serial.print("[DEBUG] Parsed treatment → time=");
                  // Serial.print(t);
                  // Serial.print(", dose=");
                  // Serial.print(d);
                  // Serial.print(", dur=");
                  // Serial.println(dur);

                  if (xSemaphoreTake(xDataSemaphore, portMAX_DELAY) == pdTRUE) {
                      openaps.addInsulinTreatment(InsulinTreatment(t, d, dur));
                      xSemaphoreGive(xDataSemaphore);
                  }

                  pos = bolusArray.indexOf("}", pos) + 1;
              }
          }
      }


      attributeReceived     = true;
      newInsulinTreatment   = true;
      mqttClient.unsubscribe("cis441-541/Steady_State/vp-attributes");
      Serial.println("Patient Profile processed and unsubscribed from vp-attributes.");
    }
  }

  // 2. Handle cgm-openaps (CGM data)
  else if (topic.endsWith("/cgm-openaps") ) {
    int gStart = payload.indexOf("\"Glucose\":") + 10;
    int gEnd   = payload.indexOf(",", gStart);
    float glucose = payload.substring(gStart, gEnd).toFloat();

    int tStart = payload.indexOf("\"time\":") + 7;
    int tEnd   = payload.indexOf("}", tStart);
    long timeVal = payload.substring(tStart, tEnd).toInt();

    openaps.noteNewBG(glucose, timeVal);  // update prev_BG and last_BG

    // update shared BG data
    if (xSemaphoreTake(xDataSemaphore, portMAX_DELAY) == pdTRUE) {
      current_BG   = glucose;
      current_time = timeVal;
      newBGData    = true;
      xSemaphoreGive(xDataSemaphore);
    }

    // Serial.print("Updated BG from CGM: ");
    // Serial.println(glucose);
  }

  // 3. Unknown Topic (safety)
  else {
    Serial.println("Unknown topic received – ignoring.");
  }
}

void TaskMQTT(void *pvParameters) {
    // TODO: Implement MQTT task
    // Continuously poll for MQTT messages
    unsigned long last = 0;
    for (;;) {
        mqttClient.poll();
        if (millis() - last > 2000) { /*Serial.println("[TaskMQTT] polling");*/ last = millis(); }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void TaskOpenAPS(void *pvParameters) {
    // TODO: Implement OpenAPS task
    // Process new data, calculate basal rate, and publish to MQTT

  for (;;) {
    bool localNew = false;
    bool localNewIns = false;
    float bgValue = 0.0f;
    long  tValue = 0;

    if (xSemaphoreTake(xDataSemaphore, portMAX_DELAY) == pdTRUE) {
      localNew        = newBGData;
      localNewIns     = newInsulinTreatment;
      bgValue         = current_BG;
      tValue          = current_time;
      newBGData       = false;
      newInsulinTreatment = false;
      xSemaphoreGive(xDataSemaphore);
    }

    if (localNew || localNewIns) {
      // Milestone 2 algorithm：calculate activity/IOB/forecast，and update window treatment
      float insulin_rate = openaps.get_basal_rate(tValue, bgValue);  // U/hr
      String msg = String("{\"insulin_rate\": ") + String(insulin_rate, 3) + "}";
      mqttClient.beginMessage("cis441-541/Steady_State/insulin-pump-openaps");
      mqttClient.print(msg);
      mqttClient.endMessage();

      // Serial.print("[OpenAPS] Time="); Serial.print(tValue);
      // Serial.print(" Current BG="); Serial.print(bgValue);
      // Serial.print(" → insulin_rate (basal)="); Serial.println(insulin_rate, 3);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void setup() {
    // TODO: Implement setup function
    // Initialize Serial, WiFi, MQTT, OpenAPS, mutex, and tasks
    // Subscribe to necessary MQTT topics
    // Request virtual patient profile
    // 1. Initialize serial communication
    Serial.begin(115200);
    while (!Serial);
    Serial.println("OpenAPS: Starting up ...");

    // 2. Connect to wifi
    int status = WL_IDLE_STATUS;
    Serial.print("Connecting to WiFi...");
    while (status != WL_CONNECTED) {
        status = WiFi.begin(SECRET_SSID, SECRET_PASS);
        delay(5000);
    }
    Serial.println("Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

   //5. Create binary semaphore for shared data
    xDataSemaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(xDataSemaphore);   // initialize as 'available'

    // 3. Configure and connect MQTT client
    mqttClient.setUsernamePassword("cis441-541_2025", "cukwy2-geNwit-puqced");
    mqttClient.onMessage(onMqttMessage);

    if (!mqttClient.connect("mqtt-dev.precise.seas.upenn.edu", 1883)) {
        Serial.print("MQTT connection failed, error=");
        Serial.println(mqttClient.connectError());
        while (1);  // stop here if broker is unreachable
    }
    Serial.println("Connected to MQTT broker.");

    //4. Subscribe to required topics and fanout
    mqttClient.subscribe("cis441-541/Steady_State/vp-attributes");
    mqttClient.subscribe("cis441-541/Steady_State/vp-attributes/response/+");

    // Ask VP to send the Patient Profile (empty JSON body is fine)
    mqttClient.beginMessage("cis441-541/Steady_State/vp-attributes/request/1");
    mqttClient.print("{}");
    mqttClient.endMessage();

    mqttClient.subscribe("cis441-541/Steady_State/cgm-openaps");
    // mqttClient.subscribe("cis441-541/Steady_State/cgm");


    //6. Create FreeRTOS task
    BaseType_t res1 = xTaskCreate(TaskMQTT, "TaskMQTT", 1024, NULL, 2, &TaskMQTTHandle);
    BaseType_t res2 = xTaskCreate(TaskOpenAPS, "TaskOpenAPS", 2048, NULL, 1, &TaskOpenAPSHandle);

    Serial.print("TaskMQTT create: "); Serial.println(res1 == pdPASS ? "OK" : "FAIL");
    Serial.print("TaskOpenAPS create: "); Serial.println(res2 == pdPASS ? "OK" : "FAIL");
    
    //6. start scheduler
    vTaskStartScheduler();
}

void loop() {
    // Empty. Tasks are handled by FreeRTOS
}


