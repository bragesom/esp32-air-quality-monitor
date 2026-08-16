/**
 * Logging Task - Core 0
 *
 * Batches readings and appends them to daily CSV files: /logs/YYYY-MM-DD.csv
 * (UTC roll), format `iso,epoch,co2,temp,rh`.
 *
 * Retention: the oldest day-files are pruned once either LOG_MAX_DAYS or
 * LOG_MAX_BYTES is exceeded. Deleting whole old files (rather than rewriting a
 * ring in place) is deliberate: an in-place circular rewrite would hammer the
 * same sectors repeatedly. Wear levelling itself is done by the card's
 * controller; what we control is write amplification, so we keep writes
 * append-only and batched.
 *
 * Locking: the SD card is mounted once and kept mounted. flushLogBuffer() takes
 * fileSysMutex FIRST and only then checks for active downloads -- starting a
 * download also requires that mutex, so there is no check-then-act window.
 *
 * Also owns the in-RAM ring of recent samples so short chart ranges can be
 * served without touching the card at all.
 */

#include "logging.h"
#include "../shared.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <string.h>

#define LOG_DIR           "/logs"
#define FLUSH_INTERVAL_MS 60000UL
#define REMOUNT_RETRY_MS  30000UL

// Retention caps (whichever is hit first).
#define LOG_MAX_DAYS      365
#define LOG_MAX_BYTES     (512ULL * 1024ULL * 1024ULL)

static bool sdInitialized = false;
static SensorReading logBuffer[LOG_BUFFER_SIZE];
static int bufferIndex = 0;

// ── Recent-sample ring (RAM) ───────────────────────────────────
static SensorReading ring[LOG_RING_SIZE];
static size_t ringCount = 0;
static size_t ringHead = 0;
static SemaphoreHandle_t ringMutex = nullptr;

void ringPush(const SensorReading& r) {
    if (!ringMutex || r.timestamp == 0) return;
    if (xSemaphoreTake(ringMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    ring[ringHead] = r;
    ringHead = (ringHead + 1) % LOG_RING_SIZE;
    if (ringCount < LOG_RING_SIZE) ringCount++;
    xSemaphoreGive(ringMutex);
}

size_t ringQuery(uint32_t from, uint32_t to, SensorReading* out, size_t maxOut,
                 uint32_t* oldestHeld) {
    if (oldestHeld) *oldestHeld = 0;
    if (!ringMutex || !out || maxOut == 0) return 0;
    if (xSemaphoreTake(ringMutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

    const size_t start = (ringCount == LOG_RING_SIZE) ? ringHead : 0;

    if (ringCount > 0 && oldestHeld) {
        *oldestHeld = ring[start % LOG_RING_SIZE].timestamp;
    }

    // Count matches first so we can decimate evenly if there are more than
    // the caller can take.
    size_t matches = 0;
    for (size_t i = 0; i < ringCount; i++) {
        const SensorReading& r = ring[(start + i) % LOG_RING_SIZE];
        if (r.timestamp >= from && r.timestamp <= to) matches++;
    }

    const size_t stride = (matches > maxOut) ? ((matches + maxOut - 1) / maxOut) : 1;

    size_t n = 0, seen = 0;
    for (size_t i = 0; i < ringCount && n < maxOut; i++) {
        const SensorReading& r = ring[(start + i) % LOG_RING_SIZE];
        if (r.timestamp >= from && r.timestamp <= to) {
            if (seen % stride == 0) out[n++] = r;
            seen++;
        }
    }

    xSemaphoreGive(ringMutex);
    return n;
}

// ── Paths ──────────────────────────────────────────────────────
static void logPathFor(uint32_t ts, char* out, size_t n) {
    time_t t = (time_t)ts;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char day[16];
    strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
    snprintf(out, n, LOG_DIR "/%s.csv", day);
}

static void ensureLogDir() {
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
}

// ── Retention ──────────────────────────────────────────────────
// Deletes oldest day-files until within both caps. Caller must hold
// fileSysMutex. Names are YYYY-MM-DD.csv so lexical order == chronological.
static void pruneLogs() {
    for (int guard = 0; guard < 64; guard++) {
        File dir = SD.open(LOG_DIR);
        if (!dir) return;

        uint32_t count = 0;
        uint64_t total = 0;
        char oldest[32] = "";

        File e = dir.openNextFile();
        while (e) {
            if (!e.isDirectory()) {
                const char* full = e.name();
                const char* slash = strrchr(full, '/');
                const char* base = slash ? slash + 1 : full;
                count++;
                total += e.size();
                if (oldest[0] == '\0' || strcmp(base, oldest) < 0) {
                    strncpy(oldest, base, sizeof(oldest) - 1);
                    oldest[sizeof(oldest) - 1] = '\0';
                }
            }
            e = dir.openNextFile();
        }
        dir.close();
        esp_task_wdt_reset();

        if (count <= 1) return;                                   // never drop the only file
        if (count <= LOG_MAX_DAYS && total <= LOG_MAX_BYTES) return;

        char victim[64];
        snprintf(victim, sizeof(victim), LOG_DIR "/%s", oldest);
        if (!SD.remove(victim)) return;
        Serial.printf("Log retention: removed %s\n", victim);
    }
}

// ── Mount ──────────────────────────────────────────────────────
bool initSDCard() {
    if (!ringMutex) ringMutex = xSemaphoreCreateMutex();

    if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("SD: mutex timeout");
        return false;
    }
    sdInitialized = SD.begin(SD_CS_PIN);
    if (sdInitialized) {
        ensureLogDir();
        pruneLogs();
    }
    xSemaphoreGive(fileSysMutex);

    sdOnline = sdInitialized;
    Serial.println(sdInitialized ? "SD: ready" : "SD: not found (logging disabled)");
    return sdInitialized;
}

// Attempt to remount after a failure so a transient error doesn't disable the
// card permanently.
static bool tryRemount() {
    if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (sdStreamsGet() > 0) { xSemaphoreGive(fileSysMutex); return false; }

    SD.end();
    bool ok = SD.begin(SD_CS_PIN);
    if (ok) {
        ensureLogDir();
        pruneLogs();
    }
    sdInitialized = ok;
    xSemaphoreGive(fileSysMutex);

    sdOnline = ok;
    if (ok) Serial.println("SD: remounted");
    return ok;
}

// ── Writing ────────────────────────────────────────────────────
static void writeRow(File& f, const SensorReading& r) {
    time_t t = (time_t)r.timestamp;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    f.printf("%s,%lu,%u,%.1f,%.1f\n",
             iso, (unsigned long)r.timestamp, r.co2,
             r.temperature / 10.0f, r.humidity / 10.0f);
}

// Returns true when the buffer was written (or was empty); false when deferred
// or failed so the caller retries soon.
static bool flushLogBuffer() {
    if (!sdInitialized || bufferIndex == 0) return true;

    // Mutex first, THEN the stream check -- a download can only start while
    // holding this same mutex, so it cannot slip in behind the check.
    if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (sdStreamsGet() > 0) {
        xSemaphoreGive(fileSysMutex);
        return false;
    }

    bool ok = true;
    bool rolled = false;
    int i = 0;

    while (i < bufferIndex && ok) {
        char path[64];
        logPathFor(logBuffer[i].timestamp, path, sizeof(path));
        const bool isNew = !SD.exists(path);

        File f = SD.open(path, FILE_APPEND);
        if (!f) { ok = false; break; }
        if (isNew) {
            f.println("iso,epoch,co2,temp,rh");
            rolled = true;
        }

        // Write every consecutive entry that belongs to this day-file.
        while (i < bufferIndex) {
            char p2[64];
            logPathFor(logBuffer[i].timestamp, p2, sizeof(p2));
            if (strcmp(p2, path) != 0) break;
            writeRow(f, logBuffer[i]);
            i++;
        }
        f.close();
    }

    if (ok && rolled) pruneLogs();   // day rolled over: enforce retention
    xSemaphoreGive(fileSysMutex);

    if (ok) {
        Serial.printf("Logged %d row(s)\n", bufferIndex);
        bufferIndex = 0;
        sdOnline = true;
    } else {
        Serial.println("SD: append failed");
        sdOnline = false;
    }
    return ok;
}

// ── Task ───────────────────────────────────────────────────────
void logTaskFunction(void* parameter) {
    (void)parameter;
    esp_task_wdt_add(NULL);
    Serial.printf("[Core %d] Log task started\n", xPortGetCoreID());

    SensorReading reading;
    uint32_t lastFlush = millis();
    uint32_t lastRemountTry = millis();

    while (true) {
        esp_task_wdt_reset();

        if (xQueueReceive(sensorQueue, &reading, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (bufferIndex < LOG_BUFFER_SIZE) {
                logBuffer[bufferIndex++] = reading;
            } else {
                droppedSamples++;   // visible via /api/status
            }
            if (bufferIndex >= LOG_BUFFER_SIZE - 1) {
                if (flushLogBuffer()) lastFlush = millis();
            }
        }

        if (bufferIndex > 0 && millis() - lastFlush > FLUSH_INTERVAL_MS) {
            if (flushLogBuffer()) lastFlush = millis();
        }

        // Recover from a card that went away or a transient write failure.
        if (!sdOnline && millis() - lastRemountTry > REMOUNT_RETRY_MS) {
            lastRemountTry = millis();
            tryRemount();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
