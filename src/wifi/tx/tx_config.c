#include "tx_config.h"

#include <string.h>

#include "nvs.h"      /* nvs_open/set_blob/get_blob/commit/close */
#include "esp_log.h"  /* ESP_LOGW при ошибке записи */

static const char *TAG = "TX_CONFIG";

/* --- Формат хранения в NVS ---
 * Namespace и ключ — короткие строки (у NVS есть лимит длины имени). Пишем ОДНОЙ
 * версионированной записью (blob), а не набором отдельных ключей: так все поля
 * сохраняются/читаются атомарно и легко расширять формат, подняв version. */
#define TX_CFG_NS       "tx_cfg"          /* namespace настроек TX */
#define TX_CFG_KEY      "cfg"             /* ключ записи внутри namespace */
#define TX_CFG_MAGIC    0x54584346UL      /* 'T''X''C''F' — метка «это наш формат» */
#define TX_CFG_VERSION  1U                /* версия раскладки записи */

/* Запись на диск. packed — чтобы раскладка байт не зависела от выравнивания
 * компилятора (иначе blob мог бы «поехать» между сборками). ssid_len хранит
 * фактическую длину имени (без NUL) — по ней проверяем целостность при чтении. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  ssid_len;                          /* 1..TX_CONFIG_SSID_MAX_LEN */
    uint16_t deauth_reason;
    uint8_t  burst_count;
    uint8_t  channel;
    char     ssid[TX_CONFIG_SSID_MAX_LEN + 1U]; /* имя сети + NUL */
} tx_config_record_t;

/* Фиксируем ожидаемый размер: если кто-то изменит поля, сборка упадёт здесь, а не
 * тихо испортит хранимый формат. 4+1+1+2+1+1+33 = 43. */
_Static_assert(sizeof(tx_config_record_t) == 43U, "tx_config NVS layout");

/* =====================================================================
 *  TX CONFIG — реализация модели настроек пакета
 * =====================================================================
 *
 * Здесь всего два «секрета»:
 *   1) единственный экземпляр настроек s_config — живёт всю жизнь программы;
 *   2) он СРАЗУ инициализирован значениями по умолчанию прямо в объявлении,
 *      поэтому отдельный вызов init при старте не нужен — модель готова к работе
 *      с первой же секунды.
 */

/* Единственный экземпляр настроек. static — виден только внутри этого файла:
 * снаружи к нему ходят лишь через геттеры/сеттеры (инкапсуляция). Инициализатор
 * задаёт дефолты по тем же константам, что объявлены в заголовке — один источник
 * правды, без дублирования чисел. */
static tx_config_t s_config = {
    .ssid          = TX_CONFIG_SSID_DEFAULT,
    .deauth_reason = TX_CONFIG_REASON_DEFAULT,
    .burst_count   = TX_CONFIG_BURST_DEFAULT,
    .channel       = TX_CONFIG_CHANNEL_DEFAULT,
};

void tx_config_reset_defaults(void)
{
    /* Переинициализируем тем же набором дефолтов. Используем временную константу
     * и копируем её целиком — так не забудем ни одного поля, если их станет
     * больше. */
    static const tx_config_t defaults = {
        .ssid          = TX_CONFIG_SSID_DEFAULT,
        .deauth_reason = TX_CONFIG_REASON_DEFAULT,
        .burst_count   = TX_CONFIG_BURST_DEFAULT,
        .channel       = TX_CONFIG_CHANNEL_DEFAULT,
    };
    s_config = defaults;
}

/* --- Геттеры --- */

const char *tx_config_ssid(void)
{
    return s_config.ssid;
}

uint16_t tx_config_deauth_reason(void)
{
    return s_config.deauth_reason;
}

uint8_t tx_config_burst_count(void)
{
    return s_config.burst_count;
}

uint8_t tx_config_channel(void)
{
    return s_config.channel;
}

/* --- Сеттеры (с проверкой границ) --- */

bool tx_config_set_ssid(const char *ssid)
{
    /* Шаг 1. Отвергаем то, из чего нельзя сделать корректный beacon. */
    if (ssid == NULL) {
        return false;
    }
    /* Длину считаем безопасно: strnlen не пойдёт дальше (MAX+1) байт, даже если
     * строка не завершена NUL в пределах нашего интереса. */
    size_t len = strnlen(ssid, TX_CONFIG_SSID_MAX_LEN + 1U);
    if (len == 0U) {
        return false; /* пустое имя не принимаем */
    }

    /* Шаг 2. Слишком длинное имя не ошибка — просто обрезаем до максимума. */
    if (len > TX_CONFIG_SSID_MAX_LEN) {
        len = TX_CONFIG_SSID_MAX_LEN;
    }

    /* Шаг 3. Копируем ровно len байт и ставим завершающий NUL вручную. memcpy
     * (а не strncpy) берём намеренно: длину мы уже посчитали и зажали, так нагляднее
     * видно, что и сколько копируется, без сюрпризов с добивкой нулями. */
    memcpy(s_config.ssid, ssid, len);
    s_config.ssid[len] = '\0';
    return true;
}

void tx_config_set_deauth_reason(uint16_t reason)
{
    if (reason < TX_CONFIG_REASON_MIN) {
        reason = TX_CONFIG_REASON_MIN;
    } else if (reason > TX_CONFIG_REASON_MAX) {
        reason = TX_CONFIG_REASON_MAX;
    }
    s_config.deauth_reason = reason;
}

void tx_config_set_burst_count(uint8_t count)
{
    if (count < TX_CONFIG_BURST_MIN) {
        count = TX_CONFIG_BURST_MIN;
    } else if (count > TX_CONFIG_BURST_MAX) {
        count = TX_CONFIG_BURST_MAX;
    }
    s_config.burst_count = count;
}

void tx_config_set_channel(uint8_t channel)
{
    /* 0 = «авто» — особое допустимое значение, его не трогаем. Иначе зажимаем в
     * диапазон реальных каналов 2.4 ГГц. */
    if (channel != TX_CONFIG_CHANNEL_AUTO) {
        if (channel < TX_CONFIG_CHANNEL_MIN) {
            channel = TX_CONFIG_CHANNEL_MIN;
        } else if (channel > TX_CONFIG_CHANNEL_MAX) {
            channel = TX_CONFIG_CHANNEL_MAX;
        }
    }
    s_config.channel = channel;
}

/* --- Сохранение в NVS --- */

/* Проверка прочитанной записи: наша метка, знакомая версия и корректное имя
 * (длина в границах и ровно там стоит завершающий NUL). Так битый/чужой blob не
 * попадёт в настройки. */
static bool record_valid(const tx_config_record_t *r)
{
    return r->magic == TX_CFG_MAGIC &&
           r->version == TX_CFG_VERSION &&
           r->ssid_len >= 1U && r->ssid_len <= TX_CONFIG_SSID_MAX_LEN &&
           r->ssid[r->ssid_len] == '\0' &&
           strnlen(r->ssid, (size_t)r->ssid_len + 1U) == r->ssid_len;
}

esp_err_t tx_config_save(void)
{
    /* Собираем запись из текущих настроек. Инициализируем нулями, чтобы «хвост»
     * буфера ssid не содержал мусора стека в сохранённом blob. */
    tx_config_record_t rec = {0};
    rec.magic   = TX_CFG_MAGIC;
    rec.version = TX_CFG_VERSION;

    size_t len = strnlen(s_config.ssid, TX_CONFIG_SSID_MAX_LEN);
    rec.ssid_len = (uint8_t)len;
    memcpy(rec.ssid, s_config.ssid, len);
    rec.ssid[len] = '\0';

    rec.deauth_reason = s_config.deauth_reason;
    rec.burst_count   = s_config.burst_count;
    rec.channel       = s_config.channel;

    /* Открываем namespace на запись, кладём blob, фиксируем. Хэндл закрываем в
     * любом случае. Ошибку логируем именем кода (без значений настроек). */
    nvs_handle_t h = 0;
    esp_err_t e = nvs_open(TX_CFG_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, TX_CFG_KEY, &rec, sizeof(rec));
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    if (h) {
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(e));
    }
    return e;
}

esp_err_t tx_config_load(void)
{
    /* Namespace могли ещё ни разу не создать — тогда nvs_open(READONLY) вернёт
     * ESP_ERR_NVS_NOT_FOUND. Это штатно: просто остаёмся на дефолтах. */
    nvs_handle_t h = 0;
    esp_err_t e = nvs_open(TX_CFG_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        return e;
    }

    tx_config_record_t rec = {0};
    size_t n = sizeof(rec);
    e = nvs_get_blob(h, TX_CFG_KEY, &rec, &n);
    nvs_close(h);
    if (e != ESP_OK) {
        return e; /* в т.ч. NOT_FOUND — записи нет, дефолты остаются */
    }
    if (n != sizeof(rec) || !record_valid(&rec)) {
        return ESP_ERR_INVALID_STATE; /* размер/формат не тот — дефолты остаются */
    }

    /* Применяем через сеттеры: они — единственный источник правды о границах, так
     * мы не продублируем здесь проверки диапазонов. SSID уже проверен record_valid. */
    tx_config_set_ssid(rec.ssid);
    tx_config_set_deauth_reason(rec.deauth_reason);
    tx_config_set_burst_count(rec.burst_count);
    tx_config_set_channel(rec.channel);
    return ESP_OK;
}
