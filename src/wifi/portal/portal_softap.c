#include "portal_softap.h"

#include <stdbool.h>
#include <string.h>

esp_err_t portal_softap_build_config(wifi_config_t *out,
                                     const char *ssid,
                                     const char *password,
                                     uint8_t channel,
                                     uint8_t max_connections)
{
    /* --- Шаг 1. Проверяем вход ------------------------------------------- */

    if (out == NULL || ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* strnlen с ограничением: не читаем дальше максимально допустимой длины + 1.
     * Если строка не закончилась в пределах лимита — считаем её слишком длинной.
     * +1 нужен, чтобы отличить "ровно максимум" от "ещё длиннее". */
    size_t ssid_len = strnlen(ssid, PORTAL_SOFTAP_SSID_MAX_LEN + 1U);
    if (ssid_len < PORTAL_SOFTAP_SSID_MIN_LEN ||
        ssid_len > PORTAL_SOFTAP_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Пароль: NULL или пустая строка => открытая сеть. Иначе проверяем длину по
     * правилам WPA2-PSK (8..63). Отдельная переменная open фиксирует решение,
     * чтобы ниже выбрать authmode и не копировать пароль в открытую сеть. */
    size_t pass_len = (password != NULL)
        ? strnlen(password, PORTAL_SOFTAP_PASS_MAX_LEN + 1U)
        : 0U;
    bool open_network = (pass_len == 0U);
    if (!open_network &&
        (pass_len < PORTAL_SOFTAP_PASS_MIN_LEN ||
         pass_len > PORTAL_SOFTAP_PASS_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Канал точки доступа. 14-й канал в большинстве регионов запрещён и только
     * для DSSS, поэтому для AP ограничиваемся безопасным диапазоном 1..13. */
    if (channel < 1U || channel > 13U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (max_connections < 1U ||
        max_connections > PORTAL_SOFTAP_MAX_CONNECTIONS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* --- Шаг 2. Обнуляем всю структуру ---------------------------------- */

    /* wifi_config_t — union (.sta/.ap на одной памяти). Полный обнуление
     * гарантирует, что все поля, которые мы НЕ трогаем явно (ssid_hidden,
     * pairwise_cipher, ftm_responder и т.д.), будут предсказуемыми нулями. */
    memset(out, 0, sizeof(*out));

    /* --- Шаг 3. Заполняем ветку .ap ------------------------------------- */

    /* SSID. Копируем ровно ssid_len байт и отдельно сообщаем драйверу длину в
     * ssid_len: так имя может содержать любые байты и не обязано быть
     * null-терминированным внутри поля фиксированного размера. */
    memcpy(out->ap.ssid, ssid, ssid_len);
    out->ap.ssid_len = (uint8_t)ssid_len;

    /* Канал вещания. */
    out->ap.channel = channel;

    /* Сколько клиентов пускаем одновременно. */
    out->ap.max_connection = max_connections;

    /* Тип защиты и пароль.
     *   - открытая сеть: authmode = OPEN, пароль оставляем нулевым (memset уже
     *     обнулил), копировать нечего;
     *   - защищённая: authmode = WPA2-PSK, копируем пароль. */
    if (open_network) {
        out->ap.authmode = WIFI_AUTH_OPEN;
    } else {
        out->ap.authmode = WIFI_AUTH_WPA2_PSK;
        memcpy(out->ap.password, password, pass_len);
        /* PMF (Protected Management Frames): для WPA2 объявляем, что умеем его
         * (capable), но не требуем (required=false) — иначе старые клиенты не
         * подключатся. Для открытой сети PMF неприменим. */
        out->ap.pmf_cfg.capable = true;
        out->ap.pmf_cfg.required = false;
    }

    /* Интервал маяков (beacon). 100 — стандартное значение в "time units" (TU),
     * 1 TU = 1024 мкс, то есть маяк примерно каждые 102.4 мс. Это то, что видят
     * сканеры чужих устройств как нашу сеть. */
    out->ap.beacon_interval = 100;

    /* ssid_hidden = 0 (из memset) — сеть видима в списке. Для портала это норма:
     * пользователь должен её найти и подключиться. */

    return ESP_OK;
}
