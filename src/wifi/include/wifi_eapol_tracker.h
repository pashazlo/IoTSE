#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef WIFI_EAPOL_TRACKER_HOST_TEST
typedef int esp_err_t;
#else
#include "esp_err.h"
#endif

#include "wifi_eapol_parser.h"

#define WIFI_EAPOL_TRACKER_CAPACITY 64U

typedef enum {
    WIFI_EAPOL_SESSION_EMPTY = 0,
    WIFI_EAPOL_SESSION_PARTIAL,
    WIFI_EAPOL_SESSION_COMPLETE,
} wifi_eapol_session_state_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t sta[6];
    int64_t first_seen_us;
    int64_t last_seen_us;
    uint8_t channel;
    int8_t rssi_last;
    uint32_t eapol_frame_count;
    uint32_t eapol_key_count;
    uint32_t malformed_count;
    uint32_t unknown_classification_count;
    uint32_t message_count[4]; /* M1..M4 */
    uint32_t retransmission_count;
    uint32_t exchange_restart_count;
    uint64_t last_replay_counter;
    uint64_t message_replay_counter[4];
    uint8_t observed_mask; /* bits 0..3 = M1..M4 for current exchange */
    wifi_eapol_session_state_t state;
    uint32_t exchange_generation;
    uint32_t update_generation;
} wifi_eapol_session_t;

typedef struct {
    uint64_t eapol_frames_seen;
    uint64_t eapol_parse_ok;
    uint64_t eapol_parse_malformed;
    uint64_t eapol_key_frames;
    uint64_t classified_m1;
    uint64_t classified_m2;
    uint64_t classified_m3;
    uint64_t classified_m4;
    uint64_t classified_unknown;
    uint64_t sessions_created;
    uint64_t sessions_evicted;
    uint64_t exchange_restarts;
    uint64_t retransmissions_observed;
    uint64_t unsupported_identity;
    uint16_t table_count;
    uint16_t table_capacity;
    uint16_t table_high_watermark;
} wifi_eapol_tracker_diagnostics_t;

typedef struct {
    uint32_t generation;
    wifi_eapol_tracker_diagnostics_t diagnostics;
    uint16_t count;
    wifi_eapol_session_t sessions[WIFI_EAPOL_TRACKER_CAPACITY];
} wifi_eapol_tracker_snapshot_t;

/* Snapshot is intentionally bounded but large. Never allocate it on a small
 * FreeRTOS task stack; use static storage, heap, or PSRAM. */

typedef struct {
    uint8_t bssid[6];
    uint8_t sta[6];
    wifi_frame_direction_t direction;
    uint8_t channel;
    int8_t rssi;
    int64_t timestamp_us;
    const wifi_eapol_info_t *eapol; /* Borrowed for this call only. */
    wifi_eapol_message_t message;
} wifi_eapol_observation_t;

esp_err_t wifi_eapol_tracker_init(void);
void wifi_eapol_tracker_reset(void);
bool wifi_eapol_tracker_observe(const wifi_eapol_observation_t *observation);
bool wifi_eapol_tracker_get_snapshot(wifi_eapol_tracker_snapshot_t *snapshot);
bool wifi_eapol_tracker_get_diagnostics(
    wifi_eapol_tracker_diagnostics_t *diagnostics);
/* Number of currently retained sessions whose M1..M4 exchange passed the
 * tracker's replay-counter coherence check. Uses the tracker mutex. */
bool wifi_eapol_tracker_get_completed_count(uint16_t *completed_count);
size_t wifi_eapol_tracker_state_size(void);
size_t wifi_eapol_tracker_snapshot_size(void);
