/**
 * Web endpoints + WebSocket ownership
 *
 * REST API and the *only* place the AsyncWebSocket client list is driven from an
 * application task. The SD card is mounted once at boot and kept mounted; the
 * handlers never call SD.begin()/SD.end() — they take fileSysMutex for the short
 * duration of an access.
 *
 * WebSocket model:
 *   HTTP/WS *callbacks* run on AsyncTCP's own task (CONFIG_ASYNC_TCP_RUNNING_CORE),
 *   not here. webTaskFunction() is the single application task that broadcasts
 *   time/sensor/status frames and runs cleanupClients() — so exactly one task
 *   mutates the client list from the app side. loop() performs no WebSocket work.
 *
 * Downloads:
 *   A file download streams asynchronously long after handleFileDownload()
 *   returns. It bumps a stream COUNTER (sdStreamsInc/Dec, guarded by a spinlock)
 *   while holding fileSysMutex, and the log task checks that counter *after*
 *   taking the same mutex, so writes defer without a check-then-act window. The
 *   counter is decremented exactly once per stream by a shared_ptr guard shared
 *   between the body filler and onDisconnect.
 *
 * Routes:
 *   POST   /api/timeset            {timestamp|epoch}
 *   GET    /api/status
 *   GET    /api/settings           |  POST /api/settings (form: interval 10-1800)
 *   GET    /api/calibration        |  POST /api/calibration (form: asc, frc, tempOffset)
 *   GET    /api/measurements       ?from&to&limit   (limit<=1 → latest reading)
 *   GET    /api/fs/info
 *   GET    /api/fs/list            ?path
 *   GET    /api/fs/preview         ?path&lines
 *   GET    /api/fs/download        ?path
 *   DELETE /api/fs/delete          ?path
 *   WS     /ws
 */

#include "endpoints.h"
#include "../shared.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <SPIFFS.h>
#include <SD.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <memory>
#include <stdlib.h>
#include <string.h>

extern AsyncWebSocket webSocket;

#define MAX_POINTS     240   // cap on chart points returned by /api/measurements
#define SCAN_MAX_DAYS  366   // bound the day-file loop (retention keeps <= 365 days)

// Set by the WS connect callback (AsyncTCP task) so the web task pushes a full
// frame set to a freshly connected client without the callback itself calling
// textAll (keeping all broadcasts on one task).
static volatile bool wsForceBroadcast = false;

// ═══════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════

static bool validPath(const String& p) {
    return p.length() > 0 && p.indexOf("..") == -1;
}

static String normalizePath(String p) {
    if (!p.startsWith("/")) p = "/" + p;
    if (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
    return p;
}

static String contentTypeFor(const String& name) {
    if (name.endsWith(".csv"))  return "text/csv";
    if (name.endsWith(".txt"))  return "text/plain";
    if (name.endsWith(".json")) return "application/json";
    if (name.endsWith(".html")) return "text/html";
    if (name.endsWith(".js"))   return "application/javascript";
    if (name.endsWith(".css"))  return "text/css";
    return "application/octet-stream";
}

// Parse "23.4" / "-1.2" / "45" to tenths (23.4 -> 234). No float round-trip.
static int32_t parseTenths(const char* s) {
    if (!s) return 0;
    while (*s == ' ') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    int32_t whole = 0;
    while (*s >= '0' && *s <= '9') { whole = whole * 10 + (*s - '0'); s++; }
    int32_t tenths = 0;
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') tenths = s[1] - '0';
    int32_t v = whole * 10 + tenths;
    return neg ? -v : v;
}

// ═══════════════════════════════════════════════════════════════
// WebSocket — event callback (AsyncTCP task) + broadcasts (web task)
// ═══════════════════════════════════════════════════════════════

static void onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                             AwsEventType type, void* arg, uint8_t* data, size_t len) {
    (void)server; (void)arg; (void)data; (void)len;
    if (type == WS_EVT_CONNECT) {
        Serial.printf("WS client #%u connected (%s)\n",
                      client->id(), client->remoteIP().toString().c_str());
        wsForceBroadcast = true;   // web task sends a full frame set next tick
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("WS client #%u disconnected\n", client->id());
    }
}

// All three broadcasters are called ONLY from webTaskFunction.
static void broadcastSensor() {
    SensorReading s = {0, 0, 0, 0};
    if (xSemaphoreTake(readingMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s = latestReading;
        xSemaphoreGive(readingMutex);
    }
    char m[192];
    snprintf(m, sizeof(m),
        "{\"type\":\"sensor\",\"timestamp\":%lu,\"co2\":%u,\"temp\":%.1f,\"rh\":%.1f}",
        (unsigned long)s.timestamp, s.co2, s.temperature / 10.0f, s.humidity / 10.0f);
    webSocket.textAll(m);
}

static void broadcastTime() {
    uint32_t ts = timeIsSet ? (uint32_t)time(nullptr) : 0;
    char m[128];
    snprintf(m, sizeof(m),
        "{\"type\":\"time\",\"timestamp\":%lu,\"uptime\":%lu,\"interval\":%u}",
        (unsigned long)ts, (unsigned long)(millis() / 1000), measureInterval);
    webSocket.textAll(m);
}

static void broadcastStatus() {
    char m[224];
    snprintf(m, sizeof(m),
        "{\"type\":\"status\",\"sensor\":%s,\"sd\":%s,\"oled\":%s,\"wifi\":true,"
        "\"timeIsSet\":%s,\"uptime\":%lu,\"dropped\":%lu}",
        sensorOnline ? "true" : "false",
        sdOnline     ? "true" : "false",
        oledOnline   ? "true" : "false",
        timeIsSet    ? "true" : "false",
        (unsigned long)(millis() / 1000),
        (unsigned long)droppedSamples);
    webSocket.textAll(m);
}

// ═══════════════════════════════════════════════════════════════
// Core endpoints
// ═══════════════════════════════════════════════════════════════

static void handleTimeSet(AsyncWebServerRequest* request, JsonVariant json) {
    if (!json.is<JsonObject>()) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    JsonObject obj = json.as<JsonObject>();
    uint32_t epoch = 0;
    if (obj["timestamp"].is<uint32_t>())   epoch = obj["timestamp"].as<uint32_t>();
    else if (obj["epoch"].is<uint32_t>())  epoch = obj["epoch"].as<uint32_t>();

    if (epoch < 1000000000UL) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid timestamp\"}");
        return;
    }

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    timeIsSet  = true;
    clockDirty = true;   // ask the loop task to persist the clock to NVS now

    Serial.printf("Time synced: epoch %lu\n", (unsigned long)epoch);
    request->send(200, "application/json",
                  "{\"success\":true,\"status\":\"success\",\"message\":\"Time synchronized\"}");
}

static void handleStatus(AsyncWebServerRequest* request) {
    char r[384];
    snprintf(r, sizeof(r),
        "{\"success\":true,\"timeIsSet\":%s,\"uptime\":%lu,\"freeHeap\":%u,"
        "\"wsClients\":%u,\"measureInterval\":%u,\"dropped\":%lu,"
        "\"sensor\":%s,\"sd\":%s,\"oled\":%s,\"wifi\":true}",
        timeIsSet ? "true" : "false",
        (unsigned long)(millis() / 1000),
        ESP.getFreeHeap(),
        webSocket.count(),
        measureInterval,
        (unsigned long)droppedSamples,
        sensorOnline ? "true" : "false",
        sdOnline     ? "true" : "false",
        oledOnline   ? "true" : "false");
    request->send(200, "application/json", r);
}

static void handleSettings(AsyncWebServerRequest* request) {
    if (request->method() == HTTP_POST) {
        bool ok = false;
        const char* msg = "No changes";
        if (request->hasParam("interval", true)) {
            long v = request->getParam("interval", true)->value().toInt();
            if (v >= INTERVAL_MIN && v <= INTERVAL_MAX) {
                measureInterval = (uint16_t)v;
                settingsDirty = true;   // loop task persists to NVS
                ok = true;
                msg = "Interval updated";
            } else {
                msg = "Invalid interval (10-1800)";
            }
        }
        char r[192];
        snprintf(r, sizeof(r),
                 "{\"success\":%s,\"message\":\"%s\",\"measureInterval\":%u}",
                 ok ? "true" : "false", msg, measureInterval);
        request->send(ok ? 200 : 400, "application/json", r);
        return;
    }

    // GET
    char r[160];
    snprintf(r, sizeof(r),
             "{\"success\":true,\"measureInterval\":%u,\"timeIsSet\":%s,\"uptime\":%lu}",
             measureInterval,
             timeIsSet ? "true" : "false",
             (unsigned long)(millis() / 1000));
    request->send(200, "application/json", r);
}

// ═══════════════════════════════════════════════════════════════
// Calibration — sets pending flags; the SENSOR TASK does the I2C writes
// ═══════════════════════════════════════════════════════════════

static void handleCalibrationGet(AsyncWebServerRequest* request) {
    char r[256];
    snprintf(r, sizeof(r),
        "{\"success\":true,\"asc\":%s,\"tempOffset\":%.1f,\"lastCalibration\":%lu,"
        "\"frcMin\":%d,\"frcMax\":%d,\"frcPending\":%s}",
        ascEnabled ? "true" : "false",
        calTempOffsetC10 / 10.0f,
        (unsigned long)lastCalibrationEpoch,
        FRC_MIN, FRC_MAX,
        calFrcPending ? "true" : "false");
    request->send(200, "application/json", r);
}

static void handleCalibrationPost(AsyncWebServerRequest* request) {
    bool changed = false;
    char msg[96];
    strcpy(msg, "No changes");

    if (request->hasParam("asc", true)) {
        String v = request->getParam("asc", true)->value();
        bool on = (v == "1" || v == "true" || v == "on");
        ascEnabled  = on;
        calAscDirty = true;               // sensor task applies + persists
        changed = true;
        snprintf(msg, sizeof(msg), "ASC %s", on ? "enabled" : "disabled");
    }

    if (request->hasParam("tempOffset", true)) {
        float off = request->getParam("tempOffset", true)->value().toFloat();
        if (off < 0)  off = 0;            // SCD30 offset is a positive self-heating comp.
        if (off > 20) off = 20;
        calTempOffsetC10   = (int16_t)(off * 10.0f + 0.5f);
        calTempOffsetDirty = true;
        changed = true;
        snprintf(msg, sizeof(msg), "Temp offset %.1f C", off);
    }

    if (request->hasParam("frc", true)) {
        long ppm = request->getParam("frc", true)->value().toInt();
        if (ppm < FRC_MIN || ppm > FRC_MAX) {
            request->send(400, "application/json",
                "{\"success\":false,\"error\":\"FRC out of range (400-2000)\"}");
            return;
        }
        calFrcPending = (uint16_t)ppm;    // one-shot; sensor task consumes it
        changed = true;
        snprintf(msg, sizeof(msg), "Recalibrating to %ld ppm", ppm);
    }

    char r[256];
    snprintf(r, sizeof(r),
        "{\"success\":%s,\"message\":\"%s\",\"asc\":%s,\"tempOffset\":%.1f,\"frcPending\":%s}",
        changed ? "true" : "false", msg,
        ascEnabled ? "true" : "false",
        calTempOffsetC10 / 10.0f,
        calFrcPending ? "true" : "false");
    request->send(200, "application/json", r);
}

// ═══════════════════════════════════════════════════════════════
// Measurements — RAM ring fast path, else decimated day-file scan
// ═══════════════════════════════════════════════════════════════

static void handleMeasurements(AsyncWebServerRequest* request) {
    if (!timeIsSet) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Time not set\"}");
        return;
    }

    uint32_t from  = 0;
    uint32_t to    = (uint32_t)time(nullptr);
    uint32_t limit = 1;
    if (request->hasParam("from")) from = (uint32_t)request->getParam("from")->value().toInt();
    if (request->hasParam("to"))   to   = (uint32_t)request->getParam("to")->value().toInt();
    if (request->hasParam("limit")) {
        long l = request->getParam("limit")->value().toInt();
        limit = (l < 1) ? 1 : (uint32_t)l;
    }

    // Latest reading — always the newest sample.
    if (limit <= 1) {
        SensorReading s = {0, 0, 0, 0};
        if (xSemaphoreTake(readingMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s = latestReading;
            xSemaphoreGive(readingMutex);
        }
        char r[256];
        snprintf(r, sizeof(r),
            "{\"success\":true,\"count\":1,\"data\":[{\"timestamp\":%lu,\"co2\":%u,"
            "\"temperature\":%d,\"humidity\":%u}]}",
            (unsigned long)s.timestamp, s.co2, s.temperature, s.humidity);
        request->send(200, "application/json", r);
        return;
    }

    uint32_t want = limit > MAX_POINTS ? MAX_POINTS : limit;
    if (want < 1) want = 1;

    SensorReading* out = (SensorReading*)malloc(MAX_POINTS * sizeof(SensorReading));
    if (!out) {
        request->send(500, "application/json", "{\"success\":false,\"error\":\"Out of memory\"}");
        return;
    }
    size_t n = 0;

    // 1) RAM ring fast path: if the ring's oldest sample is at/older than `from`,
    //    it fully covers [from,to] (the ring always holds the newest samples) and
    //    we never touch the SD card — the dashboard/"Last Hour" cost nothing.
    uint32_t oldestHeld = 0;
    size_t rn = ringQuery(from, to, out, want, &oldestHeld);
    bool servedFromRing = (oldestHeld != 0 && from >= oldestHeld);

    if (servedFromRing) {
        n = rn;
    } else if (sdOnline && to >= from) {
        // 2) SD day-file scan with single-pass adaptive decimation. Only the day
        //    files intersecting [from,to] are opened, and the mutex is released
        //    between files so the log task isn't starved.
        uint32_t earliest = (to > (uint32_t)SCAN_MAX_DAYS * 86400UL)
                          ? to - (uint32_t)SCAN_MAX_DAYS * 86400UL : 0;
        if (from < earliest) from = earliest;

        uint32_t stride   = 1;   // sample every stride-th matching row
        uint64_t matchIdx = 0;
        uint32_t dayStart = from - (from % 86400UL);

        for (uint32_t d = dayStart; d <= to; d += 86400UL) {
            time_t tt = (time_t)d;
            struct tm tmv;
            gmtime_r(&tt, &tmv);
            char day[16];
            strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
            char path[40];
            snprintf(path, sizeof(path), "/logs/%s.csv", day);

            if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(2000)) != pdTRUE) break;
            if (SD.exists(path)) {
                File f = SD.open(path, FILE_READ);
                if (f) {
                    char line[128];
                    f.readBytesUntil('\n', line, sizeof(line) - 1);   // skip header
                    while (f.available()) {
                        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
                        if (len <= 0) continue;
                        line[len] = '\0';

                        char* fld[5]; int nf = 0; char* p = line;
                        fld[nf++] = p;
                        while (*p && nf < 5) { if (*p == ',') { *p = '\0'; fld[nf++] = p + 1; } p++; }
                        if (nf < 5) continue;

                        uint32_t ts = strtoul(fld[1], nullptr, 10);
                        if (ts < from || ts > to) continue;

                        if (matchIdx % stride == 0) {
                            SensorReading r;
                            r.timestamp   = ts;
                            r.co2         = (uint16_t)strtoul(fld[2], nullptr, 10);
                            r.temperature = (int16_t)parseTenths(fld[3]);
                            r.humidity    = (uint16_t)parseTenths(fld[4]);
                            out[n++] = r;
                            if (n >= want) {                 // full: keep every other, halve
                                size_t w = 0;
                                for (size_t i = 0; i < n; i += 2) out[w++] = out[i];
                                n = w;
                                stride *= 2;
                            }
                        }
                        matchIdx++;
                    }
                    f.close();
                }
            }
            xSemaphoreGive(fileSysMutex);
        }
    }

    // Serialize (temperature/humidity stay in 0.1-unit ints; the UI scales).
    const size_t BUF = 20480;
    char* buf = (char*)malloc(BUF);
    if (!buf) {
        free(out);
        request->send(500, "application/json", "{\"success\":false,\"error\":\"Out of memory\"}");
        return;
    }
    int off = snprintf(buf, BUF, "{\"success\":true,\"data\":[");
    for (size_t i = 0; i < n && off < (int)BUF - 120; i++) {
        off += snprintf(buf + off, BUF - off,
            "%s{\"timestamp\":%lu,\"co2\":%u,\"temperature\":%d,\"humidity\":%u}",
            i ? "," : "",
            (unsigned long)out[i].timestamp, out[i].co2,
            out[i].temperature, out[i].humidity);
    }
    snprintf(buf + off, BUF - off, "],\"count\":%u,\"source\":\"%s\"}",
             (unsigned)n, servedFromRing ? "ram" : "sd");
    request->send(200, "application/json", buf);
    free(buf);
    free(out);
}

// ═══════════════════════════════════════════════════════════════
// Filesystem endpoints (SD stays mounted; guarded by fileSysMutex)
// ═══════════════════════════════════════════════════════════════

static void handleFSInfo(AsyncWebServerRequest* request) {
    uint64_t spiUsed = SPIFFS.usedBytes();
    uint64_t spiTotal = SPIFFS.totalBytes();
    uint64_t sdTotal = 0, sdUsed = 0;
    uint32_t fileCount = 0;
    bool sd = false;

    if (sdOnline && xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        sd = true;
        sdTotal = SD.totalBytes();
        sdUsed  = SD.usedBytes();
        File root = SD.open("/");
        if (root && root.isDirectory()) {
            File e = root.openNextFile();
            while (e) { if (!e.isDirectory()) fileCount++; e = root.openNextFile(); }
            root.close();
        }
        xSemaphoreGive(fileSysMutex);
    }

    char r[384];
    snprintf(r, sizeof(r),
        "{\"success\":true,"
        "\"spiffs\":{\"used\":%llu,\"total\":%llu,\"free\":%llu},"
        "\"sd\":{\"available\":%s,\"total\":%llu,\"used\":%llu,\"free\":%llu,\"fileCount\":%u}}",
        spiUsed, spiTotal, spiTotal - spiUsed,
        sd ? "true" : "false",
        sdTotal, sdUsed, sdTotal - sdUsed, fileCount);
    request->send(200, "application/json", r);
}

static void handleFSList(AsyncWebServerRequest* request) {
    String path = "/";
    if (request->hasParam("path")) path = request->getParam("path")->value();
    if (path.indexOf("..") != -1) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid path\"}");
        return;
    }
    path = normalizePath(path);

    if (!sdOnline) {
        request->send(200, "application/json",
                      "{\"success\":true,\"path\":\"/\",\"files\":[]}");
        return;
    }

    const size_t BUF = 8192;
    char* buf = (char*)malloc(BUF);
    if (!buf) {
        request->send(500, "application/json", "{\"success\":false,\"error\":\"Out of memory\"}");
        return;
    }
    int offset = snprintf(buf, BUF, "{\"success\":true,\"path\":\"%s\",\"files\":[", path.c_str());

    if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        File root = SD.open(path);
        if (root && root.isDirectory()) {
            bool first = true;
            File e = root.openNextFile();
            while (e && offset < (int)BUF - 200) {
                const char* full = e.name();
                const char* slash = strrchr(full, '/');
                const char* base = slash ? slash + 1 : full;
                if (base[0] != '.') {
                    String full_path = (path == "/") ? ("/" + String(base))
                                                     : (path + "/" + String(base));
                    offset += snprintf(buf + offset, BUF - offset,
                        "%s{\"name\":\"%s\",\"path\":\"%s\",\"size\":%u,\"isDir\":%s,\"lastModified\":0}",
                        first ? "" : ",",
                        base, full_path.c_str(),
                        e.isDirectory() ? 0 : (uint32_t)e.size(),
                        e.isDirectory() ? "true" : "false");
                    first = false;
                }
                e = root.openNextFile();
            }
            root.close();
        }
        xSemaphoreGive(fileSysMutex);
    }

    snprintf(buf + offset, BUF - offset, "]}");
    request->send(200, "application/json", buf);
    free(buf);
}

static void handleFilePreview(AsyncWebServerRequest* request) {
    String path = request->hasParam("path") ? request->getParam("path")->value() : "";
    int lines = 40;
    if (request->hasParam("lines")) {
        lines = request->getParam("lines")->value().toInt();
        if (lines < 1) lines = 40;
        if (lines > 200) lines = 200;
    }
    if (!validPath(path)) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid path\"}");
        return;
    }
    path = normalizePath(path);
    if (!sdOnline) {
        request->send(404, "application/json", "{\"success\":false,\"error\":\"SD not available\"}");
        return;
    }

    const size_t BUF = 24576;
    char* out = (char*)malloc(BUF);
    if (!out) {
        request->send(500, "application/json", "{\"success\":false,\"error\":\"Out of memory\"}");
        return;
    }
    int offset = snprintf(out, BUF, "{\"success\":true,\"path\":\"%s\",\"content\":\"", path.c_str());
    bool ok = false;

    if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (SD.exists(path)) {
            File f = SD.open(path, FILE_READ);
            if (f) {
                int read = 0;
                while (f.available() && read < lines && offset < (int)BUF - 8) {
                    String line = f.readStringUntil('\n');
                    for (size_t i = 0; i < line.length() && offset < (int)BUF - 8; i++) {
                        char c = line[i];
                        switch (c) {
                            case '"':  out[offset++] = '\\'; out[offset++] = '"';  break;
                            case '\\': out[offset++] = '\\'; out[offset++] = '\\'; break;
                            case '\r': break;
                            default:
                                if ((unsigned char)c >= 32) out[offset++] = c;
                        }
                    }
                    out[offset++] = '\\'; out[offset++] = 'n';
                    read++;
                }
                f.close();
                ok = true;
            }
        }
        xSemaphoreGive(fileSysMutex);
    }

    if (ok) {
        snprintf(out + offset, BUF - offset, "\"}");
        request->send(200, "application/json", out);
    } else {
        request->send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
    }
    free(out);
}

static void handleDeleteFile(AsyncWebServerRequest* request) {
    String path = request->hasParam("path") ? request->getParam("path")->value() : "";
    if (!validPath(path)) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid path\"}");
        return;
    }
    path = normalizePath(path);
    if (path == "/" || !sdOnline) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Cannot delete\"}");
        return;
    }

    bool ok = false;
    const char* msg = "File not found";
    if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (SD.exists(path)) {
            ok = SD.remove(path);
            msg = ok ? "Deleted" : "Delete failed";
        }
        xSemaphoreGive(fileSysMutex);
    }
    char r[192];
    snprintf(r, sizeof(r), "{\"success\":%s,\"message\":\"%s\",\"path\":\"%s\"}",
             ok ? "true" : "false", msg, path.c_str());
    request->send(ok ? 200 : 400, "application/json", r);
}

// GET /api/fs/download — stream a file from SD. Increments the stream counter
// (under fileSysMutex, so the log task can't slip a write in behind the check)
// and decrements exactly once when the stream ends OR the client disconnects,
// via a shared_ptr whose deleter runs when the last reference is dropped.
static void handleFileDownload(AsyncWebServerRequest* request) {
    String path = request->hasParam("path") ? request->getParam("path")->value() : "";
    if (!validPath(path)) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid path\"}");
        return;
    }
    path = normalizePath(path);
    if (!sdOnline) {
        request->send(404, "application/json", "{\"success\":false,\"error\":\"SD not available\"}");
        return;
    }

    if (xSemaphoreTake(fileSysMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        request->send(503, "application/json", "{\"success\":false,\"error\":\"Busy\"}");
        return;
    }

    if (!SD.exists(path)) {
        xSemaphoreGive(fileSysMutex);
        request->send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
        return;
    }

    File file = SD.open(path, FILE_READ);
    if (!file) {
        xSemaphoreGive(fileSysMutex);
        request->send(404, "application/json", "{\"success\":false,\"error\":\"Open failed\"}");
        return;
    }

    sdStreamsInc();   // while holding the mutex: no check-then-act window vs the log task

    // Runs sdStreamsDec() once when the last copy (filler + onDisconnect) is gone.
    auto streamGuard = std::shared_ptr<void>(nullptr, [](void*) { sdStreamsDec(); });

    size_t fsize = file.size();
    String name = path.substring(path.lastIndexOf('/') + 1);

    AsyncWebServerResponse* response = request->beginResponse(
        contentTypeFor(name), fsize,
        [file, fsize, streamGuard](uint8_t* buffer, size_t maxLen, size_t index) mutable -> size_t {
            size_t n = file.read(buffer, maxLen);
            if (n == 0 || index + n >= fsize) file.close();
            return n;
        });
    response->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    request->onDisconnect([streamGuard]() { /* guard released with this lambda */ });
    request->send(response);

    // Reads happen later in the filler (AsyncTCP task); safe to release now.
    xSemaphoreGive(fileSysMutex);
}

// ═══════════════════════════════════════════════════════════════
// Setup + web task (single owner of all WebSocket I/O)
// ═══════════════════════════════════════════════════════════════

void setupWebEndpoints(AsyncWebServer& server, AsyncWebSocket& ws) {
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);

    AsyncCallbackJsonWebHandler* timeset =
        new AsyncCallbackJsonWebHandler("/api/timeset");
    timeset->onRequest([](AsyncWebServerRequest* req, JsonVariant& json) {
        handleTimeSet(req, json);
    });
    server.addHandler(timeset);

    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/settings", HTTP_GET, handleSettings);
    server.on("/api/settings", HTTP_POST, handleSettings);
    server.on("/api/calibration", HTTP_GET, handleCalibrationGet);
    server.on("/api/calibration", HTTP_POST, handleCalibrationPost);
    server.on("/api/measurements", HTTP_GET, handleMeasurements);

    server.on("/api/fs/info", HTTP_GET, handleFSInfo);
    server.on("/api/fs/list", HTTP_GET, handleFSList);
    server.on("/api/fs/preview", HTTP_GET, handleFilePreview);
    server.on("/api/fs/download", HTTP_GET, handleFileDownload);
    server.on("/api/fs/delete", HTTP_DELETE, handleDeleteFile);

    server.serveStatic("/", SPIFFS, "/www/").setDefaultFile("index.html");

    server.onNotFound([](AsyncWebServerRequest* request) {
        if (request->url().startsWith("/api/")) {
            request->send(404, "application/json", "{\"success\":false,\"error\":\"Not found\"}");
        } else {
            request->send(404, "text/plain", "Not found");
        }
    });

    Serial.println("Web endpoints registered");
}

// The ONLY application task that touches the WebSocket client list: broadcasts
// live frames and runs cleanupClients(). HTTP/WS callbacks run on AsyncTCP's
// task; this one just drives the periodic pushes and the watchdog.
void webTaskFunction(void* parameter) {
    (void)parameter;
    esp_task_wdt_add(NULL);
    Serial.printf("[Core %d] Web task started (owns all WebSocket I/O)\n", xPortGetCoreID());

    uint32_t lastSeq = 0;
    uint32_t lastTime = 0, lastStatus = 0, lastCleanup = 0;

    while (true) {
        esp_task_wdt_reset();
        uint32_t now = millis();

        if (now - lastCleanup > 1000) {
            webSocket.cleanupClients(MAX_WEBSOCKET_CLIENTS);
            lastCleanup = now;
        }

        if (webSocket.count() > 0) {
            bool force = wsForceBroadcast;
            if (force) wsForceBroadcast = false;

            uint32_t seq = readingSeq;
            if (force || seq != lastSeq) {   // a new reading landed
                broadcastSensor();
                lastSeq = seq;
            }
            if (force || now - lastTime > 1000) {     // ~1 Hz live clock
                broadcastTime();
                lastTime = now;
            }
            if (force || now - lastStatus > 3000) {   // status every 3 s
                broadcastStatus();
                lastStatus = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
