#pragma once

/* =====================================================================
 *  TX FRAME BUILDER — сборка готовых 802.11-кадров в буфер
 * =====================================================================
 *
 * ГДЕ МЫ. Слой 1 (tx_hal_80211) умеет ОТПРАВИТЬ готовый буфер в эфир. Но этот
 * буфер кто-то должен СОБРАТЬ — байт в байт, по стандарту 802.11. Это и есть
 * задача этого слоя: на вход — параметры (имя сети, MAC, канал), на выход —
 * заполненный буфер кадра и его длина. Радио не трогаем: чистые функции.
 *
 * ПЕРВЫЙ КАДР — BEACON (маяк). Точки доступа периодически рассылают beacon, по
 * которому все вокруг видят сеть (SSID, канал, скорости, тип защиты). Если мы
 * соберём корректный beacon и будем его слать, наша «сеть» появится в списке
 * Wi-Fi на любом телефоне — удобно для проверки.
 *
 * СТРУКТУРА BEACON-КАДРА, которую мы собираем (всё без FCS — её добавит радио):
 *
 *   [ MAC-заголовок управления, 24 байта ]
 *     Frame Control (2)   — тип=management, подтип=beacon
 *     Duration (2)        — 0
 *     Address 1 / DA (6)  — получатель: широковещательный FF:FF:FF:FF:FF:FF
 *     Address 2 / SA (6)  — отправитель: наш MAC
 *     Address 3 / BSSID(6)— идентификатор сети: тоже наш MAC (для AP SA==BSSID)
 *     Sequence Control(2) — 0 (номер проставит система при отправке)
 *
 *   [ Фиксированные поля beacon, 12 байт ]
 *     Timestamp (8)       — 0 (служебное время, получателю для синхронизации)
 *     Beacon Interval (2) — как часто шлём маяк, в TU (1 TU = 1024 мкс)
 *     Capability Info (2) — флаги возможностей сети (у нас бит ESS)
 *
 *   [ Информационные элементы, формат TLV: тег(1) длина(1) значение(len) ]
 *     SSID (тег 0)            — имя сети
 *     Supported Rates (тег 1) — поддерживаемые скорости
 *     DS Parameter Set(тег 3) — номер канала (1 байт)
 *
 * ПОРЯДОК БАЙТ. Многобайтовые числовые поля 802.11 идут в эфире little-endian
 * (младший байт первым). Поэтому 16-битные поля мы пишем двумя байтами вручную —
 * так виден порядок, и код не зависит от порядка байт процессора.
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Максимальный размер, который может занять собранный beacon: заголовок 24 +
 * фиксированные 12 + SSID (2+32) + Supported Rates (2+8) + DS (2+1) = 83 байта.
 * Округлим вверх до 128 — удобный размер буфера под кадр с запасом. */
#define TX_BEACON_MAX_LEN 128U

/* Собрать beacon-кадр в out.
 *
 * out         — буфер, куда писать кадр;
 * out_size    — размер буфера out в байтах (нужен >= фактической длины кадра);
 * out_len     — сюда запишется фактическая длина собранного кадра;
 * source_mac  — 6 байт нашего MAC (станет и SA, и BSSID кадра);
 * ssid        — имя сети, строка 1..32 байта;
 * channel     — канал 1..13 (попадёт в DS Parameter Set).
 *
 * Возврат:
 *   ESP_OK               — кадр собран, длина в *out_len;
 *   ESP_ERR_INVALID_ARG  — NULL-аргумент, пустой/слишком длинный SSID или канал
 *                          вне 1..13;
 *   ESP_ERR_INVALID_SIZE — out_size мал для кадра.
 *
 * Чистая функция: только заполняет буфер, ничего не передаёт. */
esp_err_t tx_frame_build_beacon(uint8_t *out, size_t out_size, size_t *out_len,
                                const uint8_t source_mac[6],
                                const char *ssid, uint8_t channel);

/* =====================================================================
 *  DEAUTH — кадр деаутентификации (отключение клиента от точки доступа)
 * =====================================================================
 *
 * Deauth — это тоже management-кадр, но подтип 12 (у beacon был 8). Отличия от
 * beacon для разбора:
 *   - другой подтип в Frame Control;
 *   - нет информационных элементов; тело — всего 2 байта «Reason Code»;
 *   - адреса имеют СМЫСЛ: кадр будто бы от точки доступа клиенту.
 *
 * Структура (24 заголовок + 2 тело = 26 байт, без FCS):
 *   Frame Control (2)   — management, подтип deauth
 *   Duration (2)        — 0
 *   Address 1 / DA (6)  — КОМУ: MAC клиента (или broadcast — отключить всех)
 *   Address 2 / SA (6)  — ОТ КОГО: MAC точки (мы «от её имени»)
 *   Address 3 / BSSID(6)— сеть: MAC точки
 *   Sequence Control(2) — 0 (проставит система)
 *   Reason Code (2)     — причина, little-endian
 *
 * ТОЛЬКО для авторизованного тестирования на своём оборудовании.
 */

/* Deauth фиксирован по длине: 24 (заголовок) + 2 (reason) = 26 байт. */
#define TX_DEAUTH_LEN 26U

/* Собрать deauth-кадр.
 *
 * out/out_size/out_len — как у beacon-строителя;
 * bssid   — MAC точки доступа; идёт и отправителем (SA, Addr2), и как BSSID
 *           (Addr3): мы формируем кадр от имени точки;
 * target  — MAC клиента, которого отключаем (DA, Addr1). Чтобы отключить ВСЕХ
 *           сразу — передай широковещательный FF:FF:FF:FF:FF:FF;
 * reason_code — код причины 802.11 (например 1 = Unspecified, 7 = Class-3 frame
 *           from nonassociated STA). Пишется little-endian.
 *
 * Возврат: ESP_OK | ESP_ERR_INVALID_ARG (NULL) | ESP_ERR_INVALID_SIZE (<26).
 * Чистая функция: только заполняет буфер. */
esp_err_t tx_frame_build_deauth(uint8_t *out, size_t out_size, size_t *out_len,
                                const uint8_t bssid[6],
                                const uint8_t target[6],
                                uint16_t reason_code);

/* =====================================================================
 *  AUTH + ASSOC-REQ — кадры «клиента», нужные для активной добычи PMKID
 * =====================================================================
 *
 * ЗАЧЕМ. Чтобы получить PMKID «по кнопке» без чужого клиента (clientless), мы
 * сами прикидываемся станцией: шлём точке Open-System Authentication, затем
 * Association Request с её параметрами защиты (RSN-IE). В ответ точка начинает
 * хендшейк и присылает нам M1 с PMKID — его ловит существующий приёмный тракт.
 *
 * Это management-кадры:
 *   auth      — подтип 11 (0xB0 в первом байте Frame Control);
 *   assoc-req — подтип 0  (0x00).
 * Адреса в обоих: Addr1=DA=точка, Addr2=SA=наш MAC, Addr3=BSSID=точка — то есть
 * кадр реально ОТ НАС (в отличие от deauth, где мы маскируемся под точку).
 *
 * ТОЛЬКО для авторизованного тестирования на своём оборудовании.
 */

/* Auth фиксирован: 24 (заголовок) + 6 (тело: algo+seq+status) = 30 байт. */
#define TX_AUTH_LEN 30U

/* Собрать Open-System Authentication (seq 1) от нашего имени к точке.
 *   bssid      — MAC точки (Addr1 и Addr3);
 *   source_mac — наш MAC (Addr2).
 * Возврат: ESP_OK | ESP_ERR_INVALID_ARG (NULL) | ESP_ERR_INVALID_SIZE (<30). */
esp_err_t tx_frame_build_auth(uint8_t *out, size_t out_size, size_t *out_len,
                              const uint8_t bssid[6],
                              const uint8_t source_mac[6]);

/* Максимум для assoc-req: 24 + cap(2) + listen(2) + SSID(2+32) + Rates(2+8)
 * + RSN-IE(2+20) = 94 байта. Округляем до 128. */
#define TX_ASSOC_REQ_MAX_LEN 128U

/* ВЫБОР ЗАЩИТЫ ДЛЯ ASSOC-REQ.
 *
 * Клиент в Association Request заявляет РОВНО ПО ОДНОМУ выбранному набору: один
 * group cipher, один pairwise cipher и один AKM (из тех, что рекламирует точка).
 * Селекторы 802.11 — это 4 байта «OUI(3)+type(1)», например 00-0F-AC-04 = CCMP.
 *
 * Чтобы этот слой НЕ зависел от парсера RSN (wifi_rsn_parser), берём селекторы как
 * СЫРЫЕ 4 байта: у парсера они уже лежат готовыми (wifi_cipher_suite_t.raw и
 * wifi_akm_suite_t.raw), и вызывающий (движок) просто копирует их сюда. Так RSN-IE
 * в нашем assoc совпадает с тем, что реально заявляет целевая точка. */
typedef struct {
    uint16_t version;             /* версия RSN, обычно 1 */
    uint8_t  group_cipher[4];     /* сырой селектор group cipher */
    uint8_t  pairwise_cipher[4];  /* ОДИН выбранный pairwise cipher */
    uint8_t  akm[4];              /* ОДИН выбранный AKM */
    uint16_t rsn_capabilities;    /* RSN Capabilities как у сети (PMF-биты и т.п.) */
} tx_rsn_selection_t;

/* Собрать Association Request от нашего имени к точке.
 *   bssid      — MAC точки (Addr1 и Addr3);
 *   source_mac — наш MAC (Addr2);
 *   ssid       — имя целевой сети, 1..32 байта (должно совпадать с сетью точки);
 *   rsn        — выбор защиты для RSN-IE. Если NULL — подставляется дефолт
 *                WPA2-PSK/CCMP (group/pairwise CCMP, AKM PSK, caps 0): удобный
 *                запасной вариант для типовой цели. Для не-типовых сетей движок
 *                заполняет rsn сырыми селекторами реальной точки (из ap_tracker).
 *
 * Возврат: ESP_OK | ESP_ERR_INVALID_ARG (NULL/плохой SSID) | ESP_ERR_INVALID_SIZE. */
esp_err_t tx_frame_build_assoc_req(uint8_t *out, size_t out_size, size_t *out_len,
                                   const uint8_t bssid[6],
                                   const uint8_t source_mac[6],
                                   const char *ssid,
                                   const tx_rsn_selection_t *rsn);

#ifdef __cplusplus
}
#endif
