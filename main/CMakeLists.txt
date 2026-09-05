file(GLOB_RECURSE MY_SRCS
    "apps/*.c"
    "apps/*.cc"
    "apps/*.cpp"
    "assets/*.c"
    "assets/*.cc"
    "assets/*.cpp"
    "hal/*.c"
    "hal/*.cc"
    "hal/*.cpp"
    "services/*.c"
    "services/*.cc"
    "services/*.cpp"
)

set(MY_INCS
    "."
)

idf_component_register(
    SRCS 
        "main.cpp"
        ${MY_SRCS}
    INCLUDE_DIRS 
        ${MY_INCS}
    REQUIRES
        bt
        smooth_ui_toolkit
        mooncake
        mooncake_log
        lvgl
        M5GFX
        M5IOE1
        M5PM1
        BMI270_BMM150_Sensor
        ArduinoJson
        78__esp-wifi-connect
        espressif__esp-dsp
        espressif__esp_codec_dev
        espressif__i2c_bus
        espressif__esp_tinyusb
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        fatfs
        nvs_flash
        wear_levelling
    EMBED_FILES
        "assets/sfx/boot_sfx.bin"
    EMBED_TXTFILES
        "hal/utils/config_ap/assets/badge_config_ap.html"
)

if(CONFIG_CODEX_MICRO_USB_HID)
    # TinyUSB uses the HID endpoint scratch buffer for control SET_REPORT too.
    # The largest feature report is 256 bytes plus its Report ID. Only interrupt
    # IN is advertised; Output and Feature reports use control SET_REPORT.
    # An interrupt OUT endpoint would incorrectly wait for this full buffer.
    idf_component_get_property(CODEX_TINYUSB_LIB espressif__tinyusb COMPONENT_LIB)
    target_compile_definitions(${CODEX_TINYUSB_LIB} PUBLIC CFG_TUD_HID_EP_BUFSIZE=257)
endif()
