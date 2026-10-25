#include "tx_frame_builder.h"

#include <string.h>

/* Теги информационных элементов (IE) из стандарта 802.11. Формат IE: один байт
 * тега, один байт длины значения, затем само значение. */
#define IE_TAG_SSID            0U
#define IE_TAG_SUPPORTED_RATES 1U
#define IE_TAG_DS_PARAMETER    3U
#define IE_TAG_RSN             48U /* 0x30 — Robust Security Network (WPA2) */

esp_err_t tx_frame_build_beacon(uint8_t *out, size_t out_size, size_t *out_len,
                                const uint8_t source_mac[6],
                                const char *ssid, uint8_t channel)
{
    /* --- Шаг 1. Проверяем вход ------------------------------------------- */

    if (out == NULL || out_len == NULL || source_mac == NULL || ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* strnlen с лимитом 33: если имя не кончилось в 32 байтах, оно слишком
     * длинное для поля SSID (максимум 32). */
    size_t ssid_len = strnlen(ssid, 33U);
    if (ssid_len < 1U || ssid_len > 32U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel < 1U || channel > 13U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Supported Rates — набор поддерживаемых скоростей. Каждый байт: старший бит
     * 0x80 помечает скорость как "basic" (обязательную), остальные 7 бит — это
     * скорость в единицах 500 кбит/с. Значения ниже:
     *   0x82 = 1,   0x84 = 2,   0x8b = 5.5, 0x96 = 11 Мбит/с (basic);
     *   0x0c = 6,   0x12 = 9,   0x18 = 12,  0x24 = 18 Мбит/с.
     * const-таблица: это данные, а не состояние. */
    static const uint8_t supported_rates[8] = {
        0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
    };

    /* --- Шаг 2. Считаем нужную длину и проверяем, что буфер её вмещает ---- */

    size_t total = 24U                        /* MAC-заголовок */
                 + 12U                        /* фиксированные поля beacon */
                 + 2U + ssid_len              /* IE SSID: тег+длина+значение */
                 + 2U + sizeof(supported_rates) /* IE Supported Rates */
                 + 2U + 1U;                   /* IE DS Parameter Set (1 байт) */
    if (out_size < total) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* --- Шаг 3. Пишем кадр. p — курсор: *p++ кладёт байт и сдвигает курсор. - */

    uint8_t *p = out;

    /* MAC-заголовок управления (24 байта). */

    /* Frame Control (2 байта).
     * Первый байт = подтип(4 бита)<<4 | тип(2)<<2 | версия(2).
     *   тип management = 00, подтип beacon = 1000b = 8, версия = 0
     *   → (8<<4)|(0<<2)|0 = 0x80. */
    *p++ = 0x80;
    *p++ = 0x00; /* второй байт Frame Control — флаги, нам все нули */

    /* Duration/ID (2 байта) — для beacon не используется, 0. */
    *p++ = 0x00;
    *p++ = 0x00;

    /* Address 1 = получатель (DA). Beacon адресуется всем → широковещательный. */
    memset(p, 0xff, 6);
    p += 6;

    /* Address 2 = отправитель (SA) = наш MAC. */
    memcpy(p, source_mac, 6);
    p += 6;

    /* Address 3 = BSSID (идентификатор сети). У точки доступа BSSID совпадает с
     * её собственным MAC, поэтому снова source_mac. */
    memcpy(p, source_mac, 6);
    p += 6;

    /* Sequence Control (2 байта) = 0. Реальный номер последовательности проставит
     * система при отправке (мы вызовем tx_hal с use_system_sequence = true). */
    *p++ = 0x00;
    *p++ = 0x00;

    /* Фиксированные поля beacon (12 байт). */

    /* Timestamp (8 байт) — счётчик времени точки, получателю для синхронизации.
     * Нам достаточно нулей. */
    memset(p, 0x00, 8);
    p += 8;

    /* Beacon Interval (2 байта) — как часто точка шлёт маяк, в TU (1 TU ≈ 1024
     * мкс). 100 TU ≈ 102.4 мс. Пишем little-endian: младший байт первым.
     * 100 = 0x0064 → байты 0x64, 0x00. */
    *p++ = 0x64;
    *p++ = 0x00;

    /* Capability Info (2 байта) — флаги возможностей сети. Бит 0 = ESS: сеть
     * инфраструктурная (есть точка доступа). 0x0001 → little-endian 0x01,0x00. */
    *p++ = 0x01;
    *p++ = 0x00;

    /* Информационные элементы (TLV: тег, длина, значение). */

    /* IE SSID (тег 0): имя сети. */
    *p++ = IE_TAG_SSID;
    *p++ = (uint8_t)ssid_len;
    memcpy(p, ssid, ssid_len);
    p += ssid_len;

    /* IE Supported Rates (тег 1): таблица скоростей. */
    *p++ = IE_TAG_SUPPORTED_RATES;
    *p++ = (uint8_t)sizeof(supported_rates);
    memcpy(p, supported_rates, sizeof(supported_rates));
    p += sizeof(supported_rates);

    /* IE DS Parameter Set (тег 3): один байт — номер канала. По нему сканеры
     * понимают, на каком канале работает сеть. */
    *p++ = IE_TAG_DS_PARAMETER;
    *p++ = 1U;
    *p++ = channel;

    /* --- Шаг 4. Длина кадра = сколько байт прошёл курсор. ----------------- */

    /* По построению (p - out) совпадает с total из шага 2. */
    *out_len = (size_t)(p - out);
    return ESP_OK;
}

esp_err_t tx_frame_build_deauth(uint8_t *out, size_t out_size, size_t *out_len,
                                const uint8_t bssid[6], const uint8_t target[6],
                                uint16_t reason_code)
{
    /* --- Шаг 1. Проверяем вход. --- */
    if (out == NULL || out_len == NULL || bssid == NULL || target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_size < TX_DEAUTH_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* --- Шаг 2. Пишем кадр по курсору p. --- */
    uint8_t *p = out;

    /* Frame Control (2 байта).
     * Первый байт = подтип<<4 | тип<<2 | версия.
     *   тип management = 00, подтип deauth = 1100b = 12, версия = 0
     *   → (12<<4)|(0<<2)|0 = 0xC0.  (у beacon здесь было 0x80 — подтип 8). */
    *p++ = 0xC0;
    *p++ = 0x00; /* флаги — нули */

    /* Duration/ID (2 байта) — не используется, 0. */
    *p++ = 0x00;
    *p++ = 0x00;

    /* Address 1 = DA = КОМУ шлём: клиент (или broadcast FF.. — всем). */
    memcpy(p, target, 6);
    p += 6;

    /* Address 2 = SA = ОТ КОГО: MAC точки доступа (кадр «от её имени»). */
    memcpy(p, bssid, 6);
    p += 6;

    /* Address 3 = BSSID = идентификатор сети = та же точка. */
    memcpy(p, bssid, 6);
    p += 6;

    /* Sequence Control (2 байта) = 0 (номер проставит система при отправке). */
    *p++ = 0x00;
    *p++ = 0x00;

    /* Тело deauth: Reason Code (2 байта), little-endian — младший байт первым. */
    *p++ = (uint8_t)(reason_code & 0xFFU);
    *p++ = (uint8_t)(reason_code >> 8);

    /* --- Шаг 3. Длина = пройденные курсором байты (должно быть 26). --- */
    *out_len = (size_t)(p - out);
    return ESP_OK;
}

esp_err_t tx_frame_build_auth(uint8_t *out, size_t out_size, size_t *out_len,
                              const uint8_t bssid[6],
                              const uint8_t source_mac[6])
{
    /* --- Шаг 1. Проверяем вход. --- */
    if (out == NULL || out_len == NULL || bssid == NULL || source_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_size < TX_AUTH_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* --- Шаг 2. Пишем кадр по курсору p. --- */
    uint8_t *p = out;

    /* Frame Control (2 байта).
     * Первый байт = подтип<<4 | тип<<2 | версия.
     *   тип management = 00, подтип authentication = 1011b = 11, версия = 0
     *   → (11<<4)|(0<<2)|0 = 0xB0.  (deauth был 0xC0, beacon 0x80.) */
    *p++ = 0xB0;
    *p++ = 0x00; /* флаги — нули (кадр от станции точке, To/From DS = 0) */

    /* Duration/ID (2 байта) — проставит система, нам 0. */
    *p++ = 0x00;
    *p++ = 0x00;

    /* В ОТЛИЧИЕ от deauth кадр реально ОТ НАС: мы — станция, точка — получатель.
     * Address 1 = DA = КОМУ: точка доступа. */
    memcpy(p, bssid, 6);
    p += 6;

    /* Address 2 = SA = ОТ КОГО: наш MAC. */
    memcpy(p, source_mac, 6);
    p += 6;

    /* Address 3 = BSSID = идентификатор сети = та же точка. */
    memcpy(p, bssid, 6);
    p += 6;

    /* Sequence Control (2 байта) = 0 (номер проставит система при отправке). */
    *p++ = 0x00;
    *p++ = 0x00;

    /* Тело authentication (6 байт), все поля little-endian:
     *   Auth Algorithm Number (2) = 0 → Open System (без пароля на этом этапе);
     *   Auth Transaction Seq Number (2) = 1 → это ПЕРВЫЙ кадр обмена (от станции);
     *   Status Code (2) = 0 → в запросе всегда 0 (успех заполняет точка в ответе). */
    *p++ = 0x00; *p++ = 0x00; /* algorithm = 0 (open system) */
    *p++ = 0x01; *p++ = 0x00; /* transaction seq = 1 */
    *p++ = 0x00; *p++ = 0x00; /* status code = 0 */

    /* --- Шаг 3. Длина = пройденные курсором байты (должно быть 30). --- */
    *out_len = (size_t)(p - out);
    return ESP_OK;
}

esp_err_t tx_frame_build_assoc_req(uint8_t *out, size_t out_size, size_t *out_len,
                                   const uint8_t bssid[6],
                                   const uint8_t source_mac[6],
                                   const char *ssid,
                                   const tx_rsn_selection_t *rsn)
{
    /* --- Шаг 1. Проверяем вход. --- */
    if (out == NULL || out_len == NULL || bssid == NULL ||
        source_mac == NULL || ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strnlen(ssid, 33U);
    if (ssid_len < 1U || ssid_len > 32U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Те же скорости, что в beacon (точке достаточно базового набора). */
    static const uint8_t supported_rates[8] = {
        0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
    };

    /* Дефолт на случай rsn == NULL: WPA2-PSK / CCMP — типовая цель PMKID.
     * OUI 00-0F-AC — назначенный IEEE суффикс наборов шифров/AKM RSN.
     * ...-04 = CCMP (шифр), ...-02 = PSK (AKM). */
    static const tx_rsn_selection_t default_wpa2_psk = {
        .version          = 1U,
        .group_cipher     = { 0x00, 0x0f, 0xac, 0x04 }, /* CCMP */
        .pairwise_cipher  = { 0x00, 0x0f, 0xac, 0x04 }, /* CCMP */
        .akm              = { 0x00, 0x0f, 0xac, 0x02 }, /* PSK */
        .rsn_capabilities = 0U,
    };
    const tx_rsn_selection_t *sel = (rsn != NULL) ? rsn : &default_wpa2_psk;

    /* RSN-IE собираем из выбора sel. Значение всегда 20 байт (один group, один
     * pairwise, один AKM), формат (всё многобайтовое — little-endian):
     *   Version (2) | Group Cipher (4) | Pairwise Count (2)=1 | Pairwise (4)
     *   | AKM Count (2)=1 | AKM (4) | RSN Capabilities (2).
     * Станция в assoc заявляет РОВНО ПО ОДНОМУ набору — поэтому count-ы = 1, а сами
     * селекторы берём сырыми байтами (совпадают с тем, что рекламирует точка). */
    uint8_t rsn_ie_value[20];
    {
        uint8_t *r = rsn_ie_value;
        *r++ = (uint8_t)(sel->version & 0xFFU);
        *r++ = (uint8_t)(sel->version >> 8);
        memcpy(r, sel->group_cipher, 4); r += 4;
        *r++ = 0x01; *r++ = 0x00;                 /* pairwise count = 1 */
        memcpy(r, sel->pairwise_cipher, 4); r += 4;
        *r++ = 0x01; *r++ = 0x00;                 /* AKM count = 1 */
        memcpy(r, sel->akm, 4); r += 4;
        *r++ = (uint8_t)(sel->rsn_capabilities & 0xFFU);
        *r++ = (uint8_t)(sel->rsn_capabilities >> 8);
    }

    /* --- Шаг 2. Считаем длину и проверяем буфер. --- */
    size_t total = 24U                           /* MAC-заголовок */
                 + 2U + 2U                        /* Capability Info + Listen Interval */
                 + 2U + ssid_len                  /* IE SSID */
                 + 2U + sizeof(supported_rates)   /* IE Supported Rates */
                 + 2U + sizeof(rsn_ie_value);     /* IE RSN */
    if (out_size < total) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* --- Шаг 3. Пишем кадр по курсору p. --- */
    uint8_t *p = out;

    /* Frame Control: тип management = 00, подтип assoc-request = 0000b = 0, версия 0
     * → (0<<4)|(0<<2)|0 = 0x00. */
    *p++ = 0x00;
    *p++ = 0x00; /* флаги — нули */

    /* Duration/ID (2) = 0. */
    *p++ = 0x00;
    *p++ = 0x00;

    /* Адреса — кадр ОТ НАС точке (как в auth). */
    memcpy(p, bssid, 6);       p += 6; /* Addr1 = DA = точка */
    memcpy(p, source_mac, 6);  p += 6; /* Addr2 = SA = наш MAC */
    memcpy(p, bssid, 6);       p += 6; /* Addr3 = BSSID = точка */

    /* Sequence Control (2) = 0 (проставит система). */
    *p++ = 0x00;
    *p++ = 0x00;

    /* Фиксированные поля assoc-request.
     * Capability Info (2, LE): бит0 ESS + бит4 Privacy (сеть с шифрованием).
     *   0x0011 → байты 0x11, 0x00. */
    *p++ = 0x11;
    *p++ = 0x00;

    /* Listen Interval (2, LE): как часто станция «просыпается» слушать (в beacon
     * intervals). 10 → 0x000A → байты 0x0A, 0x00. */
    *p++ = 0x0A;
    *p++ = 0x00;

    /* IE SSID (тег 0): должен совпадать с именем целевой сети. */
    *p++ = IE_TAG_SSID;
    *p++ = (uint8_t)ssid_len;
    memcpy(p, ssid, ssid_len);
    p += ssid_len;

    /* IE Supported Rates (тег 1). */
    *p++ = IE_TAG_SUPPORTED_RATES;
    *p++ = (uint8_t)sizeof(supported_rates);
    memcpy(p, supported_rates, sizeof(supported_rates));
    p += sizeof(supported_rates);

    /* IE RSN (тег 48): заявляем WPA2-PSK/CCMP. */
    *p++ = IE_TAG_RSN;
    *p++ = (uint8_t)sizeof(rsn_ie_value);
    memcpy(p, rsn_ie_value, sizeof(rsn_ie_value));
    p += sizeof(rsn_ie_value);

    /* --- Шаг 4. Длина = пройденные курсором байты. --- */
    *out_len = (size_t)(p - out);
    return ESP_OK;
}
