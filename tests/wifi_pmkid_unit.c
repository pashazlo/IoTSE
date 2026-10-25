/* Host-тест экстрактора PMKID (wifi_pmkid_parser).
 *
 * Сборка и запуск (из корня репозитория):
 *   gcc -Wall -Wextra -std=c11 tests/wifi_pmkid_unit.c \
 *       src/wifi/wifi_pmkid_parser.c -Isrc/wifi/include -o /tmp/pmkid_test \
 *   && /tmp/pmkid_test && echo OK
 *
 * Чистый host-тест: радио/RTOS/ESP-IDF не нужны — парсер только разбирает байты.
 *
 * ВАЖНО про размеры. PMKID-KDE на проводе = 22 байта:
 *   [0]=0xDD тег, [1]=0x14 длина(=20=OUI+type+PMKID), [2..4]=OUI, [5]=type,
 *   [6..21]=PMKID(16). Байт длины (0x14) считает ТОЛЬКО то, что после него,
 *   поэтому сам KDE в Key Data занимает 2 + 20 = 22 байта.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wifi_pmkid_parser.h"

#define PMKID_KDE_WIRE_LEN 22U   /* полный размер PMKID-KDE в Key Data */

/* Эталонный PMKID (16 различимых байт — удобно сверять точность копирования). */
static const uint8_t k_pmkid[WIFI_PMKID_LEN] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

/* Собрать корректный EAPOL-Key (M1) с заданной областью Key Data.
 * Возвращает полную длину кадра. kd_len ограничен одним байтом (нам хватает). */
static size_t build_eapol_key(uint8_t *buf, const uint8_t *key_data,
                              uint8_t kd_len)
{
    buf[0] = 2U;                 /* version */
    buf[1] = 3U;                 /* type = EAPOL-Key */
    uint16_t body = (uint16_t)(95U + kd_len);
    buf[2] = (uint8_t)(body >> 8);
    buf[3] = (uint8_t)(body & 0xFFU);

    uint8_t *key = buf + 4;
    memset(key, 0, 95U);
    key[0] = 2U;                 /* descriptor type RSN (реалистично) */
    key[93] = 0U;                /* key_data_length high */
    key[94] = kd_len;            /* key_data_length low  */
    memcpy(key + 95, key_data, kd_len);
    return 4U + 95U + (size_t)kd_len;
}

/* Корректный PMKID-KDE (22 байта): DD 14 00-0F-AC 04 <PMKID[16]>. */
static void make_pmkid_kde(uint8_t out[PMKID_KDE_WIRE_LEN])
{
    out[0] = 0xDDU;
    out[1] = 0x14U;              /* len = 20 (OUI+type+PMKID) */
    out[2] = 0x00U; out[3] = 0x0FU; out[4] = 0xACU; out[5] = 0x04U;
    memcpy(out + 6, k_pmkid, WIFI_PMKID_LEN);
}

/* 1. Валидный M1 с PMKID → true и точные 16 байт. */
static void test_valid_pmkid(void)
{
    uint8_t kde[PMKID_KDE_WIRE_LEN];
    make_pmkid_kde(kde);
    uint8_t frame[160];
    size_t n = build_eapol_key(frame, kde, PMKID_KDE_WIRE_LEN);

    uint8_t got[WIFI_PMKID_LEN] = {0};
    assert(wifi_pmkid_extract_from_eapol(frame, n, got));
    assert(memcmp(got, k_pmkid, WIFI_PMKID_LEN) == 0);
}

/* 2. PMKID-KDE стоит ПОСЛЕ другого (не-PMKID) KDE → всё равно находим. */
static void test_pmkid_after_other_kde(void)
{
    /* Чужой KDE: DD 06 00-0F-AC 0A AA BB (на проводе 8 байт, type 0x0A). */
    uint8_t other[8] = {0xDD, 0x06, 0x00, 0x0F, 0xAC, 0x0A, 0xAA, 0xBB};
    uint8_t kde[8 + PMKID_KDE_WIRE_LEN];
    memcpy(kde, other, 8);
    make_pmkid_kde(kde + 8);

    uint8_t frame[160];
    size_t n = build_eapol_key(frame, kde, (uint8_t)sizeof(kde));
    uint8_t got[WIFI_PMKID_LEN] = {0};
    assert(wifi_pmkid_extract_from_eapol(frame, n, got));
    assert(memcmp(got, k_pmkid, WIFI_PMKID_LEN) == 0);
}

/* 3. Key Data есть, но PMKID-KDE нет (нужный размер, но type != 0x04) → false. */
static void test_no_pmkid(void)
{
    uint8_t kde[PMKID_KDE_WIRE_LEN];
    make_pmkid_kde(kde);
    kde[5] = 0x01U;              /* портим type: 0x04 → 0x01 */

    uint8_t frame[160];
    size_t n = build_eapol_key(frame, kde, PMKID_KDE_WIRE_LEN);
    uint8_t got[WIFI_PMKID_LEN] = {0};
    assert(!wifi_pmkid_extract_from_eapol(frame, n, got));
}

/* 4. Неправильный OUI → false (строгая сверка). */
static void test_wrong_oui(void)
{
    uint8_t kde[PMKID_KDE_WIRE_LEN];
    make_pmkid_kde(kde);
    kde[2] = 0x00U; kde[3] = 0x50U; kde[4] = 0xF2U; /* Microsoft OUI вместо RSN */

    uint8_t frame[160];
    size_t n = build_eapol_key(frame, kde, PMKID_KDE_WIRE_LEN);
    uint8_t got[WIFI_PMKID_LEN] = {0};
    assert(!wifi_pmkid_extract_from_eapol(frame, n, got));
}

/* 5. Не EAPOL-Key (type != 3) → false. */
static void test_not_eapol_key(void)
{
    uint8_t kde[PMKID_KDE_WIRE_LEN];
    make_pmkid_kde(kde);
    uint8_t frame[160];
    size_t n = build_eapol_key(frame, kde, PMKID_KDE_WIRE_LEN);
    frame[1] = 0U;              /* type = EAPOL-Start, не Key */
    uint8_t got[WIFI_PMKID_LEN] = {0};
    assert(!wifi_pmkid_extract_from_eapol(frame, n, got));
}

/* 6. Рассинхрон body_length и key_data_length → false (кристальная строгость). */
static void test_length_mismatch(void)
{
    uint8_t kde[PMKID_KDE_WIRE_LEN];
    make_pmkid_kde(kde);
    uint8_t frame[160];
    size_t n = build_eapol_key(frame, kde, PMKID_KDE_WIRE_LEN);
    frame[4 + 94] -= 1U;        /* key_data_length (key+94) занижаем на 1 */
    uint8_t got[WIFI_PMKID_LEN] = {0};
    assert(!wifi_pmkid_extract_from_eapol(frame, n, got));
}

/* 7. NULL-аргументы и слишком короткий ввод → false, без падения. */
static void test_null_and_short(void)
{
    uint8_t got[WIFI_PMKID_LEN] = {0};
    uint8_t dummy[4] = {2, 3, 0, 0};
    assert(!wifi_pmkid_extract_from_eapol(NULL, 10, got));
    assert(!wifi_pmkid_extract_from_eapol(dummy, 10, NULL));
    assert(!wifi_pmkid_extract_from_eapol(dummy, 0, got));
    assert(!wifi_pmkid_extract_from_eapol(dummy, 3, got)); /* короче заголовка */
}

/* 8. Любой префикс валидного кадра: true только на полной длине, иначе false,
 *    и ни один вызов не выходит за границы (запускать под ASAN для гарантии). */
static void test_every_prefix(void)
{
    uint8_t kde[PMKID_KDE_WIRE_LEN];
    make_pmkid_kde(kde);
    uint8_t frame[160];
    size_t n = build_eapol_key(frame, kde, PMKID_KDE_WIRE_LEN);

    uint8_t got[WIFI_PMKID_LEN];
    for (size_t k = 0; k <= n; ++k) {
        bool r = wifi_pmkid_extract_from_eapol(frame, k, got);
        assert(r == (k == n));
    }
}

int main(void)
{
    test_valid_pmkid();
    test_pmkid_after_other_kde();
    test_no_pmkid();
    test_wrong_oui();
    test_not_eapol_key();
    test_length_mismatch();
    test_null_and_short();
    test_every_prefix();
    printf("wifi_pmkid_unit: all tests passed\n");
    return 0;
}
