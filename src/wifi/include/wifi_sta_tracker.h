#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef WIFI_STA_TRACKER_HOST_TEST
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif
#include "wifi_frame_parser.h"
#include "wifi_mgmt_parser.h"

#define WIFI_STA_TRACKER_CAPACITY 128U

typedef enum {
    WIFI_STA_REL_UNKNOWN = 0, WIFI_STA_REL_OBSERVED, WIFI_STA_REL_PROBING,
    WIFI_STA_REL_AUTHENTICATING, WIFI_STA_REL_ASSOCIATING,
    WIFI_STA_REL_AUTHENTICATED_OBSERVED, WIFI_STA_REL_AUTH_FAILED_OBSERVED,
    WIFI_STA_REL_ASSOCIATED_OBSERVED, WIFI_STA_REL_ASSOC_FAILED_OBSERVED,
    WIFI_STA_REL_DATA_RELATION_OBSERVED, WIFI_STA_REL_DISCONNECTED_OBSERVED,
} wifi_sta_relationship_state_t;

typedef enum {
    WIFI_STA_SOURCE_NONE = 0, WIFI_STA_SOURCE_MGMT_AUTH,
    WIFI_STA_SOURCE_MGMT_ASSOC, WIFI_STA_SOURCE_MGMT_REASSOC,
    WIFI_STA_SOURCE_DATA_TO_DS, WIFI_STA_SOURCE_DATA_FROM_DS,
    WIFI_STA_SOURCE_MGMT_DISCONNECT,
} wifi_sta_relationship_source_t;

typedef struct {
    uint8_t mac[6];
    bool locally_administered;
    int64_t first_seen_us, last_seen_us;
    uint8_t rx_channel;
    int8_t rssi_last, rssi_min, rssi_max;
    int64_t rssi_sum;
    uint32_t rssi_sample_count;
    uint64_t frame_count, management_count, data_count, control_count;
    uint64_t tx_observed_count, rx_observed_count, eapol_observed_count;
    wifi_sta_relationship_state_t relationship_state;
    wifi_sta_relationship_source_t relationship_source;
    bool has_related_bssid;
    uint8_t related_bssid[6];
    uint32_t update_generation;
} wifi_tracked_sta_t;

typedef struct {
    uint64_t observations_received, sta_created, existing_sta_observations, sta_evicted;
    uint64_t invalid_mac_ignored, ambiguous_relation_count, relationship_observations;
    uint16_t table_count, table_capacity, table_high_watermark;
} wifi_sta_tracker_diagnostics_t;

typedef struct {
    uint32_t generation;
    wifi_sta_tracker_diagnostics_t diagnostics;
    uint16_t count;
    wifi_tracked_sta_t records[WIFI_STA_TRACKER_CAPACITY];
} wifi_sta_tracker_snapshot_t;

/* Full snapshot is about 16 KiB. DO NOT allocate it on a small FreeRTOS task
 * stack. Use static storage, heap/PSRAM, or another persistent caller buffer. */

typedef struct {
    const wifi_frame_info_t *frame;
    const wifi_mgmt_info_t *management; /* NULL for DATA/CTRL. */
    int64_t timestamp_us;
} wifi_sta_observation_t;

esp_err_t wifi_sta_tracker_init(void);
void wifi_sta_tracker_reset(void);
bool wifi_sta_tracker_observe(const wifi_sta_observation_t *observation);
bool wifi_sta_tracker_get_snapshot(wifi_sta_tracker_snapshot_t *snapshot);
bool wifi_sta_tracker_get_diagnostics(wifi_sta_tracker_diagnostics_t *diagnostics);
int8_t wifi_sta_record_average_rssi(const wifi_tracked_sta_t *record);
