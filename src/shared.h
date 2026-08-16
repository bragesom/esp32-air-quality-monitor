#pragma once

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// Forward declarations for web components (avoid circular includes)
class AsyncWebServer;
class AsyncWebSocket;

// ═══════════════════════════════════════════════════════════════
// DATA CONTRACT
// ═══════════════════════════════════════════════════════════════

// Single packed reading passed between tasks and logged to SD (10 bytes).
struct SensorReading {
    uint32_t timestamp;    // Unix epoch seconds (0 until the clock is known)
    uint16_t co2;          // CO2 in ppm (0-9999)
    int16_t  temperature;  // Temperature in 0.1 °C units (-400..800)
    uint16_t humidity;     // Relative humidity in 0.1 % units (0..1000)
};

// ═══════════════════════════════════════════════════════════════
// HARDWARE PINS
// ═══════════════════════════════════════════════════════════════

#define SCD30_SDA_PIN   16   // I2C bus 1 (Wire1) – SCD30, max 100 kHz
#define SCD30_SCL_PIN   17
#define OLED_SDA_PIN    21   // I2C bus 0 (Wire0) – SSD1306 @ 0x3C
#define OLED_SCL_PIN    22
#define SD_CS_PIN        5   // SPI (default VSPI) chip-select – strapping pin

// ═══════════════════════════════════════════════════════════════
// LIMITS
// ═══════════════════════════════════════════════════════════════

#define LOG_BUFFER_SIZE          16   // Readings batched per SD flush
#define LOG_RING_SIZE           360   // Recent samples kept in RAM (~1h @ 10s)
#define MAX_WEBSOCKET_CLIENTS     4

// Measurement interval bounds. 1800 s is the SCD30's hard maximum – the API,
// NVS validation and the sensor clamp must all agree or they diverge silently.
#define INTERVAL_MIN             10
#define INTERVAL_MAX           1800

// Forced-recalibration bounds (SCD30 accepts 400..2000 ppm).
#define FRC_MIN                 400
#define FRC_MAX                2000

// ═══════════════════════════════════════════════════════════════
// HARDWARE INIT (implemented in the task .cpp files)
// ═══════════════════════════════════════════════════════════════

bool initSensor();
bool initOLED();
bool initSDCard();

// ═══════════════════════════════════════════════════════════════
// TASK ENTRY POINTS
// ═══════════════════════════════════════════════════════════════

void sensorTaskFunction(void* parameter);
void logTaskFunction(void* parameter);
void oledTaskFunction(void* parameter);
void webTaskFunction(void* parameter);

// ═══════════════════════════════════════════════════════════════
// INTER-TASK QUEUES / SYNCHRONIZATION (defined in main.cpp)
// ═══════════════════════════════════════════════════════════════

extern QueueHandle_t sensorQueue;      // Sensor reading → logging task
extern QueueHandle_t displayQueue;     // Sensor reading → OLED task
extern SemaphoreHandle_t fileSysMutex; // Serializes all SD/SPIFFS access
extern SemaphoreHandle_t readingMutex; // Guards latestReading

// ═══════════════════════════════════════════════════════════════
// SHARED STATE
// ═══════════════════════════════════════════════════════════════

extern volatile bool timeIsSet;        // A real wall-clock time is known
extern volatile bool clockDirty;       // Request an NVS persist of the clock
extern volatile bool settingsDirty;    // Request an NVS persist of settings
extern volatile uint16_t measureInterval;

// Latest reading (thread-safe access via readingMutex)
extern SensorReading latestReading;
extern volatile uint32_t readingSeq;   // Bumped by the sensor task per reading

// Best-effort hardware status for the UI / OLED
extern volatile bool sensorOnline;
extern volatile bool sdOnline;
extern volatile bool oledOnline;

// Readings lost because the log buffer was full (surfaced in /api/status so
// loss is never silent).
extern volatile uint32_t droppedSamples;

// ═══════════════════════════════════════════════════════════════
// SD DOWNLOAD STREAM COUNT
// ═══════════════════════════════════════════════════════════════
// A download streams asynchronously long after its handler returns, so the log
// task must not write meanwhile. This is a COUNTER, not a flag: concurrent
// downloads would otherwise clear each other's guard. Increment happens while
// holding fileSysMutex, and the log task checks it *after* taking that mutex,
// which removes the check-then-act window.

int  sdStreamsGet();
void sdStreamsInc();
void sdStreamsDec();

// ═══════════════════════════════════════════════════════════════
// SENSOR CALIBRATION
// ═══════════════════════════════════════════════════════════════
// The SCD30 lives on Wire1 and is owned by the sensor task. Web handlers must
// never touch it directly; they set these pending flags and the sensor task
// applies them on its next iteration.

extern volatile bool     ascEnabled;          // Auto self-calibration (default OFF)
extern volatile bool     calAscDirty;         // Apply ascEnabled to the sensor
extern volatile uint16_t calFrcPending;       // 0 = none, else ppm to force
extern volatile int16_t  calTempOffsetC10;    // Temperature offset in 0.1 °C
extern volatile bool     calTempOffsetDirty;  // Apply calTempOffsetC10
extern volatile uint32_t lastCalibrationEpoch;// When FRC was last applied

// ═══════════════════════════════════════════════════════════════
// RECENT-SAMPLE RING (implemented in tasks/logging.cpp)
// ═══════════════════════════════════════════════════════════════
// Short chart ranges are served from RAM so they never contend with the log
// task for the SD card.

void   ringPush(const SensorReading& r);
// Copies samples with from <= ts <= to into out (chronological order).
// *oldestHeld receives the oldest timestamp currently in the ring (0 if empty),
// letting the caller decide whether the ring fully covers the request.
size_t ringQuery(uint32_t from, uint32_t to, SensorReading* out, size_t maxOut,
                 uint32_t* oldestHeld);
