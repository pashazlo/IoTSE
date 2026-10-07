#pragma once
#include "wifi_sta_tracker.h"
typedef struct { bool occupied; wifi_tracked_sta_t record; } wifi_sta_slot_t;
typedef struct {
    uint32_t generation;
    wifi_sta_tracker_diagnostics_t diagnostics;
    wifi_sta_slot_t slots[WIFI_STA_TRACKER_CAPACITY];
} wifi_sta_tracker_state_t;
void wifi_sta_tracker_core_init(wifi_sta_tracker_state_t *state);
void wifi_sta_tracker_core_reset(wifi_sta_tracker_state_t *state);
bool wifi_sta_tracker_core_observe(wifi_sta_tracker_state_t *state,
                                   const wifi_sta_observation_t *observation);
void wifi_sta_tracker_core_snapshot(const wifi_sta_tracker_state_t *state,
                                    wifi_sta_tracker_snapshot_t *snapshot);
