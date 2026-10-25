#pragma once
/* ЛИКБЕЗ (заголовочный файл .h — это ИНТЕРФЕЙС модуля, только объявления):
 *
 * #pragma once — страж включения. Препроцессор подставит этот файл в каждую
 * единицу трансляции (.c) ровно один раз, даже если его подключили транзитивно
 * десять раз. Без стража мы получили бы повторные определения типов и ошибку
 * "redefinition". Классическая переносимая альтернатива — тройка
 * #ifndef SUBGHZ_HAL_CC1101_H / #define ... / #endif в конце; #pragma once
 * короче и не требует выдумывать уникальное имя макроса.
 *
 * Правило разделения: в .h кладём ОБЪЯВЛЕНИЯ (что существует — прототипы, типы,
 * константы), в .c — ОПРЕДЕЛЕНИЯ (как это работает — тела функций). Тогда любой
 * другой модуль, подключив .h, знает сигнатуры, а линковщик свяжет вызовы с
 * телами из .c. Исключение — static inline и макросы: они целиком живут в .h. */

#include <stdbool.h>   /* тип bool/true/false — в Си до C99 их не было        */
#include <stddef.h>    /* size_t, NULL — "служебные" стандартные типы         */
#include <stdint.h>    /* int-ы фиксированной ширины: uint8_t, int16_t, ...   */

#include "esp_err.h"   /* esp_err_t — код возврата ESP-IDF (ESP_OK, ESP_ERR_*)*/

/* This header exposes subghz_rs_gf256_t in the RS adapter config below, so it
 * must pull in its own dependency rather than rely on the includer's order. */
#include "subghz_rs_math.h"

/* extern "C" — нужен ТОЛЬКО когда этот .h подключают из C++ (__cplusplus
 * определён). C++ "украшает" имена функций типами аргументов (name mangling),
 * чтобы поддержать перегрузку; Си так не делает. Обёртка extern "C" { ... }
 * говорит C++-компилятору: "эти символы линкуй по C-правилам", иначе линковщик
 * не найдёт функции, собранные как C. В чистом Си макрос __cplusplus не задан,
 * и весь блок просто исчезает — накладных расходов ноль. */
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * РАСПИНОВКА CC1101 (модуль Ebyte E07-433M20S с PA/LNA) — по схеме платы
 *
 * Радио сидит на ВЫДЕЛЕННОЙ RF-шине SPI, а не на общей шине дисплея/SD
 * (общая шина: SCK=18 MOSI=17 MISO=8, см. components/spi_bus — её не трогаем).
 * Драйвер сам поднимает отдельный SPI3_HOST на пинах ниже:
 *
 *   RF_SCK   -> IO13  (ESP32-S3)
 *   RF_MOSI  -> IO12
 *   RF_MISO  -> IO11
 *   RF_CSN   -> IO46  (ручной CS: нужен CHIP_RDY handshake по datasheet)
 *   RF_GDO0  -> IO9   (raw async stream для RMT capture)
 *   GDO2     -> не подключён (-1)
 *   RX_EN    -> IO2   (внешний LNA; high = RX path включён)
 *   TX_EN    -> IO1   (внешний PA;  high = TX path включён)
 *
 * PA и LNA никогда не включаются одновременно — только через
 * subghz_cc1101_set_rf_path(). На этапе raw RX PA всегда выключен.
 *
 * Значения регистров/strobes/IOCFG сверены с datasheet TI CC1101 (SWRS061)
 * и эталонным драйвером furi_hal_subghz (cc1101_regs.h), присланным
 * пользователем. Дословный код Flipper не копируется: другой API, другой
 * SPI-бэкенд (ESP-IDF), assert() заменён на возврат ошибок.
 * ==========================================================================*/
#define SUBGHZ_CC1101_SCK_GPIO    13
#define SUBGHZ_CC1101_MOSI_GPIO   12
#define SUBGHZ_CC1101_MISO_GPIO   11
#define SUBGHZ_CC1101_CS_GPIO     46
#define SUBGHZ_CC1101_GDO0_GPIO   9
#define SUBGHZ_CC1101_GDO2_GPIO   (-1)  /* на плате не распаян */
#define SUBGHZ_CC1101_RX_EN_GPIO  2     /* E07-433M20S: RX_EN (LNA) */
#define SUBGHZ_CC1101_TX_EN_GPIO  1     /* E07-433M20S: TX_EN (PA)  */

/* Кварц CC1101 на E07-433M20S: 26 MHz. Если модуль с другим кварцем —
 * менять здесь, все расчёты частоты завязаны на эту константу. */
#define SUBGHZ_CC1101_QUARTZ_HZ   26000000UL
#define SUBGHZ_CC1101_FDIV        0x10000UL   /* FREQ = f * 2^16 / f_xosc */
#define SUBGHZ_CC1101_FMASK       0xFFFFFFUL  /* 24-bit frequency word    */

/* === УРОК: #define для карты регистров =====================================
 * Ниже сотни #define — это препроцессорные КОНСТАНТЫ-ПОДСТАНОВКИ. Ключевое для
 * понимания Си:
 *   - #define работает ДО компилятора: он просто меняет текст "SRES" на "0x30U"
 *     во всех местах. У макроса нет типа и нет области видимости как у
 *     переменной, он не занимает ни байта памяти в прошивке.
 *   - 0x30U — шестнадцатеричный литерал; суффикс U делает его unsigned, чтобы
 *     битовые операции и сравнения с беззнаковыми не давали предупреждений и
 *     сюрпризов со знаковым расширением.
 *   - Почему не 'enum'? Для регистрового "словаря" макросы традиционны и дают
 *     ровно тот байт, что уходит в SPI. (enum int-типа тут был бы шире и
 *     потребовал бы приведения.)
 * strobe — это однобайтовая команда чипу (адрес без данных): "сделай X сейчас".
 * =========================================================================== */
/* --- CC1101 command strobes (один байт адреса, без данных) ---------------- */
#define SUBGHZ_CC1101_STROBE_SRES     0x30U  /* reset chip                  */
#define SUBGHZ_CC1101_STROBE_SFSTXON  0x31U  /* enable/calibrate freq synth */
#define SUBGHZ_CC1101_STROBE_SXOFF    0x32U  /* turn off crystal oscillator */
#define SUBGHZ_CC1101_STROBE_SCAL     0x33U  /* calibrate freq synthesizer  */
#define SUBGHZ_CC1101_STROBE_SRX      0x34U  /* enable RX                   */
#define SUBGHZ_CC1101_STROBE_STX      0x35U  /* enable TX                   */
#define SUBGHZ_CC1101_STROBE_SIDLE    0x36U  /* exit RX/TX, go IDLE         */
#define SUBGHZ_CC1101_STROBE_SWOR     0x38U  /* start Wake-on-Radio         */
#define SUBGHZ_CC1101_STROBE_SPWD     0x39U  /* power down when CSn high    */
#define SUBGHZ_CC1101_STROBE_SFRX     0x3AU  /* flush RX FIFO (IDLE/OVF)    */
#define SUBGHZ_CC1101_STROBE_SFTX     0x3BU  /* flush TX FIFO (IDLE/UNF)    */
#define SUBGHZ_CC1101_STROBE_SWORRST  0x3CU  /* reset WOR RTC to Event1     */
#define SUBGHZ_CC1101_STROBE_SNOP     0x3DU  /* no operation (status poll)  */

/* --- CC1101 configuration registers --------------------------------------- */
#define SUBGHZ_CC1101_REG_IOCFG2      0x00U  /* GDO2 output config          */
#define SUBGHZ_CC1101_REG_IOCFG1      0x01U  /* GDO1 output config          */
#define SUBGHZ_CC1101_REG_IOCFG0      0x02U  /* GDO0 output config          */
#define SUBGHZ_CC1101_REG_FIFOTHR     0x03U  /* RX/TX FIFO thresholds       */
#define SUBGHZ_CC1101_REG_SYNC1       0x04U  /* sync word, high byte        */
#define SUBGHZ_CC1101_REG_SYNC0       0x05U  /* sync word, low byte         */
#define SUBGHZ_CC1101_REG_PKTLEN      0x06U  /* packet length               */
#define SUBGHZ_CC1101_REG_PKTCTRL1    0x07U  /* packet automation control   */
#define SUBGHZ_CC1101_REG_PKTCTRL0    0x08U  /* packet automation control   */
#define SUBGHZ_CC1101_REG_ADDR        0x09U  /* device address              */
#define SUBGHZ_CC1101_REG_CHANNR      0x0AU  /* channel number              */
#define SUBGHZ_CC1101_REG_FSCTRL1     0x0BU  /* freq synthesizer control    */
#define SUBGHZ_CC1101_REG_FSCTRL0     0x0CU  /* freq synthesizer control    */
#define SUBGHZ_CC1101_REG_FREQ2       0x0DU  /* frequency control word, high*/
#define SUBGHZ_CC1101_REG_FREQ1       0x0EU  /* frequency control word, mid */
#define SUBGHZ_CC1101_REG_FREQ0       0x0FU  /* frequency control word, low */
#define SUBGHZ_CC1101_REG_MDMCFG4     0x10U  /* modem configuration         */
#define SUBGHZ_CC1101_REG_MDMCFG3     0x11U  /* modem configuration         */
#define SUBGHZ_CC1101_REG_MDMCFG2     0x12U  /* modem configuration         */
#define SUBGHZ_CC1101_REG_MDMCFG1     0x13U  /* modem configuration         */
#define SUBGHZ_CC1101_REG_MDMCFG0     0x14U  /* modem configuration         */
#define SUBGHZ_CC1101_REG_DEVIATN     0x15U  /* modem deviation setting     */
#define SUBGHZ_CC1101_REG_MCSM2       0x16U  /* main radio control SM       */
#define SUBGHZ_CC1101_REG_MCSM1       0x17U  /* main radio control SM       */
#define SUBGHZ_CC1101_REG_MCSM0       0x18U  /* main radio control SM       */
#define SUBGHZ_CC1101_REG_FOCCFG      0x19U  /* freq offset compensation    */
#define SUBGHZ_CC1101_REG_BSCFG       0x1AU  /* bit synchronization config  */
#define SUBGHZ_CC1101_REG_AGCCTRL2    0x1BU  /* AGC control                 */
#define SUBGHZ_CC1101_REG_AGCCTRL1    0x1CU  /* AGC control                 */
#define SUBGHZ_CC1101_REG_AGCCTRL0    0x1DU  /* AGC control                 */
#define SUBGHZ_CC1101_REG_WOREVT1     0x1EU  /* WOR event 0 timeout, high   */
#define SUBGHZ_CC1101_REG_WOREVT0     0x1FU  /* WOR event 0 timeout, low    */
#define SUBGHZ_CC1101_REG_WORCTRL     0x20U  /* wake on radio control       */
#define SUBGHZ_CC1101_REG_FREND1      0x21U  /* front end RX configuration  */
#define SUBGHZ_CC1101_REG_FREND0      0x22U  /* front end TX configuration  */
#define SUBGHZ_CC1101_REG_FSCAL3      0x23U  /* freq synthesizer calibration*/
#define SUBGHZ_CC1101_REG_FSCAL2      0x24U  /* freq synthesizer calibration*/
#define SUBGHZ_CC1101_REG_FSCAL1      0x25U  /* freq synthesizer calibration*/
#define SUBGHZ_CC1101_REG_FSCAL0      0x26U  /* freq synthesizer calibration*/
#define SUBGHZ_CC1101_REG_RCCTRL1     0x27U  /* RC oscillator configuration */
#define SUBGHZ_CC1101_REG_RCCTRL0     0x28U  /* RC oscillator configuration */
#define SUBGHZ_CC1101_REG_FSTEST      0x29U  /* freq synthesizer cal control*/
#define SUBGHZ_CC1101_REG_PTEST       0x2AU  /* production test             */
#define SUBGHZ_CC1101_REG_AGCTEST     0x2BU  /* AGC test                    */
#define SUBGHZ_CC1101_REG_TEST2       0x2CU  /* various test settings       */
#define SUBGHZ_CC1101_REG_TEST1       0x2DU  /* various test settings       */
#define SUBGHZ_CC1101_REG_TEST0       0x2EU  /* various test settings       */

/* --- CC1101 status registers (читаются с битами READ|BURST, т.е. |0xC0) ---- */
#define SUBGHZ_CC1101_STATUS_PARTNUM    0x30U  /* chip part number          */
#define SUBGHZ_CC1101_STATUS_VERSION    0x31U  /* chip version              */
#define SUBGHZ_CC1101_STATUS_FREQEST    0x32U  /* frequency offset estimate */
#define SUBGHZ_CC1101_STATUS_LQI        0x33U  /* link quality indicator    */
#define SUBGHZ_CC1101_STATUS_RSSI       0x34U  /* received signal strength  */
#define SUBGHZ_CC1101_STATUS_MARCSTATE  0x35U  /* main radio control state  */
#define SUBGHZ_CC1101_STATUS_WORTIME1   0x36U  /* WOR time, high byte       */
#define SUBGHZ_CC1101_STATUS_WORTIME0   0x37U  /* WOR time, low byte        */
#define SUBGHZ_CC1101_STATUS_PKTSTATUS  0x38U  /* GDOx and packet status    */
#define SUBGHZ_CC1101_STATUS_VCO_VC_DAC 0x39U  /* PLL calibration setting   */
#define SUBGHZ_CC1101_STATUS_TXBYTES    0x3AU  /* underflow|TX FIFO count   */
#define SUBGHZ_CC1101_STATUS_RXBYTES    0x3BU  /* overflow|RX FIFO count    */
#define SUBGHZ_CC1101_STATUS_RCCTRL1    0x3CU  /* last RC osc calibration   */
#define SUBGHZ_CC1101_STATUS_RCCTRL0    0x3DU  /* last RC osc calibration   */

/* === УРОК: биты собираются операцией ИЛИ (|) ===============================
 * Адресный байт SPI = номер регистра, "обогащённый" битами режима. В коде это
 * будет, например:  tx[0] = REG_IOCFG0 | WRITE_BURST;
 * Побитовое ИЛИ (|) ВЫСТАВЛЯЕТ нужные биты, не трогая остальные. Парные приёмы:
 *   & (И)  — проверить/обнулить биты по маске (status & STATE_MASK);
 *   >> <<  — сдвиг, чтобы "пододвинуть" поле к нулевому биту (см. ниже декодеры);
 *   ~ (НЕ) — инверсия маски, чтобы СБРОСИТЬ бит: x &= ~MASK.
 * Маски 0x80/0xC0/0x40 — это одиночные/парные старшие биты адресного байта. */
/* --- Биты доступа к адресному байту --------------------------------------- */
#define SUBGHZ_CC1101_READ_SINGLE     0x80U  /* read one register           */
#define SUBGHZ_CC1101_READ_BURST      0xC0U  /* read burst / status register*/
#define SUBGHZ_CC1101_WRITE_BURST     0x40U  /* write burst                 */
#define SUBGHZ_CC1101_FIFO            0x3FU  /* RX/TX FIFO (по направлению) */
#define SUBGHZ_CC1101_FIFO_SIZE       64U
#define SUBGHZ_CC1101_PATABLE         0x3EU  /* PA power table, 8 bytes     */

/* --- RXBYTES/TXBYTES byte layout (datasheet: bit7 = flag, bits 6..0 = count) */
#define SUBGHZ_CC1101_RXBYTES_OVERFLOW_MASK   0x80U
#define SUBGHZ_CC1101_TXBYTES_UNDERFLOW_MASK  0x80U
#define SUBGHZ_CC1101_FIFO_COUNT_MASK         0x7FU

/* --- Status byte (возвращается на MISO во время адресного байта) ---------- */
#define SUBGHZ_CC1101_STATUS_CHIP_RDYN   0x80U  /* 1 = crystal NOT ready    */
#define SUBGHZ_CC1101_STATUS_STATE_MASK  0x70U  /* current radio state      */
#define SUBGHZ_CC1101_STATUS_FIFO_MASK   0x0FU  /* FIFO bytes available     */

/* === УРОК: typedef enum ====================================================
 * enum задаёт набор ИМЕНОВАННЫХ целочисленных констант. Здесь значения заданы
 * явно (0..7), потому что они должны совпасть с аппаратным полем STATE чипа —
 * на enum с автонумерацией полагаться в таком случае нельзя. 'typedef' перед
 * enum даёт короткое имя типу (subghz_cc1101_state_t), чтобы писать его без
 * слова 'enum'. Плюс против голых #define: это настоящий ТИП — компилятор и
 * отладчик знают имена значений, а switch по нему может предупредить о
 * необработанной ветке. */
/* Radio state in the status byte (STATE field, bits 6..4). */
typedef enum {
    SUBGHZ_CC1101_STATE_IDLE = 0,
    SUBGHZ_CC1101_STATE_RX = 1,
    SUBGHZ_CC1101_STATE_TX = 2,
    SUBGHZ_CC1101_STATE_FSTXON = 3,
    SUBGHZ_CC1101_STATE_CALIBRATE = 4,
    SUBGHZ_CC1101_STATE_SETTLING = 5,
    SUBGHZ_CC1101_STATE_RXFIFO_OVERFLOW = 6,
    SUBGHZ_CC1101_STATE_TXFIFO_UNDERFLOW = 7,
} subghz_cc1101_state_t;

/* === УРОК: 'static inline' в заголовке ======================================
 * Эти три крошечные функции-декодера ОПРЕДЕЛЕНЫ прямо в .h. Почему так можно:
 *   - 'inline' — подсказка компилятору встроить тело в точку вызова (для
 *     "status & mask" это буквально одна-две инструкции, вызов дороже);
 *   - 'static' даёт внутреннее связывание: каждая единица трансляции получает
 *     СВОЮ копию функции, поэтому нет ошибки "multiple definition" при линковке,
 *     когда .h включили в несколько .c. Без static inline определение функции в
 *     заголовке нарушило бы правило одного определения (ODR).
 * Техника внутри: (byte & MASK) == 0U — ПРОВЕРКА бита; оператор '&' выделяет
 * интересующие биты, сравнение даёт bool. Ниже (status_state) — (byte & MASK)
 * затем '>> 4': маской отрезаем лишнее, сдвигом пододвигаем поле к нулю, получая
 * число 0..7. Это стандартный способ "достать битовое поле" руками. */
static inline bool subghz_cc1101_status_chip_rdy(uint8_t status_byte)
{
    return (status_byte & SUBGHZ_CC1101_STATUS_CHIP_RDYN) == 0U;
}

static inline uint8_t subghz_cc1101_status_state(uint8_t status_byte)
{
    return (status_byte & SUBGHZ_CC1101_STATUS_STATE_MASK) >> 4;
}

static inline uint8_t subghz_cc1101_status_fifo_bytes(uint8_t status_byte)
{
    return status_byte & SUBGHZ_CC1101_STATUS_FIFO_MASK;
}

/* --- IOCFGx pin function values (datasheet table; checked against the
 * reference driver's CC1101Iocfg enum) -------------------------------------- */
#define SUBGHZ_CC1101_IOCFG_RX_FIFO_THRESHOLD   0x00U
#define SUBGHZ_CC1101_IOCFG_TX_FIFO_THRESHOLD   0x02U
#define SUBGHZ_CC1101_IOCFG_RX_OVERFLOW         0x04U
#define SUBGHZ_CC1101_IOCFG_TX_UNDERFLOW        0x05U
#define SUBGHZ_CC1101_IOCFG_SYNC_WORD           0x06U
#define SUBGHZ_CC1101_IOCFG_PACKET              0x07U
#define SUBGHZ_CC1101_IOCFG_PREAMBLE            0x08U
#define SUBGHZ_CC1101_IOCFG_CLEAR_CHANNEL       0x09U
#define SUBGHZ_CC1101_IOCFG_LOCK_DETECTOR       0x0AU
#define SUBGHZ_CC1101_IOCFG_SERIAL_CLOCK        0x0BU
#define SUBGHZ_CC1101_IOCFG_SERIAL_SYNC_OUT     0x0CU
#define SUBGHZ_CC1101_IOCFG_SERIAL_DATA_OUT     0x0DU  /* raw async stream */
#define SUBGHZ_CC1101_IOCFG_CARRIER_SENSE       0x0EU
#define SUBGHZ_CC1101_IOCFG_CRC_OK              0x0FU
#define SUBGHZ_CC1101_IOCFG_CHIP_RDYN           0x29U
#define SUBGHZ_CC1101_IOCFG_XOSC_STABLE         0x2BU
#define SUBGHZ_CC1101_IOCFG_HIGH_Z              0x2EU
#define SUBGHZ_CC1101_IOCFG_HW_TO_ZERO          0x2FU
#define SUBGHZ_CC1101_IOCFG_CLK_XOSC_192        0x3FU  /* IOCFG0 reset default */

/* --- MARCSTATE values ------------------------------------------------------ */
#define SUBGHZ_CC1101_MARCSTATE_IDLE  0x01U
#define SUBGHZ_CC1101_MARCSTATE_RX    0x0DU
#define SUBGHZ_CC1101_MARCSTATE_TX    0x13U

/*
 * External RF front-end path of the E07-433M20S module. RX_EN and TX_EN are
 * never high at the same time — the switch selects one path; both low keeps
 * the front-end idle. Managed only through subghz_cc1101_set_rf_path().
 */
typedef enum {
    SUBGHZ_CC1101_RF_PATH_OFF = 0,
    SUBGHZ_CC1101_RF_PATH_RX,
    SUBGHZ_CC1101_RF_PATH_TX,
} subghz_cc1101_rf_path_t;

/*
 * Static driver configuration. Created by the caller (app_main for the boot
 * probe), owned by the caller; the driver copies the values it needs at
 * subghz_cc1101_init() and does NOT keep the pointer.
 *
 * spi_clock_hz == 0 selects the safe default (5 MHz; CC1101 allows up to
 * 10 MHz, conservative for module wiring). The SCK/MOSI/MISO pins are fixed
 * by the board schematic (SUBGHZ_CC1101_*_GPIO defines above), so only the
 * per-module lines stay configurable.
 * gdo0_gpio/gdo2_gpio may be -1 when those lines are not wired.
 */
typedef struct {
    int cs_gpio;
    int gdo0_gpio;
    int gdo2_gpio;
    uint32_t spi_clock_hz;
} subghz_cc1101_config_t;

/* Returns the config built from the SUBGHZ_CC1101_*_GPIO defines above. */
subghz_cc1101_config_t subghz_cc1101_config_default(void);

/*
 * Brings up the dedicated RF SPI host (SPI3_HOST) and registers the CC1101 on
 * it using manual CS control — the CHIP_RDY handshake required by the
 * datasheet cannot be done with a driver-managed CS. The shared display/SD
 * bus (spi_bus component) is not touched.
 *
 * Thread-safety: init/deinit are expected from app_main context only.
 * The data-path functions below may be called from any task; every SPI
 * exchange is serialized through the driver's private mutex with a bounded
 * timeout, so a stuck caller surfaces as an ESP_ERR_TIMEOUT log.
 */
esp_err_t subghz_cc1101_init(const subghz_cc1101_config_t *config);
void subghz_cc1101_deinit(void);
bool subghz_cc1101_is_initialized(void);

/*
 * Hardware reset via SRES strobe, followed by a settle delay.
 * ESP_ERR_TIMEOUT here almost always means a wiring/power problem: the chip
 * never pulled SO (MISO) low after CS went low.
 */
esp_err_t subghz_cc1101_reset(void);

/*
 * Sends a command strobe. status_byte (may be NULL) receives the status byte
 * shifted out during the address byte: CHIP_RDYn | STATE | FIFO bytes.
 * This and the wait helper below replace the reference driver's assert-based
 * flow: callers get esp_err_t instead of a reboot on unexpected chip state.
 */
esp_err_t subghz_cc1101_strobe(uint8_t strobe, uint8_t *status_byte);

/* Polls the status byte with SNOP until STATE == expected_state or timeout.
 * Used after SRX/STX/SIDLE instead of blind delays. */
esp_err_t subghz_cc1101_wait_status_state(uint8_t expected_state, uint32_t timeout_ms);

/* Configuration register access (addr 0x00..0x2E). */
esp_err_t subghz_cc1101_write_reg(uint8_t addr, uint8_t value);
esp_err_t subghz_cc1101_read_reg(uint8_t addr, uint8_t *value);

/* Status register access (addr 0x30..0x3D, read with the burst bit set). */
esp_err_t subghz_cc1101_read_status(uint8_t addr, uint8_t *value);

/* Convenience wrappers matching the reference driver's high-level API. */
esp_err_t subghz_cc1101_get_status(uint8_t *status_byte);   /* SNOP */
esp_err_t subghz_cc1101_shutdown(uint8_t *status_byte);     /* SPWD */
esp_err_t subghz_cc1101_calibrate(uint8_t *status_byte);    /* SCAL */
esp_err_t subghz_cc1101_switch_to_idle(uint8_t *status_byte);
esp_err_t subghz_cc1101_switch_to_rx(uint8_t *status_byte);
esp_err_t subghz_cc1101_switch_to_tx(uint8_t *status_byte);
esp_err_t subghz_cc1101_flush_rx(uint8_t *status_byte);
esp_err_t subghz_cc1101_flush_tx(uint8_t *status_byte);

/*
 * Programs FREQ2/1/0 for the requested carrier frequency and returns the
 * actually synthesized frequency (same formula as the reference driver:
 * control = f * 2^16 / 26 MHz). A read-back check proves the writes landed.
 * Allowed range per datasheet: 300..928 MHz.
 */
esp_err_t subghz_cc1101_set_frequency(uint32_t frequency_hz, uint32_t *real_frequency_hz);

/* PATABLE burst write (8 PA levels). Needed for the TX stage, not for RX. */
esp_err_t subghz_cc1101_set_pa_table(const uint8_t table[8]);

/*
 * Drives the external front-end path (RX_EN / TX_EN) with mutual exclusion.
 * enter_raw_rx() enables the LNA path itself; exit_raw_rx() turns it off.
 * TX path exists for the future TX stage — nothing enables it on this stage.
 */
esp_err_t subghz_cc1101_set_rf_path(subghz_cc1101_rf_path_t path);

/*
 * FIFO access. Callers are expected to be serialized (one owner task, e.g.
 * the future subghz worker): the driver reuses two internal 66-byte staging
 * buffers, so these functions are NOT re-entrant. read_fifo reports the
 * RXBYTES overflow flag so the caller can SFRX and count lost packets.
 */
esp_err_t subghz_cc1101_write_fifo(const uint8_t *data, uint8_t size);
esp_err_t subghz_cc1101_read_fifo(uint8_t *data, uint8_t capacity, uint8_t *size,
                                  bool *overflow);

/*
 * Enters raw asynchronous RX streaming on GDO0 (the .sub capture path):
 *   SIDLE -> set_frequency -> IOCFG0=SERIAL_DATA_OUT -> MDMCFG2=0x30
 *   (ASK/OOK, no sync) -> PKTCTRL0=0x32 (async serial, no CRC, infinite)
 *   -> SRX -> wait STATUS.STATE==RX.
 *
 * MDMCFG4/MDMCFG3 are intentionally left at post-reset defaults and only
 * read back: the log prints the resulting RX bandwidth and data rate
 * computed with the datasheet formulas, so the effective modem settings are
 * proven from hardware, not assumed. Tune them only after air tests.
 *
 * real_frequency_hz (may be NULL) receives the synthesized carrier.
 */
esp_err_t subghz_cc1101_enter_raw_rx(uint32_t frequency_hz, uint32_t *real_frequency_hz);

/* Leaves RX and resets the chip back to post-reset defaults (SIDLE + SRES). */
esp_err_t subghz_cc1101_exit_raw_rx(void);

/*
 * Presence self-check with a verbose log. Sequence:
 *   1. pin map dump (RF bus pins + CS/GDO);
 *   2. SRES reset with CHIP_RDY handshake;
 *   3. PARTNUM/VERSION read (expected 0x00 / 0x14; 0x00 or 0xFF in VERSION
 *      means "module does not answer");
 *   4. scratch write/read on PKTLEN to prove register writes land;
 *   5. key register dump for eyeballing;
 *   6. MARCSTATE/state transitions IDLE -> RX -> IDLE via SRX/SIDLE with
 *      SNOP-based waits instead of blind delays.
 *
 * Every step logs OK/WARN/FAIL; the test continues after failures so the log
 * shows the whole picture in one boot. Returns ESP_OK only if all hard checks
 * passed, ESP_FAIL otherwise. Never touches UI, files, or other modules.
 */
esp_err_t subghz_cc1101_selftest(void);

/*
 * One-call boot entry point used by app_main: builds the default config, runs
 * init and the self-check. Fail-soft by contract: any error is logged and
 * returned, the caller continues booting the rest of the firmware.
 */
esp_err_t subghz_cc1101_boot_probe(void);

/* --- Packet RX mode (sync word + CRC, fixed/variable length) ----------------
 *
 * This is the framed packet path (not raw async). The CC1101 outputs a
 * packet-received pulse on GDO0 (IOCFG0=0x06). We hook a GPIO interrupt on
 * GDO0, read the FIFO from the ISR (via a queue to a worker task), and
 * deliver complete packets to the caller's callback.
 *
 * The driver owns the SPI mutex; the ISR only posts to a queue. The worker
 * task (created by subghz_cc1101_rx_start) drains the queue, reads the FIFO,
 * and calls the packet callback. This keeps the ISR tiny and SPI-safe.
 *
 * Typical register flow (datasheet):
 *   IOCFG0 = 0x06          -- GDO0 asserts on sync word / packet received
 *   PKTCTRL1 = 0x04        -- PREAMBLE_QUAL=1, ADR_CHK=0 (no addr check)
 *   PKTCTRL0 = 0x05        -- PKT_FORMAT=1 (variable), CRC_EN=1, WHITENING=1
 *   PKTLEN   = max payload -- max packet length for variable mode
 *   SYNC1/SYNC0 = 0xD391   -- 16-bit sync word (configurable)
 *   MDMCFG2  = 0x30        -- ASK/OOK, no preamble/sync (overridden below)
 *   MDMCFG1  = 0x22        -- 4 preamble bytes, sync word 16/16
 *   MDMCFG0  = 0xF8        -- channel spacing (not used in single channel)
 *   FREQ2/1/0              -- carrier frequency
 *   DEVIATN  = 0x00        -- ASK has no deviation
 *   MCSM1    = 0x0C        -- CCA_MODE=0, RX_OFF_MODE=1 (IDLE), TX_OFF_MODE=0
 *   MCSM0    = 0x18        -- FS_AUTOCAL=1 (cal every RX/TX), PO_TIMEOUT=14
 *   FSCTRL1  = 0x0C        -- IF freq
 *   FREND1   = 0xB6        -- RX front-end
 *   AGCCTRL2 = 0x07        -- AGC
 *
 * The caller provides a config struct with frequency, sync word, callback,
 * and optional user context. The driver applies a sensible default register
 * set and only overrides what the caller specifies.
 * -------------------------------------------------------------------------- */

/* === УРОК: непрозрачный дескриптор (opaque handle) =========================
 * 'struct subghz_cc1101_rx_handle_t' здесь только ОБЪЯВЛЕН (неполный/incomplete
 * type), а его поля спрятаны в .c. Снаружи виден лишь УКАЗАТЕЛЬ на него. Так мы
 * прячем реализацию: пользователь держит "ручку" и передаёт её обратно в API,
 * но не может залезть внутрь и не зависит от размера структуры. Классический
 * способ инкапсуляции в Си (аналог приватных полей класса). */
typedef struct subghz_cc1101_rx_handle_t *subghz_cc1101_rx_handle_t;

/* === УРОК: указатель на функцию (callback) =================================
 * Синтаксис читаем "изнутри наружу": (*subghz_cc1101_rx_callback_t) — имя есть
 * указатель; скобки аргументов справа => он указывает на ФУНКЦИЮ; 'void' слева
 * => функция ничего не возвращает. Итог: тип "указатель на функцию
 * (const uint8_t*, uint8_t, int16_t, uint8_t, void*) -> void".
 * Зачем: драйвер вызывает ВАШУ функцию на каждый принятый кадр, ничего не зная
 * о ней заранее — это инверсия управления (нижний слой зовёт верхний). Параметр
 * 'void *user_ctx' протаскивает ваш произвольный контекст без глобальных
 * переменных: драйвер хранит его как есть и отдаёт обратно в колбэк. */
typedef void (*subghz_cc1101_rx_callback_t)(const uint8_t *data,
                                            uint8_t len,
                                            int16_t rssi_dbm,
                                            uint8_t lqi,
                                            void *user_ctx);

/* --- Anti-replay (sliding sequence window) integration point ---------------
 *
 * Receiver-side defense ONLY. When enabled, the RX worker extracts a monotonic
 * sequence counter from each CRC-valid frame and asks the window validator
 * whether it is strictly newer than the last accepted value, within a bounded
 * look-ahead. Replayed or stale frames are DROPPED before they reach the upper
 * callback, so the protocol layer never sees a duplicate. This is the same
 * class of check as IPsec/DTLS anti-replay (RFC 6479): it rejects old traffic,
 * it does not generate, predict, or forge it.
 *
 * Allocation-free: the window state is a single caller-owned struct. The RX
 * worker is the ONLY writer of that state (single-writer discipline), so the
 * hot path needs no lock; the upper layer may read it between frames. The ops
 * vector and state must outlive the RX session (rx_start copies the config by
 * value, but these are pointers into caller-owned storage).
 * -------------------------------------------------------------------------- */

/* Width of the sequence counter embedded in the payload. */
typedef enum {
    SUBGHZ_CC1101_SEQ_WIDTH_U16 = 0,
    SUBGHZ_CC1101_SEQ_WIDTH_U32 = 1,
} subghz_cc1101_seq_width_t;

/* Caller-owned, allocation-free window state. Zero-initialize before use
 * (primed=false); the first accepted frame seeds last_counter. Owned by the
 * RX worker for the duration of the RX session. */
typedef struct {
    uint32_t last_counter;  /* last accepted sequence value */
    bool primed;            /* false until the first frame seeds last_counter */
} subghz_cc1101_seq_state_t;

/* === УРОК: структура из указателей на функции = ручная "таблица методов" ===
 * Это тот же приём, что виртуальные функции в C++ (vtable), но собранный руками
 * на чистом Си. Структура хранит несколько указателей на функции; "подключить
 * поведение" = заполнить эти поля своими функциями. Вызов выглядит так:
 *     if (ops->validate_sequence_window != NULL) {        // строгая NULL-проверка!
 *         ok = ops->validate_sequence_window(...);        // косвенный вызов
 *     }
 * Плюсы: полиморфизм без наследования, разные реализации для разных протоколов,
 * и всё без динамической памяти. Обязательное правило Си: каждый указатель на
 * функцию ПЕРЕД вызовом проверяем на NULL — иначе разыменование нуля = краш. */
/* The validation interface. Every member is optional and is NULL-checked
 * before use by the worker. */
typedef struct subghz_cc1101_antireplay_ops {
    /* Returns true iff 'incoming' is strictly newer than state->last_counter
     * and no further than 'window' steps ahead, in modular unsigned arithmetic
     * at the given counter width. Returns false to drop the frame. */
    bool (*validate_sequence_window)(const subghz_cc1101_seq_state_t *state,
                                     uint32_t incoming,
                                     uint32_t window,
                                     subghz_cc1101_seq_width_t width);
    /* Optional payload hook. The HAL holds no key material; this is a stub
     * extension point for a protocol layer. Return true to keep the frame,
     * false to drop it. */
    bool (*parse_secure_payload)(const uint8_t *data, uint8_t len, void *key_ctx);
    /* Commits 'accepted' as the new last-seen value after a frame passes. */
    void (*sync_sequence_state)(subghz_cc1101_seq_state_t *state,
                                uint32_t accepted);
} subghz_cc1101_antireplay_ops_t;

/* Per-session anti-replay configuration. Set .ops to NULL to disable the
 * feature entirely (the worker then behaves exactly as before). */
typedef struct {
    const subghz_cc1101_antireplay_ops_t *ops; /* NULL => feature disabled */
    subghz_cc1101_seq_state_t *state;          /* caller-owned window state */
    void *key_ctx;                             /* opaque, to parse_secure_payload */
    uint8_t seq_offset;                        /* byte offset of counter in payload */
    subghz_cc1101_seq_width_t seq_width;       /* U16 or U32 */
    bool big_endian;                           /* counter byte order in payload */
    uint32_t look_ahead;                       /* accept window size, e.g. 16 */
} subghz_cc1101_antireplay_config_t;

typedef struct {
    uint32_t frequency_hz;
    uint16_t sync_word;
    uint8_t max_payload_len;
    bool whitening;
    bool crc_en;
    bool addr_check;
    uint8_t own_addr;
    subghz_cc1101_rx_callback_t callback;
    void *user_ctx;
    int task_priority;
    uint32_t task_stack_size;
    subghz_cc1101_antireplay_config_t antireplay; /* .ops=NULL disables it */
} subghz_cc1101_rx_config_t;

/* === УРОК: составной литерал + инициализация по именам полей (C99) =========
 * (subghz_cc1101_rx_config_t){ .frequency_hz = ..., ... } — это "составной
 * литерал": безымянный объект структуры, созданный прямо в выражении и
 * возвращаемый ПО ЗНАЧЕНИЮ (копией). '.field = value' — designated initializers:
 * задаём только нужные поля по имени, остальные гарантированно обнуляются
 * стандартом. Плюсы против ручного присваивания по одному полю: не зависит от
 * порядка полей, не забудешь обнулить новое поле, и всё видно одним выражением.
 * Паттерн "функция_default()" даёт вызывающему заведомо валидную заготовку,
 * которую он правит точечно. */
static inline subghz_cc1101_rx_config_t subghz_cc1101_rx_config_default(void)
{
    return (subghz_cc1101_rx_config_t){
        .frequency_hz = 433920000UL,
        .sync_word = 0xD391,
        .max_payload_len = 61,
        .whitening = true,
        .crc_en = true,
        .addr_check = false,
        .own_addr = 0x00,
        .callback = NULL,
        .user_ctx = NULL,
        .task_priority = 10,
        .task_stack_size = 4096,
        .antireplay = {
            .ops = NULL,           /* disabled unless the caller opts in */
            .state = NULL,
            .key_ctx = NULL,
            .seq_offset = 0,
            .seq_width = SUBGHZ_CC1101_SEQ_WIDTH_U16,
            .big_endian = false,
            .look_ahead = 16,
        },
    };
}

/* Default .validate_sequence_window: overflow-correct modular window check.
 * With both counters reduced to the counter width, accepts iff the unsigned
 * modular distance (incoming - last) lies in [1, window]. A distance of 0 is
 * an exact replay (rejected); a distance above 'window' is stale or forged
 * (rejected). The width mask makes the subtraction wrap correctly, e.g. a u16
 * rollover 0xFFFF -> 0x0000 is a distance of 1. Returns true before the window
 * is primed so the first frame can seed the state. */
bool subghz_cc1101_default_validate_window(const subghz_cc1101_seq_state_t *state,
                                           uint32_t incoming,
                                           uint32_t window,
                                           subghz_cc1101_seq_width_t width);

/* Default .sync_sequence_state: records 'accepted' and marks the state primed. */
void subghz_cc1101_default_sync_state(subghz_cc1101_seq_state_t *state,
                                      uint32_t accepted);

/* Ready-made ops vector wired to the defaults above (parse_secure_payload is
 * NULL — no key handling lives in the HAL). Points to static storage. */
const subghz_cc1101_antireplay_ops_t *subghz_cc1101_default_antireplay_ops(void);

/* Starts framed packet RX on the given frequency. The driver:
 *   - programs the CC1101 for packet mode with sync/CRC/whitening,
 *   - installs a GPIO interrupt on GDO0,
 *   - spawns a worker task that reads FIFO on interrupt,
 *   - invokes callback for each valid packet (CRC OK).
 * Returns a handle for subghz_cc1101_rx_stop().
 * Fail-soft: logs errors, returns NULL on failure.
 */
subghz_cc1101_rx_handle_t subghz_cc1101_rx_start(const subghz_cc1101_rx_config_t *config);

/* Stops packet RX, frees the worker task, removes GPIO interrupt,
 * returns chip to IDLE and resets registers to post-reset defaults. */
void subghz_cc1101_rx_stop(subghz_cc1101_rx_handle_t handle);

/* --- Reed-Solomon integration point ----------------------------------------
 *
 * The protocol layer (outside this HAL) will call subghz_rs_math.h functions.
 * This HAL only provides a thin, allocation-free adapter so the worker task
 * can push raw symbols into an RS codec context and get corrected bytes out.
 *
 * The caller supplies an already-initialized subghz_rs_gf256_t context and
 * the RS code parameters (n, k, first_root). The driver stores them and the
 * protocol layer calls subghz_cc1101_rs_decode() from its callback or
 * polling loop.
 * -------------------------------------------------------------------------- */

typedef struct {
    const subghz_rs_gf256_t *gf;
    uint8_t n;               /* codeword length (e.g., 255) */
    uint8_t k;               /* message length */
    uint8_t first_root;      /* usually 1 for RS(255,k) */
    uint8_t syndrome_count;  /* n - k = 2t */
} subghz_cc1101_rs_config_t;

/* Attempts to correct errors in 'data' (length 'len' <= n) using the provided
 * RS config. The buffer is modified in-place on success.
 * Returns true if correction succeeded (syndromes zero after), false otherwise.
 * The caller must ensure 'data' has capacity for 'n' bytes (padded with zeros).
 */
bool subghz_cc1101_rs_decode(const subghz_cc1101_rs_config_t *rs_cfg,
                             uint8_t *data,
                             uint8_t len);

/* --- Red Team / Packet & Raw TX Extensions API --------------------------- */

typedef struct {
    uint32_t frequency_hz;
    uint16_t sync_word;
    bool whitening;
    bool crc_en;
    int8_t pa_power_index;      /* Индекс мощности PA (0..7) */
} subghz_cc1101_tx_config_t;

static inline subghz_cc1101_tx_config_t subghz_cc1101_tx_config_default(void)
{
    return (subghz_cc1101_tx_config_t){
        .frequency_hz = 433920000UL, /* 433.92 MHz по умолчанию */
        .sync_word = 0xD391,
        .whitening = true,
        .crc_en = true,
        .pa_power_index = 7,         /* Максимальная мощность (+20 dBm для E07) */
    };
}

/* Синхронная отправка структурированного пакета через FIFO */
esp_err_t subghz_cc1101_send_packet(const subghz_cc1101_tx_config_t *config,
                                    const uint8_t *data, uint8_t len);

/* Перевод чипа в режим сырого асинхронного TX (для RMT реплея .sub файлов) */
esp_err_t subghz_cc1101_enter_raw_tx(uint32_t frequency_hz);

/* Выход из сырого TX и сброс чипа */
esp_err_t subghz_cc1101_exit_raw_tx(void);

/* Тестовая функция для проверки передачи в эфир (Beacon) */
esp_err_t subghz_cc1101_test_tx_beacon(void);

#ifdef __cplusplus
}
#endif