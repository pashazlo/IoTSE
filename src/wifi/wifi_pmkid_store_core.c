#include "wifi_pmkid_store_internal.h"

#include <string.h>

/* Чистая логика PMKID-хранилища. Никаких статиков, mutex и RTOS: всё работает
 * над переданным состоянием — так логику дедупликации и границ ловит host-тест. */

/* Совпадают ли две находки по ключу уникальности: BSSID + STA + PMKID.
 * Именно эта тройка определяет «тот же результат» (одна и та же пара устройств и
 * тот же PMKID); всё остальное — RSSI/время/ssid — вспомогательное. */
static bool same_key(const wifi_pmkid_record_t *a, const wifi_pmkid_record_t *b)
{
    return memcmp(a->bssid, b->bssid, sizeof(a->bssid)) == 0 &&
           memcmp(a->sta, b->sta, sizeof(a->sta)) == 0 &&
           memcmp(a->pmkid, b->pmkid, sizeof(a->pmkid)) == 0;
}

void wifi_pmkid_store_core_reset(wifi_pmkid_store_state_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

bool wifi_pmkid_store_core_observe(wifi_pmkid_store_state_t *s,
                                   const wifi_pmkid_record_t *rec)
{
    if (s == NULL || rec == NULL) {
        return false;
    }
    /* Любое наблюдение учитываем в total_seen (включая дубликаты и переполнение). */
    s->total_seen++;

    /* Уже видели такую находку? Тогда это дубль — не добавляем. */
    for (uint16_t i = 0; i < s->count; ++i) {
        if (same_key(&s->records[i], rec)) {
            return false;
        }
    }

    /* Новая уникальная, но места нет — отметим переполнение. */
    if (s->count >= WIFI_PMKID_STORE_CAPACITY) {
        s->dropped_full++;
        return false;
    }

    /* Кладём копию. ssid копируем с гарантированным завершающим NUL: источник мог
     * прийти без него (на всякий случай обрезаем по нашему пределу). */
    wifi_pmkid_record_t *dst = &s->records[s->count];
    memcpy(dst->pmkid, rec->pmkid, sizeof(dst->pmkid));
    memcpy(dst->bssid, rec->bssid, sizeof(dst->bssid));
    memcpy(dst->sta, rec->sta, sizeof(dst->sta));
    size_t slen = strnlen(rec->ssid, WIFI_PMKID_SSID_MAX);
    memcpy(dst->ssid, rec->ssid, slen);
    dst->ssid[slen] = '\0';
    dst->rssi = rec->rssi;
    dst->ts_us = rec->ts_us;

    s->count++;
    return true;
}

void wifi_pmkid_store_core_snapshot(const wifi_pmkid_store_state_t *s,
                                    wifi_pmkid_store_snapshot_t *out)
{
    if (s == NULL || out == NULL) {
        return;
    }
    out->count = s->count;
    out->total_seen = s->total_seen;
    out->dropped_full = s->dropped_full;
    /* Копируем только занятые записи; хвост обнуляем, чтобы в снимке не было мусора. */
    memset(out->records, 0, sizeof(out->records));
    for (uint16_t i = 0; i < s->count; ++i) {
        out->records[i] = s->records[i];
    }
}
