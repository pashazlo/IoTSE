#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-S3 wifi_pkt_rx_ctrl_t.sig_len is 12 bits and includes the 4-byte FCS.
 * IoTSE intentionally stores the complete driver-provided MPDU without FCS. */
#define WIFI_PROMISCUOUS_DRIVER_MAX_LEN 4095U
#define WIFI_PROMISCUOUS_FCS_LEN 4U
#define WIFI_PROMISCUOUS_CAPTURE_MAX_LEN \
    (WIFI_PROMISCUOUS_DRIVER_MAX_LEN - WIFI_PROMISCUOUS_FCS_LEN)
#define WIFI_PROMISCUOUS_POOL_SLOTS 32U
#define WIFI_PROMISCUOUS_QUEUE_LEN WIFI_PROMISCUOUS_POOL_SLOTS

/* Temporary, counter-only AP/STA path diagnostics. This never filters frames.
 * Set to 0 for normal builds after the hardware capture investigation. */
#ifndef WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
#define WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS 1
#endif

typedef enum {
    WIFI_TARGET_DIAG_NONE = 0,
    WIFI_TARGET_DIAG_AP,
    WIFI_TARGET_DIAG_STA,
} wifi_target_diag_kind_t;

typedef struct {
    uint16_t original_length; /* Original 802.11 MPDU length, FCS excluded. */
    uint16_t captured_length; /* IoTSE-owned bytes, FCS excluded. */
    int8_t rssi;
    uint8_t channel;
    uint8_t legacy_rate;
    uint8_t sig_mode;
    uint8_t mcs;
    bool bandwidth_40mhz;
    bool short_gi;
    wifi_promiscuous_pkt_type_t type;
    wifi_target_diag_kind_t target_diag;
    const uint8_t *payload;
    uint8_t ingress_slot; /* Opaque pool token; release after processing. */
} wifi_raw_packet_t;

typedef struct {
    uint64_t rx_total;
    uint64_t enqueued;
    uint32_t queue_current;
    uint32_t queue_high_water;
    uint32_t dropped;
    uint32_t dropped_full;
    uint32_t dropped_oversize;
    uint32_t dropped_invalid;
} wifi_promiscuous_snapshot_t;

typedef struct {
    bool enabled;
    uint64_t callback_ap;
    uint64_t callback_sta;
    uint64_t ap_beacon;
    uint64_t ap_auth;
    uint64_t ap_assoc_response;
    uint64_t ap_data;
    uint64_t ap_ampdu;
    uint64_t sta_auth;
    uint64_t sta_assoc_request;
    uint64_t sta_data;
    uint64_t sta_ampdu;
    uint64_t invalid_ap;
    uint64_t invalid_sta;
    uint64_t oversize_ap;
    uint64_t oversize_sta;
    uint64_t full_ap;
    uint64_t full_sta;
    uint64_t enqueued_ap;
    uint64_t enqueued_sta;
    uint64_t analyzed_ap;
    uint64_t analyzed_sta;
    uint64_t parser_failed_ap;
    uint64_t parser_failed_sta;
    uint64_t captured_ap;
    uint64_t captured_sta;
    uint64_t capture_failed_ap;
    uint64_t capture_failed_sta;
    uint64_t exported_ap;
    uint64_t exported_sta;
} wifi_target_diag_snapshot_t;

/* Worker only, before first buffered capture. Retained across capture sessions. */
esp_err_t wifi_promiscuous_init(void);

/* The pool and queues have application lifetime, matching the permanent
 * analyzer task. There is intentionally no public deinit: deleting them
 * without joining both ESP Wi-Fi callback and analyzer would invalidate
 * borrowed packet.payload pointers. */
void wifi_promiscuous_reset(void);

/* ESP-IDF Wi-Fi task callback. Never call it directly. */
void wifi_promiscuous_rx_cb(
    void *buffer,
    wifi_promiscuous_pkt_type_t type
);

/* Non-blocking consumer API. Packet processing must happen outside callback. */
bool wifi_promiscuous_receive(wifi_raw_packet_t *out_packet);
bool wifi_promiscuous_receive_wait(wifi_raw_packet_t *out_packet,
                                   TickType_t wait);
/* Completes ANALYZER_OWNED -> FREE. Exactly one release per received packet. */
void wifi_promiscuous_release(const wifi_raw_packet_t *packet);
uint32_t wifi_promiscuous_dropped_count(void);
uint32_t wifi_promiscuous_queued_count(void);
bool wifi_promiscuous_get_snapshot(wifi_promiscuous_snapshot_t *snapshot);
bool wifi_target_diag_get_snapshot(wifi_target_diag_snapshot_t *snapshot);
void wifi_target_diag_note_analyzed(wifi_target_diag_kind_t target,
                                    bool parsed);
void wifi_target_diag_note_capture(wifi_target_diag_kind_t target,
                                   bool stored);
void wifi_target_diag_note_export(uint32_t ap_records, uint32_t sta_records);

#ifdef __cplusplus
}
#endif
