set(SRCS
    "main.c"

    "storage/storage.c"
    "storage/storage_sd.c"
    "storage/storage_manager.c"
    "storage/file_service.c"

    "fm/fm.c"
    "fm/fm_text_edit.c"
    "fm/fm_text_view.c"
    "fm/fm_worker.c"
    "fm/ui_file_editor.c"

    "ui_engine/ui.c"
    "ui_engine/ui_controller.c"
    "ui_engine/ui_cursor.c"
    "ui_engine/ui_event.c"
    "ui_engine/ui_focus.c"
    "ui_engine/ui_keyboard.c"
    "ui_engine/ui_menu.c"
    "ui_engine/ui_render.c"
    "ui_engine/ui_screen.c"
    "ui_engine/ui_wifi_connect.c"
    "ui_engine/ui_wifi_monitor.c"
    "ui_engine/ui_wifi_map.c"
    "ui_engine/ui_wifi_export.c"
    "ui_engine/widgets/ui_battery.c"
    "ui_engine/widgets/ui_clock.c"
    "ui_engine/widgets/ui_logo.c"
    "ui_engine/widgets/ui_popup.c"
    "ui_engine/widgets/ui_wifi.c"

    "wifi/wifi.c"
    "wifi/wifi_analyzer.c"
    "wifi/wifi_ap_tracker.c"
    "wifi/wifi_ap_tracker_core.c"
    "wifi/wifi_banner.c"
    "wifi/wifi_channel_hopper.c"
    "wifi/wifi_capture_store.c"
    "wifi/wifi_capture_export.c"
    "wifi/wifi_credentials.c"
    "wifi/wifi_known_networks_core.c"
    "wifi/wifi_connection_manager_core.c"
    "wifi/wifi_eapol_parser.c"
    "wifi/wifi_eapol_tracker.c"
    "wifi/wifi_eapol_tracker_core.c"
    "wifi/wifi_frame_parser.c"
    "wifi/wifi_ie_iterator.c"
    "wifi/wifi_mgmt_parser.c"
    "wifi/wifi_rsn_parser.c"
    "wifi/wifi_scan_lifecycle.c"
    "wifi/wifi_sta_tracker.c"
    "wifi/wifi_sta_tracker_core.c"
    "wifi/wifi_host_classifier.c"
    "wifi/wifi_host_discovery.c"
    "wifi/wifi_ieee80211_codec.c"
    "wifi/wifi_map_store.c"
    "wifi/wifi_pcap_codec.c"
    "wifi/wifi_promiscuous.c"
    "wifi/wifi_port_scan.c"
    "wifi/wifi_worker.c"

    "subghz/subghz_rs_math.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "."
        "assets"
        "ui_engine"
        "ui_engine/include"
        "ui_engine/widgets"
        "ui_engine/widgets/include"
        "storage"
        "storage/include"
        "fm"
        "fm/include"
        "wifi"
        "wifi/include"
        "subghz"
        "subghz/include"
    REQUIRES
        buttons
        display
        gfx_canvas
        spi_bus
        driver
        freertos
        nvs_flash
        fatfs
        wear_levelling
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        esp_driver_sdmmc
        esp_driver_sdspi
        sdmmc
        lwip
)

# Keep printf-family diagnostics strict for IoTSE-owned production sources.
# In particular this catches malformed PRI macros before firmware reaches HW.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include${CMAKE_CURRENT_SOURCE_DIR}/iotse_format_check.h
    -Wformat=2
    -Werror=format
    -Werror=format-security
)

add_custom_target(iotse_format_audit ALL
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_format_strings.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Auditing IoTSE printf/PRI format strings"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} iotse_format_audit)
