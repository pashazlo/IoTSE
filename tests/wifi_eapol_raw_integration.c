#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wifi_eapol_tracker_internal.h"

#define AP_MAC  {0x02,0x11,0x22,0x33,0x44,0x55}
#define STA1_MAC {0x02,0xAA,0xBB,0xCC,0xDD,0x01}
#define STA2_MAC {0x02,0xAA,0xBB,0xCC,0xDD,0x02}

static const uint8_t AP[6] = AP_MAC;
static const uint8_t STA1[6] = STA1_MAC;
static const uint8_t STA2[6] = STA2_MAC;

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}
static void put_be64(uint8_t *p, uint64_t value)
{
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)value; value >>= 8; }
}

static size_t make_frame(uint8_t *out, bool ap_to_sta, bool qos,
                         uint8_t eapol_type, uint8_t descriptor,
                         uint16_t key_info, uint64_t replay,
                         uint8_t nonce_seed)
{
    memset(out, 0, 256);
    uint16_t fc = qos ? 0x0088U : 0x0008U;
    fc |= ap_to_sta ? 0x0200U : 0x0100U;
    out[0] = (uint8_t)fc; out[1] = (uint8_t)(fc >> 8);
    memcpy(out + 4, ap_to_sta ? STA1 : AP, 6);
    memcpy(out + 10, ap_to_sta ? AP : STA1, 6);
    memcpy(out + 16, AP, 6);
    size_t offset = qos ? 26U : 24U;
    static const uint8_t snap[8] = {0xAA,0xAA,0x03,0,0,0,0x88,0x8E};
    memcpy(out + offset, snap, sizeof(snap)); offset += sizeof(snap);
    out[offset] = 2; out[offset + 1] = eapol_type;
    if (eapol_type != 3U) {
        put_be16(out + offset + 2, 0);
        return offset + 4U;
    }
    put_be16(out + offset + 2, 95);
    uint8_t *key = out + offset + 4U;
    key[0] = descriptor;
    put_be16(key + 1, key_info);
    put_be16(key + 3, 16);
    put_be64(key + 5, replay);
    for (unsigned i = 0; i < 32; ++i) key[13 + i] = (uint8_t)(nonce_seed + i);
    put_be16(key + 93, 0);
    return offset + 4U + 95U;
}

static bool feed(wifi_eapol_tracker_state_t *state, const uint8_t *raw,
                 size_t length, int64_t timestamp,
                 wifi_eapol_message_t *message_out)
{
    wifi_frame_info_t frame;
    if (!wifi_frame_parse(raw, (uint16_t)length, -45, 6, &frame) ||
        !frame.is_eapol) return false;
    assert((size_t)frame.eapol_offset + frame.eapol_length == length);
    wifi_eapol_info_t eapol;
    (void)wifi_eapol_parse(raw + frame.eapol_offset, frame.eapol_length,
                           &eapol);
    uint8_t bssid[6], sta[6]; wifi_frame_direction_t direction;
    if (!wifi_frame_get_infrastructure_roles(&frame, bssid, sta, &direction))
        return false;
    wifi_eapol_message_t message = wifi_eapol_classify(&eapol, direction);
    wifi_eapol_observation_t observation = {
        .direction = direction, .channel = frame.channel, .rssi = frame.rssi,
        .timestamp_us = timestamp, .eapol = &eapol, .message = message,
    };
    memcpy(observation.bssid, bssid, 6); memcpy(observation.sta, sta, 6);
    if (message_out != NULL) *message_out = message;
    return wifi_eapol_tracker_core_observe(state, &observation);
}

static wifi_eapol_session_t only_session(wifi_eapol_tracker_state_t *state)
{
    wifi_eapol_tracker_snapshot_t snapshot;
    wifi_eapol_tracker_core_snapshot(state, &snapshot);
    assert(snapshot.count == 1);
    return snapshot.sessions[0];
}

static void set_sta(uint8_t *raw, bool ap_to_sta, const uint8_t sta[6])
{
    memcpy(raw + (ap_to_sta ? 4U : 10U), sta, 6);
}

static void test_messages_and_sessions(void)
{
    uint8_t raw[256]; wifi_eapol_message_t message;
    wifi_eapol_tracker_state_t state;
    wifi_eapol_tracker_core_init(&state);
    const uint16_t infos[4] = {0x008A, 0x010A, 0x03CA, 0x030A};
    const bool ap_to_sta[4] = {true, false, true, false};
    for (unsigned i = 0; i < 4; ++i) {
        size_t n = make_frame(raw, ap_to_sta[i], false, 3, 2, infos[i],
                              i < 2 ? 10 : 11, (uint8_t)(20 + i));
        assert(feed(&state, raw, n, 100 + i, &message));
        assert(message == (wifi_eapol_message_t)(WIFI_EAPOL_MESSAGE_M1 + i));
    }
    wifi_eapol_session_t session = only_session(&state);
    assert(session.state == WIFI_EAPOL_SESSION_COMPLETE);
    assert(session.observed_mask == 0x0F);

    /* 5/6: normal retransmitted M1 and M3 are counted, not state-corrupting. */
    size_t n = make_frame(raw, true, false, 3, 2, infos[2], 11, 22);
    assert(feed(&state, raw, n, 200, NULL));
    n = make_frame(raw, true, false, 3, 2, infos[0], 10, 20);
    assert(feed(&state, raw, n, 201, NULL));
    session = only_session(&state);
    assert(session.retransmission_count == 2);
    assert(session.state == WIFI_EAPOL_SESSION_COMPLETE);

    /* 7/8: capture may begin at M2 or M3 and remains PARTIAL. */
    wifi_eapol_tracker_core_reset(&state);
    n = make_frame(raw, false, false, 3, 2, infos[1], 30, 1);
    assert(feed(&state, raw, n, 300, NULL));
    assert(only_session(&state).state == WIFI_EAPOL_SESSION_PARTIAL);
    wifi_eapol_tracker_core_reset(&state);
    n = make_frame(raw, true, false, 3, 2, infos[2], 31, 2);
    assert(feed(&state, raw, n, 301, NULL));
    assert(only_session(&state).state == WIFI_EAPOL_SESSION_PARTIAL);

    /* 9/10: missing M2 or M4 can never become COMPLETE. */
    wifi_eapol_tracker_core_reset(&state);
    for (unsigned i = 0; i < 4; ++i) if (i != 1) {
        n = make_frame(raw, ap_to_sta[i], false, 3, 2, infos[i],
                       i < 2 ? 40 : 41, (uint8_t)i);
        assert(feed(&state, raw, n, 400 + i, NULL));
    }
    assert(only_session(&state).state == WIFI_EAPOL_SESSION_PARTIAL);
    wifi_eapol_tracker_core_reset(&state);
    for (unsigned i = 0; i < 3; ++i) {
        n = make_frame(raw, ap_to_sta[i], false, 3, 2, infos[i],
                       i < 2 ? 50 : 51, (uint8_t)i);
        assert(feed(&state, raw, n, 500 + i, NULL));
    }
    assert(only_session(&state).state == WIFI_EAPOL_SESSION_PARTIAL);

    /* 11/14: a different M1 replay/nonce restarts the bounded exchange. */
    n = make_frame(raw, true, false, 3, 2, infos[0], 60, 90);
    assert(feed(&state, raw, n, 600, NULL));
    session = only_session(&state);
    assert(session.observed_mask == 1 && session.exchange_restart_count == 1);

    /* 11: a second complete exchange replaces, but never mixes with, first. */
    wifi_eapol_tracker_core_reset(&state);
    for (unsigned exchange = 0; exchange < 2; ++exchange)
        for (unsigned i = 0; i < 4; ++i) {
            uint64_t base = 100U + exchange * 10U;
            n = make_frame(raw, ap_to_sta[i], false, 3, 2, infos[i],
                           i < 2 ? base : base + 1U,
                           (uint8_t)(exchange * 20U + i));
            assert(feed(&state, raw, n, 700 + exchange * 10 + i, NULL));
        }
    session = only_session(&state);
    assert(session.state == WIFI_EAPOL_SESSION_COMPLETE);
    assert(session.exchange_restart_count == 1U);
    assert(session.message_replay_counter[0] == 110U);
    assert(session.message_replay_counter[3] == 111U);
}

static void test_identity_and_non_key(void)
{
    uint8_t raw[256]; wifi_eapol_tracker_state_t state;
    wifi_eapol_tracker_core_init(&state);
    /* 12: two STA exchanges on one AP are truly interleaved and independent. */
    const uint16_t infos[4] = {0x008A, 0x010A, 0x03CA, 0x030A};
    const bool directions[4] = {true, false, true, false};
    int64_t timestamp = 1;
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned station = 0; station < 2; ++station) {
            size_t n = make_frame(raw, directions[i], false, 3, 2, infos[i],
                                  i < 2 ? 10U + station : 11U + station,
                                  (uint8_t)(station * 32U + i));
            set_sta(raw, directions[i], station == 0 ? STA1 : STA2);
            assert(feed(&state, raw, n, timestamp++, NULL));
        }
    }
    wifi_eapol_tracker_snapshot_t snapshot;
    wifi_eapol_tracker_core_snapshot(&state, &snapshot);
    assert(snapshot.count == 2);
    for (unsigned i = 0; i < 2; ++i) {
        assert(snapshot.sessions[i].state == WIFI_EAPOL_SESSION_COMPLETE);
        assert(snapshot.sessions[i].observed_mask == 0x0FU);
    }

    /* 13: same STA on another BSSID is another pair. */
    size_t n = make_frame(raw, true, false, 3, 2, 0x008A, 30, 1);
    uint8_t ap2[6] = {0x02,1,2,3,4,6};
    memcpy(raw + 4, STA1, 6); memcpy(raw + 10, ap2, 6);
    assert(feed(&state, raw, n, timestamp++, NULL));
    wifi_eapol_tracker_core_snapshot(&state, &snapshot);
    assert(snapshot.count == 3);
    bool found_ap2 = false;
    for (unsigned i = 0; i < snapshot.count; ++i)
        if (memcmp(snapshot.sessions[i].bssid, ap2, 6) == 0 &&
            memcmp(snapshot.sessions[i].sta, STA1, 6) == 0) found_ap2 = true;
    assert(found_ap2);

    /* 15: ordinary DATA has no EAPOL range. */
    memset(raw, 0, sizeof(raw)); raw[0] = 0x08; raw[1] = 0x02;
    wifi_frame_info_t frame;
    assert(wifi_frame_parse(raw, 32, -1, 1, &frame));
    assert(!frame.is_eapol && frame.eapol_length == 0);

    /* 16/17: non-Key and unknown EAPOL types parse but do not classify. */
    n = make_frame(raw, true, false, 0, 0, 0, 0, 0);
    assert(feed(&state, raw, n, 4, NULL));
    n = make_frame(raw, true, false, 99, 0, 0, 0, 0);
    assert(feed(&state, raw, n, 5, NULL));

    /* 18/19: unsupported descriptor and conflicting flags stay UNKNOWN. */
    wifi_eapol_message_t message;
    n = make_frame(raw, true, false, 3, 9, 0x008A, 2, 1);
    assert(feed(&state, raw, n, 6, &message)); assert(message == WIFI_EAPOL_MESSAGE_UNKNOWN);
    n = make_frame(raw, true, false, 3, 2, 0x018A, 2, 1);
    assert(feed(&state, raw, n, 7, &message)); assert(message == WIFI_EAPOL_MESSAGE_UNKNOWN);

    /* 20: QoS header offset is owned by the frame parser. */
    n = make_frame(raw, true, true, 3, 2, 0x008A, 3, 2);
    assert(feed(&state, raw, n, 8, &message)); assert(message == WIFI_EAPOL_MESSAGE_M1);
}

static void test_malformed_and_prefixes(void)
{
    uint8_t raw[256];
    size_t full = make_frame(raw, true, false, 3, 2, 0x008A, 9, 7);
    uint8_t fixtures[9][256]; size_t lengths[9];
    const uint16_t infos[4] = {0x008A, 0x010A, 0x03CA, 0x030A};
    const bool directions[4] = {true, false, true, false};
    for (unsigned i = 0; i < 4; ++i)
        lengths[i] = make_frame(fixtures[i], directions[i], false, 3, 2,
                                infos[i], i < 2 ? 20 : 21, (uint8_t)i);
    lengths[4] = make_frame(fixtures[4], true, true, 3, 2, 0x008A, 20, 1);
    lengths[5] = make_frame(fixtures[5], true, false, 0, 0, 0, 0, 0);
    lengths[6] = make_frame(fixtures[6], true, false, 99, 0, 0, 0, 0);
    lengths[7] = make_frame(fixtures[7], true, false, 3, 1, 0x008A, 20, 1);
    lengths[8] = make_frame(fixtures[8], true, false, 3, 2, 0x018A, 20, 1);
    for (unsigned fixture = 0; fixture < 9; ++fixture)
    for (size_t prefix = 0; prefix <= lengths[fixture]; ++prefix) {
        wifi_frame_info_t a, b;
        bool pa = wifi_frame_parse(fixtures[fixture], (uint16_t)prefix,
                                   -50, 6, &a);
        bool pb = wifi_frame_parse(fixtures[fixture], (uint16_t)prefix,
                                   -50, 6, &b);
        assert(pa == pb && memcmp(&a, &b, sizeof(a)) == 0);
        if (pa && a.is_eapol) {
            wifi_eapol_info_t ea, eb;
            wifi_eapol_parse(fixtures[fixture] + a.eapol_offset,
                             a.eapol_length, &ea);
            wifi_eapol_parse(fixtures[fixture] + b.eapol_offset,
                             b.eapol_length, &eb);
            assert(memcmp(&ea, &eb, sizeof(ea)) == 0);
            if (prefix < lengths[fixture])
                assert(ea.status == WIFI_EAPOL_PARSE_MALFORMED);
            wifi_eapol_tracker_state_t state;
            wifi_eapol_tracker_core_init(&state);
            assert(feed(&state, fixtures[fixture], prefix,
                        1000 + (int64_t)prefix, NULL));
            assert(state.diagnostics.table_count <= WIFI_EAPOL_TRACKER_CAPACITY);
            if (ea.status == WIFI_EAPOL_PARSE_MALFORMED) {
                wifi_eapol_session_t session = only_session(&state);
                assert(session.observed_mask == 0U);
                assert(session.state == WIFI_EAPOL_SESSION_EMPTY);
                assert(session.malformed_count == 1U);
            }
        }
    }
    /* 21/22 are covered by prefixes; direct checks document boundaries. */
    wifi_frame_info_t frame;
    assert(!wifi_frame_parse(raw, 23, -1, 1, &frame));
    assert(wifi_frame_parse(raw, 31, -1, 1, &frame) && !frame.is_eapol);

    /* 23: truncated EAPOL header. */
    assert(wifi_frame_parse(raw, 35, -1, 1, &frame) && frame.is_eapol);
    wifi_eapol_info_t eapol;
    assert(wifi_eapol_parse(raw + frame.eapol_offset, frame.eapol_length,
                            &eapol) == WIFI_EAPOL_PARSE_MALFORMED);
    /* 24: declared body exceeds capture. */
    size_t offset = frame.eapol_offset; put_be16(raw + offset + 2, 200);
    assert(wifi_eapol_parse(raw + offset, full - offset, &eapol) ==
           WIFI_EAPOL_PARSE_MALFORMED);
    /* 25: fixed Key fields truncated. */
    put_be16(raw + offset + 2, 95);
    assert(wifi_eapol_parse(raw + offset, 4 + 94, &eapol) ==
           WIFI_EAPOL_PARSE_MALFORMED);
    /* 26: key_data_length exceeds declared Key body. */
    put_be16(raw + offset + 4 + 93, 1);
    assert(wifi_eapol_parse(raw + offset, full - offset, &eapol) ==
           WIFI_EAPOL_PARSE_MALFORMED);

    /* Invalid public enum cannot index tracker arrays; tracker reclassifies. */
    full = make_frame(raw, true, false, 3, 2, 0x008A, 70, 1);
    assert(wifi_frame_parse(raw, (uint16_t)full, -40, 6, &frame));
    assert(wifi_eapol_parse(raw + frame.eapol_offset, frame.eapol_length,
                            &eapol) == WIFI_EAPOL_PARSE_OK);
    wifi_eapol_tracker_state_t safe;
    wifi_eapol_tracker_core_init(&safe);
    wifi_eapol_observation_t bad = {
        .bssid = {0x02,1,2,3,4,5}, .sta = {0x02,6,7,8,9,10},
        .direction = WIFI_FRAME_DIRECTION_AP_TO_STA, .channel = 6,
        .rssi = -40, .timestamp_us = 1, .eapol = &eapol,
        .message = (wifi_eapol_message_t)99,
    };
    assert(wifi_eapol_tracker_core_observe(&safe, &bad));
    assert(only_session(&safe).observed_mask == 0U);

    /* Delayed old M3 must not erase a newer M1 exchange. */
    wifi_eapol_tracker_core_reset(&safe);
    full = make_frame(raw, true, false, 3, 2, 0x008A, 80, 3);
    assert(feed(&safe, raw, full, 10, NULL));
    full = make_frame(raw, true, false, 3, 2, 0x03CA, 71, 4);
    assert(feed(&safe, raw, full, 11, NULL));
    wifi_eapol_session_t session = only_session(&safe);
    assert(session.observed_mask == 1U);
    assert(session.message_replay_counter[0] == 80U);
}

int main(void)
{
    test_messages_and_sessions();
    test_identity_and_non_key();
    test_malformed_and_prefixes();
    printf("wifi_eapol_raw_integration: PASS (26 cases + exhaustive prefixes)\n");
    return 0;
}
