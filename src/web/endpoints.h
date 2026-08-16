#pragma once

#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include "../shared.h"

// Register all HTTP routes and the WebSocket handler on the given server.
void setupWebEndpoints(AsyncWebServer& server, AsyncWebSocket& webSocket);

// Web maintenance task (Core 1): WebSocket client cleanup + watchdog feed.
void webTaskFunction(void* parameter);
