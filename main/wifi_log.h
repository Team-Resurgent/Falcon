// Falcon — WiFi UDP debug logging. Connects STA and mirrors every ESP_LOG line
// as a UDP broadcast (port 5555), plus a 1 Hz heartbeat of the stream counters.
// Lets a PC on the same LAN watch the camera's behavior while it's plugged into
// a far-away Xbox (no serial cable to that PC needed).
#pragma once
void wifi_log_start(void);
