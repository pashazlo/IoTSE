#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi_channel_hopper.h"

#define WIFI_ANALYZER_MAX_CHANNELS 14U

typedef struct {
    uint8_t channel;
    uint64_t dwell_time_us;
    uint64_t total_frames;
    uint32_t frames_per_second;
    uint64_t bytes;
    uint64_t management_frames;
    uint64_t data_frames;
    uint64_t control_frames;
    uint64_t eapol_frames;
    int64_t rssi_sum;
    uint64_t rssi_count;
    int8_t rssi_min;
    int8_t rssi_max;
    int64_t last_seen_us;
} wifi_channel_observation_t;

typedef struct {
    uint64_t packets;
    uint64_t management_frames;
    uint64_t data_frames;
    uint64_t control_frames;
    uint64_t eapol_frames;
    uint64_t parser_failures;
    uint64_t mgmt_frames_parsed;
    uint64_t mgmt_frames_malformed;
    uint64_t rsn_ie_seen;
    uint64_t rsn_parse_ok;
    uint64_t rsn_parse_malformed;
    uint64_t ie_chain_malformed;
    int64_t last_timestamp_us;
    uint32_t correlation_id;
    uint8_t last_channel;
} wifi_analyzer_snapshot_t;

typedef struct {
    uint64_t rx_total;
    uint32_t rx_packets_per_second;
    uint32_t ingress_queue_current;
    uint32_t ingress_queue_capacity;
    uint32_t ingress_queue_high_water;
    uint32_t ingress_dropped;
    uint64_t ingress_enqueued;
    uint32_t ingress_dropped_full;
    uint32_t ingress_dropped_oversize;
    uint32_t ingress_dropped_invalid;
    uint64_t analyzer_processed;
    uint64_t parser_failures;
    uint64_t mgmt_frames_parsed;
    uint64_t mgmt_frames_malformed;
    uint64_t rsn_ie_seen;
    uint64_t rsn_parse_ok;
    uint64_t rsn_parse_malformed;
    uint64_t ie_chain_malformed;
    uint32_t ring_bytes;
    uint32_t ring_records;
    uint32_t ring_high_water_bytes;
    uint32_t ring_high_water_records;
    uint32_t overwritten_records;
    uint32_t overwritten_bytes;
    uint32_t dropped_busy;
    uint32_t dropped_frozen;
    uint32_t dropped_oversize;
    uint32_t corruption_resets;
    uint32_t save_busy_count;
    uint8_t active_bank;
    int8_t sealed_bank;
    bool writer_busy;
    uint8_t current_channel;
    wifi_channel_mode_t channel_mode;
    uint32_t free_internal_heap;
    uint32_t min_internal_heap;
    uint32_t free_psram;
    uint32_t min_psram;
    uint32_t analyzer_stack_hwm;
    uint32_t writer_stack_hwm;
} wifi_pipeline_diagnostics_t;

esp_err_t wifi_analyzer_init(void);
/* Synchronous session barrier. Requires radio producer stopped. On ESP_OK,
 * analyzer owns no packet and no pre-reset READY packet can update state. */
esp_err_t wifi_analyzer_reset(void);
void wifi_analyzer_set_correlation_id(uint32_t correlation_id);
bool wifi_analyzer_get_snapshot(wifi_analyzer_snapshot_t *snapshot);
bool wifi_analyzer_get_channel_observations(
    wifi_channel_observation_t observations[WIFI_ANALYZER_MAX_CHANNELS],
    uint8_t *first_channel, uint8_t *last_channel);
bool wifi_analyzer_get_diagnostics(wifi_pipeline_diagnostics_t *snapshot);
