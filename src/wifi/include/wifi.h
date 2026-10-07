#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi_security_types.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_MAX_APS 64
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64

typedef enum {
    WIFI_RSSI_GOOD = 0,
    WIFI_RSSI_FAIR,
    WIFI_RSSI_WEAK,
} wifi_rssi_quality_t;

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t primary_channel;
    int8_t secondary_channel;
    wifi_security_t security;
} wifi_ap_info_t;

typedef struct {
    uint16_t count;
    uint16_t total_found;
    bool truncated;
    wifi_ap_info_t records[WIFI_SCAN_MAX_APS];
} wifi_scan_data_t;

typedef struct {
    uint8_t channel;  /* 0 scans all channels allowed by the country config. */
    bool show_hidden;
    bool passive;
    uint16_t dwell_ms; /* 0 uses the ESP-IDF default; maximum is 1500 ms. */
} wifi_scan_options_t;

typedef struct {
    bool connected;
    char ssid[WIFI_SSID_MAX_LEN + 1];
    uint8_t bssid[6];
    uint8_t sta_mac[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    int8_t rssi;
} wifi_connection_info_t;

typedef void (*wifi_scan_done_callback_t)(uint32_t scan_id, bool success,
                                          void *context);
typedef enum {
    WIFI_RADIO_CONN_ASSOCIATED = 0,
    WIFI_RADIO_CONN_GOT_IP,
    WIFI_RADIO_CONN_DISCONNECTED,
    WIFI_RADIO_CONN_LOST_IP,
} wifi_radio_connection_event_type_t;
typedef struct {
    wifi_radio_connection_event_type_t type;
    uint32_t attempt_id;
    uint32_t elapsed_ms;
    uint16_t disconnect_reason;
    uint8_t bssid[6];
    uint8_t channel;
    wifi_auth_mode_t authmode;
    int8_t rssi;
    bool authmode_valid;
    bool rssi_valid;
} wifi_radio_connection_event_t;
typedef void (*wifi_connect_event_callback_t)(
    const wifi_radio_connection_event_t *event, void *context);

/* Radio backend. These functions must be called only by WiFi Worker. */
esp_err_t wifi_radio_start(wifi_scan_done_callback_t callback, void *context);
esp_err_t wifi_radio_stop(void);
esp_err_t wifi_radio_scan_start(const wifi_scan_options_t *options,
                                uint32_t scan_id);
esp_err_t wifi_radio_scan_cancel(void);
esp_err_t wifi_radio_scan_collect(wifi_scan_data_t *out_data);
void wifi_radio_set_connect_callback(
    wifi_connect_event_callback_t callback,
    void *context
);
esp_err_t wifi_radio_connect(
    const char *ssid,
    const char *password,
    const uint8_t bssid_lock[6],
    uint32_t attempt_id
);
esp_err_t wifi_radio_disconnect(void);
esp_err_t wifi_radio_get_connection_info(wifi_connection_info_t *out_info);
esp_err_t wifi_radio_set_mode_promiscuous(
    uint8_t channel,
    wifi_promiscuous_cb_t callback
);
/* Keep callback-owned data alive until the worker reports DISABLED. */
esp_err_t wifi_radio_disable_promiscuous(void);
bool wifi_radio_is_promiscuous_enabled(void);
bool wifi_radio_promiscuous_needs_cleanup(void);
/* Worker only; current configured country range, restricted to S3 2.4 GHz. */
esp_err_t wifi_radio_get_channel_range(uint8_t *first, uint8_t *last);
esp_err_t wifi_radio_promiscuous_set_channel(uint8_t channel);

wifi_rssi_quality_t wifi_rssi_quality(int8_t rssi);
const char *wifi_security_name(wifi_security_t security);
const char *wifi_disconnect_reason_name(uint16_t reason);

#ifdef __cplusplus
}
#endif
