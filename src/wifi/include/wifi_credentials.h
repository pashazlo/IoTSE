#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "wifi.h"
#include "wifi_known_networks_core.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {char ssid[WIFI_SSID_MAX_LEN+1];char password[WIFI_PASSWORD_MAX_LEN+1];bool auto_connect;} wifi_saved_credentials_t;

/* Worker-owned application database. ESP-IDF Wi-Fi storage remains RAM-only. */
esp_err_t wifi_credentials_load_database(wifi_known_network_db_t *db);
esp_err_t wifi_credentials_save_database(const wifi_known_network_db_t *db);
esp_err_t wifi_credentials_upsert_success(wifi_known_network_db_t *db,
    const char *ssid,const char *password,wifi_security_t security,
    uint8_t channel,const uint8_t bssid_hint[6]);
esp_err_t wifi_credentials_set_network_auto(wifi_known_network_db_t *db,
    const char *ssid,bool enabled);
esp_err_t wifi_credentials_forget_network(wifi_known_network_db_t *db,
    const char *ssid);

/* Compatibility helpers for older callers; operate on the most recent entry. */
esp_err_t wifi_credentials_load(wifi_saved_credentials_t *out_credentials);
esp_err_t wifi_credentials_save(const char *ssid,const char *password,bool auto_connect);
esp_err_t wifi_credentials_set_auto_connect(bool enabled);
esp_err_t wifi_credentials_forget(void);
#ifdef __cplusplus
}
#endif