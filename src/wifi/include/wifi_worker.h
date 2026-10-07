#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi.h"
#include "wifi_connection_manager_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_WORKER_STATE_OFF = 0,
    WIFI_WORKER_STATE_STARTING,
    WIFI_WORKER_STATE_IDLE,
    WIFI_WORKER_STATE_SCANNING,
    WIFI_WORKER_STATE_ASSOCIATING,
    WIFI_WORKER_STATE_WAITING_IP,
    WIFI_WORKER_STATE_RETRY_WAIT,
    WIFI_WORKER_STATE_DISCONNECTING,
    WIFI_WORKER_STATE_SWITCHING_NETWORK,
    WIFI_WORKER_STATE_CONNECTED,
    WIFI_WORKER_STATE_PROMISCUOUS,
    WIFI_WORKER_STATE_STOPPING,
    WIFI_WORKER_STATE_ERROR,
} wifi_worker_state_t;

typedef enum {
    WIFI_WORKER_CMD_NONE = 0,
    WIFI_WORKER_CMD_START,
    WIFI_WORKER_CMD_SCAN,
    WIFI_WORKER_CMD_CONNECT,
    WIFI_WORKER_CMD_DISCONNECT,
    WIFI_WORKER_CMD_ENABLE_PROMISCUOUS,
    WIFI_WORKER_CMD_DISABLE_PROMISCUOUS,
    WIFI_WORKER_CMD_STOP,
    WIFI_WORKER_CMD_HOPPER_START,
    WIFI_WORKER_CMD_HOPPER_STOP,
    WIFI_WORKER_CMD_SET_CHANNEL,
    WIFI_WORKER_CMD_SET_AUTO_CONNECT,
    WIFI_WORKER_CMD_FORGET_NETWORK,
} wifi_worker_cmd_type_t;

typedef enum {
    WIFI_WORKER_RESULT_NONE = 0,
    WIFI_WORKER_RESULT_STARTED,
    WIFI_WORKER_RESULT_SCAN_READY,
    WIFI_WORKER_RESULT_CONNECTED,
    WIFI_WORKER_RESULT_DISCONNECTED,
    WIFI_WORKER_RESULT_PROMISCUOUS_ENABLED,
    WIFI_WORKER_RESULT_PROMISCUOUS_DISABLED,
    WIFI_WORKER_RESULT_STOPPED,
    WIFI_WORKER_RESULT_ERROR,
    WIFI_WORKER_RESULT_HOPPER_STARTED,
    WIFI_WORKER_RESULT_HOPPER_STOPPED,
    WIFI_WORKER_RESULT_CHANNEL_SET,
    WIFI_WORKER_RESULT_AUTO_CONNECT_UPDATED,
    WIFI_WORKER_RESULT_NETWORK_FORGOTTEN,
} wifi_worker_result_type_t;

typedef struct {
    wifi_worker_cmd_type_t type;
    uint32_t request_id;
    union {
        wifi_scan_options_t scan;
        uint32_t hopper_interval_ms;
        uint8_t channel;
        struct {
            char ssid[WIFI_SSID_MAX_LEN + 1];
            char password[WIFI_PASSWORD_MAX_LEN + 1];
            uint8_t bssid[6];
            uint8_t channel;
            wifi_security_t security;
            bool bssid_lock;
            bool candidate_known;
            int8_t candidate_rssi;
            uint32_t candidate_age_ms;
        } connect;
        struct {
            uint8_t channel;
            wifi_promiscuous_cb_t callback;
        } promiscuous;
        struct {
            char ssid[WIFI_SSID_MAX_LEN + 1];
            bool enabled;
        } auto_connect;
        struct {
            char ssid[WIFI_SSID_MAX_LEN + 1];
        } forget;
    } data;
} wifi_worker_cmd_t;

typedef struct {
    wifi_worker_result_type_t type;
    wifi_worker_cmd_type_t command;
    uint32_t request_id;
    uint32_t snapshot_generation;
    esp_err_t error;
    uint8_t channel; /* Actual channel applied by SET_CHANNEL. */
    uint16_t disconnect_reason;
    wifi_conn_failure_t failure_class;
    uint32_t attempt_id;
} wifi_worker_result_t;

typedef struct {
    uint32_t generation;
    wifi_scan_data_t scan;
} wifi_scan_snapshot_t;

typedef struct {
    uint32_t generation;
    wifi_connection_info_t info;
} wifi_connection_snapshot_t;

esp_err_t wifi_worker_init(void);

/* These functions enqueue work and never wait for the radio operation. */
esp_err_t wifi_worker_send_start(uint32_t *request_id);
esp_err_t wifi_worker_send_scan(
    const wifi_scan_options_t *options,
    uint32_t *request_id
);
esp_err_t wifi_worker_send_stop(uint32_t *request_id);
esp_err_t wifi_worker_send_hopper_start(uint32_t interval_ms, uint32_t *request_id);
esp_err_t wifi_worker_send_hopper_stop(uint32_t *request_id);
esp_err_t wifi_worker_send_set_channel(uint8_t channel, uint32_t *request_id);
esp_err_t wifi_worker_send_disconnect(uint32_t *request_id);
esp_err_t wifi_worker_send_enable_promiscuous(
    uint8_t channel,
    wifi_promiscuous_cb_t callback,
    uint32_t *request_id
);
esp_err_t wifi_worker_send_disable_promiscuous(uint32_t *request_id);
esp_err_t wifi_worker_send_connect(
    const char *ssid,
    const char *password,
    const uint8_t bssid[6],
    uint32_t *request_id
);
/* Uses Worker-owned credentials; the password is never returned to UI. */
esp_err_t wifi_worker_send_connect_saved(
    const char *ssid,
    const uint8_t bssid[6],
    uint32_t *request_id
);
bool wifi_worker_has_saved_network(const char *ssid);
bool wifi_worker_get_saved_network_state(
    const char *ssid,
    bool *auto_connect_enabled
);
esp_err_t wifi_worker_send_set_auto_connect(
    const char *ssid,
    bool enabled,
    uint32_t *request_id
);
esp_err_t wifi_worker_send_forget_network(
    const char *ssid,
    uint32_t *request_id
);

bool wifi_worker_receive_result(wifi_worker_result_t *result);
bool wifi_worker_get_scan_snapshot(wifi_scan_snapshot_t *snapshot);
bool wifi_worker_get_connection_snapshot(wifi_connection_snapshot_t *snapshot);
/* Lock-free snapshot of the ESP-IDF country channel range. */
bool wifi_worker_get_channel_range(uint8_t *first, uint8_t *last);
wifi_worker_state_t wifi_worker_get_state(void);
bool wifi_worker_request_pending(void);
/* True while STA has an acquired IP, including during a background scan. */
bool wifi_worker_is_connected(void);
bool wifi_worker_get_connection_diagnostics(wifi_conn_sm_t *snapshot);

/*
 * Air-monitor lifecycle gate. Suspending prevents saved-network retries and
 * cancels an automatic connection attempt, but never interrupts a connection
 * that the user established manually. Resume only schedules work in Worker.
 */
void wifi_worker_suspend_auto_connect(void);
void wifi_worker_resume_auto_connect(void);
bool wifi_worker_auto_connect_is_suspended(void);

#ifdef __cplusplus
}
#endif
