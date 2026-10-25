/* ============================================================================
 *  subghz_rmt_raw.c — УЧЕБНО ПРОКОММЕНТИРОВАННАЯ ВЕРСИЯ
 *
 *  Назначение модуля: захватить "сырой" демодулированный поток, который CC1101
 *  выдаёт ногой GDO0, с помощью аппаратного блока RMT (Remote Control) ESP32-S3
 *  и вывести тайминги фронтов в лог. Это первый шаг конвейера Sub-GHz: отсюда
 *  потом рождаются .sub-файлы.
 *
 *  Как читать этот файл с точки зрения Си:
 *    - здесь нет ни одного malloc/free: вся память либо статическая (на весь
 *      срок жизни программы), либо локальная (на стеке задачи). Для встроенных
 *      систем это осознанный выбор — предсказуемость важнее гибкости.
 *    - модуль разделён на два "мира": контекст прерывания (ISR) и обычная
 *      задача (task). Между ними — очередь FreeRTOS. Понимание, какой код в
 *      каком мире живёт, — ключ ко всему файлу.
 * ==========================================================================*/

#include "subghz_rmt_raw.h"   /* собственный публичный интерфейс модуля      */

/* Стандартная библиотека Си. Подключаем ровно то, что реально используем: */
#include <stdio.h>   /* snprintf — безопасное форматирование в буфер        */
#include <string.h>  /* memcpy   — побайтовое копирование памяти            */

/* Заголовки ESP-IDF (драйверы и ОС). Угловые скобки/кавычки тут не про Си-
 * семантику, а про то, где компилятор ищет файл; для фреймворка принято "". */
#include "driver/rmt_rx.h"         /* API аппаратного приёмника RMT          */
#include "esp_log.h"               /* ESP_LOGI/W/E — логирование по уровням  */
#include "freertos/FreeRTOS.h"     /* базовые типы ОС (TickType_t, BaseType_t)*/
#include "freertos/queue.h"        /* очередь сообщений между ISR и задачей   */
#include "freertos/task.h"         /* создание/удаление задач                */

#include "subghz_hal_cc1101.h"     /* чтобы спросить у HAL про GDO0 и режим RX*/

/* 'static' на глобальном уровне = внутреннее связывание (internal linkage):
 * имя видно только в этом .c-файле и не конфликтует с такими же именами в
 * других модулях. Это basic-приём инкапсуляции в Си — аналог private. */
static const char *TAG = "subghz_raw";

/* -------------------------------------------------------------------------
 * Константы конфигурации. #define — это текстовая подстановка препроцессором
 * ДО компиляции: нигде не занимает памяти, не имеет типа и области видимости
 * в обычном смысле. Суффикс 'U' делает литерал беззнаковым (unsigned), что
 * убирает предупреждения при сравнении с беззнаковыми переменными.
 * ------------------------------------------------------------------------- */

/* 1 тик = 1 мкс: длительности в логе читаются прямо как микросекунды, а
 * 15-битное поле длительности RMT (макс. 32767 тиков) покрывает любой OOK-символ. */
#define SUBGHZ_RMT_RAW_RESOLUTION_HZ   1000000UL
/* ESP32-S3: один RMT-блок памяти = 48 символов; 4 блока = 192 символа — это
 * максимум для одного RX-канала. Статические буферы лежат во внутренней RAM,
 * как того требует RMT. */
#define SUBGHZ_RMT_RAW_MEM_SYMBOLS     192U
#define SUBGHZ_RMT_RAW_BUF_COUNT       2U
/* Пачка (burst) считается законченной после >10 мс тишины на линии — типовые
 * 433 МГц-пульты повторяют посылку с паузами 10..30 мс, поэтому одна пачка ~=
 * одной передаче кадра. */
#define SUBGHZ_RMT_RAW_IDLE_NS         10000000UL
/* Аппаратный фильтр "дребезга": игнорировать фронты короче 1 мкс. */
#define SUBGHZ_RMT_RAW_GLITCH_NS       1000UL

#define SUBGHZ_RMT_RAW_QUEUE_LEN       4U
#define SUBGHZ_RMT_RAW_TASK_STACK      4096U
#define SUBGHZ_RMT_RAW_TASK_PRIO       2U
/* Пачки с меньшим числом полупериодов — это шум/одиночные фронты, а не кадры;
 * они идут только в статистику, чтобы лог в шумной комнате оставался читаемым. */
#define SUBGHZ_RMT_RAW_MIN_FRAME_SYMS  8U
#define SUBGHZ_RMT_RAW_VERBOSE_BURSTS  3U
#define SUBGHZ_RMT_RAW_LOG_HALVES      24U
#define SUBGHZ_RMT_RAW_STATS_MS        10000U

/* Структура-сообщение, которую ISR кладёт в очередь, а задача забирает.
 * Передаём НЕ сам буфер (это было бы дорого и опасно), а лишь индекс буфера и
 * число символов — "кто" и "сколько". Это идиома "передавай дескриптор, а не
 * данные". typedef даёт короткое имя типу, чтобы не писать 'struct ...' всюду. */
typedef struct {
    uint8_t buf_index;     /* какой из статических буферов заполнен (0..N-1) */
    uint16_t num_symbols;  /* сколько символов в нём лежит                   */
} subghz_rmt_raw_event_t;

/* Файловые (static) переменные состояния модуля. Живут всю программу, по
 * умолчанию обнулены (статическая память инициализируется нулями стандартом Си). */
static rmt_channel_handle_t s_chan;   /* дескриптор аппаратного RX-канала     */
static QueueHandle_t s_queue;         /* очередь ISR -> задача                */
static TaskHandle_t s_task;           /* дескриптор задачи-логгера            */
/* Два статических буфера под символы RMT. Двумерный массив: [номер буфера][символы].
 * Двойная буферизация ("ping-pong") — пока один отдан драйверу под запись,
 * другой задача может копировать. */
static rmt_symbol_word_t s_bufs[SUBGHZ_RMT_RAW_BUF_COUNT][SUBGHZ_RMT_RAW_MEM_SYMBOLS];

/* 'volatile' обязателен: переменную пишет ISR, а читает задача. volatile
 * запрещает компилятору кэшировать значение в регистре и заставляет каждый раз
 * читать/писать реальную ячейку памяти. Это НЕ атомарность и НЕ барьер — просто
 * гарантия, что оптимизатор не "выкинет" обновление из другого контекста. */
static volatile uint32_t s_dropped;   /* пишется в ISR, читается в задаче     */
static uint32_t s_bursts;             /* принадлежит только задаче            */
static uint32_t s_noise;              /* принадлежит только задаче            */
static uint64_t s_symbols;            /* принадлежит только задаче            */
static uint16_t s_max_symbols;        /* принадлежит только задаче            */

static bool s_started;
static bool s_deinit;
static bool s_full_dump;   /* режим записи: печатать сырые полупериоды каждого кадра */

/* Конфиг приёма RMT. 'const' => только для чтения: кладём в ПЗУ, защищаем от
 * случайной записи, и можем переиспользовать один и тот же объект на каждый
 * rmt_receive(). Инициализация по именам полей (designated initializers) —
 * стандарт C99: читаемо и устойчиво к перестановке полей в структуре. */
static const rmt_receive_config_t s_rx_config = {
    .signal_range_min_ns = SUBGHZ_RMT_RAW_GLITCH_NS,
    .signal_range_max_ns = SUBGHZ_RMT_RAW_IDLE_NS,
    .flags = {
        .en_partial_rx = false,
    },
};

/*
 * Контекст ПРЕРЫВАНИЯ (ISR). Здесь действуют жёсткие правила: нельзя блокировать-
 * ся, нельзя malloc, нельзя логировать, нельзя трогать SPI. Задача функции —
 * понять, какой из статических буферов только что заполнил драйвер, и отдать его
 * индекс задаче через очередь. Приём НЕ перезапускаем здесь — это делает задача
 * после копирования, поэтому в буфер не начнут писать, пока он ещё в очереди.
 * При переполнении очереди пачка теряется и считается; ISR никогда не ждёт.
 */
static bool subghz_rmt_raw_on_done(rmt_channel_handle_t channel,
                                   const rmt_rx_done_event_data_t *edata,
                                   void *user_ctx)
{
    /* (void)x — идиома "я сознательно не использую этот параметр". Гасит
     * предупреждение -Wunused-parameter, не порождая никакого кода. */
    (void)channel;
    (void)user_ctx;

    /* Драйвер сообщает в edata->received_symbols УКАЗАТЕЛЬ на тот буфер, что он
     * заполнил. Мы не знаем его индекс, поэтому сравниваем указатели (сравнение
     * адресов, а не содержимого!) со своими буферами и находим совпадение. */
    int index = -1;
    for (int i = 0; i < (int)SUBGHZ_RMT_RAW_BUF_COUNT; ++i) {
        if (edata->received_symbols == s_bufs[i]) {
            index = i;
            break;
        }
    }
    /* Защита от нештатного: чужой буфер, пустая или переполненная пачка. */
    if (index < 0 || edata->num_symbols == 0U ||
        edata->num_symbols > SUBGHZ_RMT_RAW_MEM_SYMBOLS) {
        s_dropped++;
        return false;
    }

    subghz_rmt_raw_event_t event = {
        .buf_index = (uint8_t)index,
        .num_symbols = (uint16_t)edata->num_symbols,
    };
    /* ...FromISR-варианты API — специальные версии для прерываний: они не
     * блокируются и сообщают через out-параметр, не нужно ли разбудить задачу
     * с более высоким приоритетом. */
    BaseType_t higher_prio_woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &event, &higher_prio_woken) != pdTRUE) {
        s_dropped++;   /* очередь полна — честно теряем пачку, но не висим */
    }
    /* Возврат true => драйвер сделает немедленное переключение контекста на
     * разбуженную задачу (меньше задержка), иначе продолжит как обычно. */
    return higher_prio_woken == pdTRUE;
}

/* Перезапуск приёма в конкретный буфер. Вызывается ТОЛЬКО из задачи. */
static void subghz_rmt_raw_restart_receive(uint8_t buf_index)
{
    if (!s_started) {
        return;   /* модуль останавливают — не трогаем железо */
    }
    /* sizeof(s_bufs[buf_index]) — размер ОДНОЙ строки массива в байтах
     * (192 * sizeof(rmt_symbol_word_t)), а не число элементов. rmt_receive ждёт
     * размер буфера именно в байтах. */
    esp_err_t err = rmt_receive(s_chan, s_bufs[buf_index],
                                sizeof(s_bufs[buf_index]), &s_rx_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_receive restart failed: %s", esp_err_to_name(err));
    }
}

/* Печатает сырые полупериоды одного кадра, по 16 полупериодов в строке. Работает
 * в задаче, не в ISR. Символы — это приватная копия задачи, исходный буфер уже
 * возвращён в RMT. */
static void subghz_rmt_raw_dump_frame(uint32_t burst_no,
                                      const rmt_symbol_word_t *frame,
                                      uint16_t num_symbols)
{
    ESP_LOGI(TAG, "burst #%lu: %u sym dump:", (unsigned long)burst_no,
             (unsigned)num_symbols);
    char line[160];   /* буфер строки на стеке; собираем текст порциями */
    int pos = 0;      /* текущая позиция записи внутри line */
    for (uint16_t i = 0; i < num_symbols; ++i) {
        /* Один rmt_symbol_word_t хранит ДВА полупериода (level/duration).
         * level0/level1 и duration0/duration1 — это битовые поля внутри 32-бит
         * слова (duration:15 бит, level:1 бит). Разворачиваем их в цикле. */
        for (unsigned half = 0; half < 2U; ++half) {
            const uint16_t level = (half == 0U) ? frame[i].level0 : frame[i].level1;
            const uint16_t duration = (half == 0U) ? frame[i].duration0 : frame[i].duration1;
            /* snprintf возвращает, СКОЛЬКО символов он ХОТЕЛ БЫ записать (без \0),
             * даже если места не хватило. Поэтому pos может "перескочить" конец
             * буфера — именно от этого защищает проверка ниже. Второй аргумент
             * (size) snprintf никогда не даст выйти за line физически. */
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %c%u",
                            level ? '+' : '-', (unsigned)duration);
            /* Если строка почти заполнена — сбрасываем её в лог и начинаем заново. */
            if (pos > 0 && (size_t)pos >= sizeof(line) - 12U) {
                ESP_LOGI(TAG, "%s", line);
                pos = 0;
                line[0] = '\0';
            }
        }
    }
    if (pos > 0) {
        ESP_LOGI(TAG, "%s", line);   /* допечатать хвост последней строки */
    }
}

/* Задача-логгер: бесконечный цикл, пока не попросят остановиться (s_deinit). */
static void subghz_rmt_raw_task(void *arg)
{
    (void)arg;
    subghz_rmt_raw_event_t event;
    TickType_t last_stats = xTaskGetTickCount();   /* отметка времени в тиках ОС */

    while (!s_deinit) {
        /* xQueueReceive блокирует задачу до прихода события ИЛИ таймаута
         * (500 мс). Таймаут нужен, чтобы периодически проверять s_deinit и
         * печатать статистику, даже когда эфир молчит. */
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(500)) == pdTRUE) {
            const uint16_t n = event.num_symbols;

            /* Полная приватная копия кадра, затем буфер сразу возвращается в RMT:
             * окно простоя захвата минимально, а всё ниже печатается уже из копии.
             * 192 символа * 4 байта = 768 Б стека — помещается в 4096 Б с запасом. */
            uint16_t copy = n;
            if (copy > SUBGHZ_RMT_RAW_MEM_SYMBOLS) {
                copy = SUBGHZ_RMT_RAW_MEM_SYMBOLS;   /* страховка от выхода за буфер */
            }
            rmt_symbol_word_t frame[SUBGHZ_RMT_RAW_MEM_SYMBOLS];   /* копия на стеке */
            /* copy * sizeof(frame[0]) — число байт: число элементов умножаем на
             * размер одного элемента. Классическая формула размера для memcpy. */
            memcpy(frame, s_bufs[event.buf_index], copy * sizeof(frame[0]));
            subghz_rmt_raw_restart_receive(event.buf_index);   /* буфер — обратно в RMT */

            if (n < SUBGHZ_RMT_RAW_MIN_FRAME_SYMS) {
                s_noise++;   /* слишком коротко для кадра — это шум */
            } else {
                s_bursts++;
                s_symbols += n;
                if (n > s_max_symbols) {
                    s_max_symbols = n;
                }

                if (s_full_dump) {
                    subghz_rmt_raw_dump_frame(s_bursts, frame, copy);
                } else {
                    /* Краткий режим: печатаем заголовок и первые несколько
                     * полупериодов только для самых первых пачек, чтобы не
                     * топить лог. */
                    char line[224];
                    int pos = snprintf(line, sizeof(line), "burst #%lu: %u sym",
                                       (unsigned long)s_bursts, (unsigned)n);
                    if (s_bursts <= SUBGHZ_RMT_RAW_VERBOSE_BURSTS && pos > 0) {
                        /* тернарный оператор a?b:c — компактный выбор значения:
                         * сколько символов реально показать. */
                        const uint16_t shown = (copy < SUBGHZ_RMT_RAW_LOG_HALVES / 2U)
                                                   ? copy
                                                   : (SUBGHZ_RMT_RAW_LOG_HALVES / 2U);
                        for (uint16_t i = 0; i < shown && pos > 0 &&
                             (size_t)pos < sizeof(line) - 12U; ++i) {
                            pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                                            " %c%u%c%u",
                                            frame[i].level0 ? '+' : '-',
                                            (unsigned)frame[i].duration0,
                                            frame[i].level1 ? '+' : '-',
                                            (unsigned)frame[i].duration1);
                        }
                        if (n > shown && pos > 0 && (size_t)pos < sizeof(line) - 5U) {
                            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " ...");
                        }
                    }
                    ESP_LOGI(TAG, "%s", line);
                }
            }
        }

        /* Периодическая статистика. Вычитание тиков корректно работает даже при
         * переполнении счётчика тиков — это свойство беззнаковой арифметики. */
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_stats) >= pdMS_TO_TICKS(SUBGHZ_RMT_RAW_STATS_MS)) {
            last_stats = now;
            ESP_LOGI(TAG,
                     "raw stats: bursts=%lu symbols=%llu max_sym=%u noise=%lu queue_dropped=%lu",
                     (unsigned long)s_bursts, (unsigned long long)s_symbols,
                     (unsigned)s_max_symbols, (unsigned long)s_noise,
                     (unsigned long)s_dropped);
        }
    }

    /* Задача сама публикует свой выход и удаляет себя. s_task=NULL ДО
     * vTaskDelete — чтобы stop() увидел, что задача завершилась, и мог спокойно
     * освобождать ресурсы. vTaskDelete(NULL) = "удали текущую задачу". */
    s_task = NULL;
    vTaskDelete(NULL);
}

/* Создание RMT-канала, регистрация колбэка, запуск задачи. Каждый шаг может
 * провалиться, и на каждом провале мы ОТКАТЫВАЕМ уже захваченные ресурсы в
 * обратном порядке — это ручной аналог RAII/goto-cleanup в Си. */
static esp_err_t subghz_rmt_raw_channel_create(void)
{
    s_queue = xQueueCreate(SUBGHZ_RMT_RAW_QUEUE_LEN, sizeof(subghz_rmt_raw_event_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "event queue allocation failed");
        return ESP_ERR_NO_MEM;
    }

    if (SUBGHZ_CC1101_GDO0_GPIO < 0) {
        ESP_LOGE(TAG, "GDO0 is not wired on this board — raw capture impossible");
        vQueueDelete(s_queue);   /* откат: освобождаем уже созданную очередь */
        s_queue = NULL;          /* и обнуляем указатель, чтобы не было "висячего" */
        return ESP_ERR_INVALID_STATE;
    }

    rmt_rx_channel_config_t chan_config = {
        .gpio_num = SUBGHZ_CC1101_GDO0_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = SUBGHZ_RMT_RAW_RESOLUTION_HZ,
        .mem_block_symbols = SUBGHZ_RMT_RAW_MEM_SYMBOLS,
    };
    esp_err_t err = rmt_new_rx_channel(&chan_config, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        vQueueDelete(s_queue);
        s_queue = NULL;
        return err;
    }

    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = subghz_rmt_raw_on_done,   /* указатель на функцию-колбэк */
    };
    err = rmt_rx_register_event_callbacks(s_chan, &callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt callback registration failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);   /* откат по стеку ресурсов: сначала канал... */
        s_chan = NULL;
        vQueueDelete(s_queue);     /* ...потом очередь */
        s_queue = NULL;
        return err;
    }

    BaseType_t created = xTaskCreate(subghz_rmt_raw_task, "subghz_raw",
                                     SUBGHZ_RMT_RAW_TASK_STACK, NULL,
                                     SUBGHZ_RMT_RAW_TASK_PRIO, &s_task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "logger task creation failed");
        rmt_del_channel(s_chan);
        s_chan = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "RMT capture configured: GDO0=IO%d, 1 us/tick (%lu Hz), buf %u sym x%u, burst ends after %lu us silence, glitch filter %lu ns",
             SUBGHZ_CC1101_GDO0_GPIO,
             (unsigned long)SUBGHZ_RMT_RAW_RESOLUTION_HZ,
             (unsigned)SUBGHZ_RMT_RAW_MEM_SYMBOLS, (unsigned)SUBGHZ_RMT_RAW_BUF_COUNT,
             (unsigned long)(SUBGHZ_RMT_RAW_IDLE_NS / 1000UL),
             (unsigned long)SUBGHZ_RMT_RAW_GLITCH_NS);
    return ESP_OK;
}

/* Общая реализация старта: один код для двух публичных режимов (обычный/запись).
 * Разница только в флаге full_dump — классика "один приватный хелпер, две
 * тонкие обёртки". */
static esp_err_t subghz_rmt_raw_start_internal(uint32_t frequency_hz, bool full_dump)
{
    if (s_started) {
        ESP_LOGW(TAG, "capture already running");
        return ESP_ERR_INVALID_STATE;   /* защита от повторного старта */
    }
    if (!subghz_cc1101_is_initialized()) {
        ESP_LOGE(TAG, "CC1101 HAL is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Сначала переводим чип в сырой асинхронный RX на нужной частоте. real_hz
     * вернёт фактическую частоту (синтезатор дискретный, точного попадания нет). */
    uint32_t real_hz = 0;
    esp_err_t err = subghz_cc1101_enter_raw_rx(frequency_hz, &real_hz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enter_raw_rx failed: %s", esp_err_to_name(err));
        return err;
    }

    err = subghz_rmt_raw_channel_create();
    if (err != ESP_OK) {
        (void)subghz_cc1101_exit_raw_rx();   /* откат: вернуть чип из raw RX */
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        (void)subghz_cc1101_exit_raw_rx();
        return err;
    }

    /* Публикуем состояние и обнуляем статистику ТОЛЬКО после того, как всё
     * железо поднялось успешно. Порядок важен: s_started=true должно стать
     * видимым до первого restart_receive. */
    s_deinit = false;
    s_full_dump = full_dump;
    s_started = true;
    s_bursts = 0;
    s_noise = 0;
    s_symbols = 0;
    s_max_symbols = 0;
    s_dropped = 0;

    subghz_rmt_raw_restart_receive(0);   /* первый приём — в буфер 0 */

    ESP_LOGI(TAG,
             "raw capture RUNNING @ %lu Hz, mode=%s — press a 433 MHz remote; data and stats go to this log",
             (unsigned long)real_hz,
             full_dump ? "RECORD (full frame dump)" : "summary");
    return ESP_OK;
}

/* --- Публичный API (объявлен в subghz_rmt_raw.h) ------------------------- */

/* Обычный режим: только сводка по пачкам. */
esp_err_t subghz_rmt_raw_capture_start(uint32_t frequency_hz)
{
    return subghz_rmt_raw_start_internal(frequency_hz, false);
}

/* Режим записи: полный дамп сырых полупериодов каждого кадра. */
esp_err_t subghz_rmt_raw_record_start(uint32_t frequency_hz)
{
    return subghz_rmt_raw_start_internal(frequency_hz, true);
}

/* Остановка захвата. Порядок операций критичен для безопасного завершения: */
void subghz_rmt_raw_capture_stop(void)
{
    if (!s_started) {
        return;   /* не запущено — выходим молча (функция безопасна к повторам) */
    }
    s_started = false;                    /* 1. запрет на рестарт приёма */
    (void)rmt_disable(s_chan);            /* 2. глушим аппаратный канал */
    (void)subghz_cc1101_exit_raw_rx();    /* 3. возвращаем чип из raw RX */

    /* 4. Даём задаче-логгеру увидеть s_deinit и выйти; она сама себя удаляет
     * (s_task=NULL внутри задачи). Ждём это с таймаутом — "join с дедлайном":
     * не блокируемся навсегда, если задача зависла. */
    s_deinit = true;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    while (s_task != NULL && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* уступаем процессор, не крутим вхолостую */
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "logger task did not exit in time");
    }

    /* 5. Освобождаем ресурсы ТОЛЬКО после выхода задачи — иначе задача могла бы
     * обратиться к уже удалённому каналу/очереди. Проверка на NULL делает
     * освобождение идемпотентным. */
    if (s_chan != NULL) {
        rmt_del_channel(s_chan);
        s_chan = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    ESP_LOGI(TAG, "raw capture stopped");
}

/* Простой геттер состояния. Возврат по значению bool — копия, менять извне
 * внутреннее состояние нельзя. */
bool subghz_rmt_raw_is_running(void)
{
    return s_started;
}
