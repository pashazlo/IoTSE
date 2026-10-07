#include "wifi_eapol_tracker_internal.h"

#include <limits.h>
#include <string.h>

#define ALL_MESSAGES_MASK 0x0FU

_Static_assert(sizeof(wifi_eapol_tracker_state_t) == 13952U,
               "Update the documented EAPOL tracker PSRAM budget");
_Static_assert(sizeof(wifi_eapol_tracker_snapshot_t) == 8840U,
               "Update the documented EAPOL snapshot size");

static void inc64(uint64_t *value) { if (*value != UINT64_MAX) ++*value; }
static void inc32(uint32_t *value) { if (*value != UINT32_MAX) ++*value; }
static uint32_t next_generation(uint32_t value)
{
    ++value;
    return value == 0U ? 1U : value;
}
static bool same_mac(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}
static bool valid_unicast(const uint8_t mac[6])
{
    bool nonzero = false;
    if ((mac[0] & 1U) != 0U) return false;
    for (unsigned i = 0; i < 6; ++i) nonzero |= mac[i] != 0U;
    return nonzero;
}
static int find_pair(const wifi_eapol_tracker_state_t *state,
                     const uint8_t bssid[6], const uint8_t sta[6])
{
    for (unsigned i = 0; i < WIFI_EAPOL_TRACKER_CAPACITY; ++i)
        if (state->slots[i].occupied &&
            same_mac(state->slots[i].session.bssid, bssid) &&
            same_mac(state->slots[i].session.sta, sta)) return (int)i;
    return -1;
}
static unsigned select_slot(const wifi_eapol_tracker_state_t *state,
                            bool *evicted)
{
    for (unsigned i = 0; i < WIFI_EAPOL_TRACKER_CAPACITY; ++i)
        if (!state->slots[i].occupied) { *evicted = false; return i; }
    unsigned oldest = 0;
    for (unsigned i = 1; i < WIFI_EAPOL_TRACKER_CAPACITY; ++i)
        if (state->slots[i].session.last_seen_us <
            state->slots[oldest].session.last_seen_us) oldest = i;
    *evicted = true;
    return oldest;
}
static unsigned message_index(wifi_eapol_message_t message)
{
    return (unsigned)message - (unsigned)WIFI_EAPOL_MESSAGE_M1;
}
static void begin_exchange(wifi_eapol_slot_t *slot, bool restart)
{
    if (restart) inc32(&slot->session.exchange_restart_count);
    slot->session.observed_mask = 0;
    slot->session.state = WIFI_EAPOL_SESSION_EMPTY;
    memset(slot->session.message_replay_counter, 0,
           sizeof(slot->session.message_replay_counter));
    slot->replay_valid_mask = 0;
    memset(slot->m1_nonce, 0, sizeof(slot->m1_nonce));
    memset(slot->m3_nonce, 0, sizeof(slot->m3_nonce));
    slot->session.exchange_generation =
        next_generation(slot->session.exchange_generation);
}
static bool is_duplicate(const wifi_eapol_slot_t *slot,
                         wifi_eapol_message_t message,
                         const wifi_eapol_info_t *key)
{
    unsigned index = message_index(message);
    uint8_t bit = (uint8_t)(1U << index);
    if ((slot->replay_valid_mask & bit) == 0U ||
        slot->session.message_replay_counter[index] != key->replay_counter)
        return false;
    if (message == WIFI_EAPOL_MESSAGE_M1)
        return memcmp(slot->m1_nonce, key->nonce, sizeof(key->nonce)) == 0;
    if (message == WIFI_EAPOL_MESSAGE_M3)
        return memcmp(slot->m3_nonce, key->nonce, sizeof(key->nonce)) == 0;
    return true;
}
static bool coherent_complete(const wifi_eapol_slot_t *slot)
{
    if (slot->session.observed_mask != ALL_MESSAGES_MASK) return false;
    uint64_t base = slot->session.message_replay_counter[0];
    if (base == UINT64_MAX) return false;
    return slot->session.message_replay_counter[1] == base &&
           slot->session.message_replay_counter[2] == base + 1U &&
           slot->session.message_replay_counter[3] == base + 1U;
}

void wifi_eapol_tracker_core_init(wifi_eapol_tracker_state_t *state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->generation = 1U;
    state->diagnostics.table_capacity = WIFI_EAPOL_TRACKER_CAPACITY;
}

void wifi_eapol_tracker_core_reset(wifi_eapol_tracker_state_t *state)
{
    if (state == NULL) return;
    uint32_t generation = next_generation(state->generation);
    memset(state, 0, sizeof(*state));
    state->generation = generation;
    state->diagnostics.table_capacity = WIFI_EAPOL_TRACKER_CAPACITY;
}

bool wifi_eapol_tracker_core_observe(
    wifi_eapol_tracker_state_t *state, const wifi_eapol_observation_t *o)
{
    if (state == NULL || o == NULL || o->eapol == NULL) return false;
    inc64(&state->diagnostics.eapol_frames_seen);
    if (o->timestamp_us <= 0 || !valid_unicast(o->bssid) ||
        !valid_unicast(o->sta) || o->direction == WIFI_FRAME_DIRECTION_UNKNOWN) {
        inc64(&state->diagnostics.unsupported_identity);
        return false;
    }

    int found = find_pair(state, o->bssid, o->sta);
    bool evicted = false;
    unsigned index = found >= 0 ? (unsigned)found : select_slot(state, &evicted);
    wifi_eapol_slot_t *slot = &state->slots[index];
    if (found < 0) {
        if (evicted) inc64(&state->diagnostics.sessions_evicted);
        else {
            ++state->diagnostics.table_count;
            if (state->diagnostics.table_count >
                state->diagnostics.table_high_watermark)
                state->diagnostics.table_high_watermark =
                    state->diagnostics.table_count;
        }
        memset(slot, 0, sizeof(*slot));
        slot->occupied = true;
        memcpy(slot->session.bssid, o->bssid, 6);
        memcpy(slot->session.sta, o->sta, 6);
        slot->session.first_seen_us = o->timestamp_us;
        inc64(&state->diagnostics.sessions_created);
    }
    wifi_eapol_session_t *session = &slot->session;
    if (o->timestamp_us > session->last_seen_us)
        session->last_seen_us = o->timestamp_us;
    session->channel = o->channel;
    session->rssi_last = o->rssi;
    inc32(&session->eapol_frame_count);

    const wifi_eapol_info_t *key = o->eapol;
    if (key->status != WIFI_EAPOL_PARSE_OK) {
        inc32(&session->malformed_count);
        inc64(&state->diagnostics.eapol_parse_malformed);
    } else {
        inc64(&state->diagnostics.eapol_parse_ok);
        if (key->is_key) {
            inc32(&session->eapol_key_count);
            inc64(&state->diagnostics.eapol_key_frames);
        }
    }

    wifi_eapol_message_t message = o->message;
    wifi_eapol_message_t verified = wifi_eapol_classify(key, o->direction);
    if (message < WIFI_EAPOL_MESSAGE_UNKNOWN ||
        message > WIFI_EAPOL_MESSAGE_M4 || message != verified)
        message = WIFI_EAPOL_MESSAGE_UNKNOWN;
    if (key->status != WIFI_EAPOL_PARSE_OK || !key->is_key ||
        message == WIFI_EAPOL_MESSAGE_UNKNOWN) {
        if (key->status == WIFI_EAPOL_PARSE_OK && key->is_key) {
            inc32(&session->unknown_classification_count);
            inc64(&state->diagnostics.classified_unknown);
        }
        state->generation = next_generation(state->generation);
        session->update_generation = state->generation;
        return true;
    }

    unsigned mi = message_index(message);
    bool duplicate = is_duplicate(slot, message, key);
    if (duplicate) {
        inc32(&session->retransmission_count);
        inc64(&state->diagnostics.retransmissions_observed);
    } else {
        bool restart = false;
        bool incoherent = false;
        if (message == WIFI_EAPOL_MESSAGE_M1 && session->observed_mask != 0U)
            restart = true;
        else if (message == WIFI_EAPOL_MESSAGE_M2 &&
                 (slot->replay_valid_mask & 0x01U) != 0U &&
                 key->replay_counter != session->message_replay_counter[0])
            incoherent = true;
        else if ((message == WIFI_EAPOL_MESSAGE_M3 ||
                  message == WIFI_EAPOL_MESSAGE_M4) &&
                 (slot->replay_valid_mask & 0x01U) != 0U &&
                 (session->message_replay_counter[0] == UINT64_MAX ||
                  key->replay_counter !=
                      session->message_replay_counter[0] + 1U))
            incoherent = true;
        if (incoherent) {
            inc32(&session->unknown_classification_count);
            inc64(&state->diagnostics.classified_unknown);
            state->generation = next_generation(state->generation);
            session->update_generation = state->generation;
            return true;
        }
        if (restart) {
            begin_exchange(slot, true);
            inc64(&state->diagnostics.exchange_restarts);
        } else if (session->exchange_generation == 0U) {
            begin_exchange(slot, false);
        }
        session->message_replay_counter[mi] = key->replay_counter;
        slot->replay_valid_mask |= (uint8_t)(1U << mi);
        session->observed_mask |= (uint8_t)(1U << mi);
        if (message == WIFI_EAPOL_MESSAGE_M1)
            memcpy(slot->m1_nonce, key->nonce, sizeof(slot->m1_nonce));
        if (message == WIFI_EAPOL_MESSAGE_M3)
            memcpy(slot->m3_nonce, key->nonce, sizeof(slot->m3_nonce));
    }
    inc32(&session->message_count[mi]);
    session->last_replay_counter = key->replay_counter;
    uint64_t *classified[] = {
        &state->diagnostics.classified_m1,
        &state->diagnostics.classified_m2,
        &state->diagnostics.classified_m3,
        &state->diagnostics.classified_m4,
    };
    inc64(classified[mi]);
    session->state = coherent_complete(slot) ? WIFI_EAPOL_SESSION_COMPLETE
                                              : WIFI_EAPOL_SESSION_PARTIAL;
    state->generation = next_generation(state->generation);
    session->update_generation = state->generation;
    return true;
}

void wifi_eapol_tracker_core_snapshot(
    const wifi_eapol_tracker_state_t *state,
    wifi_eapol_tracker_snapshot_t *snapshot)
{
    if (state == NULL || snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = state->generation;
    snapshot->diagnostics = state->diagnostics;
    for (unsigned i = 0; i < WIFI_EAPOL_TRACKER_CAPACITY; ++i)
        if (state->slots[i].occupied)
            snapshot->sessions[snapshot->count++] = state->slots[i].session;
}
