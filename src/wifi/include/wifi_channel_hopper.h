#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    WIFI_CHANNEL_MODE_FIXED = 0,
    WIFI_CHANNEL_MODE_SURVEY,
} wifi_channel_mode_t;

typedef struct {
    wifi_channel_mode_t mode;
    uint8_t current_channel;
    uint8_t first_channel;
    uint8_t last_channel;
    uint32_t dwell_ms;
    uint64_t dwell_time_us[14];
    bool running;
} wifi_channel_engine_snapshot_t;

/* Asynchronous: ESP_OK means queued, NOT executed. Read worker results.
 * interval 0 = 250 ms; otherwise 50..60000 ms. stop also returns queue errors.
 */
esp_err_t wifi_channel_hopper_start(uint32_t interval_ms);
esp_err_t wifi_channel_hopper_stop(void);
bool wifi_channel_hopper_is_running(void);
/* Tick failure stops hopping only; promiscuous stays enabled on last channel. */
esp_err_t wifi_channel_hopper_last_error(void);
uint8_t wifi_channel_hopper_current_channel(void);
wifi_channel_mode_t wifi_channel_engine_mode(void);
bool wifi_channel_engine_get_snapshot(wifi_channel_engine_snapshot_t *snapshot);
#ifdef __cplusplus
}
#endif
