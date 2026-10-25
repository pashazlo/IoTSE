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
/* Число слотов пула приёма promiscuous. Каждый слот — это ingress_slot_t
 * размером 4112 байт (payload до 4091 байт + метаданные), и ВЕСЬ пул лежит
 * в PSRAM (см. heap_caps_calloc с MALLOC_CAP_SPIRAM в wifi_promiscuous.c).
 *
 * Зачем поднимаем 32 -> 64: это очередь между коротким callback драйвера Wi-Fi
 * и задачей-анализатором. Если анализатор на мгновение не успевает разгрести
 * пул, callback не находит свободный слот и инкрементит dropped_full (в логе
 * WIFI_PIPE это DROP ... FULL=..). Больше слотов = длиннее буфер всплесков
 * трафика = меньше FULL-потерь. Это дополняет драйверные RX-буферы из Фазы 3:
 * Фаза 3 ловит потери ДО callback, этот пул — потери ПОСЛЕ callback.
 *
 * Бюджет PSRAM (на плате свободно ~7 МБ, так что запас огромный):
 *   32 слота  -> 32 * 4112  = 131 584 байта (~128 КБ);
 *   64 слота  -> 64 * 4112  = 263 168 байт  (~257 КБ);  <-- сейчас
 *   128 слотов-> 128 * 4112 = 526 336 байт  (~514 КБ).
 *
 * ЖЁСТКИЙ ПОТОЛОК: индекс слота хранится и передаётся ТОЛЬКО как uint8_t —
 * в поле wifi_raw_packet_t.ingress_slot, как элемент обеих FreeRTOS-очередей
 * (xQueueCreate(..., sizeof(uint8_t))) и как счётчик цикла заполнения free
 * queue (for (uint8_t i = ...)). uint8_t вмещает 0..255, поэтому POOL_SLOTS
 * НЕ ДОЛЖЕН превышать 256; при 256 счётчик цикла никогда не достигнет границы
 * и зациклится. Значения 64 и 128 полностью безопасны. Если на плате при
 * нагрузке FULL всё ещё растёт — следующий шаг 64 -> 128 (просто PSRAM). */
#define WIFI_PROMISCUOUS_POOL_SLOTS 64U
/* Глубина ready-очереди повторяет число слотов: больше одного слота на кадр
 * в очереди всё равно не бывает, поэтому обе величины связаны одним макросом. */
#define WIFI_PROMISCUOUS_QUEUE_LEN WIFI_PROMISCUOUS_POOL_SLOTS

/* Temporary, counter-only AP/STA path diagnostics. This never filters frames.
 * Set to 0 for normal builds after the hardware capture investigation. */
#ifndef WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS
#define WIFI_PROMISCUOUS_TARGET_DIAGNOSTICS 1
#endif

/* Фаза 2: захват кадров с НЕВЕРНОЙ контрольной суммой (bad FCS).
 *
 * По умолчанию ВЫКЛЮЧЕНО (0) — нормальная сборка ведёт себя ровно как раньше:
 * кадр с rx_ctrl.rx_state != 0 отбраковывается как invalid в callback, а в
 * драйверный promiscuous-фильтр бит FCSFAIL не добавляется.
 *
 * При включении (1) происходит ДВА согласованных изменения:
 *   1) в wifi.c в маску esp_wifi_set_promiscuous_filter добавляется
 *      WIFI_PROMIS_FILTER_MASK_FCSFAIL, иначе драйвер просто не отдаст такие
 *      кадры в callback, и флаг ниже всегда был бы false;
 *   2) в wifi_promiscuous_rx_cb кадр с rx_state != 0 НЕ выбрасывается, а
 *      помечается полем fcs_bad = true и проходит дальше по конвейеру.
 *
 * Зачем: на зашумлённом канале или слабом RSSI часть кадров приходит с битым
 * FCS. Иногда их полезно видеть (анализ помех, восстановление частично
 * повреждённых рукопожатий). Но это «грязные» данные: содержимое может быть
 * искажено, поэтому потребители (EAPOL-парсер, классификатор) при желании
 * должны пропускать кадры с fcs_bad == true. Длина/границы buffer'а при этом
 * по-прежнему строго проверяются (см. oversize-guard в callback), так что
 * битый FCS не создаёт выхода за пределы памяти. */
#ifndef WIFI_PROMISCUOUS_CAPTURE_FCS_FAIL
#define WIFI_PROMISCUOUS_CAPTURE_FCS_FAIL 0
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
    /* true, если кадр пришёл с битым FCS (только при включённом
     * WIFI_PROMISCUOUS_CAPTURE_FCS_FAIL; иначе всегда false). «Грязный» кадр —
     * содержимое может быть искажено. */
    bool fcs_bad;
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
