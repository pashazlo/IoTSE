#include "wifi_pmkid_parser.h"

#include <string.h>

/* =====================================================================
 *  Реализация извлечения PMKID из EAPOL-Key. Чистый разбор байт.
 * =====================================================================
 *
 * Карта смещений (всё относительно начала EAPOL-кадра `eapol`):
 *
 *   EAPOL-заголовок (4 байта):
 *     [0] protocol version
 *     [1] packet type        (3 = EAPOL-Key)
 *     [2..3] body length      (big-endian) — длина тела за заголовком
 *
 *   Тело EAPOL-Key (ключ = eapol + 4), фиксированная часть 95 байт:
 *     [0]      descriptor type
 *     [1..2]   key information  (big-endian)
 *     [3..4]   key length
 *     [5..12]  replay counter (8)
 *     [13..44] key nonce (32)
 *     [45..60] key IV (16)
 *     [61..68] key RSC (8)
 *     [69..76] key ID / reserved (8)
 *     [77..92] key MIC (16)           <-- допускаем классические 16 байт
 *     [93..94] key data length (big-endian)
 *     [95..]   key data (KDE...)      <-- здесь ищем PMKID-KDE
 */

#define EAPOL_HEADER_LEN            4U
#define EAPOL_TYPE_KEY              3U
#define EAPOL_KEY_FIXED_BODY_LEN    95U   /* фикс. часть тела до key data */
#define EAPOL_KEY_DATA_LEN_OFFSET   93U   /* смещение key_data_length в теле ключа */

/* Минимальная длина KDE PMKID: len-байт охватывает OUI(3)+type(1)+PMKID(16). */
#define PMKID_KDE_LEN               (4U + WIFI_PMKID_LEN) /* == 20 */
#define KDE_ELEMENT_ID              0xDDU                 /* vendor-specific KDE */

/* Прочитать 16-битное big-endian значение (EAPOL/802.11 — сетевой порядок байт). */
static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Пройти по KDE в Key Data и найти PMKID-KDE (OUI 00-0F-AC, type 0x04).
 * key_data указывает на начало области, length — её длина. */
static bool find_pmkid_kde(const uint8_t *key_data, size_t length,
                           uint8_t out_pmkid[WIFI_PMKID_LEN])
{
    const uint8_t *p = key_data;
    const uint8_t *end = key_data + length;

    /* Каждой итерации нужно минимум 2 байта: тег и длина. */
    while ((size_t)(end - p) >= 2U) {
        uint8_t id = p[0];
        uint8_t len = p[1];

        /* Все настоящие KDE начинаются с 0xDD; длина 0 — это паддинг/конец.
         * Как только видим не-KDE — дальше только заполнение, выходим. */
        if (id != KDE_ELEMENT_ID || len == 0U) {
            break;
        }
        /* Тело KDE (len байт) должно умещаться в оставшийся буфер. */
        if ((size_t)(end - p) < (size_t)(2U + len)) {
            break;
        }
        /* Строгая проверка именно PMKID-KDE: длина ровно 20, OUI 00-0F-AC,
         * type 0x04. Любое отклонение — это другой KDE, пропускаем. */
        if (len == PMKID_KDE_LEN &&
            p[2] == 0x00U && p[3] == 0x0FU && p[4] == 0xACU && p[5] == 0x04U) {
            memcpy(out_pmkid, p + 6, WIFI_PMKID_LEN);
            return true;
        }
        p += 2U + len; /* к следующему KDE */
    }
    return false;
}

bool wifi_pmkid_extract_from_eapol(const uint8_t *eapol, size_t eapol_len,
                                   uint8_t out_pmkid[WIFI_PMKID_LEN])
{
    if (eapol == NULL || out_pmkid == NULL) {
        return false;
    }
    /* --- EAPOL-заголовок --- */
    if (eapol_len < EAPOL_HEADER_LEN) {
        return false;
    }
    if (eapol[1] != EAPOL_TYPE_KEY) {
        return false; /* не EAPOL-Key — PMKID искать негде */
    }
    uint16_t body_len = read_be16(eapol + 2);
    if ((size_t)body_len > eapol_len - EAPOL_HEADER_LEN) {
        return false; /* тело не умещается в доступные байты */
    }

    /* --- Фиксированная часть тела ключа --- */
    if (body_len < EAPOL_KEY_FIXED_BODY_LEN) {
        return false;
    }
    const uint8_t *key = eapol + EAPOL_HEADER_LEN;
    uint16_t key_data_len = read_be16(key + EAPOL_KEY_DATA_LEN_OFFSET);

    /* Строгая сверка: тело = фикс. часть + key data, без «лишних» байт. */
    if ((size_t)body_len != (size_t)EAPOL_KEY_FIXED_BODY_LEN + (size_t)key_data_len) {
        return false;
    }
    /* Key data заведомо не вместит PMKID-KDE — выходим рано. На проводе KDE — это
     * 2 байта заголовка (тег+длина) плюс его тело PMKID_KDE_LEN. */
    if (key_data_len < 2U + PMKID_KDE_LEN) {
        return false;
    }

    const uint8_t *key_data = key + EAPOL_KEY_FIXED_BODY_LEN;
    return find_pmkid_kde(key_data, key_data_len, out_pmkid);
}
