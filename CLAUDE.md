# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 air quality monitor ("Theia" / "AQ-Meter Pro"). Reads CO2/temperature/humidity from an
SCD30 sensor, logs to an SD card as daily CSV files, shows values on an SSD1306 OLED, and
serves an **offline** web UI + JSON API over a self-hosted WiFi access point. FreeRTOS tasks
are pinned across both cores. UI, code and docs are in English.

## Build & Upload

PlatformIO project. Single environment **`esp32_3mb`** (4MB flash, ~2MB app + ~2MB SPIFFS,
`partitions_3mb.csv`).

```bash
pio run                              # build firmware
pio run --target upload              # flash firmware over USB
pio run --target uploadfs            # flash SPIFFS (web files in data/www)
pio device monitor                   # serial monitor @ 115200
```

Firmware **and** filesystem must both be uploaded after web-asset changes. `upload_complete.ps1`
(Windows) / `upload_complete.sh` (POSIX) do the full sequence (build → upload → uploadfs →
monitor). There are no unit tests (`test/` is an empty scaffold).

`.github/workflows/build.yml` runs `pio run` and `pio run --target buildfs` on push and PR.

### Credentials

The WiFi AP credentials live in **`src/secrets.h`**, which is **gitignored**. `src/main.cpp`
includes it and `#error`s with a useful message if the macros are missing.
`src/secrets.example.h` is the committed template — a fresh clone must copy it to
`src/secrets.h` before building, and CI does exactly that. Never hardcode `AP_SSID` /
`AP_PASS` back into `main.cpp`, and never commit `secrets.h`.

### Web asset pipeline

`compress_web_files.py` is wired as a PlatformIO `pre:` script (`extra_scripts` in
`platformio.ini`), so it runs on every `pio run` and gzips the files in `data/www/` to `.gz`;
`serveStatic` prefers the `.gz` variant. **Edit the source files in `data/www/`, never the
`.gz`** — the `.gz` are build output and are gitignored. When adding a web file, add its name
to `FILES_TO_COMPRESS` in `compress_web_files.py`. The script calls `main()`
**unconditionally**: PlatformIO runs it under SCons where `__name__ == "SCons.Script"`, so an
`if __name__ == "__main__"` guard would silently never run and ship zero `.gz` files. The
script also deletes orphan `.gz` whose source is gone.

## Architecture

Core split (`xTaskCreatePinnedToCore` in `src/main.cpp`):

- **Core 0** — `SensorTask` (poll SCD30 + apply calibration), `LogTask` (daily CSV writes +
  retention + RAM ring), `OLEDTask` (display).
- **Core 1** — `WebTask` — the single application task that drives all WebSocket broadcasts
  (`time`/`sensor`/`status`) and `cleanupClients()`.

**WebSocket ownership:** HTTP/WS *callbacks* run on **AsyncTCP's own task**
(`CONFIG_ASYNC_TCP_RUNNING_CORE`), not on `WebTask`. `WebTask` exists so exactly one
application task ever touches the client list. `src/main.cpp` owns setup (watchdog, NVS
restore, WiFi AP, queues/mutexes, task launch); its `loop()` does **only** NVS persistence and
heap reporting — **no WebSocket calls**. The broadcasts live in `endpoints.cpp`'s
`webTaskFunction`. The AP defaults to SSID `AQ-Meter-Pro` at `http://192.168.4.1`.

Data flow: `SensorTask` → `sensorQueue` → `LogTask` (writes `/logs/YYYY-MM-DD.csv`, header
`iso,epoch,co2,temp,rh`) and → `displayQueue` → `OLEDTask`. Latest reading is shared via
`latestReading` guarded by `readingMutex`; the sensor task bumps `readingSeq` so `webTaskFunction`
knows when to broadcast. `src/shared.h` is the contract between tasks (SensorReading, pins,
queue / mutex externs, shared + calibration flags, ring API).

**SD access:** the card is mounted **once** in `initSDCard()` and kept mounted. Every access is
serialized by `fileSysMutex`. A download increments a **stream counter** (`sdStreamsInc/Dec`,
spinlock-guarded) while holding `fileSysMutex`; `flushLogBuffer()` takes the mutex *first* and
only then checks the counter, so writes defer while a file streams with no check-then-act race.
The counter is decremented exactly once per stream via a `shared_ptr` guard. Endpoints never
call `SD.begin()`/`SD.end()`.

### Time & logging model (NVS-persisted, no RTC)

The wall clock is **persisted to NVS** and restored on boot, so the device logs standalone
across reboots. Logging is gated only while the clock has **never** been set (fresh device) —
the sensor task only enqueues to `sensorQueue` when `timeIsSet`, so every logged row has a real
epoch. Live readings / OLED / WebSocket are never gated. `POST /api/timeset` sets the clock and
flags `clockDirty`; `loop()` performs all NVS writes (clock + interval + calibration) to keep
NVS single-threaded. Without an RTC the clock can't measure power-off gaps (see README).

**Storage & retention:** daily files under `/logs/` (UTC roll). Oldest day-files are pruned
once `LOG_MAX_DAYS` (365) or `LOG_MAX_BYTES` (512 MB) is exceeded — at boot and on day-roll,
never per write. `LogTask` also owns an in-RAM ring (`LOG_RING_SIZE` recent samples) that
serves short chart ranges without touching the card.

### Calibration (SCD30 owned by the sensor task)

`GET|POST /api/calibration` sets **pending flags** (`calAscDirty`, `calFrcPending`,
`calTempOffsetDirty` in `shared.h`); `sensorTaskFunction` applies them via Wire1 on its next
loop and reports the result. Web handlers must **never** touch the SCD30 directly — that would
race Wire1. `ascEnabled` defaults **OFF** and persists in NVS.

### Source files

`src/tasks/{sensor,logging,oled}.{cpp,h}` and `src/web/endpoints.{cpp,h}`. Each `.cpp` has a
matching header, and the headers declare exactly what is built.

## HTTP / WebSocket API (`src/web/endpoints.cpp`)

Registered in `setupWebEndpoints` (which also installs the WS event handler): `POST /api/timeset`,
`GET /api/status` (includes `dropped`), `GET|POST /api/settings` (interval **10–1800 s**),
`GET|POST /api/calibration` (`asc`, `frc` 400–2000, `tempOffset`), `GET /api/measurements`
(`limit<=1` → latest reading; otherwise a decimated range served from the RAM ring when it fully
covers the request, else a `/logs` day-file scan), and `/api/fs/{info,list,preview,download,delete}`.
WebSocket at `/ws` emits `time` / `sensor` / `status` JSON frames consumed by `data/www/common.js`.

**Unit mismatch to watch:** the WS `sensor` frame sends `temp`/`rh` as real floats, while
`/api/measurements` sends `temperature`/`humidity` as 0.1-unit integers. Different field names
*and* different scales.

## Web UI (`data/www/`)

Offline-first, no CDN. Pages: `index.html` (dashboard — live readings, mini-chart, **Settings**
+ **Calibration** cards), `chart.html` (history), `files.html` (SD file tools with directory
browsing: list/preview/download/delete — no upload), `clock.html` (manual re-sync, linked from
every nav). Shared scripts: `common.js` (WebSocket, clock, auto time-sync, status),
`chart-lite.js` (tiny canvas charts). Per-page: `dashboard.js`, `charts.js`, `files.js`. Shared
`style.css` (only classes the pages/JS actually use). `charts.js` keeps raw API rows and
re-maps them through the selected metric in `redraw()`, so switching metric is instant.

`chart-lite.js` sizes its backing store from the canvas's CSS box, so chart containers must
give the canvas an explicit `width`/`height` in CSS — otherwise it falls back to the 300x150
intrinsic default and misbehaves on HiDPI screens.

## Screenshots (`docs/images/`)

The README screenshots are rendered from the real `data/www` assets against a mock backend
with **simulated** values, not from a live device. If the UI changes materially, they need
re-rendering, and their captions must keep saying the data is simulated.

## Hardware pins (`src/shared.h`)

SCD30 on I2C bus 1 (Wire1, SDA 16 / SCL 17, **100 kHz max**), OLED on I2C bus 0 (Wire0, SDA 21 /
SCL 22, 0x3C), SD card CS on GPIO 5 (SPI). GPIO 16/17 are free on WROOM-32 (this board) but are
PSRAM on WROVER; GPIO 5 is a strapping pin.

## Constraints

- Keep per-op SD work short (watchdog is 30 s; all tasks subscribe and reset it).
- Build flags in `platformio.ini` disable ArduinoJson doubles/64-bit/unicode and the SSD1306
  splash to save flash; don't reintroduce those casually.
