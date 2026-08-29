# Compile the descriptors/streaming for the selected mode (menuconfig -> Falcon).
set(falcon_srcs "falcon_main.c")

if(CONFIG_FALCON_MODE_UVC)
    list(APPEND falcon_srcs "uvc_descriptors.c" "uvc_device.c")
    # esp_tinyusb does not expose the TinyUSB video class; enable it (and match our
    # EP0 size) on the TinyUSB library target so usbd.c registers the video driver
    # and video_device.c actually compiles. Values must match main/tusb_config.h.
    set(falcon_tusb_defs
        CFG_TUD_ENDPOINT0_SIZE=64
        CFG_TUD_VIDEO=1
        CFG_TUD_VIDEO_STREAMING=1
        CFG_TUD_VIDEO_STREAMING_BULK=0
        CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=512)
else()
    list(APPEND falcon_srcs
        "usb_descriptors.c"
        "ov519_control.c"
        "ov519_stream.c"
        "falcon_class.c"
        "wifi_log.c")
    # Byte-faithful EP0 size 8 for the OV519 camera (esp_tinyusb defaults to 64).
    set(falcon_tusb_defs CFG_TUD_ENDPOINT0_SIZE=8)
endif()

idf_component_register(
    SRCS ${falcon_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_hw_support
)

# Inject the settings the TinyUSB library must share with our sources directly onto
# its target (esp_tinyusb passes its own tusb_config.h to the library privately).
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(falcon_tusb_name tinyusb)          # local component
else()
    set(falcon_tusb_name espressif__tinyusb)  # managed component
endif()
idf_component_get_property(falcon_tusb_lib ${falcon_tusb_name} COMPONENT_LIB)
target_compile_definitions(${falcon_tusb_lib} PRIVATE ${falcon_tusb_defs})
