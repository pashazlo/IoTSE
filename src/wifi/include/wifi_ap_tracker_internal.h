#pragma once

#include "wifi_ap_tracker.h"

typedef struct {
    bool occupied;
    wifi_tracked_ap_t record;
} wifi_ap_tracker_slot_t;

typedef struct {
    uint32_t generation;
    wifi_ap_tracker_diagnostics_t diagnostics;
    wifi_ap_tracker_slot_t slots[WIFI_AP_TRACKER_CAPACITY];
} wifi_ap_tracker_state_t;

void wifi_ap_tracker_core_init(wifi_ap_tracker_state_t *state);
void wifi_ap_tracker_core_reset(wifi_ap_tracker_state_t *state);
bool wifi_ap_tracker_core_observe(wifi_ap_tracker_state_t *state,
                                  const wifi_ap_observation_t *observation);
void wifi_ap_tracker_core_snapshot(const wifi_ap_tracker_state_t *state,
                                   wifi_ap_tracker_snapshot_t *snapshot);
