// Falcon — shared constants + entry point for the UVC dev mode.
#pragma once

#define FALCON_FRAME_W     320
#define FALCON_FRAME_H     240
#define FALCON_FRAME_RATE  15

// Spawn the UVC frame-feeder task (called from app_main in UVC mode).
void falcon_uvc_start(void);
