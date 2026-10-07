#include "wifi_promiscuous.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

typedef struct {
    uint16_t original_length;
    uint16_t captured_length;
    int8_t rssi;
    uint8_t channel;
    uint8_t legacy_rate;
    uint8_t sig_mode;
    uint8_t mcs;
    bool bandwidth_40mhz;
    bool short_gi;
    wifi_promiscuous_pkt_type_t type;
    wifi_target_diag_kind_t target_diag;
    uint8_t payload[WIFI_PROMISCUOUS_CAPTURE_MAX_LEN];
} ingress_slot_t;

_Static_assert(sizeof(ingress_slot_t) == 4112U,
               "Update the documented ingress PSRAM budget");

/* The free queue owns FREE slots; the ready queue owns RX-complete slots.
 * A dequeued ready slot is ANALYZER_OWNED until wifi_promiscuous_release(). */
static _Atomic(QueueHandle_t) s_ready_queue;
static _Atomic(QueueHandle_t) s_free_queue;
static ingress_slot_t *s_pool;
static atomic_uint_fast64_t s_enqueued;
static atomic_uint_fast32_t s_dropped_full;
static atomic_uint_fast32_t s_dropped_oversize;
static atomic_uint_fast32_t s_dropped_invalid;
static atomic_uint_fast64_t s_rx_total;
static atomic_uint_fast32_t s_queue_high_water;

#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
enum { DIAG_AP = 0, DIAG_STA, DIAG_TARGETS };
typedef enum {
    DIAG_CB, DIAG_BEACON, DIAG_AUTH, DIAG_ASSOC, DIAG_DATA, DIAG_AMPDU,
    DIAG_INVALID, DIAG_OVERSIZE, DIAG_FULL, DIAG_ENQUEUED,
    DIAG_ANALYZED, DIAG_PARSE_FAIL, DIAG_CAPTURED, DIAG_CAPTURE_FAIL,
    DIAG_EXPORTED, DIAG_COUNTERS
} target_counter_t;
static atomic_uint_fast64_t s_target[DIAG_TARGETS][DIAG_COUNTERS];
static const uint8_t s_diag_ap[6] = {0xb0,0xa7,0xb9,0x86,0x5c,0x52};
static const uint8_t s_diag_sta[6] = {0x3c,0xcd,0x40,0xad,0x2f,0x2b};

static wifi_target_diag_kind_t classify_target(const wifi_promiscuous_pkt_t *p,
                                                wifi_promiscuous_pkt_type_t type)
{
    /* addr2 ends at byte 15 for MGMT/DATA. sig_len includes the four-byte FCS. */
    if ((type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) ||
        p->rx_ctrl.sig_len < 16U + WIFI_PROMISCUOUS_FCS_LEN) return WIFI_TARGET_DIAG_NONE;
    if (memcmp(p->payload + 10, s_diag_ap, 6) == 0) return WIFI_TARGET_DIAG_AP;
    if (memcmp(p->payload + 10, s_diag_sta, 6) == 0) return WIFI_TARGET_DIAG_STA;
    return WIFI_TARGET_DIAG_NONE;
}

static void count_target(wifi_target_diag_kind_t target, target_counter_t counter)
{
    if (target == WIFI_TARGET_DIAG_AP || target == WIFI_TARGET_DIAG_STA)
        atomic_fetch_add_explicit(&s_target[target - WIFI_TARGET_DIAG_AP][counter],
                                  1U, memory_order_relaxed);
}
#endif

static void update_queue_high_water(uint32_t value)
{
    uint_fast32_t current = atomic_load_explicit(
        &s_queue_high_water, memory_order_relaxed);
    while (value > current && !atomic_compare_exchange_weak_explicit(
        &s_queue_high_water, &current, value,
        memory_order_relaxed, memory_order_relaxed)) {}
}

esp_err_t wifi_promiscuous_init(void)
{
    if (atomic_load_explicit(&s_ready_queue, memory_order_acquire) != NULL)
        return ESP_OK;
    ingress_slot_t *pool = heap_caps_calloc(
        WIFI_PROMISCUOUS_POOL_SLOTS, sizeof(*pool),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    QueueHandle_t free_queue = xQueueCreate(WIFI_PROMISCUOUS_POOL_SLOTS,
                                            sizeof(uint8_t));
    QueueHandle_t ready_queue = xQueueCreate(WIFI_PROMISCUOUS_QUEUE_LEN,
                                             sizeof(uint8_t));
    if (pool == NULL || free_queue == NULL || ready_queue == NULL) {
        if (ready_queue != NULL) vQueueDelete(ready_queue);
        if (free_queue != NULL) vQueueDelete(free_queue);
        if (pool != NULL) heap_caps_free(pool);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < WIFI_PROMISCUOUS_POOL_SLOTS; ++i) {
        if (xQueueSend(free_queue, &i, 0) != pdTRUE) {
            vQueueDelete(ready_queue);
            vQueueDelete(free_queue);
            heap_caps_free(pool);
            return ESP_FAIL;
        }
    }
    s_pool = pool;
    atomic_store_explicit(&s_free_queue, free_queue, memory_order_release);
    atomic_store_explicit(&s_ready_queue, ready_queue, memory_order_release);
    atomic_store_explicit(&s_enqueued, 0, memory_order_release);
    atomic_store_explicit(&s_dropped_full, 0, memory_order_release);
    atomic_store_explicit(&s_dropped_oversize, 0, memory_order_release);
    atomic_store_explicit(&s_dropped_invalid, 0, memory_order_release);
    atomic_store_explicit(&s_rx_total, 0, memory_order_release);
    atomic_store_explicit(&s_queue_high_water, 0, memory_order_release);
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
    memset(s_target, 0, sizeof(s_target));
#endif
    return ESP_OK;
}

void wifi_promiscuous_reset(void)
{
    QueueHandle_t ready = atomic_load_explicit(
        &s_ready_queue, memory_order_acquire);
    QueueHandle_t free_queue = atomic_load_explicit(
        &s_free_queue, memory_order_acquire);
    uint8_t slot;
    /* Called with the radio producer stopped. Drain only READY ownership;
     * a slot already held by analyzer will return through release(). */
    while (ready != NULL && free_queue != NULL &&
           xQueueReceive(ready, &slot, 0) == pdTRUE) {
        (void)xQueueSend(free_queue, &slot, 0);
    }
    atomic_store_explicit(&s_enqueued, 0, memory_order_release);
    atomic_store_explicit(&s_dropped_full, 0, memory_order_release);
    atomic_store_explicit(&s_dropped_oversize, 0, memory_order_release);
    atomic_store_explicit(&s_dropped_invalid, 0, memory_order_release);
    atomic_store_explicit(&s_rx_total, 0, memory_order_release);
    atomic_store_explicit(&s_queue_high_water, 0, memory_order_release);
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
    memset(s_target, 0, sizeof(s_target));
#endif
}

void wifi_promiscuous_rx_cb(
    void *buffer,
    wifi_promiscuous_pkt_type_t type
)
{
    QueueHandle_t ready = atomic_load_explicit(
        &s_ready_queue, memory_order_acquire);
    QueueHandle_t free_queue = atomic_load_explicit(
        &s_free_queue, memory_order_acquire);
    if (buffer == NULL || ready == NULL || free_queue == NULL || s_pool == NULL)
        return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL)
        return;
    atomic_fetch_add_explicit(&s_rx_total, 1, memory_order_relaxed);

    const wifi_promiscuous_pkt_t *packet = buffer;
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
    wifi_target_diag_kind_t target = classify_target(packet, type);
    count_target(target, DIAG_CB);
    if (packet->rx_ctrl.aggregation != 0U) count_target(target, DIAG_AMPDU);
    if (target != WIFI_TARGET_DIAG_NONE && packet->rx_ctrl.sig_len >= 2U) {
        const uint16_t fc = (uint16_t)packet->payload[0] |
                            ((uint16_t)packet->payload[1] << 8);
        const uint8_t subtype = (uint8_t)((fc >> 4) & 0x0fU);
        if (type == WIFI_PKT_DATA) count_target(target, DIAG_DATA);
        else if (subtype == 8U && target == WIFI_TARGET_DIAG_AP)
            count_target(target, DIAG_BEACON);
        else if (subtype == 11U) count_target(target, DIAG_AUTH);
        else if (subtype == 1U && target == WIFI_TARGET_DIAG_AP)
            count_target(target, DIAG_ASSOC);
        else if (subtype == 0U && target == WIFI_TARGET_DIAG_STA)
            count_target(target, DIAG_ASSOC);
    }
#else
    wifi_target_diag_kind_t target = WIFI_TARGET_DIAG_NONE;
#endif
    if (packet->rx_ctrl.rx_state != 0 ||
        packet->rx_ctrl.sig_len <= WIFI_PROMISCUOUS_FCS_LEN) {
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
        count_target(target, DIAG_INVALID);
#endif
        atomic_fetch_add_explicit(&s_dropped_invalid, 1,
                                  memory_order_relaxed);
        return;
    }
    const uint16_t driver_length = packet->rx_ctrl.sig_len;
    const uint16_t frame_length = driver_length - WIFI_PROMISCUOUS_FCS_LEN;
    if (driver_length > WIFI_PROMISCUOUS_DRIVER_MAX_LEN ||
        frame_length > WIFI_PROMISCUOUS_CAPTURE_MAX_LEN) {
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
        count_target(target, DIAG_OVERSIZE);
#endif
        atomic_fetch_add_explicit(&s_dropped_oversize, 1,
                                  memory_order_relaxed);
        return;
    }
    uint8_t slot_index;
    if (xQueueReceive(free_queue, &slot_index, 0) != pdTRUE) {
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
        count_target(target, DIAG_FULL);
#endif
        atomic_fetch_add_explicit(&s_dropped_full, 1, memory_order_relaxed);
        return;
    }
    update_queue_high_water(WIFI_PROMISCUOUS_POOL_SLOTS -
        (uint32_t)uxQueueMessagesWaiting(free_queue));
    ingress_slot_t *slot = &s_pool[slot_index];
    /* Assign metadata field-by-field: an aggregate assignment would create or
     * clear a ~4 KiB temporary on the Wi-Fi callback stack. */
    slot->original_length = frame_length;
    slot->captured_length = frame_length;
    slot->rssi = packet->rx_ctrl.rssi;
    slot->channel = packet->rx_ctrl.channel;
    slot->legacy_rate = packet->rx_ctrl.rate;
    slot->sig_mode = packet->rx_ctrl.sig_mode;
    slot->mcs = packet->rx_ctrl.mcs;
    slot->bandwidth_40mhz = packet->rx_ctrl.cwb != 0;
    slot->short_gi = packet->rx_ctrl.sgi != 0;
    slot->type = type;
    slot->target_diag = target;
    memcpy(slot->payload, packet->payload, frame_length);

    /* ESP-IDF invokes this callback from its Wi-Fi task, not from an ISR. */
    if (xQueueSend(ready, &slot_index, 0) != pdTRUE) {
        (void)xQueueSend(free_queue, &slot_index, 0);
        atomic_fetch_add_explicit(&s_dropped_full, 1, memory_order_relaxed);
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
        count_target(target, DIAG_FULL);
#endif
    } else {
        atomic_fetch_add_explicit(&s_enqueued, 1, memory_order_relaxed);
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
        count_target(target, DIAG_ENQUEUED);
#endif
    }
}

bool wifi_promiscuous_receive(wifi_raw_packet_t *out_packet)
{
    return wifi_promiscuous_receive_wait(out_packet, 0);
}

bool wifi_promiscuous_receive_wait(wifi_raw_packet_t *out_packet,
                                   TickType_t wait)
{
    QueueHandle_t ready = atomic_load_explicit(
        &s_ready_queue, memory_order_acquire);
    uint8_t index;
    if (out_packet == NULL || ready == NULL || s_pool == NULL ||
        xQueueReceive(ready, &index, wait) != pdTRUE) return false;
    const ingress_slot_t *slot = &s_pool[index];
    *out_packet = (wifi_raw_packet_t) {
        .original_length = slot->original_length,
        .captured_length = slot->captured_length,
        .rssi = slot->rssi, .channel = slot->channel,
        .legacy_rate = slot->legacy_rate, .sig_mode = slot->sig_mode,
        .mcs = slot->mcs, .bandwidth_40mhz = slot->bandwidth_40mhz,
        .short_gi = slot->short_gi, .type = slot->type,
        .target_diag = slot->target_diag,
        .payload = slot->payload, .ingress_slot = index,
    };
    return true;
}

void wifi_promiscuous_release(const wifi_raw_packet_t *packet)
{
    QueueHandle_t free_queue = atomic_load_explicit(
        &s_free_queue, memory_order_acquire);
    if (packet == NULL || free_queue == NULL ||
        packet->ingress_slot >= WIFI_PROMISCUOUS_POOL_SLOTS) return;
    uint8_t index = packet->ingress_slot;
    (void)xQueueSend(free_queue, &index, 0);
}

uint32_t wifi_promiscuous_dropped_count(void)
{
    return (uint32_t)atomic_load_explicit(
        &s_dropped_full, memory_order_acquire) +
        (uint32_t)atomic_load_explicit(&s_dropped_oversize,
                                      memory_order_acquire) +
        (uint32_t)atomic_load_explicit(&s_dropped_invalid,
                                      memory_order_acquire);
}

uint32_t wifi_promiscuous_queued_count(void)
{
    QueueHandle_t ready = atomic_load_explicit(
        &s_ready_queue, memory_order_acquire);
    return ready != NULL
        ? (uint32_t)uxQueueMessagesWaiting(ready)
        : 0;
}

bool wifi_promiscuous_get_snapshot(wifi_promiscuous_snapshot_t *out)
{
    QueueHandle_t ready = atomic_load_explicit(
        &s_ready_queue, memory_order_acquire);
    if (out == NULL || ready == NULL) return false;
    *out = (wifi_promiscuous_snapshot_t) {
        .rx_total = atomic_load_explicit(&s_rx_total, memory_order_acquire),
        .enqueued = atomic_load_explicit(&s_enqueued, memory_order_acquire),
        .queue_current = (uint32_t)uxQueueMessagesWaiting(ready),
        .queue_high_water = (uint32_t)atomic_load_explicit(
            &s_queue_high_water, memory_order_acquire),
        .dropped_full = (uint32_t)atomic_load_explicit(
            &s_dropped_full, memory_order_acquire),
        .dropped_oversize = (uint32_t)atomic_load_explicit(
            &s_dropped_oversize, memory_order_acquire),
        .dropped_invalid = (uint32_t)atomic_load_explicit(
            &s_dropped_invalid, memory_order_acquire),
    };
    out->dropped = out->dropped_full + out->dropped_oversize +
                   out->dropped_invalid;
    return true;
}

bool wifi_target_diag_get_snapshot(wifi_target_diag_snapshot_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
#define LOAD(t, c) atomic_load_explicit(&s_target[(t)][(c)], memory_order_acquire)
    *out = (wifi_target_diag_snapshot_t) {
        .enabled = true,
        .callback_ap = LOAD(DIAG_AP, DIAG_CB), .callback_sta = LOAD(DIAG_STA, DIAG_CB),
        .ap_beacon = LOAD(DIAG_AP, DIAG_BEACON), .ap_auth = LOAD(DIAG_AP, DIAG_AUTH),
        .ap_assoc_response = LOAD(DIAG_AP, DIAG_ASSOC), .ap_data = LOAD(DIAG_AP, DIAG_DATA),
        .ap_ampdu = LOAD(DIAG_AP, DIAG_AMPDU),
        .sta_auth = LOAD(DIAG_STA, DIAG_AUTH),
        .sta_assoc_request = LOAD(DIAG_STA, DIAG_ASSOC), .sta_data = LOAD(DIAG_STA, DIAG_DATA),
        .sta_ampdu = LOAD(DIAG_STA, DIAG_AMPDU),
        .invalid_ap = LOAD(DIAG_AP, DIAG_INVALID), .invalid_sta = LOAD(DIAG_STA, DIAG_INVALID),
        .oversize_ap = LOAD(DIAG_AP, DIAG_OVERSIZE), .oversize_sta = LOAD(DIAG_STA, DIAG_OVERSIZE),
        .full_ap = LOAD(DIAG_AP, DIAG_FULL), .full_sta = LOAD(DIAG_STA, DIAG_FULL),
        .enqueued_ap = LOAD(DIAG_AP, DIAG_ENQUEUED), .enqueued_sta = LOAD(DIAG_STA, DIAG_ENQUEUED),
        .analyzed_ap = LOAD(DIAG_AP, DIAG_ANALYZED), .analyzed_sta = LOAD(DIAG_STA, DIAG_ANALYZED),
        .parser_failed_ap = LOAD(DIAG_AP, DIAG_PARSE_FAIL),
        .parser_failed_sta = LOAD(DIAG_STA, DIAG_PARSE_FAIL),
        .captured_ap = LOAD(DIAG_AP, DIAG_CAPTURED), .captured_sta = LOAD(DIAG_STA, DIAG_CAPTURED),
        .capture_failed_ap = LOAD(DIAG_AP, DIAG_CAPTURE_FAIL),
        .capture_failed_sta = LOAD(DIAG_STA, DIAG_CAPTURE_FAIL),
        .exported_ap = LOAD(DIAG_AP, DIAG_EXPORTED), .exported_sta = LOAD(DIAG_STA, DIAG_EXPORTED),
    };
#undef LOAD
#endif
    return true;
}

void wifi_target_diag_note_analyzed(wifi_target_diag_kind_t target, bool parsed)
{
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
    count_target(target, DIAG_ANALYZED);
    if (!parsed) count_target(target, DIAG_PARSE_FAIL);
#else
    (void)target; (void)parsed;
#endif
}

void wifi_target_diag_note_capture(wifi_target_diag_kind_t target, bool stored)
{
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
    count_target(target, stored ? DIAG_CAPTURED : DIAG_CAPTURE_FAIL);
#else
    (void)target; (void)stored;
#endif
}

void wifi_target_diag_note_export(uint32_t ap_records, uint32_t sta_records)
{
#if WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
    atomic_fetch_add_explicit(&s_target[DIAG_AP][DIAG_EXPORTED], ap_records,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&s_target[DIAG_STA][DIAG_EXPORTED], sta_records,
                              memory_order_relaxed);
#else
    (void)ap_records; (void)sta_records;
#endif
}
