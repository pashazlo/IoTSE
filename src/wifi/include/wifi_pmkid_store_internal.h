#pragma once

/* Внутренний слой PMKID-хранилища: состояние + ЧИСТЫЕ функции над ним.
 * Без статиков, mutex и RTOS — поэтому проверяется host-тестом. Публичная обёртка
 * (wifi_pmkid_store.c) держит единственный экземпляр этого состояния и mutex. */

#include "wifi_pmkid_store.h"

typedef struct {
    uint16_t count;        /* занятые записи [0..WIFI_PMKID_STORE_CAPACITY] */
    uint32_t total_seen;   /* все вызовы observe (вкл. дубли и переполнение) */
    uint32_t dropped_full; /* новые уникальные, отброшенные из-за лимита */
    wifi_pmkid_record_t records[WIFI_PMKID_STORE_CAPACITY];
} wifi_pmkid_store_state_t;

/* Сбросить состояние к пустому. */
void wifi_pmkid_store_core_reset(wifi_pmkid_store_state_t *s);

/* Учесть находку. true — добавлена новая уникальная запись; false — дубликат
 * (ключ BSSID+STA+PMKID уже есть) либо нет места. Счётчики обновляются всегда. */
bool wifi_pmkid_store_core_observe(wifi_pmkid_store_state_t *s,
                                   const wifi_pmkid_record_t *rec);

/* Скопировать состояние в снимок для UI. */
void wifi_pmkid_store_core_snapshot(const wifi_pmkid_store_state_t *s,
                                    wifi_pmkid_store_snapshot_t *out);
