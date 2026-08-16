/**
 * Sensor Task - Core 0
 *
 * Polls the SCD30 (I2C bus 1 / Wire1, clamped to 100 kHz — the sensor's hard max)
 * and dispatches each reading:
 *   - latestReading + RAM ring + OLED display: ALWAYS
 *   - logging queue: only once the clock is known (timeIsSet), so logged rows
 *     always carry a real epoch timestamp.
 *
 * This task OWNS the SCD30. All I2C writes to it — interval, ASC, forced
 * recalibration, temperature offset — happen here; web handlers only set the
 * pending flags in shared.h. That keeps Wire1 single-threaded.
 */

#include "sensor.h"
#include "../shared.h"
#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_SCD30_Arduino_Library.h>
#include <esp_task_wdt.h>
#include <time.h>

#define REINIT_RETRY_MS 30000UL

static SCD30 airSensor;
static TwoWire sensorWire = TwoWire(1);
static uint32_t lastSensorOkMs = 0;

static uint16_t clampInterval(uint16_t s) {
    if (s < INTERVAL_MIN) return INTERVAL_MIN;
    if (s > INTERVAL_MAX) return INTERVAL_MAX;   // 1800 s = SCD30 maximum
    return s;
}

bool initSensor() {
    sensorWire.begin(SCD30_SDA_PIN, SCD30_SCL_PIN, 100000);
    if (!airSensor.begin(sensorWire)) return false;

    airSensor.setMeasurementInterval(clampInterval(measureInterval));
    airSensor.setAutoSelfCalibration(ascEnabled);   // defaults OFF
    if (calTempOffsetC10 != 0) {
        airSensor.setTemperatureOffset(calTempOffsetC10 / 10.0f);
    }

    lastSensorOkMs = millis();
    Serial.printf("SCD30: initialized (ASC %s)\n", ascEnabled ? "on" : "off");
    return true;
}

// Apply calibration commands queued by the web API. Runs on this task only.
static void applyPendingCalibration() {
    if (calAscDirty) {
        if (airSensor.setAutoSelfCalibration(ascEnabled)) {
            Serial.printf("SCD30: ASC %s\n", ascEnabled ? "enabled" : "disabled");
            settingsDirty = true;
        } else {
            Serial.println("SCD30: ASC change failed");
        }
        calAscDirty = false;
    }

    if (calTempOffsetDirty) {
        if (airSensor.setTemperatureOffset(calTempOffsetC10 / 10.0f)) {
            Serial.printf("SCD30: temperature offset %.1f C\n", calTempOffsetC10 / 10.0f);
            settingsDirty = true;
        } else {
            Serial.println("SCD30: temperature offset change failed");
        }
        calTempOffsetDirty = false;
    }

    uint16_t frc = calFrcPending;
    if (frc != 0) {
        if (airSensor.setForcedRecalibrationFactor(frc)) {
            lastCalibrationEpoch = timeIsSet ? (uint32_t)time(nullptr) : 0;
            settingsDirty = true;
            Serial.printf("SCD30: forced recalibration to %u ppm\n", frc);
        } else {
            Serial.println("SCD30: forced recalibration failed");
        }
        calFrcPending = 0;
    }
}

void sensorTaskFunction(void* parameter) {
    (void)parameter;
    esp_task_wdt_add(NULL);
    Serial.printf("[Core %d] Sensor task started\n", xPortGetCoreID());

    uint16_t appliedInterval = clampInterval(measureInterval);
    uint32_t lastReinitMs = millis();
    lastSensorOkMs = millis();

    while (true) {
        esp_task_wdt_reset();

        applyPendingCalibration();

        // Re-pace the sensor if the interval changed via /api/settings. Store
        // the CLAMPED value so NVS/UI/sensor can never disagree.
        const uint16_t wantInterval = clampInterval(measureInterval);
        if (wantInterval != appliedInterval) {
            if (airSensor.setMeasurementInterval(wantInterval)) {
                appliedInterval = wantInterval;
                Serial.printf("SCD30: interval -> %us\n", appliedInterval);
            }
        }

        if (airSensor.dataAvailable()) {
            SensorReading r;
            r.timestamp   = timeIsSet ? (uint32_t)time(nullptr) : 0;
            r.co2         = (uint16_t)airSensor.getCO2();
            r.temperature = (int16_t)(airSensor.getTemperature() * 10.0f);
            r.humidity    = (uint16_t)(airSensor.getHumidity() * 10.0f);

            sensorOnline   = true;
            lastSensorOkMs = millis();

            // Share for live display / WebSocket / API.
            if (xSemaphoreTake(readingMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                latestReading = r;
                readingSeq++;
                xSemaphoreGive(readingMutex);
            }

            // Recent-sample ring (serves short chart ranges without SD).
            ringPush(r);

            // OLED display: always (replace oldest if the queue is full).
            if (xQueueSend(displayQueue, &r, 0) != pdTRUE) {
                SensorReading discard;
                xQueueReceive(displayQueue, &discard, 0);
                xQueueSend(displayQueue, &r, 0);
            }

            // Logging: only when we have a real clock.
            if (timeIsSet && xQueueSend(sensorQueue, &r, 0) != pdTRUE) {
                droppedSamples++;
                Serial.println("Sensor queue full (logging)");
            }

            Serial.printf("CO2 %u ppm  T %.1fC  RH %.1f%%%s\n",
                          r.co2, r.temperature / 10.0f, r.humidity / 10.0f,
                          timeIsSet ? "" : "  (time not set)");
        }

        // Liveness: if the sensor stops answering, say so and try to recover
        // instead of reporting "online" forever.
        uint32_t staleAfter = (uint32_t)appliedInterval * 3000UL;
        if (staleAfter < 90000UL) staleAfter = 90000UL;

        if (millis() - lastSensorOkMs > staleAfter) {
            if (sensorOnline) {
                sensorOnline = false;
                Serial.println("SCD30: no data - marking offline");
            }
            if (millis() - lastReinitMs > REINIT_RETRY_MS) {
                lastReinitMs = millis();
                Serial.println("SCD30: attempting re-init");
                initSensor();   // resets the staleness window on success
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
