#include "wifi_analyzer.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_capture_store.h"
#include "wifi_ap_tracker.h"
#include "wifi_eapol_parser.h"
#include "wifi_eapol_tracker.h"
#include "wifi_frame_parser.h"
#include "wifi_mgmt_parser.h"
#include "wifi_promiscuous.h"
#include "wifi_sta_tracker.h"
#include "wifi_worker.h"

#define ANALYZER_TASK_STACK    4096
#define ANALYZER_TASK_PRIORITY 3
#define DIAGNOSTIC_PERIOD_US   1000000LL

static const char *TAG = "WIFI_PIPE";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static wifi_analyzer_snapshot_t s_snapshot;
static wifi_channel_observation_t s_channels[WIFI_ANALYZER_MAX_CHANNELS];
static wifi_pipeline_diagnostics_t s_diagnostics;
static SemaphoreHandle_t s_reset_mutex;
static SemaphoreHandle_t s_quiesced;
static SemaphoreHandle_t s_resume;
static atomic_bool s_pause_requested;

static void update_diagnostics(int64_t now, uint64_t *previous_rx,
                               int64_t *previous_time)
{
    wifi_promiscuous_snapshot_t ingress = {0};
    wifi_capture_store_snapshot_t ring = {0};
    wifi_channel_engine_snapshot_t engine = {0};
    wifi_ap_tracker_diagnostics_t ap = {0};
    wifi_sta_tracker_diagnostics_t sta = {0};
    /* Static to keep the diagnostic snapshot off the analyzer task stack. */
    static wifi_target_diag_snapshot_t target;
    (void)wifi_promiscuous_get_snapshot(&ingress);
    (void)wifi_capture_store_get_snapshot(&ring);
    (void)wifi_channel_engine_get_snapshot(&engine);
    (void)wifi_ap_tracker_get_diagnostics(&ap);
    (void)wifi_sta_tracker_get_diagnostics(&sta);
    (void)wifi_target_diag_get_snapshot(&target);
    int64_t elapsed = now - *previous_time;
    uint64_t delta = ingress.rx_total >= *previous_rx
        ? ingress.rx_total - *previous_rx : ingress.rx_total;
    uint32_t rate = elapsed > 0
        ? (uint32_t)((delta * 1000000ULL) / (uint64_t)elapsed) : 0U;
    *previous_rx = ingress.rx_total;
    *previous_time = now;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_diagnostics = (wifi_pipeline_diagnostics_t) {
            .rx_total = ingress.rx_total,
            .rx_packets_per_second = rate,
            .ingress_queue_current = ingress.queue_current,
            .ingress_queue_capacity = WIFI_PROMISCUOUS_QUEUE_LEN,
            .ingress_queue_high_water = ingress.queue_high_water,
            .ingress_dropped = ingress.dropped,
            .ingress_enqueued = ingress.enqueued,
            .ingress_dropped_full = ingress.dropped_full,
            .ingress_dropped_oversize = ingress.dropped_oversize,
            .ingress_dropped_invalid = ingress.dropped_invalid,
            .analyzer_processed = s_snapshot.packets,
            .parser_failures = s_snapshot.parser_failures,
            .mgmt_frames_parsed = s_snapshot.mgmt_frames_parsed,
            .mgmt_frames_malformed = s_snapshot.mgmt_frames_malformed,
            .rsn_ie_seen = s_snapshot.rsn_ie_seen,
            .rsn_parse_ok = s_snapshot.rsn_parse_ok,
            .rsn_parse_malformed = s_snapshot.rsn_parse_malformed,
            .ie_chain_malformed = s_snapshot.ie_chain_malformed,
            .ring_bytes = ring.used_bytes,
            .ring_records = ring.record_count,
            .ring_high_water_bytes = ring.high_water_bytes,
            .ring_high_water_records = ring.high_water_records,
            .overwritten_records = ring.overwritten_records,
            .overwritten_bytes = ring.overwritten_bytes,
            .dropped_busy = ring.dropped_busy,
            .dropped_frozen = ring.dropped_frozen,
            .dropped_oversize = ring.dropped_oversize,
            .corruption_resets = ring.corruption_resets,
            .save_busy_count = ring.save_busy_count,
            .active_bank = ring.active_bank,
            .sealed_bank = ring.sealed_bank,
            .writer_busy = ring.saving,
            .current_channel = engine.current_channel,
            .channel_mode = engine.mode,
            .free_internal_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            .min_internal_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            .free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            .min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
            .analyzer_stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL),
            .writer_stack_hwm = ring.writer_stack_hwm,
        };
        xSemaphoreGive(s_lock);
    }
    if (wifi_worker_get_state() != WIFI_WORKER_STATE_PROMISCUOUS) return;
    ESP_LOGI(TAG,
             "RX=%" PRIu32 "/s Q=%" PRIu32 "/%u HWM=%" PRIu32
             " DROP=%" PRIu32 "(FULL=%" PRIu32 " OVER=%" PRIu32
             " INVALID=%" PRIu32 ") CH=%u MODE=%s RING=%" PRIu32
             "KB/%uKB OVERWRITE=%" PRIu32 " WRITER=%s PSRAM=%zu"
             " HEAP=%zu AP=%u/%u EVI=%" PRIu64 " AP_BAD=%" PRIu64
             " STA=%u/%u STA_EVI=%" PRIu64 " AMBIG=%" PRIu64,
             rate, ingress.queue_current, WIFI_PROMISCUOUS_QUEUE_LEN,
             ingress.queue_high_water, ingress.dropped, ingress.dropped_full,
             ingress.dropped_oversize, ingress.dropped_invalid,
             engine.current_channel,
             engine.mode == WIFI_CHANNEL_MODE_SURVEY ? "SURVEY" : "FIXED",
             ring.used_bytes / 1024U, WIFI_CAPTURE_BANK_BYTES / 1024U,
             ring.overwritten_records, ring.saving ? "BUSY" : "IDLE",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             ap.table_count, ap.table_capacity, ap.ap_evicted,
             ap.malformed_ignored, sta.table_count, sta.table_capacity,
             sta.sta_evicted, sta.ambiguous_relation_count);
    if (target.enabled) {
        ESP_LOGI(TAG,
                 "TARGET AP cb=%" PRIu64 " bcn=%" PRIu64 " auth=%" PRIu64
                 " assoc_rsp=%" PRIu64 " data=%" PRIu64 " ampdu=%" PRIu64
                 " drop[i/o/f]=%" PRIu64 "/%" PRIu64 "/%" PRIu64
                 " q=%" PRIu64 " ana=%" PRIu64 " pf=%" PRIu64
                 " cap=%" PRIu64 " capfail=%" PRIu64 " exp=%" PRIu64,
                 target.callback_ap, target.ap_beacon, target.ap_auth,
                 target.ap_assoc_response, target.ap_data, target.ap_ampdu,
                 target.invalid_ap,
                 target.oversize_ap, target.full_ap, target.enqueued_ap,
                 target.analyzed_ap, target.parser_failed_ap,
                 target.captured_ap, target.capture_failed_ap,
                 target.exported_ap);
        ESP_LOGI(TAG,
                 "TARGET STA cb=%" PRIu64 " auth=%" PRIu64
                 " assoc_req=%" PRIu64 " data=%" PRIu64 " ampdu=%" PRIu64
                 " drop[i/o/f]=%" PRIu64 "/%" PRIu64 "/%" PRIu64
                 " q=%" PRIu64 " ana=%" PRIu64 " pf=%" PRIu64
                 " cap=%" PRIu64 " capfail=%" PRIu64 " exp=%" PRIu64,
                 target.callback_sta, target.sta_auth,
                 target.sta_assoc_request, target.sta_data, target.sta_ampdu,
                 target.invalid_sta, target.oversize_sta, target.full_sta,
                 target.enqueued_sta, target.analyzed_sta,
                 target.parser_failed_sta, target.captured_sta,
                 target.capture_failed_sta, target.exported_sta);
    }
}

static void observe_packet(const wifi_raw_packet_t *packet,
                           const wifi_frame_info_t *info, bool parsed,
                           const wifi_mgmt_info_t *mgmt,
                           int64_t timestamp)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    ++s_snapshot.packets;
    if (!parsed) ++s_snapshot.parser_failures;
    if (packet->type == WIFI_PKT_MGMT) ++s_snapshot.management_frames;
    if (packet->type == WIFI_PKT_DATA) ++s_snapshot.data_frames;
    if (packet->type == WIFI_PKT_CTRL) ++s_snapshot.control_frames;
    if (parsed && info->is_eapol) ++s_snapshot.eapol_frames;
    if (mgmt != NULL) {
        if (mgmt->status == WIFI_PARSE_VALID) ++s_snapshot.mgmt_frames_parsed;
        if (mgmt->status == WIFI_PARSE_MALFORMED ||
            mgmt->status == WIFI_PARSE_PARTIAL) ++s_snapshot.mgmt_frames_malformed;
        if (mgmt->rsn_ie_seen) {
            ++s_snapshot.rsn_ie_seen;
            if (mgmt->rsn.status == WIFI_PARSE_VALID) ++s_snapshot.rsn_parse_ok;
            else if (mgmt->rsn.status == WIFI_PARSE_MALFORMED)
                ++s_snapshot.rsn_parse_malformed;
        }
        if (mgmt->ie_chain_malformed) ++s_snapshot.ie_chain_malformed;
    }
    s_snapshot.last_channel = packet->channel;
    s_snapshot.last_timestamp_us = timestamp;

    if (packet->channel >= 1U && packet->channel <= WIFI_ANALYZER_MAX_CHANNELS) {
        wifi_channel_observation_t *ch = &s_channels[packet->channel - 1U];
        ch->channel = packet->channel;
        ++ch->total_frames;
        ch->bytes += packet->original_length;
        if (packet->type == WIFI_PKT_MGMT) ++ch->management_frames;
        if (packet->type == WIFI_PKT_DATA) ++ch->data_frames;
        if (packet->type == WIFI_PKT_CTRL) ++ch->control_frames;
        if (parsed && info->is_eapol) ++ch->eapol_frames;
        ch->rssi_sum += packet->rssi;
        ++ch->rssi_count;
        if (ch->rssi_count == 1U || packet->rssi < ch->rssi_min) ch->rssi_min = packet->rssi;
        if (ch->rssi_count == 1U || packet->rssi > ch->rssi_max) ch->rssi_max = packet->rssi;
        ch->last_seen_us = timestamp;
    }
    xSemaphoreGive(s_lock);
}

static void analyzer_task(void *unused)
{
    (void)unused;
    wifi_raw_packet_t packet;
    int64_t previous_log = esp_timer_get_time();
    uint64_t previous_rx = 0;
    while (true) {
        if (atomic_load_explicit(&s_pause_requested, memory_order_acquire)) {
            (void)xSemaphoreGive(s_quiesced);
            (void)xSemaphoreTake(s_resume, portMAX_DELAY);
            continue;
        }
        bool received = wifi_promiscuous_receive_wait(&packet, pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        if (received) {
            wifi_frame_info_t info;
            bool parsed = wifi_frame_parse(packet.payload, packet.captured_length,
                                           packet.rssi, packet.channel, &info);
            if (parsed && info.is_eapol) {
                wifi_eapol_info_t eapol;
                (void)wifi_eapol_parse(packet.payload + info.eapol_offset,
                                       info.eapol_length, &eapol);
                uint8_t bssid[6], sta[6];
                wifi_frame_direction_t direction;
                if (wifi_frame_get_infrastructure_roles(&info, bssid, sta,
                                                        &direction)) {
                    wifi_eapol_observation_t observation = {
                        .direction = direction,
                        .channel = info.channel,
                        .rssi = info.rssi,
                        .timestamp_us = now,
                        .eapol = &eapol,
                        .message = wifi_eapol_classify(&eapol, direction),
                    };
                    memcpy(observation.bssid, bssid, sizeof(bssid));
                    memcpy(observation.sta, sta, sizeof(sta));
                    (void)wifi_eapol_tracker_observe(&observation);
                }
            }
            wifi_mgmt_info_t mgmt;
            wifi_mgmt_info_t *mgmt_ptr = NULL;
            if (parsed && info.type == WIFI_FRAME_TYPE_MGMT) {
                (void)wifi_mgmt_parse(packet.payload, packet.captured_length,
                                      &info, &mgmt);
                mgmt_ptr = &mgmt;
                if (info.subtype == WIFI_FRAME_SUBTYPE_BEACON ||
                    info.subtype == WIFI_FRAME_SUBTYPE_PROBE_RESP) {
                    const wifi_ap_observation_t observation = {
                        .frame = &info,
                        .management = &mgmt,
                        .timestamp_us = now,
                    };
                    (void)wifi_ap_tracker_observe(&observation);
                }
            }
            if (parsed) {
                const wifi_sta_observation_t observation = {
                    .frame = &info,
                    .management = mgmt_ptr,
                    .timestamp_us = now,
                };
                (void)wifi_sta_tracker_observe(&observation);
            }
            wifi_target_diag_note_analyzed(packet.target_diag, parsed);
            bool stored = wifi_capture_store_append(&packet);
            wifi_target_diag_note_capture(packet.target_diag, stored);
            observe_packet(&packet, &info, parsed, mgmt_ptr, now);
            wifi_promiscuous_release(&packet);
        }
        if (now - previous_log >= DIAGNOSTIC_PERIOD_US) {
            update_diagnostics(now, &previous_rx, &previous_log);
        }
    }
}

esp_err_t wifi_analyzer_init(void)
{
    if (s_task != NULL) return ESP_OK;
    esp_err_t err = wifi_capture_store_init();
    if (err != ESP_OK) return err;
    err = wifi_promiscuous_init();
    if (err != ESP_OK) return err;
    err = wifi_ap_tracker_init();
    if (err != ESP_OK) return err;
    err = wifi_sta_tracker_init();
    if (err != ESP_OK) return err;
    err = wifi_eapol_tracker_init();
    if (err != ESP_OK) return err;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_channels, 0, sizeof(s_channels));
    memset(&s_diagnostics, 0, sizeof(s_diagnostics));
    s_reset_mutex = xSemaphoreCreateMutex();
    s_quiesced = xSemaphoreCreateBinary();
    s_resume = xSemaphoreCreateBinary();
    if (s_reset_mutex == NULL || s_quiesced == NULL || s_resume == NULL) {
        if (s_reset_mutex != NULL) vSemaphoreDelete(s_reset_mutex);
        if (s_quiesced != NULL) vSemaphoreDelete(s_quiesced);
        if (s_resume != NULL) vSemaphoreDelete(s_resume);
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(analyzer_task, "wifi_analyzer", ANALYZER_TASK_STACK, NULL,
                    ANALYZER_TASK_PRIORITY, &s_task) != pdPASS) {
        vSemaphoreDelete(s_resume);
        vSemaphoreDelete(s_quiesced);
        vSemaphoreDelete(s_reset_mutex);
        s_resume = s_quiesced = s_reset_mutex = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_analyzer_reset(void)
{
    if (s_task == NULL || s_reset_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (wifi_worker_get_state() == WIFI_WORKER_STATE_PROMISCUOUS)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_reset_mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    atomic_store_explicit(&s_pause_requested, true, memory_order_release);
    if (xSemaphoreTake(s_quiesced, portMAX_DELAY) != pdTRUE) {
        atomic_store_explicit(&s_pause_requested, false, memory_order_release);
        xSemaphoreGive(s_reset_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        uint32_t correlation_id = s_snapshot.correlation_id;
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        memset(s_channels, 0, sizeof(s_channels));
        s_snapshot.correlation_id = correlation_id;
        xSemaphoreGive(s_lock);
    }
    wifi_capture_store_reset();
    wifi_promiscuous_reset();
    wifi_ap_tracker_reset();
    wifi_sta_tracker_reset();
    wifi_eapol_tracker_reset();
    atomic_store_explicit(&s_pause_requested, false, memory_order_release);
    (void)xSemaphoreGive(s_resume);
    xSemaphoreGive(s_reset_mutex);
    return ESP_OK;
}

void wifi_analyzer_set_correlation_id(uint32_t correlation_id)
{
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_snapshot.correlation_id = correlation_id;
        xSemaphoreGive(s_lock);
    }
    wifi_capture_store_set_correlation_id(correlation_id);
}

bool wifi_analyzer_get_snapshot(wifi_analyzer_snapshot_t *out)
{
    if (out == NULL || s_lock == NULL ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(2)) != pdTRUE) return false;
    *out = s_snapshot;
    xSemaphoreGive(s_lock);
    return true;
}

bool wifi_analyzer_get_channel_observations(
    wifi_channel_observation_t out[WIFI_ANALYZER_MAX_CHANNELS],
    uint8_t *first_channel, uint8_t *last_channel)
{
    if (out == NULL || s_lock == NULL ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(2)) != pdTRUE) return false;
    memcpy(out, s_channels, sizeof(s_channels));
    xSemaphoreGive(s_lock);
    wifi_channel_engine_snapshot_t engine;
    if (!wifi_channel_engine_get_snapshot(&engine)) return false;
    if (first_channel != NULL) *first_channel = engine.first_channel;
    if (last_channel != NULL) *last_channel = engine.last_channel;
    for (uint8_t i = 0; i < WIFI_ANALYZER_MAX_CHANNELS; ++i) {
        out[i].channel = i + 1U;
        out[i].dwell_time_us = engine.dwell_time_us[i];
        out[i].frames_per_second = engine.dwell_time_us[i] != 0U
            ? (uint32_t)((out[i].total_frames * 1000000ULL) /
                         engine.dwell_time_us[i]) : 0U;
    }
    return true;
}

bool wifi_analyzer_get_diagnostics(wifi_pipeline_diagnostics_t *out)
{
    if (out == NULL || s_lock == NULL ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(2)) != pdTRUE) return false;
    *out = s_diagnostics;
    xSemaphoreGive(s_lock);
    return true;
}
