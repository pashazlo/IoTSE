#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef WIFI_AP_TRACKER_HOST_TEST
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif
#include "wifi_frame_parser.h"
#include "wifi_mgmt_parser.h"

#define WIFI_AP_TRACKER_CAPACITY 64U

typedef struct {
    uint8_t bssid[6];
    wifi_ssid_kind_t ssid_state;
    uint8_t ssid_length;
    char ssid[WIFI_MGMT_SSID_MAX_LEN + 1U];
    uint8_t rx_channel;
    bool has_advertised_channel;
    uint8_t advertised_channel;
    bool channel_mismatch;
    uint32_t channel_mismatch_count;
    int8_t rssi_last;
    int8_t rssi_min;
    int8_t rssi_max;
    int64_t rssi_sum;
    uint32_t rssi_sample_count;
    int64_t first_seen_us;
    int64_t last_seen_us;
    uint64_t frame_count;
    uint64_t beacon_count;
    uint64_t probe_response_count;
    bool has_beacon_interval;
    uint16_t beacon_interval;
    bool has_capability;
    uint16_t capability;
    bool capability_ess;
    bool capability_ibss;
    bool capability_privacy;
    bool rsn_present;
    wifi_rsn_info_t rsn;
    uint32_t update_generation;
} wifi_tracked_ap_t;

typedef struct {
    uint64_t observations_received;
    uint64_t ap_created;
    uint64_t ap_updated;
    uint64_t ap_evicted;
    uint64_t malformed_ignored;
    uint64_t channel_mismatch_count;
    uint16_t table_count;
    uint16_t table_capacity;
    uint16_t table_high_watermark;
} wifi_ap_tracker_diagnostics_t;

typedef struct {
    uint32_t generation;
    wifi_ap_tracker_diagnostics_t diagnostics;
    uint16_t count;
    wifi_tracked_ap_t records[WIFI_AP_TRACKER_CAPACITY];
} wifi_ap_tracker_snapshot_t;

/* About 20 KiB at the current capacity. Consumers must keep this in static
 * storage or PSRAM; never place a complete snapshot on a small task stack. */

typedef struct {
    const wifi_frame_info_t *frame;
    const wifi_mgmt_info_t *management;
    int64_t timestamp_us;
} wifi_ap_observation_t;

esp_err_t wifi_ap_tracker_init(void);
void wifi_ap_tracker_reset(void);
bool wifi_ap_tracker_observe(const wifi_ap_observation_t *observation);
bool wifi_ap_tracker_get_snapshot(wifi_ap_tracker_snapshot_t *snapshot);
bool wifi_ap_tracker_get_diagnostics(wifi_ap_tracker_diagnostics_t *diagnostics);
int8_t wifi_ap_record_average_rssi(const wifi_tracked_ap_t *record);
