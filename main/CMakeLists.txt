# --- Vendored dwc2 EP0-SETUP-DMA crash fix (idempotent, auto-applied) ---------
# esp_tinyusb is a managed component (re-downloaded pristine on each fetch), so the
# ESP32-S3 EP0-SETUP DMA crash fix in dcd_dwc2.c must be re-applied here. Safe to
# run every configure -- it no-ops if already patched. See patches/README.md.
idf_build_get_property(_falcon_python PYTHON)
execute_process(
    COMMAND "${_falcon_python}" "${CMAKE_CURRENT_SOURCE_DIR}/../patches/apply_tinyusb_patch.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/.."
    RESULT_VARIABLE _falcon_patch_rc
    OUTPUT_VARIABLE _falcon_patch_out
    ERROR_VARIABLE  _falcon_patch_out)
message(STATUS "Falcon dwc2 patch: ${_falcon_patch_out}")
if(NOT _falcon_patch_rc EQUAL 0)
    message(WARNING "Falcon: dwc2 EP0 patch NOT applied (DMA mode will crash on the Xbox). See patches/README.md")
endif()

# Compile the descriptors/streaming for the selected mode (menuconfig -> Falcon).
set(falcon_srcs "falcon_main.c")
set(falcon_priv_requires "")

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
        "falcon_class.c")
    # Optional WiFi UDP debug logging (Kconfig: FALCON_WIFI_LOG, default off).
    # Only compiled when enabled, so the project builds with no wifi_creds.h present.
    if(CONFIG_FALCON_WIFI_LOG)
        list(APPEND falcon_srcs "wifi_log.c")
        list(APPEND falcon_priv_requires esp_wifi nvs_flash esp_netif esp_event esp_hw_support)
    endif()
    # Byte-faithful EP0 size 8 for the OV519 camera (esp_tinyusb defaults to 64).
    set(falcon_tusb_defs CFG_TUD_ENDPOINT0_SIZE=8)
endif()

idf_component_register(
    SRCS ${falcon_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${falcon_priv_requires}
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
