#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wifi_eapol_tracker_internal.h"

static uint32_t rng = 0x514E67A3U;
static uint32_t next_random(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}
static void put_be16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }

static const wifi_eapol_session_t *find_session(
    const wifi_eapol_tracker_snapshot_t *snapshot,
    const uint8_t bssid[6], const uint8_t sta[6])
{
    for (unsigned i = 0; i < snapshot->count; ++i)
        if (memcmp(snapshot->sessions[i].bssid, bssid, 6) == 0 &&
            memcmp(snapshot->sessions[i].sta, sta, 6) == 0)
            return &snapshot->sessions[i];
    return NULL;
}

int main(void)
{
    wifi_eapol_tracker_state_t state;
    wifi_eapol_tracker_core_init(&state);
    uint8_t bytes[192];
    bool capacity_seen = false, eviction_seen = false;
    for (unsigned iteration = 0; iteration < 50000; ++iteration) {
        size_t length = next_random() % sizeof(bytes);
        for (size_t i = 0; i < length; ++i) bytes[i] = (uint8_t)next_random();
        wifi_eapol_info_t a, b;
        wifi_eapol_parse(bytes, length, &a);
        wifi_eapol_parse(bytes, length, &b);
        assert(memcmp(&a, &b, sizeof(a)) == 0);

        if ((iteration % 7U) == 0U) {
            memset(bytes, 0, sizeof(bytes));
            bytes[0] = 2; bytes[1] = 3; put_be16(bytes + 2, 95);
            bytes[4] = 2; put_be16(bytes + 5, 0x008A);
            put_be16(bytes + 4 + 93, 0);
            assert(wifi_eapol_parse(bytes, 99, &a) == WIFI_EAPOL_PARSE_OK);
            wifi_eapol_observation_t o = {
                .bssid = {0x02,0,0,(uint8_t)(iteration >> 8),
                          (uint8_t)iteration,1},
                .sta = {0x02,1,0,(uint8_t)(iteration >> 8),
                        (uint8_t)iteration,2},
                .direction = WIFI_FRAME_DIRECTION_AP_TO_STA,
                .channel = (uint8_t)(1U + iteration % 14U),
                .rssi = -60, .timestamp_us = (int64_t)iteration + 1,
                .eapol = &a, .message = WIFI_EAPOL_MESSAGE_M1,
            };
            assert(wifi_eapol_tracker_core_observe(&state, &o));
            memset(&a, 0xA5, sizeof(a)); /* Tracker must have copied values. */
            wifi_eapol_tracker_snapshot_t copied;
            wifi_eapol_tracker_core_snapshot(&state, &copied);
            const wifi_eapol_session_t *saved = find_session(
                &copied, o.bssid, o.sta);
            assert(saved != NULL && saved->observed_mask == 1U &&
                   saved->message_count[0] >= 1U &&
                   saved->message_replay_counter[0] == 0U);
        }
        if ((iteration % 13U) == 0U) {
            wifi_eapol_info_t malformed;
            memset(&malformed, 0, sizeof(malformed));
            malformed.status = WIFI_EAPOL_PARSE_MALFORMED;
            wifi_eapol_observation_t invalid = {
                .bssid = {0x02,0xFE,0,0,0,1},
                .sta = {0x02,0xFE,0,0,0,2},
                .direction = WIFI_FRAME_DIRECTION_AP_TO_STA,
                .channel = 1, .rssi = -80,
                .timestamp_us = (int64_t)iteration + 1,
                .eapol = &malformed, .message = WIFI_EAPOL_MESSAGE_UNKNOWN,
            };
            wifi_eapol_tracker_snapshot_t before, after;
            wifi_eapol_tracker_core_snapshot(&state, &before);
            const wifi_eapol_session_t *old = find_session(
                &before, invalid.bssid, invalid.sta);
            uint8_t old_mask = old == NULL ? 0U : old->observed_mask;
            wifi_eapol_session_state_t old_state = old == NULL
                ? WIFI_EAPOL_SESSION_EMPTY : old->state;
            assert(wifi_eapol_tracker_core_observe(&state, &invalid));
            memset(&malformed, 0x5A, sizeof(malformed));
            wifi_eapol_tracker_core_snapshot(&state, &after);
            const wifi_eapol_session_t *current = find_session(
                &after, invalid.bssid, invalid.sta);
            assert(current != NULL && current->malformed_count >= 1U);
            assert(current->observed_mask == old_mask &&
                   current->state == old_state);
        }
        if (state.diagnostics.table_count == WIFI_EAPOL_TRACKER_CAPACITY)
            capacity_seen = true;
        if (state.diagnostics.sessions_evicted != 0U) eviction_seen = true;
        if (iteration != 0U && (iteration % 997U) == 0U) {
            uint32_t before = state.generation;
            wifi_eapol_tracker_core_reset(&state);
            assert(state.generation != before);
            assert(state.diagnostics.table_count == 0U);
            assert(state.diagnostics.table_capacity == WIFI_EAPOL_TRACKER_CAPACITY);
        }
        if ((iteration % 101U) == 0U) {
            wifi_eapol_tracker_snapshot_t snapshot;
            wifi_eapol_tracker_core_snapshot(&state, &snapshot);
            assert(snapshot.count <= WIFI_EAPOL_TRACKER_CAPACITY);
            assert(snapshot.diagnostics.table_count == snapshot.count);
            assert(snapshot.diagnostics.table_high_watermark <=
                   WIFI_EAPOL_TRACKER_CAPACITY);
            for (unsigned i = 0; i < snapshot.count; ++i) {
                assert(snapshot.sessions[i].first_seen_us <=
                       snapshot.sessions[i].last_seen_us);
                for (unsigned j = i + 1; j < snapshot.count; ++j)
                    assert(memcmp(snapshot.sessions[i].bssid,
                                  snapshot.sessions[j].bssid, 6) != 0 ||
                           memcmp(snapshot.sessions[i].sta,
                                  snapshot.sessions[j].sta, 6) != 0);
            }
        }
    }
    wifi_eapol_tracker_snapshot_t snapshot;
    wifi_eapol_tracker_core_snapshot(&state, &snapshot);
    assert(snapshot.count <= WIFI_EAPOL_TRACKER_CAPACITY);
    assert(capacity_seen && eviction_seen);
    printf("wifi_eapol_stress: PASS (50000 deterministic inputs, count=%u, "
           "state=%zu, snapshot=%zu)\n", snapshot.count, sizeof(state),
           sizeof(snapshot));
    return 0;
}
