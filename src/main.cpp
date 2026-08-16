/**
 * ESP32 Air Quality Monitor ("Theia" / AQ-Meter Pro)
 *
 * - SCD30 CO2/temperature/humidity sensor on its own I2C bus (Wire1 @ 100 kHz)
 * - SSD1306 OLED on Wire0, MicroSD (VSPI) for daily CSV logs under /logs
 * - Self-hosted WiFi AP + AsyncWebServer/WebSocket UI (offline, no internet)
 *
 * Time model (no RTC):
 *   The wall clock is persisted to NVS and restored on boot, so the device logs
 *   autonomously across reboots without needing a browser. Logging is gated only
 *   until the clock has been set at least once (fresh device). A connected browser
 *   re-syncs the clock precisely via POST /api/timeset. Without an RTC the clock
 *   cannot measure power-off gaps; it resumes from the last saved time until the
 *   next browser sync.
 *
 * Tasks:
 *   Core 0 – SensorTask (owns the SCD30), LogTask, OLEDTask
 *   Core 1 – WebTask: performs ALL WebSocket work (broadcasts + client cleanup).
 *
 *   NOTE: HTTP/WebSocket *callbacks* do not run on WebTask — AsyncTCP owns its
 *   own task (core per CONFIG_ASYNC_TCP_RUNNING_CORE). WebTask exists so that
 *   exactly one application task touches the AsyncWebSocket client list; this
 *   loop() deliberately performs no WebSocket calls at all.
 *
 *   loop() is limited to NVS persistence and heap reporting.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "tasks/sensor.h"
#include "tasks/logging.h"
#include "tasks/oled.h"
#include "web/endpoints.h"
#include "shared.h"

// AP_SSID / AP_PASS live here so credentials stay out of version control.
// Copy src/secrets.example.h to src/secrets.h to build.
#include "secrets.h"

// ═══════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════

#if !defined(AP_SSID) || !defined(AP_PASS)
#error "Missing AP_SSID / AP_PASS. Copy src/secrets.example.h to src/secrets.h."
#endif

#define WDT_TIMEOUT   30            // seconds

#define STACK_SENSOR  6144
#define STACK_LOG     8192
#define STACK_WEB     8192
#define STACK_OLED    4096

#define INTERVAL_DEFAULT   10

#define NVS_NAMESPACE      "aqm"
#define NVS_KEY_INTERVAL   "interval"
#define NVS_KEY_BOOTCOUNT  "bootCount"
#define NVS_KEY_EPOCH      "lastEpoch"
#define NVS_KEY_ASC        "asc"
#define NVS_KEY_TEMPOFF    "tempOff"
#define NVS_KEY_LASTCAL    "lastCal"
#define CLOCK_SAVE_PERIOD  (5UL * 60UL * 1000UL)

// ═══════════════════════════════════════════════════════════════
// Globals
// ═══════════════════════════════════════════════════════════════

AsyncWebServer server(80);
AsyncWebSocket webSocket("/ws");
Preferences prefs;

TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t logTaskHandle    = nullptr;
TaskHandle_t webTaskHandle    = nullptr;
TaskHandle_t oledTaskHandle   = nullptr;

QueueHandle_t sensorQueue;
QueueHandle_t displayQueue;
SemaphoreHandle_t fileSysMutex;
SemaphoreHandle_t readingMutex;

volatile bool     timeIsSet       = false;
volatile bool     clockDirty      = false;
volatile bool     settingsDirty   = false;
volatile uint16_t measureInterval = INTERVAL_DEFAULT;

SensorReading      latestReading = {0, 0, 0, 0};
volatile uint32_t  readingSeq    = 0;

volatile bool sensorOnline = false;
volatile bool sdOnline     = false;
volatile bool oledOnline   = false;

volatile uint32_t droppedSamples = 0;

// Calibration state (applied to the SCD30 by the sensor task).
volatile bool     ascEnabled           = false;   // default OFF
volatile bool     calAscDirty          = false;
volatile uint16_t calFrcPending        = 0;
volatile int16_t  calTempOffsetC10     = 0;
volatile bool     calTempOffsetDirty   = false;
volatile uint32_t lastCalibrationEpoch = 0;

static uint32_t bootCount = 0;

// ═══════════════════════════════════════════════════════════════
// SD download stream counter
// ═══════════════════════════════════════════════════════════════
// A counter (not a flag) so concurrent downloads can't clear each other's
// guard. Guarded by a spinlock because the decrement happens on the AsyncTCP
// task while the log task reads it on core 0.

static portMUX_TYPE sdStreamMux = portMUX_INITIALIZER_UNLOCKED;
static int sdStreamCount = 0;

int sdStreamsGet() {
    portENTER_CRITICAL(&sdStreamMux);
    int v = sdStreamCount;
    portEXIT_CRITICAL(&sdStreamMux);
    return v;
}

void sdStreamsInc() {
    portENTER_CRITICAL(&sdStreamMux);
    sdStreamCount++;
    portEXIT_CRITICAL(&sdStreamMux);
}

void sdStreamsDec() {
    portENTER_CRITICAL(&sdStreamMux);
    if (sdStreamCount > 0) sdStreamCount--;
    portEXIT_CRITICAL(&sdStreamMux);
}

// ═══════════════════════════════════════════════════════════════
// NVS persistence (all writes happen on this task)
// ═══════════════════════════════════════════════════════════════

static void persistClock() {
    if (!timeIsSet) return;
    time_t now = time(nullptr);
    if (now < 1000000000) return;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(NVS_KEY_EPOCH, (uint32_t)now);
    prefs.end();
}

static void persistSettings() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(NVS_KEY_INTERVAL, measureInterval);
    prefs.putBool(NVS_KEY_ASC, ascEnabled);
    prefs.putShort(NVS_KEY_TEMPOFF, calTempOffsetC10);
    prefs.putUInt(NVS_KEY_LASTCAL, lastCalibrationEpoch);
    prefs.end();
}

// ═══════════════════════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("=== ESP32 Air Quality Monitor (AQ-Meter Pro) ===");

    esp_task_wdt_init(WDT_TIMEOUT, true);
    esp_task_wdt_add(NULL);  // the loop task

    // Persistent settings + restore clock so logging can run standalone.
    prefs.begin(NVS_NAMESPACE, false);
    measureInterval = prefs.getUInt(NVS_KEY_INTERVAL, INTERVAL_DEFAULT);
    if (measureInterval < INTERVAL_MIN || measureInterval > INTERVAL_MAX) {
        measureInterval = INTERVAL_DEFAULT;
    }
    bootCount = prefs.getUInt(NVS_KEY_BOOTCOUNT, 0) + 1;
    prefs.putUInt(NVS_KEY_BOOTCOUNT, bootCount);
    uint32_t savedEpoch  = prefs.getUInt(NVS_KEY_EPOCH, 0);
    ascEnabled           = prefs.getBool(NVS_KEY_ASC, false);   // default OFF
    calTempOffsetC10     = prefs.getShort(NVS_KEY_TEMPOFF, 0);
    lastCalibrationEpoch = prefs.getUInt(NVS_KEY_LASTCAL, 0);
    prefs.end();

    if (savedEpoch >= 1000000000) {
        struct timeval tv = { .tv_sec = (time_t)savedEpoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        timeIsSet = true;
        Serial.printf("Clock restored from NVS: epoch %lu\n", (unsigned long)savedEpoch);
    } else {
        Serial.println("No saved clock - logging waits for first browser time sync.");
    }
    Serial.printf("Boot #%lu, interval %us, ASC %s\n",
                  (unsigned long)bootCount, measureInterval, ascEnabled ? "on" : "off");

    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed - restarting");
        delay(2000);
        ESP.restart();
    }
    Serial.printf("SPIFFS: %u / %u bytes used\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());

    fileSysMutex = xSemaphoreCreateMutex();
    readingMutex = xSemaphoreCreateMutex();
    sensorQueue  = xQueueCreate(8, sizeof(SensorReading));
    displayQueue = xQueueCreate(4, sizeof(SensorReading));
    if (!fileSysMutex || !readingMutex || !sensorQueue || !displayQueue) {
        Serial.println("Failed to create RTOS objects - restarting");
        delay(2000);
        ESP.restart();
    }

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 8);
    delay(500);
    Serial.printf("AP '%s' at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    sensorOnline = initSensor();
    oledOnline   = initOLED();
    sdOnline     = initSDCard();
    Serial.printf("SCD30:%s  OLED:%s  SD:%s\n",
                  sensorOnline ? "ok" : "--",
                  oledOnline   ? "ok" : "--",
                  sdOnline     ? "ok" : "--");

    xTaskCreatePinnedToCore(sensorTaskFunction, "SensorTask", STACK_SENSOR, nullptr, 2, &sensorTaskHandle, 0);
    xTaskCreatePinnedToCore(logTaskFunction,    "LogTask",    STACK_LOG,    nullptr, 2, &logTaskHandle,    0);
    xTaskCreatePinnedToCore(oledTaskFunction,   "OLEDTask",   STACK_OLED,   nullptr, 1, &oledTaskHandle,   0);
    xTaskCreatePinnedToCore(webTaskFunction,    "WebTask",    STACK_WEB,    nullptr, 3, &webTaskHandle,    1);

    setupWebEndpoints(server, webSocket);   // also installs the WS event handler
    server.begin();

    Serial.printf("Ready. Open http://%s\n", WiFi.softAPIP().toString().c_str());
    esp_task_wdt_reset();
}

// ═══════════════════════════════════════════════════════════════
// Loop - NVS persistence + heap reporting only (NO WebSocket calls)
// ═══════════════════════════════════════════════════════════════

void loop() {
    static uint32_t lastWDT = 0, lastMem = 0, lastClockSave = 0;

    uint32_t now = millis();

    if (now - lastWDT > 5000) { esp_task_wdt_reset(); lastWDT = now; }

    // Persist the clock immediately on first/forced sync, then throttled.
    if (clockDirty || (timeIsSet && now - lastClockSave > CLOCK_SAVE_PERIOD)) {
        persistClock();
        clockDirty = false;
        lastClockSave = now;
    }

    if (settingsDirty) {
        persistSettings();
        settingsDirty = false;
    }

    if (now - lastMem > 30000) {
        Serial.printf("Heap: %u free (min %u), dropped: %lu\n",
                      ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                      (unsigned long)droppedSamples);
        lastMem = now;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}
