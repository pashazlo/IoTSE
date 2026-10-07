#include "wifi.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

static const char *TAG = "WIFI";

static bool s_netif_initialized;
static bool s_event_loop_owned;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_promiscuous_enabled;
static bool s_promiscuous_cleanup_pending;
static uint8_t s_promiscuous_previous_channel;
static wifi_second_chan_t s_promiscuous_previous_second_channel;
static esp_netif_t *s_station_netif;
static esp_event_handler_instance_t s_scan_handler;
static esp_event_handler_instance_t s_connected_handler;
static esp_event_handler_instance_t s_disconnect_handler;
static esp_event_handler_instance_t s_got_ip_handler;
static esp_event_handler_instance_t s_lost_ip_handler;
static wifi_scan_done_callback_t s_scan_done_callback;
static void *s_scan_done_context;
static wifi_connect_event_callback_t s_connect_done_callback;
static uint32_t s_scan_id;
static uint32_t s_connect_attempt_id;
static uint32_t s_associated_attempt_id;
static int64_t s_connect_started_us;
static void *s_connect_done_context;

/* Native records are kept out of the worker stack. Only the worker calls us. */
static wifi_ap_record_t s_native_records[WIFI_SCAN_MAX_APS];

static wifi_security_t map_security(wifi_auth_mode_t auth_mode)
{
    switch (auth_mode) {
        case WIFI_AUTH_OPEN: return WIFI_SECURITY_OPEN;
        case WIFI_AUTH_WEP: return WIFI_SECURITY_WEP;
        case WIFI_AUTH_WPA_PSK: return WIFI_SECURITY_WPA_PSK;
        case WIFI_AUTH_WPA2_PSK: return WIFI_SECURITY_WPA2_PSK;
        case WIFI_AUTH_WPA_WPA2_PSK: return WIFI_SECURITY_WPA_WPA2_PSK;
        case WIFI_AUTH_ENTERPRISE:
        case WIFI_AUTH_WPA3_ENT_192:
        case WIFI_AUTH_WPA3_ENTERPRISE:
        case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
        case WIFI_AUTH_WPA_ENTERPRISE:
            return WIFI_SECURITY_ENTERPRISE;
        case WIFI_AUTH_WPA3_PSK: return WIFI_SECURITY_WPA3_PSK;
        case WIFI_AUTH_WPA2_WPA3_PSK: return WIFI_SECURITY_WPA2_WPA3_PSK;
        case WIFI_AUTH_WAPI_PSK: return WIFI_SECURITY_WAPI_PSK;
        case WIFI_AUTH_OWE: return WIFI_SECURITY_OWE;
        case WIFI_AUTH_DPP: return WIFI_SECURITY_DPP;
        default: return WIFI_SECURITY_UNKNOWN;
    }
}

static const char *mode_name(wifi_mode_t mode)
{
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "APSTA";
        default: return "UNKNOWN";
    }
}

static const char *bandwidth_name(wifi_bandwidth_t bandwidth)
{
    switch (bandwidth) {
        case WIFI_BW20: return "HT20";
        case WIFI_BW40: return "HT40";
        default: return "UNKNOWN";
    }
}

static const char *power_save_name(wifi_ps_type_t power_save)
{
    switch (power_save) {
        case WIFI_PS_NONE: return "NONE";
        case WIFI_PS_MIN_MODEM: return "MIN_MODEM";
        case WIFI_PS_MAX_MODEM: return "MAX_MODEM";
        default: return "UNKNOWN";
    }
}

static void log_runtime_radio_config(void)
{
    esp_err_t err;
    wifi_mode_t mode;
    wifi_country_t country;
    uint8_t protocols;
    wifi_bandwidth_t bandwidth;
    wifi_ps_type_t power_save;
    int8_t tx_limit_qdbm;

    err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK) ESP_LOGI(TAG, "RADIO mode=%s(%u)", mode_name(mode),
                                (unsigned)mode);
    else ESP_LOGW(TAG, "RADIO mode read failed: %s", esp_err_to_name(err));

    err = esp_wifi_get_country(&country);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "RADIO country=%.2s schan=%u nchan=%u policy=%u",
                 country.cc, (unsigned)country.schan, (unsigned)country.nchan,
                 (unsigned)country.policy);
    else ESP_LOGW(TAG, "RADIO country read failed: %s", esp_err_to_name(err));

    err = esp_wifi_get_protocol(WIFI_IF_STA, &protocols);
    if (err == ESP_OK) ESP_LOGI(TAG, "RADIO protocol_mask=0x%02x",
                                (unsigned)protocols);
    else ESP_LOGW(TAG, "RADIO protocol read failed: %s", esp_err_to_name(err));

    err = esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth);
    if (err == ESP_OK) ESP_LOGI(TAG, "RADIO bandwidth=%s(%u)",
                                bandwidth_name(bandwidth),
                                (unsigned)bandwidth);
    else ESP_LOGW(TAG, "RADIO bandwidth read failed: %s", esp_err_to_name(err));

    err = esp_wifi_get_ps(&power_save);
    if (err == ESP_OK) ESP_LOGI(TAG, "RADIO power_save=%s(%u)",
                                power_save_name(power_save),
                                (unsigned)power_save);
    else ESP_LOGW(TAG, "RADIO power-save read failed: %s", esp_err_to_name(err));

    err = esp_wifi_get_max_tx_power(&tx_limit_qdbm);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "RADIO configured_tx_limit=%d qdBm (1 qdBm=0.25 dBm)",
                 (int)tx_limit_qdbm);
    else ESP_LOGW(TAG, "RADIO TX-limit read failed: %s", esp_err_to_name(err));
}

static void connection_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    (void)arg;
    if (s_connect_done_callback == NULL) return;
    wifi_radio_connection_event_t out = {0};
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = event_data;
        s_associated_attempt_id = s_connect_attempt_id;
        out.type = WIFI_RADIO_CONN_ASSOCIATED;
        out.attempt_id = s_associated_attempt_id;
        if (event != NULL) {
            memcpy(out.bssid, event->bssid, 6);
            out.channel = event->channel;
            out.authmode = event->authmode;
            out.authmode_valid = true;
        }
        out.elapsed_ms = s_connect_started_us > 0
            ? (uint32_t)((esp_timer_get_time() - s_connect_started_us) / 1000)
            : 0U;
        ESP_LOGI(TAG, "STA associated attempt=%" PRIu32
                 " elapsed=%" PRIu32 "ms actual_bssid=" MACSTR
                 " channel=%u auth=%u",
                 out.attempt_id, out.elapsed_ms, MAC2STR(out.bssid),
                 (unsigned)out.channel, (unsigned)out.authmode);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        out.type = WIFI_RADIO_CONN_GOT_IP;
        /* GOT_IP is valid only after a STA_CONNECTED association boundary. */
        out.attempt_id = s_associated_attempt_id;
        const ip_event_got_ip_t *event = event_data;
        if (event != NULL) ESP_LOGI(TAG, "STA got IPv4 attempt=%" PRIu32
                                    " ip=" IPSTR, out.attempt_id,
                                    IP2STR(&event->ip_info.ip));
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        out.type = WIFI_RADIO_CONN_DISCONNECTED;
        out.attempt_id = s_associated_attempt_id != 0U
            ? s_associated_attempt_id : s_connect_attempt_id;
        if (event != NULL) {
            out.disconnect_reason = event->reason;
            memcpy(out.bssid, event->bssid, 6);
            out.rssi = event->rssi;
            out.rssi_valid = true;
        }
        out.elapsed_ms = s_connect_started_us > 0
            ? (uint32_t)((esp_timer_get_time() - s_connect_started_us) / 1000)
            : 0U;
        s_associated_attempt_id = 0U;
        ESP_LOGW(TAG, "STA disconnected attempt=%" PRIu32
                 " elapsed=%" PRIu32 "ms actual_bssid=" MACSTR
                 " rssi=%d reason=%u(%s)",
                 out.attempt_id, out.elapsed_ms, MAC2STR(out.bssid),
                 (int)out.rssi, (unsigned)out.disconnect_reason,
                 wifi_disconnect_reason_name(out.disconnect_reason));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        out.type = WIFI_RADIO_CONN_LOST_IP;
        out.attempt_id = s_associated_attempt_id;
        ESP_LOGW(TAG, "STA lost IP attempt=%" PRIu32, out.attempt_id);
    } else return;
    s_connect_done_callback(&out, s_connect_done_context);
}
static void scan_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;
    (void)event_base;

    if (event_id != WIFI_EVENT_SCAN_DONE || s_scan_done_callback == NULL) {
        return;
    }

    const wifi_event_sta_scan_done_t *done = event_data;
    bool success = done != NULL && done->status == 0;
    ESP_LOGI(TAG, "SCAN_DONE logical=%" PRIu32 " native=%u status=%" PRIu32,
             s_scan_id, done != NULL ? (unsigned)done->scan_id : 0U,
             done != NULL ? done->status : UINT32_MAX);
    s_scan_done_callback(s_scan_id, success, s_scan_done_context);
}

esp_err_t wifi_radio_start(wifi_scan_done_callback_t callback, void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_started) {
        s_scan_done_callback = callback;
        s_scan_done_context = context;
        return ESP_OK;
    }

    esp_err_t err;
    if (!s_netif_initialized) {
        err = esp_netif_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            return err;
        }
        /* esp_netif_deinit() is not supported by this ESP-IDF version. */
        s_netif_initialized = true;
    }

    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        s_event_loop_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_station_netif = esp_netif_create_default_wifi_sta();
    if (s_station_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        goto fail;
    }
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        WIFI_EVENT_SCAN_DONE,
        scan_event_handler,
        NULL,
        &s_scan_handler
    );
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
        connection_event_handler, NULL, &s_connected_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
        connection_event_handler, NULL, &s_disconnect_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        connection_event_handler, NULL, &s_got_ip_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP,
        connection_event_handler, NULL, &s_lost_ip_handler);
    if (err != ESP_OK) goto fail;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        goto fail;
    }

    s_wifi_started = true;
    s_scan_done_callback = callback;
    s_scan_done_context = context;
    ESP_LOGI(TAG, "Wi-Fi radio started in STA scan mode");
    log_runtime_radio_config();
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi radio start failed: %s", esp_err_to_name(err));
    (void)wifi_radio_stop();
    return err;
}

esp_err_t wifi_radio_stop(void)
{
    /* Do not discard ownership flags/handles on failed teardown. STOP can retry. */
    esp_err_t err;
    if (s_wifi_started && s_promiscuous_cleanup_pending) {
        err = wifi_radio_disable_promiscuous();
        if (err != ESP_OK) return err;
    }
    if (s_wifi_started) {
        err = esp_wifi_stop();
        if (err != ESP_OK) return err;
        s_wifi_started = false;
        s_promiscuous_enabled = false;
    }
    if (s_scan_handler != NULL) {
        err = esp_event_handler_instance_unregister(
            WIFI_EVENT, WIFI_EVENT_SCAN_DONE, s_scan_handler);
        if (err != ESP_OK) return err;
        s_scan_handler = NULL;
    }
    if (s_connected_handler != NULL) {
        err = esp_event_handler_instance_unregister(
            WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, s_connected_handler);
        if (err != ESP_OK) return err;
        s_connected_handler = NULL;
    }
    if (s_disconnect_handler != NULL) {
        err = esp_event_handler_instance_unregister(
            WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_disconnect_handler);
        if (err != ESP_OK) return err;
        s_disconnect_handler = NULL;
    }
    if (s_got_ip_handler != NULL) {
        err = esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, s_got_ip_handler);
        if (err != ESP_OK) return err;
        s_got_ip_handler = NULL;
    }
    if (s_lost_ip_handler != NULL) {
        err = esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_LOST_IP, s_lost_ip_handler);
        if (err != ESP_OK) return err;
        s_lost_ip_handler = NULL;
    }
    s_scan_done_callback = NULL;
    s_scan_done_context = NULL;
    s_connect_done_callback = NULL;
    s_connect_done_context = NULL;
    s_scan_id = s_connect_attempt_id = s_associated_attempt_id = 0U;
    s_connect_started_us = 0;
    if (s_wifi_initialized) {
        err = esp_wifi_deinit();
        if (err != ESP_OK) return err;
        s_wifi_initialized = false;
        s_promiscuous_cleanup_pending = false;
    }
    if (s_station_netif != NULL) {
        esp_netif_destroy_default_wifi(s_station_netif);
        s_station_netif = NULL;
    }
    if (s_event_loop_owned) {
        err = esp_event_loop_delete_default();
        if (err != ESP_OK) return err;
        s_event_loop_owned = false;
    }
    ESP_LOGI(TAG, "Wi-Fi radio stopped");
    return ESP_OK;
}

void wifi_radio_set_connect_callback(
    wifi_connect_event_callback_t callback,
    void *context
)
{
    s_connect_done_callback = callback;
    s_connect_done_context = context;
}

esp_err_t wifi_radio_connect(
    const char *ssid,
    const char *password,
    const uint8_t bssid[6],
    uint32_t attempt_id
)
{
    if (!s_wifi_started || s_promiscuous_cleanup_pending)
        return ESP_ERR_INVALID_STATE;
    if (ssid == NULL || password == NULL || attempt_id == 0U)
        return ESP_ERR_INVALID_ARG;

    size_t ssid_len = strnlen(ssid, WIFI_SSID_MAX_LEN + 1U);
    size_t password_len = strnlen(password, WIFI_PASSWORD_MAX_LEN + 1U);
    if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN ||
        password_len > WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, ssid_len);
    memcpy(config.sta.password, password, password_len);
    if (bssid != NULL) {
        memcpy(config.sta.bssid, bssid, sizeof(config.sta.bssid));
        config.sta.bssid_set = true;
    }
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    s_connect_attempt_id = attempt_id;
    s_associated_attempt_id = 0U;
    s_connect_started_us = esp_timer_get_time();
    if (config.sta.bssid_set) {
        ESP_LOGI(TAG, "DRIVER_CFG attempt=%" PRIu32
                 " bssid_set=1 bssid=" MACSTR
                 " channel=%u scan=%u sort=%u threshold_rssi=%d"
                 " threshold_auth=%u pmf_capable=%u pmf_required=%u",
                 attempt_id, MAC2STR(config.sta.bssid),
                 (unsigned)config.sta.channel,
                 (unsigned)config.sta.scan_method,
                 (unsigned)config.sta.sort_method,
                 (int)config.sta.threshold.rssi,
                 (unsigned)config.sta.threshold.authmode,
                 config.sta.pmf_cfg.capable ? 1U : 0U,
                 config.sta.pmf_cfg.required ? 1U : 0U);
    } else {
        ESP_LOGI(TAG, "DRIVER_CFG attempt=%" PRIu32
                 " bssid_set=0 bssid=unknown"
                 " channel=%u scan=%u sort=%u threshold_rssi=%d"
                 " threshold_auth=%u pmf_capable=%u pmf_required=%u",
                 attempt_id, (unsigned)config.sta.channel,
                 (unsigned)config.sta.scan_method,
                 (unsigned)config.sta.sort_method,
                 (int)config.sta.threshold.rssi,
                 (unsigned)config.sta.threshold.authmode,
                 config.sta.pmf_cfg.capable ? 1U : 0U,
                 config.sta.pmf_cfg.required ? 1U : 0U);
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (bssid != NULL) {
        ESP_LOGI(TAG,
                 "Connecting to SSID='%s', BSSID=" MACSTR ", password_len=%zu",
                 ssid, MAC2STR(bssid), password_len);
    } else {
        ESP_LOGI(TAG, "Connecting to SSID='%s', password_len=%zu",
                 ssid, password_len);
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_radio_disconnect(void)
{
    return s_wifi_started && !s_promiscuous_cleanup_pending
        ? esp_wifi_disconnect()
        : ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_radio_get_connection_info(wifi_connection_info_t *out_info)
{
    if (out_info == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_wifi_started || s_station_netif == NULL)
        return ESP_ERR_INVALID_STATE;

    memset(out_info, 0, sizeof(*out_info));
    wifi_config_t config = {0};
    esp_netif_ip_info_t ip_info = {0};
    int rssi = 0;

    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) return err;
    err = esp_wifi_get_mac(WIFI_IF_STA, out_info->sta_mac);
    if (err != ESP_OK) return err;
    err = esp_netif_get_ip_info(s_station_netif, &ip_info);
    if (err != ESP_OK) return err;
    err = esp_wifi_sta_get_rssi(&rssi);
    if (err != ESP_OK) return err;

    memcpy(out_info->ssid, config.sta.ssid, WIFI_SSID_MAX_LEN);
    out_info->ssid[WIFI_SSID_MAX_LEN] = '\0';
    memcpy(out_info->bssid, config.sta.bssid, sizeof(out_info->bssid));
    out_info->ip = ip_info.ip.addr;
    out_info->netmask = ip_info.netmask.addr;
    out_info->gateway = ip_info.gw.addr;
    out_info->rssi = (int8_t)rssi;
    out_info->connected = true;
    return ESP_OK;
}

esp_err_t wifi_radio_scan_start(const wifi_scan_options_t *options,
                                uint32_t scan_id)
{
    if (!s_wifi_started || s_promiscuous_cleanup_pending) {
        return ESP_ERR_INVALID_STATE;
    }
    if (options == NULL || options->channel > 14 || options->dwell_ms > 1500) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = options->channel,
        .show_hidden = options->show_hidden,
        .scan_type = options->passive
            ? WIFI_SCAN_TYPE_PASSIVE
            : WIFI_SCAN_TYPE_ACTIVE,
    };

    if (options->dwell_ms > 0) {
        if (options->passive) {
            config.scan_time.passive = options->dwell_ms;
        } else {
            config.scan_time.active.max = options->dwell_ms;
        }
    }

    if (scan_id == 0U) return ESP_ERR_INVALID_ARG;
    s_scan_id = scan_id;
    return esp_wifi_scan_start(&config, false);
}

esp_err_t wifi_radio_scan_cancel(void)
{
    return s_wifi_started && !s_promiscuous_cleanup_pending
        ? esp_wifi_scan_stop()
        : ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_radio_set_mode_promiscuous(
    uint8_t channel, wifi_promiscuous_cb_t callback)
{
    if (!s_wifi_started || s_promiscuous_cleanup_pending)
        return ESP_ERR_INVALID_STATE;
    if (channel == 0 || channel > 14 || callback == NULL)
        return ESP_ERR_INVALID_ARG;
    uint8_t first, last;
    esp_err_t err = wifi_radio_get_channel_range(&first, &last);
    if (err != ESP_OK) return err;
    if (channel < first || channel > last) return ESP_ERR_INVALID_ARG;
    err = esp_wifi_get_channel(&s_promiscuous_previous_channel,
                              &s_promiscuous_previous_second_channel);
    if (err != ESP_OK) return err;

    /* Pending remains set until ALL cleanup steps succeed, even on rollback. */
    s_promiscuous_cleanup_pending = true;
    err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) err = esp_wifi_set_promiscuous_rx_cb(callback);
    if (err == ESP_OK) err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        esp_err_t cleanup = wifi_radio_disable_promiscuous();
        if (cleanup != ESP_OK)
            ESP_LOGE(TAG, "Promiscuous rollback failed: %s", esp_err_to_name(cleanup));
        return err; /* Preserve original failure. Worker detects pending cleanup. */
    }
    s_promiscuous_enabled = true;
    wifi_promiscuous_filter_t filter;
    esp_err_t filter_err = esp_wifi_get_promiscuous_filter(&filter);
    if (filter_err == ESP_OK) {
        ESP_LOGI(TAG, "Promiscuous enabled channel=%u filter=0x%" PRIx32,
                 (unsigned)channel, filter.filter_mask);
    } else {
        ESP_LOGW(TAG, "Promiscuous enabled channel=%u filter=unknown (%s)",
                 (unsigned)channel, esp_err_to_name(filter_err));
    }
    return ESP_OK;
}

esp_err_t wifi_radio_disable_promiscuous(void)
{
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    if (!s_promiscuous_cleanup_pending) return ESP_OK;
    /* Also disable after failed enable; do not assume partial effects absent. */
    esp_err_t err = esp_wifi_set_promiscuous(false);
    if (err != ESP_OK) return err;
    s_promiscuous_enabled = false;
    err = esp_wifi_set_promiscuous_rx_cb(NULL);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_channel(s_promiscuous_previous_channel,
                              s_promiscuous_previous_second_channel);
    if (err != ESP_OK) return err;
    s_promiscuous_cleanup_pending = false;
    return ESP_OK;
}

bool wifi_radio_promiscuous_needs_cleanup(void)
{
    return s_promiscuous_cleanup_pending;
}

bool wifi_radio_is_promiscuous_enabled(void)
{
    return s_promiscuous_enabled;
}

esp_err_t wifi_radio_get_channel_range(uint8_t *first, uint8_t *last)
{
    if (first == NULL || last == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    wifi_country_t country = {0};
    esp_err_t err = esp_wifi_get_country(&country);
    if (err != ESP_OK) return err;
    unsigned end = (unsigned)country.schan + country.nchan;
    if (country.schan < 1 || country.nchan == 0 || end > 15)
        return ESP_ERR_INVALID_STATE;
    *first = country.schan;
    *last = (uint8_t)(end - 1);
    return ESP_OK;
}

esp_err_t wifi_radio_promiscuous_set_channel(uint8_t channel)
{
    if (!s_wifi_started || !s_promiscuous_enabled) return ESP_ERR_INVALID_STATE;
    uint8_t first, last;
    esp_err_t err = wifi_radio_get_channel_range(&first, &last);
    if (err != ESP_OK) return err;
    if (channel < first || channel > last) return ESP_ERR_INVALID_ARG;
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

esp_err_t wifi_radio_scan_collect(wifi_scan_data_t *out_data)
{
    if (!s_wifi_started || out_data == NULL) {
        return out_data == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }

    memset(out_data, 0, sizeof(*out_data));

    uint16_t total = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&total);
    if (err != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return err;
    }

    out_data->total_found = total;
    out_data->truncated = total > WIFI_SCAN_MAX_APS;
    uint16_t requested = total > WIFI_SCAN_MAX_APS ? WIFI_SCAN_MAX_APS : total;
    if (requested == 0) {
        (void)esp_wifi_clear_ap_list();
        return ESP_OK;
    }

    err = esp_wifi_scan_get_ap_records(&requested, s_native_records);
    if (err != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return err;
    }

    out_data->count = requested;
    for (uint16_t i = 0; i < requested; ++i) {
        wifi_ap_info_t *target = &out_data->records[i];
        const wifi_ap_record_t *source = &s_native_records[i];

        memcpy(target->ssid, source->ssid, WIFI_SSID_MAX_LEN);
        target->ssid[WIFI_SSID_MAX_LEN] = '\0';
        memcpy(target->bssid, source->bssid, sizeof(target->bssid));
        target->rssi = source->rssi;
        target->primary_channel = source->primary;
        target->secondary_channel = (int8_t)source->second;
        target->security = map_security(source->authmode);
    }

    return ESP_OK;
}

wifi_rssi_quality_t wifi_rssi_quality(int8_t rssi)
{
    if (rssi >= -60) {
        return WIFI_RSSI_GOOD;
    }
    if (rssi >= -75) {
        return WIFI_RSSI_FAIR;
    }
    return WIFI_RSSI_WEAK;
}

const char *wifi_disconnect_reason_name(uint16_t reason)
{
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "DISASSOC_PWRCAP_BAD";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "DISASSOC_SUPCHAN_BAD";
        case WIFI_REASON_IE_INVALID: return "IE_INVALID";
        case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
        case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
        case WIFI_REASON_AKMP_INVALID: return "AKMP_INVALID";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
        case WIFI_REASON_INVALID_RSN_IE_CAP: return "INVALID_RSN_IE_CAP";
        case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
        case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
        default: return "UNKNOWN";
    }
}

const char *wifi_security_name(wifi_security_t security)
{
    switch (security) {
        case WIFI_SECURITY_OPEN: return "Open";
        case WIFI_SECURITY_WEP: return "WEP";
        case WIFI_SECURITY_WPA_PSK: return "WPA";
        case WIFI_SECURITY_WPA2_PSK: return "WPA2";
        case WIFI_SECURITY_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_SECURITY_ENTERPRISE: return "Enterprise";
        case WIFI_SECURITY_WPA3_PSK: return "WPA3";
        case WIFI_SECURITY_WPA2_WPA3_PSK: return "WPA2/WPA3";
        case WIFI_SECURITY_WAPI_PSK: return "WAPI";
        case WIFI_SECURITY_OWE: return "OWE";
        case WIFI_SECURITY_DPP: return "DPP";
        default: return "Unknown";
    }
}
