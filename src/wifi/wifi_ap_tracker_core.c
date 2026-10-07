#include "wifi_ap_tracker_internal.h"

#include <limits.h>
#include <string.h>

static void increment_u64(uint64_t *value)
{
    if (*value != UINT64_MAX) ++*value;
}

static void increment_u32(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

static uint32_t next_generation(uint32_t value)
{
    ++value;
    return value == 0U ? 1U : value;
}

static bool valid_bssid(const uint8_t bssid[6])
{
    bool any_nonzero = false;
    if ((bssid[0] & 1U) != 0U) return false;
    for (size_t i = 0; i < 6U; ++i) any_nonzero |= bssid[i] != 0U;
    return any_nonzero;
}

static bool valid_ssid(const wifi_mgmt_info_t *management)
{
    if (management->ssid_kind > WIFI_SSID_NORMAL ||
        management->ssid_raw_length > WIFI_MGMT_SSID_MAX_LEN) return false;
    if (management->ssid_kind == WIFI_SSID_ABSENT ||
        management->ssid_kind == WIFI_SSID_EMPTY)
        return management->ssid_raw_length == 0U;
    return management->ssid_raw_length != 0U;
}

static int find_bssid(const wifi_ap_tracker_state_t *state,
                      const uint8_t bssid[6])
{
    for (size_t i = 0; i < WIFI_AP_TRACKER_CAPACITY; ++i) {
        if (state->slots[i].occupied &&
            memcmp(state->slots[i].record.bssid, bssid, 6U) == 0) return (int)i;
    }
    return -1;
}

static size_t insertion_slot(const wifi_ap_tracker_state_t *state,
                             bool *evicted)
{
    for (size_t i = 0; i < WIFI_AP_TRACKER_CAPACITY; ++i) {
        if (!state->slots[i].occupied) {
            *evicted = false;
            return i;
        }
    }
    size_t oldest = 0U;
    for (size_t i = 1U; i < WIFI_AP_TRACKER_CAPACITY; ++i) {
        if (state->slots[i].record.last_seen_us <
            state->slots[oldest].record.last_seen_us) oldest = i;
    }
    *evicted = true;
    return oldest;
}

static void update_ssid(wifi_tracked_ap_t *record,
                        const wifi_mgmt_info_t *management)
{
    if (!valid_ssid(management) || management->ssid_kind == WIFI_SSID_ABSENT)
        return;
    if (management->ssid_kind != WIFI_SSID_NORMAL &&
        record->ssid_state == WIFI_SSID_NORMAL) return;
    record->ssid_state = management->ssid_kind;
    record->ssid_length = management->ssid_raw_length;
    memset(record->ssid, 0, sizeof(record->ssid));
    if (management->ssid_kind == WIFI_SSID_NORMAL)
        memcpy(record->ssid, management->ssid, management->ssid_raw_length);
}

static bool known_pairwise(const wifi_rsn_info_t *rsn)
{
    for (uint8_t i = 0; i < rsn->pairwise_count; ++i)
        if (rsn->pairwise[i].kind != WIFI_CIPHER_UNKNOWN) return true;
    return false;
}

static bool known_akm(const wifi_rsn_info_t *rsn)
{
    for (uint8_t i = 0; i < rsn->akm_count; ++i)
        if (rsn->akms[i].kind != WIFI_AKM_UNKNOWN) return true;
    return false;
}

static bool valid_rsn(const wifi_rsn_info_t *rsn)
{
    if (rsn->status != WIFI_PARSE_VALID || rsn->version != 1U ||
        rsn->pairwise_count > WIFI_RSN_MAX_PAIRWISE_CIPHERS ||
        rsn->akm_count > WIFI_RSN_MAX_AKMS ||
        rsn->group_cipher.kind > WIFI_CIPHER_BIP_CMAC_256 ||
        rsn->group_management_cipher.kind > WIFI_CIPHER_BIP_CMAC_256 ||
        rsn->pmf > WIFI_PMF_REQUIRED) return false;
    for (uint8_t i = 0; i < rsn->pairwise_count; ++i)
        if (rsn->pairwise[i].kind > WIFI_CIPHER_BIP_CMAC_256) return false;
    for (uint8_t i = 0; i < rsn->akm_count; ++i)
        if (rsn->akms[i].kind > WIFI_AKM_OWE) return false;
    return true;
}

static void update_rsn(wifi_tracked_ap_t *record, const wifi_rsn_info_t *incoming)
{
    if (incoming->status != WIFI_PARSE_VALID) return;
    if (!record->rsn_present || record->rsn.status != WIFI_PARSE_VALID) {
        record->rsn = *incoming;
        record->rsn_present = true;
        return;
    }
    wifi_rsn_info_t merged = *incoming;
    if (incoming->group_cipher.kind == WIFI_CIPHER_UNKNOWN &&
        record->rsn.group_cipher.kind != WIFI_CIPHER_UNKNOWN)
        merged.group_cipher = record->rsn.group_cipher;
    if (!known_pairwise(incoming) && known_pairwise(&record->rsn)) {
        merged.pairwise_advertised_count = record->rsn.pairwise_advertised_count;
        merged.pairwise_count = record->rsn.pairwise_count;
        memcpy(merged.pairwise, record->rsn.pairwise, sizeof(merged.pairwise));
    }
    if (!known_akm(incoming) && known_akm(&record->rsn)) {
        merged.akm_advertised_count = record->rsn.akm_advertised_count;
        merged.akm_count = record->rsn.akm_count;
        memcpy(merged.akms, record->rsn.akms, sizeof(merged.akms));
    }
    if (incoming->pmf == WIFI_PMF_UNAVAILABLE &&
        record->rsn.pmf != WIFI_PMF_UNAVAILABLE) {
        merged.has_capabilities = record->rsn.has_capabilities;
        merged.capabilities = record->rsn.capabilities;
        merged.pmf = record->rsn.pmf;
    }
    if ((!incoming->has_group_management_cipher ||
         incoming->group_management_cipher.kind == WIFI_CIPHER_UNKNOWN) &&
        record->rsn.has_group_management_cipher &&
        record->rsn.group_management_cipher.kind != WIFI_CIPHER_UNKNOWN) {
        merged.has_group_management_cipher = true;
        merged.group_management_cipher = record->rsn.group_management_cipher;
    }
    record->rsn = merged;
    record->rsn_present = true;
}

static void update_rssi(wifi_tracked_ap_t *record, int8_t rssi)
{
    record->rssi_last = rssi;
    if (record->rssi_sample_count == 0U || rssi < record->rssi_min)
        record->rssi_min = rssi;
    if (record->rssi_sample_count == 0U || rssi > record->rssi_max)
        record->rssi_max = rssi;
    if (record->rssi_sample_count != UINT32_MAX) {
        record->rssi_sum += rssi;
        ++record->rssi_sample_count;
    }
}

void wifi_ap_tracker_core_init(wifi_ap_tracker_state_t *state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->generation = 1U;
    state->diagnostics.table_capacity = WIFI_AP_TRACKER_CAPACITY;
}

void wifi_ap_tracker_core_reset(wifi_ap_tracker_state_t *state)
{
    if (state == NULL) return;
    const uint32_t generation = next_generation(state->generation);
    memset(state, 0, sizeof(*state));
    state->generation = generation;
    state->diagnostics.table_capacity = WIFI_AP_TRACKER_CAPACITY;
}

bool wifi_ap_tracker_core_observe(wifi_ap_tracker_state_t *state,
                                  const wifi_ap_observation_t *observation)
{
    if (state == NULL || observation == NULL || observation->frame == NULL ||
        observation->management == NULL) return false;
    increment_u64(&state->diagnostics.observations_received);
    const wifi_frame_info_t *frame = observation->frame;
    const wifi_mgmt_info_t *management = observation->management;
    const bool eligible_subtype = frame->subtype == WIFI_FRAME_SUBTYPE_BEACON ||
        frame->subtype == WIFI_FRAME_SUBTYPE_PROBE_RESP;
    if (!eligible_subtype || frame->type != WIFI_FRAME_TYPE_MGMT ||
        !frame->has_bssid || !valid_bssid(frame->bssid) ||
        observation->timestamp_us <= 0 ||
        management->subtype != frame->subtype ||
        management->rx_channel != frame->channel ||
        management->status == WIFI_PARSE_MALFORMED ||
        management->status == WIFI_PARSE_UNSUPPORTED ||
        !valid_ssid(management)) {
        increment_u64(&state->diagnostics.malformed_ignored);
        return false;
    }

    int found = find_bssid(state, frame->bssid);
    const bool complete = management->status == WIFI_PARSE_VALID;
    if (found < 0 && !complete) {
        increment_u64(&state->diagnostics.malformed_ignored);
        return false;
    }
    bool evicted = false;
    size_t slot_index;
    if (found < 0) {
        slot_index = insertion_slot(state, &evicted);
        if (evicted) increment_u64(&state->diagnostics.ap_evicted);
        else {
            ++state->diagnostics.table_count;
            if (state->diagnostics.table_count >
                state->diagnostics.table_high_watermark)
                state->diagnostics.table_high_watermark =
                    state->diagnostics.table_count;
        }
        memset(&state->slots[slot_index], 0, sizeof(state->slots[slot_index]));
        state->slots[slot_index].occupied = true;
        memcpy(state->slots[slot_index].record.bssid, frame->bssid, 6U);
        state->slots[slot_index].record.first_seen_us = observation->timestamp_us;
        increment_u64(&state->diagnostics.ap_created);
    } else {
        slot_index = (size_t)found;
        increment_u64(&state->diagnostics.ap_updated);
    }

    wifi_tracked_ap_t *record = &state->slots[slot_index].record;
    if (observation->timestamp_us > record->last_seen_us)
        record->last_seen_us = observation->timestamp_us;
    increment_u64(&record->frame_count);
    if (frame->subtype == WIFI_FRAME_SUBTYPE_BEACON)
        increment_u64(&record->beacon_count);
    else increment_u64(&record->probe_response_count);
    update_rssi(record, frame->rssi);
    if (frame->channel >= 1U && frame->channel <= 14U)
        record->rx_channel = frame->channel;

    if (complete) {
        update_ssid(record, management);
        if (management->has_advertised_channel &&
            management->advertised_channel >= 1U &&
            management->advertised_channel <= 14U) {
            record->has_advertised_channel = true;
            record->advertised_channel = management->advertised_channel;
        }
        if (management->has_beacon_interval) {
            record->has_beacon_interval = true;
            record->beacon_interval = management->beacon_interval;
        }
        if (management->has_capability) {
            record->has_capability = true;
            record->capability = management->capability;
            record->capability_ess = management->capability_ess;
            record->capability_ibss = management->capability_ibss;
            record->capability_privacy = management->capability_privacy;
        }
        if (management->rsn_ie_seen) {
            if (valid_rsn(&management->rsn)) update_rsn(record, &management->rsn);
            else increment_u64(&state->diagnostics.malformed_ignored);
        }
    }
    record->channel_mismatch = record->has_advertised_channel &&
        record->rx_channel != 0U &&
        record->rx_channel != record->advertised_channel;
    if (record->channel_mismatch) {
        increment_u32(&record->channel_mismatch_count);
        increment_u64(&state->diagnostics.channel_mismatch_count);
    }
    state->generation = next_generation(state->generation);
    record->update_generation = state->generation;
    return true;
}

void wifi_ap_tracker_core_snapshot(const wifi_ap_tracker_state_t *state,
                                   wifi_ap_tracker_snapshot_t *snapshot)
{
    if (state == NULL || snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = state->generation;
    snapshot->diagnostics = state->diagnostics;
    for (size_t i = 0; i < WIFI_AP_TRACKER_CAPACITY; ++i) {
        if (state->slots[i].occupied)
            snapshot->records[snapshot->count++] = state->slots[i].record;
    }
}

int8_t wifi_ap_record_average_rssi(const wifi_tracked_ap_t *record)
{
    if (record == NULL || record->rssi_sample_count == 0U) return 0;
    return (int8_t)(record->rssi_sum / (int64_t)record->rssi_sample_count);
}
