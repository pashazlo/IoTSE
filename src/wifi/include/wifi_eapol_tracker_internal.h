#pragma once

#include "wifi_eapol_tracker.h"

typedef struct {
    bool occupied;
    wifi_eapol_session_t session;
    uint8_t replay_valid_mask;
    uint8_t m1_nonce[32];
    uint8_t m3_nonce[32];
} wifi_eapol_slot_t;

typedef struct {
    uint32_t generation;
    wifi_eapol_tracker_diagnostics_t diagnostics;
    wifi_eapol_slot_t slots[WIFI_EAPOL_TRACKER_CAPACITY];
} wifi_eapol_tracker_state_t;

void wifi_eapol_tracker_core_init(wifi_eapol_tracker_state_t *state);
void wifi_eapol_tracker_core_reset(wifi_eapol_tracker_state_t *state);
bool wifi_eapol_tracker_core_observe(
    wifi_eapol_tracker_state_t *state,
    const wifi_eapol_observation_t *observation);
void wifi_eapol_tracker_core_snapshot(
    const wifi_eapol_tracker_state_t *state,
    wifi_eapol_tracker_snapshot_t *snapshot);
