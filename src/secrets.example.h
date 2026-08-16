/**
 * secrets.example.h — template for the device's WiFi access-point credentials.
 *
 * Setup:
 *   1. Copy this file to `src/secrets.h`.
 *   2. Change AP_PASS to something of your own.
 *
 * `src/secrets.h` is gitignored, so your credentials stay out of version
 * control. This template is committed so a fresh clone still builds.
 *
 * The device hosts its own access point rather than joining an existing
 * network — there is no internet route and no station credentials involved.
 * WPA2 still requires a passphrase of at least 8 characters.
 */
#pragma once

#define AP_SSID "AQ-Meter-Pro"
#define AP_PASS "ChangeMe1234"
