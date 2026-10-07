#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wifi_sta_tracker_internal.h"

static uint32_t rng_state = 0x5A17C3E1U;
static uint32_t rnd(void) {
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state;
}
static bool unicast(const uint8_t m[6]) {
    if ((m[0] & 1U) != 0U) return false;
    uint8_t any = 0U;
    for (unsigned i = 0; i < 6U; ++i) any |= m[i];
    return any != 0U;
}
static void make_mac(uint8_t out[6], uint32_t id) {
    out[0] = 0x02U;
    out[1] = (uint8_t)(id >> 24);
    out[2] = (uint8_t)(id >> 16);
    out[3] = (uint8_t)(id >> 8);
    out[4] = (uint8_t)id;
    out[5] = (uint8_t)(id ^ 0xA5U);
}
static void validate(const wifi_sta_tracker_state_t *state,
                     wifi_sta_tracker_snapshot_t *snapshot) {
    wifi_sta_tracker_core_snapshot(state, snapshot);
    assert(snapshot->count <= WIFI_STA_TRACKER_CAPACITY);
    assert(snapshot->count == snapshot->diagnostics.table_count);
    assert(snapshot->diagnostics.table_high_watermark <= WIFI_STA_TRACKER_CAPACITY);
    for (uint16_t i = 0; i < snapshot->count; ++i) {
        const wifi_tracked_sta_t *r = &snapshot->records[i];
        assert(unicast(r->mac));
        assert(r->first_seen_us > 0 && r->first_seen_us <= r->last_seen_us);
        assert(r->rssi_sample_count > 0U);
        assert(r->rssi_min <= r->rssi_last && r->rssi_last <= r->rssi_max);
        assert(r->frame_count >= r->rssi_sample_count);
        assert(!r->has_related_bssid || unicast(r->related_bssid));
        for (uint16_t j = i + 1; j < snapshot->count; ++j)
            assert(memcmp(r->mac, snapshot->records[j].mac, 6) != 0);
    }
}
int main(void) {
    static wifi_sta_tracker_state_t state;
    static wifi_sta_tracker_snapshot_t snapshot;
    const uint8_t bssid[6] = {0x10,0x20,0x30,0x40,0x50,0x60};
    wifi_sta_tracker_core_init(&state);
    uint32_t generation = state.generation;
    int64_t now = 1;
    for (unsigned i = 0; i < 50000U; ++i) {
        wifi_frame_info_t f;
        memset(&f, 0, sizeof(f));
        uint8_t sta[6];
        make_mac(sta, rnd() % 1024U + 1U);
        f.type = WIFI_FRAME_TYPE_DATA;
        f.channel = (uint8_t)(rnd() % 14U + 1U);
        f.rssi = (int8_t)(-100 + (int)(rnd() % 81U));
        if ((rnd() & 1U) != 0U) {
            f.to_ds = true; memcpy(f.addr1,bssid,6); memcpy(f.addr2,sta,6); memcpy(f.bssid,bssid,6);
        } else {
            f.from_ds = true; memcpy(f.addr1,sta,6); memcpy(f.addr2,bssid,6); memcpy(f.bssid,bssid,6);
        }
        wifi_sta_observation_t o = {.frame=&f, .timestamp_us=now++};
        assert(wifi_sta_tracker_core_observe(&state, &o));
        assert(state.generation != generation);
        generation = state.generation;
        if ((i % 257U) == 0U) validate(&state, &snapshot);
    }
    validate(&state, &snapshot);
    assert(snapshot.count == WIFI_STA_TRACKER_CAPACITY);
    assert(snapshot.diagnostics.sta_evicted > 0U);

    wifi_frame_info_t ambiguous;
    memset(&ambiguous, 0, sizeof(ambiguous));
    ambiguous.type = WIFI_FRAME_TYPE_DATA;
    uint32_t before = state.generation;
    wifi_sta_observation_t rejected = {.frame=&ambiguous, .timestamp_us=now};
    assert(!wifi_sta_tracker_core_observe(&state, &rejected));
    assert(state.generation == before);

    wifi_sta_tracker_core_reset(&state);
    wifi_sta_tracker_core_snapshot(&state, &snapshot);
    assert(snapshot.count == 0U);
    assert(snapshot.diagnostics.table_count == 0U);
    assert(snapshot.generation != before);
    puts("wifi_sta_stress: PASS (50000 observations, deterministic seed 0x5A17C3E1)");
    return 0;
}