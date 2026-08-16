/**
 * OLED Task - Core 0
 *
 * Redraws the SSD1306 (I2C bus 0 / Wire0 @ 0x3C) whenever a new reading arrives.
 * The on-screen clock uses a fixed local-time offset (no RTC / no DST); it shows
 * "--:--" until the wall clock is known.
 */

#include "oled.h"
#include "../shared.h"
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <esp_task_wdt.h>
#include <time.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

// Local-time offset for the on-screen clock, in seconds (e.g. 2h = CEST).
#define TZ_OFFSET_SECONDS (2 * 3600)

static TwoWire oledWire = TwoWire(0);
static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &oledWire, OLED_RESET);
static bool oledInitialized = false;

bool initOLED() {
    oledWire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) return false;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("AQ-Meter Pro");
    display.println("Starting...");
    display.display();

    oledInitialized = true;
    Serial.println("OLED: initialized");
    return true;
}

static void drawReading(const SensorReading& r) {
    display.clearDisplay();

    display.setTextSize(2);
    display.setCursor(0, 0);
    display.printf("CO2:%u", r.co2);

    display.setTextSize(1);
    display.setCursor(0, 20);
    display.printf("Temp: %.1f C", r.temperature / 10.0f);
    display.setCursor(0, 30);
    display.printf("RH:   %.1f %%", r.humidity / 10.0f);

    display.setCursor(0, 45);
    if (r.co2 < 800)       display.print("Air: Good");
    else if (r.co2 < 1200) display.print("Air: OK");
    else                   display.print("Air: Poor");

    // Clock (bottom-right); "--:--" until the time is known.
    display.setCursor(78, 55);
    if (r.timestamp == 0) {
        display.print("--:--");
    } else {
        time_t local = (time_t)r.timestamp + TZ_OFFSET_SECONDS;
        struct tm tmv;
        gmtime_r(&local, &tmv);
        display.printf("%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }

    display.display();
}

void oledTaskFunction(void* parameter) {
    (void)parameter;
    esp_task_wdt_add(NULL);
    Serial.printf("[Core %d] OLED task started\n", xPortGetCoreID());

    SensorReading reading;
    while (true) {
        esp_task_wdt_reset();
        if (xQueueReceive(displayQueue, &reading, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (oledInitialized) drawReading(reading);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
