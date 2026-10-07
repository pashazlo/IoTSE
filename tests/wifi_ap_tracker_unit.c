#include <assert.h>
#include <string.h>

#include "wifi_ap_tracker_internal.h"

static wifi_ap_tracker_state_t state;
static wifi_ap_tracker_snapshot_t snapshot_a;
static wifi_ap_tracker_snapshot_t snapshot_b;

static void bssid(uint8_t out[6], uint8_t id)
{
    const uint8_t value[6] = {0x02, 0, 0, 0, 0, id};
    memcpy(out, value, sizeof(value));
}

static bool observe(uint8_t id, int64_t time_us, int8_t rssi,
                    uint8_t subtype, wifi_ssid_kind_t ssid_kind,
                    const char *ssid, uint8_t rx_channel,
                    bool advertised, uint8_t advertised_channel,
                    wifi_parse_status_t status, wifi_parse_status_t rsn_status)
{
    wifi_frame_info_t frame = {
        .type = WIFI_FRAME_TYPE_MGMT,
        .subtype = subtype,
        .rssi = rssi,
        .channel = rx_channel,
        .has_bssid = true,
    };
    bssid(frame.bssid, id);
    wifi_mgmt_info_t management = {
        .status = status,
        .subtype = subtype,
        .rx_channel = rx_channel,
        .ssid_kind = ssid_kind,
        .has_advertised_channel = advertised,
        .advertised_channel = advertised_channel,
        .rsn_ie_seen = rsn_status != WIFI_PARSE_UNSUPPORTED,
        .rsn = {
            .status = rsn_status,
            .version = 1,
            .group_cipher = {.kind = WIFI_CIPHER_CCMP_128},
            .pairwise_count = 1,
            .pairwise = {{.kind = WIFI_CIPHER_CCMP_128}},
            .akm_count = 1,
            .akms = {{.kind = WIFI_AKM_PSK}},
            .pmf = WIFI_PMF_CAPABLE,
        },
    };
    if (ssid_kind == WIFI_SSID_NORMAL) {
        management.ssid_raw_length = (uint8_t)strlen(ssid);
        memcpy(management.ssid, ssid, management.ssid_raw_length);
    } else if (ssid_kind == WIFI_SSID_ALL_ZERO) {
        management.ssid_raw_length = 4U;
    }
    wifi_ap_observation_t observation = {
        .frame = &frame,
        .management = &management,
        .timestamp_us = time_us,
    };
    return wifi_ap_tracker_core_observe(&state, &observation);
}

static const wifi_tracked_ap_t *find(const wifi_ap_tracker_snapshot_t *snapshot,
                                    uint8_t id)
{
    uint8_t expected[6];
    bssid(expected, id);
    for (uint16_t i = 0; i < snapshot->count; ++i)
        if (memcmp(snapshot->records[i].bssid, expected, 6U) == 0)
            return &snapshot->records[i];
    return NULL;
}

static void test_create_update_identity_hidden_and_rssi(void)
{
    wifi_ap_tracker_core_init(&state);
    assert(observe(1, 100, -70, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_EMPTY, "", 6, true, 6,
                   WIFI_PARSE_VALID, WIFI_PARSE_VALID));
    wifi_ap_tracker_core_snapshot(&state, &snapshot_a);
    assert(snapshot_a.count == 1);
    assert(find(&snapshot_a, 1)->ssid_state == WIFI_SSID_EMPTY);
    assert(observe(4, 150, -65, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_ALL_ZERO, "", 6, true, 6,
                   WIFI_PARSE_VALID, WIFI_PARSE_UNSUPPORTED));

    assert(observe(1, 200, -50, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_NORMAL, "Lab", 6, true, 6,
                   WIFI_PARSE_VALID, WIFI_PARSE_VALID));
    assert(observe(1, 300, -60, WIFI_FRAME_SUBTYPE_PROBE_RESP,
                   WIFI_SSID_NORMAL, "Lab", 6, true, 6,
                   WIFI_PARSE_VALID, WIFI_PARSE_VALID));
    assert(observe(2, 250, -80, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_NORMAL, "Lab", 11, true, 11,
                   WIFI_PARSE_VALID, WIFI_PARSE_VALID));
    wifi_ap_tracker_core_snapshot(&state, &snapshot_a);
    assert(snapshot_a.count == 3);
    assert(find(&snapshot_a, 4)->ssid_state == WIFI_SSID_ALL_ZERO);
    const wifi_tracked_ap_t *one = find(&snapshot_a, 1);
    assert(one != NULL && one->frame_count == 3 && one->beacon_count == 2);
    assert(one->probe_response_count == 1 && one->last_seen_us == 300);
    assert(one->ssid_state == WIFI_SSID_NORMAL && strcmp(one->ssid, "Lab") == 0);
    assert(one->rssi_last == -60 && one->rssi_min == -70 && one->rssi_max == -50);
    assert(wifi_ap_record_average_rssi(one) == -60);
}

static void test_quality_and_channel_mismatch(void)
{
    wifi_ap_tracker_core_init(&state);
    assert(observe(3, 100, -40, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_NORMAL, "Secure", 1, true, 6,
                   WIFI_PARSE_VALID, WIFI_PARSE_VALID));
    assert(observe(3, 200, -41, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_EMPTY, "", 1, false, 0,
                   WIFI_PARSE_VALID, WIFI_PARSE_MALFORMED));
    assert(observe(3, 300, -42, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_ABSENT, "", 1, false, 0,
                   WIFI_PARSE_PARTIAL, WIFI_PARSE_UNSUPPORTED));
    wifi_ap_tracker_core_snapshot(&state, &snapshot_a);
    const wifi_tracked_ap_t *record = find(&snapshot_a, 3);
    assert(record != NULL && strcmp(record->ssid, "Secure") == 0);
    assert(record->rsn_present && record->rsn.status == WIFI_PARSE_VALID);
    assert(record->rx_channel == 1 && record->advertised_channel == 6);
    assert(record->channel_mismatch && record->channel_mismatch_count == 3);
    assert(snapshot_a.diagnostics.channel_mismatch_count == 3);
}

static void test_capacity_lru_snapshot_and_reset(void)
{
    wifi_ap_tracker_core_init(&state);
    for (uint8_t i = 0; i < WIFI_AP_TRACKER_CAPACITY; ++i) {
        assert(observe(i, (int64_t)i + 1, -60, WIFI_FRAME_SUBTYPE_BEACON,
                       WIFI_SSID_NORMAL, "AP", 1, true, 1,
                       WIFI_PARSE_VALID, WIFI_PARSE_UNSUPPORTED));
    }
    wifi_ap_tracker_core_snapshot(&state, &snapshot_a);
    assert(snapshot_a.count == WIFI_AP_TRACKER_CAPACITY);
    assert(snapshot_a.diagnostics.table_high_watermark == WIFI_AP_TRACKER_CAPACITY);
    assert(observe(200, 1000, -30, WIFI_FRAME_SUBTYPE_BEACON,
                   WIFI_SSID_NORMAL, "New", 1, true, 1,
                   WIFI_PARSE_VALID, WIFI_PARSE_UNSUPPORTED));
    wifi_ap_tracker_core_snapshot(&state, &snapshot_b);
    assert(snapshot_b.count == WIFI_AP_TRACKER_CAPACITY);
    assert(snapshot_b.diagnostics.ap_evicted == 1);
    assert(find(&snapshot_b, 0) == NULL && find(&snapshot_b, 200) != NULL);
    assert(find(&snapshot_a, 0) != NULL && find(&snapshot_a, 200) == NULL);
    const uint32_t before_reset = snapshot_b.generation;
    wifi_ap_tracker_core_reset(&state);
    wifi_ap_tracker_core_snapshot(&state, &snapshot_b);
    assert(snapshot_b.count == 0 && snapshot_b.diagnostics.ap_created == 0);
    assert(snapshot_b.diagnostics.table_capacity == WIFI_AP_TRACKER_CAPACITY);
    assert(snapshot_b.generation != before_reset);
}

static void test_invalid_inputs(void)
{
    wifi_ap_tracker_core_init(&state);
    wifi_frame_info_t frame = {
        .type = WIFI_FRAME_TYPE_MGMT,
        .subtype = WIFI_FRAME_SUBTYPE_BEACON,
        .has_bssid = true,
        .channel = 1,
    };
    memset(frame.bssid, 0xff, sizeof(frame.bssid));
    wifi_mgmt_info_t management = {
        .status = WIFI_PARSE_VALID,
        .subtype = WIFI_FRAME_SUBTYPE_BEACON,
        .rx_channel = 1,
        .ssid_kind = WIFI_SSID_ABSENT,
    };
    wifi_ap_observation_t observation = {
        .frame = &frame, .management = &management, .timestamp_us = 1,
    };
    assert(!wifi_ap_tracker_core_observe(&state, &observation));
    assert(!observe(1, 2, -50, WIFI_FRAME_SUBTYPE_PROBE_REQ,
                    WIFI_SSID_NORMAL, "Client", 1, false, 0,
                    WIFI_PARSE_VALID, WIFI_PARSE_UNSUPPORTED));
    assert(!observe(2, 3, -50, WIFI_FRAME_SUBTYPE_BEACON,
                    WIFI_SSID_ABSENT, "", 1, false, 0,
                    WIFI_PARSE_MALFORMED, WIFI_PARSE_UNSUPPORTED));
    wifi_ap_tracker_core_snapshot(&state, &snapshot_a);
    assert(snapshot_a.count == 0 && snapshot_a.diagnostics.malformed_ignored == 3);
}

int main(void)
{
    test_create_update_identity_hidden_and_rssi();
    test_quality_and_channel_mismatch();
    test_capacity_lru_snapshot_and_reset();
    test_invalid_inputs();
    return 0;
}
