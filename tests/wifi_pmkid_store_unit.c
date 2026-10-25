/* Host-тест ядра PMKID-хранилища (wifi_pmkid_store_core).
 *
 * Сборка и запуск (из корня репозитория):
 *   gcc -Wall -Wextra -std=c11 -DWIFI_PMKID_STORE_HOST_TEST \
 *       tests/wifi_pmkid_store_unit.c src/wifi/wifi_pmkid_store_core.c \
 *       -Isrc/wifi/include -o /tmp/pmkid_store_test && /tmp/pmkid_store_test
 *
 * Чистый host-тест: RTOS/mutex не нужны — тестируем только логику дедупликации
 * и границ над явным состоянием.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wifi_pmkid_store_internal.h"

/* Собрать запись с управляемыми полями, чтобы легко задавать «ту же» и «другую». */
static wifi_pmkid_record_t make_rec(uint8_t pmkid_seed, uint8_t bssid_seed,
                                    uint8_t sta_seed, const char *ssid)
{
    wifi_pmkid_record_t r;
    memset(&r, 0, sizeof(r));
    memset(r.pmkid, pmkid_seed, sizeof(r.pmkid));
    memset(r.bssid, bssid_seed, sizeof(r.bssid));
    memset(r.sta, sta_seed, sizeof(r.sta));
    if (ssid != NULL) {
        snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
    }
    r.rssi = -40;
    r.ts_us = 123456;
    return r;
}

/* 1. Новая запись добавляется; дубликат по ключу — нет. */
static void test_add_and_dedup(void)
{
    wifi_pmkid_store_state_t s;
    wifi_pmkid_store_core_reset(&s);

    wifi_pmkid_record_t a = make_rec(0xAA, 0x01, 0x02, "Lab");
    assert(wifi_pmkid_store_core_observe(&s, &a));   /* новая */
    assert(s.count == 1 && s.total_seen == 1);

    assert(!wifi_pmkid_store_core_observe(&s, &a));  /* тот же ключ → дубль */
    assert(s.count == 1 && s.total_seen == 2);       /* total растёт, count нет */
    assert(s.dropped_full == 0);
}

/* 2. Отличие в любом компоненте ключа (PMKID/BSSID/STA) даёт новую запись. */
static void test_key_components(void)
{
    wifi_pmkid_store_state_t s;
    wifi_pmkid_store_core_reset(&s);

    wifi_pmkid_record_t base = make_rec(0x10, 0x20, 0x30, "N");
    assert(wifi_pmkid_store_core_observe(&s, &base));

    wifi_pmkid_record_t diff_pmkid = make_rec(0x11, 0x20, 0x30, "N");
    wifi_pmkid_record_t diff_bssid = make_rec(0x10, 0x21, 0x30, "N");
    wifi_pmkid_record_t diff_sta   = make_rec(0x10, 0x20, 0x31, "N");
    assert(wifi_pmkid_store_core_observe(&s, &diff_pmkid));
    assert(wifi_pmkid_store_core_observe(&s, &diff_bssid));
    assert(wifi_pmkid_store_core_observe(&s, &diff_sta));
    assert(s.count == 4);
}

/* 3. Переполнение: сверх ёмкости новые уникальные не кладутся, растёт dropped_full. */
static void test_overflow(void)
{
    wifi_pmkid_store_state_t s;
    wifi_pmkid_store_core_reset(&s);

    for (unsigned i = 0; i < WIFI_PMKID_STORE_CAPACITY; ++i) {
        wifi_pmkid_record_t r = make_rec((uint8_t)i, 0x01, 0x02, "x");
        assert(wifi_pmkid_store_core_observe(&s, &r));
    }
    assert(s.count == WIFI_PMKID_STORE_CAPACITY);

    wifi_pmkid_record_t extra = make_rec(0xFF, 0xFF, 0xFF, "x");
    assert(!wifi_pmkid_store_core_observe(&s, &extra)); /* места нет */
    assert(s.count == WIFI_PMKID_STORE_CAPACITY);
    assert(s.dropped_full == 1);
}

/* 4. Снимок повторяет счётчики и занятые записи. */
static void test_snapshot(void)
{
    wifi_pmkid_store_state_t s;
    wifi_pmkid_store_core_reset(&s);
    wifi_pmkid_record_t a = make_rec(0xAB, 0x0A, 0x0B, "Crystal");
    (void)wifi_pmkid_store_core_observe(&s, &a);

    wifi_pmkid_store_snapshot_t snap;
    wifi_pmkid_store_core_snapshot(&s, &snap);
    assert(snap.count == 1 && snap.total_seen == 1);
    assert(memcmp(snap.records[0].pmkid, a.pmkid, WIFI_PMKID_LEN) == 0);
    assert(strcmp(snap.records[0].ssid, "Crystal") == 0);
}

/* 5. NULL-аргументы не роняют. */
static void test_null(void)
{
    wifi_pmkid_store_state_t s;
    wifi_pmkid_store_core_reset(&s);
    wifi_pmkid_record_t a = make_rec(1, 2, 3, "n");
    assert(!wifi_pmkid_store_core_observe(NULL, &a));
    assert(!wifi_pmkid_store_core_observe(&s, NULL));
    wifi_pmkid_store_core_reset(NULL);   /* не должно падать */
    wifi_pmkid_store_core_snapshot(NULL, NULL);
}

int main(void)
{
    test_add_and_dedup();
    test_key_components();
    test_overflow();
    test_snapshot();
    test_null();
    printf("wifi_pmkid_store_unit: all tests passed\n");
    return 0;
}
