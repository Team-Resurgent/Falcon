# Compile the descriptors/streaming for the selected mode (menuconfig -> Falcon).
set(falcon_srcs "falcon_main.c")

if(CONFIG_FALCON_MODE_UVC)
    list(APPEND falcon_srcs "uvc_descriptors.c" "uvc_device.c")
else()
    list(APPEND falcon_srcs
        "usb_descriptors.c"
        "ov519_control.c"
        "ov519_stream.c"
        "falcon_class.c")
endif()

idf_component_register(
    SRCS ${falcon_srcs}
    INCLUDE_DIRS "."
)
