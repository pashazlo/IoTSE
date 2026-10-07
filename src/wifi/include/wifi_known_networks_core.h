#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_security_types.h"

#define WIFI_KNOWN_NETWORK_MAX 4U
#define WIFI_KNOWN_SSID_MAX 32U
#define WIFI_KNOWN_PASSWORD_MAX 64U
#define WIFI_KNOWN_DB_MAGIC 0x574B4E44UL
#define WIFI_KNOWN_DB_VERSION 3U

typedef struct {
    bool occupied;
    bool auto_connect;
    uint8_t security;
    uint8_t last_channel;
    bool has_bssid_hint;
    uint8_t bssid_hint[6];
    uint32_t success_sequence;
    char ssid[WIFI_KNOWN_SSID_MAX + 1U];
    char password[WIFI_KNOWN_PASSWORD_MAX + 1U];
} wifi_known_network_t;

typedef struct {
    uint32_t next_success_sequence;
    wifi_known_network_t entries[WIFI_KNOWN_NETWORK_MAX];
} wifi_known_network_db_t;

typedef struct {
    const char *ssid;
    const uint8_t *bssid;
    int8_t rssi;
    uint8_t channel;
    uint8_t security;
} wifi_known_visible_ap_t;

typedef enum {
    WIFI_KNOWN_CODEC_OK = 0,
    WIFI_KNOWN_CODEC_MISSING,
    WIFI_KNOWN_CODEC_CORRUPT,
    WIFI_KNOWN_CODEC_NO_SPACE,
} wifi_known_codec_status_t;

#define WIFI_KNOWN_DB_ENCODED_MAX 512U

void wifi_known_db_init(wifi_known_network_db_t *db);
bool wifi_known_db_valid(const wifi_known_network_db_t *db);
size_t wifi_known_db_count(const wifi_known_network_db_t *db);
size_t wifi_known_db_auto_count(const wifi_known_network_db_t *db);
int wifi_known_db_find(const wifi_known_network_db_t *db, const char *ssid);
bool wifi_known_db_upsert_success(wifi_known_network_db_t *db,
                                  const char *ssid, const char *password,
                                  uint8_t security, uint8_t channel,
                                  const uint8_t bssid_hint[6],
                                  bool preserve_auto);
bool wifi_known_db_set_auto(wifi_known_network_db_t *db,
                            const char *ssid, bool enabled);
bool wifi_known_db_forget(wifi_known_network_db_t *db, const char *ssid);
bool wifi_known_security_compatible(wifi_security_t saved,
                                    wifi_security_t visible);
int wifi_known_db_select_visible(const wifi_known_network_db_t *db,
                                 const wifi_known_visible_ap_t *visible,
                                 size_t visible_count, size_t *visible_index);
size_t wifi_known_db_encode(const wifi_known_network_db_t *db,
                            uint8_t *out, size_t capacity);
wifi_known_codec_status_t wifi_known_db_decode(const uint8_t *data, size_t size,
                                                wifi_known_network_db_t *db);
