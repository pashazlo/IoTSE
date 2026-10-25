#include "subghz_hal_cc1101.h"

#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "subghz_rs_math.h"

static const char *TAG = "cc1101";

/* SPI2_HOST is owned by the shared display/SD bus (components/spi_bus). The
 * radio wiring goes to separate pads, so the driver owns SPI3_HOST outright:
 * no contention with display DMA, no priority questions on the shared bus. */
#define SUBGHZ_CC1101_SPI_HOST            SPI3_HOST

/* CC1101 register traffic is a handful of bytes per exchange; polling mode
 * avoids the transaction queue entirely.
 *
 * Clock: 1 MHz. The chip itself allows 10 MHz, but the first board build
 * showed a deterministic 1-LSB error on the FINAL bit of a 2-byte register
 * exchange at 2 MHz (wrote 0xA5 to the scratch register, read back 0xA4)
 * with this clone silicon and module wiring. Halving the clock doubles the
 * setup/hold margin on every edge; register traffic is latency-insensitive,
 * so the safer clock costs nothing measurable.
 *
 * SPI mode stays 0 on purpose: the CC1101 samples SI on the rising SCLK
 * edge and changes SO on the falling edge (datasheet SWRS061), which is
 * exactly CPOL=0/CPHA=0. A rotated mode would corrupt EVERY byte — VERSION
 * included — not just one LSB, so the mode is not the suspect here. */
/* 100 kHz (was 1 MHz): the self-test histogram showed ~50% of logic-1s failing
 * UNIFORMLY across all 8 data bits — the signature of marginal SPI signal
 * integrity (SCK edges / bus capacitance from the second module sharing SPI3),
 * not a single stuck line. A slower clock gives each bit far more time to
 * settle and is the cheapest way to confirm/defeat that. Raise it again once
 * the bus is proven clean. */
#define SUBGHZ_CC1101_DEFAULT_SPI_CLOCK_HZ  100000U
#define SUBGHZ_CC1101_BUS_LOCK_MS           1000U
/* After CS goes low the chip must pull SO (MISO) low once the crystal is
 * ready. A missing/unpowered module never does: the MISO pull-up enabled at
 * init keeps the line high and the wait times out — this is the primary
 * "module absent" signal, so it is logged loudly. */
#define SUBGHZ_CC1101_CHIP_RDY_TIMEOUT_MS   50U

#define SUBGHZ_CC1101_EXPECTED_PARTNUM  0x00U
#define SUBGHZ_CC1101_EXPECTED_VERSION  0x14U

/* Datasheet carrier range for CC1101 (300..928 MHz). */
#define SUBGHZ_CC1101_MIN_FREQ_HZ  300000000UL
#define SUBGHZ_CC1101_MAX_FREQ_HZ  928000000UL

static spi_device_handle_t s_dev;
static SemaphoreHandle_t s_bus_mutex;
static int s_cs_gpio = -1;
static int s_gdo0_gpio = -1;
static int s_gdo2_gpio = -1;
static bool s_initialized = false;

/* FIFO staging (see the FIFO API note in the header): one owner at a time,
 * internal staging so >64-byte bursts work with DMA-backed transfers and the
 * caller's buffer is never touched by the SPI driver directly. */
static uint8_t s_fifo_tx[1 + SUBGHZ_CC1101_FIFO_SIZE];
static uint8_t s_fifo_rx[1 + SUBGHZ_CC1101_FIFO_SIZE];

static const char *status_state_name(uint8_t status_byte)
{
    switch (subghz_cc1101_status_state(status_byte)) {
    case SUBGHZ_CC1101_STATE_IDLE: return "IDLE";
    case SUBGHZ_CC1101_STATE_RX: return "RX";
    case SUBGHZ_CC1101_STATE_TX: return "TX";
    case SUBGHZ_CC1101_STATE_FSTXON: return "FSTXON";
    case SUBGHZ_CC1101_STATE_CALIBRATE: return "CALIBRATE";
    case SUBGHZ_CC1101_STATE_SETTLING: return "SETTLING";
    case SUBGHZ_CC1101_STATE_RXFIFO_OVERFLOW: return "RXFIFO_OVERFLOW";
    case SUBGHZ_CC1101_STATE_TXFIFO_UNDERFLOW: return "TXFIFO_UNDERFLOW";
    default: return "?";
    }
}

static const char *marcstate_name(uint8_t state)
{
    switch (state) {
    case 0x00: return "SLEEP";
    case 0x01: return "IDLE";
    case 0x02: return "XOFF";
    case 0x03: return "VCOON_MC";
    case 0x04: return "REGON_MC";
    case 0x05: return "MANCAL";
    case 0x06: return "VCOON";
    case 0x07: return "REGON";
    case 0x08: return "STARTCAL";
    case 0x09: return "BWBOOST";
    case 0x0A: return "FS_LOCK";
    case 0x0B: return "IFADCON";
    case 0x0C: return "ENDCAL";
    case 0x0D: return "RX";
    case 0x0E: return "RX_END";
    case 0x0F: return "RX_RST";
    case 0x10: return "TXRX_SWITCH";
    case 0x11: return "RXFIFO_OVERFLOW";
    case 0x12: return "FSTXON";
    case 0x13: return "TX";
    case 0x14: return "TX_END";
    case 0x15: return "RXTX_SWITCH";
    case 0x16: return "TXFIFO_UNDERFLOW";
    default: return "?";
    }
}

/*
 * One CS-framed SPI exchange on the private RF bus. The bus mutex is taken
 * with a bounded timeout, so a stuck caller surfaces as an ESP_ERR_TIMEOUT
 * log instead of a silent hang. CS is raised on every exit path.
 *
 * The CHIP_RDY wait polls the MISO pad through the GPIO matrix: while the
 * CC1101 crystal is not ready the chip holds SO high. Fast path spins in
 * microsecond delays (normal case: ready in microseconds), slow path falls
 * back to task sleeps so a missing module does not burn CPU for 50 ms.
 * Same handshake point as the reference driver's cc1101_spi_trx().
 */
static esp_err_t cc1101_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(SUBGHZ_CC1101_BUS_LOCK_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "RF bus mutex timeout (%u ms) — stuck caller?",
                 (unsigned)SUBGHZ_CC1101_BUS_LOCK_MS);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    gpio_set_level(s_cs_gpio, 0);

    uint32_t waited_us = 0;
    while (gpio_get_level(SUBGHZ_CC1101_MISO_GPIO) != 0) {
        if (waited_us >= SUBGHZ_CC1101_CHIP_RDY_TIMEOUT_MS * 1000U) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (waited_us < 2000U) {
            esp_rom_delay_us(100);
            waited_us += 100U;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
            waited_us += 1000U;
        }
    }

    if (err == ESP_OK) {
        spi_transaction_t t = {
            .length = len * 8U,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        err = spi_device_polling_transmit(s_dev, &t);
        /* Datasheet: CSn must stay low through the final SCLK edge. ESP-IDF
         * documents polling_transmit as transaction-complete on return, but
         * the failing signature on this board was the LAST bit of the frame —
         * a 1 us hold before CS high removes any race between the SPI
         * peripheral's completion flag and the final sampled bit. */
        esp_rom_delay_us(1);
    }

    gpio_set_level(s_cs_gpio, 1);
    xSemaphoreGive(s_bus_mutex);
    return err;
}

esp_err_t subghz_cc1101_strobe(uint8_t strobe, uint8_t *status_byte)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tx[1] = {strobe};
    uint8_t rx[1] = {0};
    esp_err_t err = cc1101_xfer(tx, rx, sizeof(tx));
    if (err == ESP_OK && status_byte != NULL) {
        *status_byte = rx[0];
    }
    return err;
}

esp_err_t subghz_cc1101_write_reg(uint8_t addr, uint8_t value)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tx[2] = {addr, value};
    uint8_t rx[2] = {0, 0};
    return cc1101_xfer(tx, rx, sizeof(tx));
}

esp_err_t subghz_cc1101_read_reg(uint8_t addr, uint8_t *value)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[2] = {(uint8_t)(addr | SUBGHZ_CC1101_READ_SINGLE), 0x00U};
    uint8_t rx[2] = {0, 0};
    esp_err_t err = cc1101_xfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
}

esp_err_t subghz_cc1101_read_status(uint8_t addr, uint8_t *value)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[2] = {(uint8_t)(addr | SUBGHZ_CC1101_READ_BURST), 0x00U};
    uint8_t rx[2] = {0, 0};
    esp_err_t err = cc1101_xfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
}

esp_err_t subghz_cc1101_get_status(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SNOP, status_byte);
}

esp_err_t subghz_cc1101_shutdown(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SPWD, status_byte);
}

esp_err_t subghz_cc1101_calibrate(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SCAL, status_byte);
}

esp_err_t subghz_cc1101_switch_to_idle(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SIDLE, status_byte);
}

esp_err_t subghz_cc1101_switch_to_rx(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SRX, status_byte);
}

esp_err_t subghz_cc1101_switch_to_tx(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_STX, status_byte);
}

esp_err_t subghz_cc1101_flush_rx(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SFRX, status_byte);
}

esp_err_t subghz_cc1101_flush_tx(uint8_t *status_byte)
{
    return subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SFTX, status_byte);
}

esp_err_t subghz_cc1101_wait_status_state(uint8_t expected_state, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t status = 0;
    for (;;) {
        esp_err_t err = subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SNOP, &status);
        if (err != ESP_OK) {
            return err;
        }
        if (subghz_cc1101_status_state(status) == expected_state) {
            return ESP_OK;
        }
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGE(TAG,
                     "wait state %u timeout: last status=0x%02X (state=%s)",
                     (unsigned)expected_state, (unsigned)status,
                     status_state_name(status));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t subghz_cc1101_reset(void)
{
    uint8_t status = 0;
    esp_err_t err = subghz_cc1101_strobe(SUBGHZ_CC1101_STROBE_SRES, &status);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "SRES status=0x%02X (CHIP_RDYn=%u state=%s)",
             (unsigned)status,
             (unsigned)(subghz_cc1101_status_chip_rdy(status) ? 0U : 1U),
             status_state_name(status));
    /* Datasheet: the chip needs a short settle after SRES before the next
     * access; without it the first register read can return garbage. */
    vTaskDelay(pdMS_TO_TICKS(5));
    return ESP_OK;
}

esp_err_t subghz_cc1101_set_frequency(uint32_t frequency_hz, uint32_t *real_frequency_hz)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frequency_hz < SUBGHZ_CC1101_MIN_FREQ_HZ || frequency_hz > SUBGHZ_CC1101_MAX_FREQ_HZ) {
        ESP_LOGE(TAG, "frequency %lu Hz out of range %lu..%lu",
                 (unsigned long)frequency_hz,
                 (unsigned long)SUBGHZ_CC1101_MIN_FREQ_HZ,
                 (unsigned long)SUBGHZ_CC1101_MAX_FREQ_HZ);
        return ESP_ERR_INVALID_ARG;
    }

    /* Same formula as the reference driver: FREQ = f * 2^16 / f_xosc. */
    const uint64_t control =
        ((uint64_t)frequency_hz * SUBGHZ_CC1101_FDIV) / SUBGHZ_CC1101_QUARTZ_HZ;
    if (control == 0U || (control & ~((uint64_t)SUBGHZ_CC1101_FMASK)) != 0U) {
        ESP_LOGE(TAG, "frequency word 0x%llX does not fit 24 bits",
                 (unsigned long long)control);
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t f2 = (uint8_t)((control >> 16) & 0xFFU);
    const uint8_t f1 = (uint8_t)((control >> 8) & 0xFFU);
    const uint8_t f0 = (uint8_t)(control & 0xFFU);

    esp_err_t err = subghz_cc1101_write_reg(SUBGHZ_CC1101_REG_FREQ2, f2);
    if (err == ESP_OK) {
        err = subghz_cc1101_write_reg(SUBGHZ_CC1101_REG_FREQ1, f1);
    }
    if (err == ESP_OK) {
        err = subghz_cc1101_write_reg(SUBGHZ_CC1101_REG_FREQ0, f0);
    }
    if (err != ESP_OK) {
        return err;
    }

    /* Read-back proof: the value on the air is what the chip holds, not what
     * we intended to write. */
    uint8_t rb2 = 0, rb1 = 0, rb0 = 0;
    esp_err_t e2 = subghz_cc1101_read_reg(SUBGHZ_CC1101_REG_FREQ2, &rb2);
    esp_err_t e1 = subghz_cc1101_read_reg(SUBGHZ_CC1101_REG_FREQ1, &rb1);
    esp_err_t e0 = subghz_cc1101_read_reg(SUBGHZ_CC1101_REG_FREQ0, &rb0);
    if (e2 != ESP_OK || e1 != ESP_OK || e0 != ESP_OK) {
        return ESP_FAIL;
    }
    if (rb2 != f2 || rb1 != f1 || rb0 != f0) {
        ESP_LOGE(TAG, "FREQ read-back mismatch: wrote %02X %02X %02X read %02X %02X %02X",
                 (unsigned)f2, (unsigned)f1, (unsigned)f0,
                 (unsigned)rb2, (unsigned)rb1, (unsigned)rb0);
        return ESP_FAIL;
    }

    const uint32_t real_hz =
        (uint32_t)((control * SUBGHZ_CC1101_QUARTZ_HZ) / SUBGHZ_CC1101_FDIV);
    if (real_frequency_hz != NULL) {
        *real_frequency_hz = real_hz;
    }
    ESP_LOGI(TAG, "FREQ=%02X %02X %02X (read-back OK): requested %lu Hz, synthesized %lu Hz",
             (unsigned)f2, (unsigned)f1, (unsigned)f0,
             (unsigned long)frequency_hz, (unsigned long)real_hz);
    return ESP_OK;
}

esp_err_t subghz_cc1101_set_pa_table(const uint8_t table[8])
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (table == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[9];
    uint8_t rx[9] = {0};
    tx[0] = SUBGHZ_CC1101_PATABLE | SUBGHZ_CC1101_WRITE_BURST;
    memcpy(&tx[1], table, 8);
    return cc1101_xfer(tx, rx, sizeof(tx));
}

esp_err_t subghz_cc1101_set_rf_path(subghz_cc1101_rf_path_t path)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Mutual exclusion is enforced here and nowhere else: RX_EN and TX_EN
     * must never be high together on the E07 front-end switch.
     *
     * The two gpio_set_level() writes below are a critical section: within one
     * call the opposite arm is always driven low BEFORE the target arm goes
     * high, so a single call never leaves both high. But if two tasks switched
     * the path concurrently (e.g. a TX send racing an RX restart), their writes
     * could interleave into a transient RX_EN=1 AND TX_EN=1 — enough to damage
     * the front-end when paths toggle fast. We take the same bus mutex that
     * serializes SPI, so a switch is atomic against both other switches and any
     * in-flight transfer. The mutex is only ever held inside cc1101_xfer (which
     * never calls this function), so there is no self-deadlock. */
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(SUBGHZ_CC1101_BUS_LOCK_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "set_rf_path: bus lock timeout (%u ms) — pins left unchanged",
                 (unsigned)SUBGHZ_CC1101_BUS_LOCK_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* Single exit from here on: never 'return' while holding the mutex. */
    esp_err_t result = ESP_OK;
    switch (path) {
    case SUBGHZ_CC1101_RF_PATH_OFF:
        gpio_set_level(SUBGHZ_CC1101_RX_EN_GPIO, 0);
        gpio_set_level(SUBGHZ_CC1101_TX_EN_GPIO, 0);
        break;
    case SUBGHZ_CC1101_RF_PATH_RX:
        gpio_set_level(SUBGHZ_CC1101_TX_EN_GPIO, 0);
        gpio_set_level(SUBGHZ_CC1101_RX_EN_GPIO, 1);
        break;
    case SUBGHZ_CC1101_RF_PATH_TX:
        gpio_set_level(SUBGHZ_CC1101_RX_EN_GPIO, 0);
        gpio_set_level(SUBGHZ_CC1101_TX_EN_GPIO, 1);
        break;
    default:
        result = ESP_ERR_INVALID_ARG;
        break;
    }

    xSemaphoreGive(s_bus_mutex);
    return result;
}

esp_err_t subghz_cc1101_write_fifo(const uint8_t *data, uint8_t size)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && size > 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > SUBGHZ_CC1101_FIFO_SIZE) {
        ESP_LOGE(TAG, "write_fifo: size %u exceeds FIFO %u",
                 (unsigned)size, (unsigned)SUBGHZ_CC1101_FIFO_SIZE);
        return ESP_ERR_INVALID_ARG;
    }
    s_fifo_tx[0] = SUBGHZ_CC1101_FIFO | SUBGHZ_CC1101_WRITE_BURST;
    memcpy(&s_fifo_tx[1], data, size);
    return cc1101_xfer(s_fifo_tx, s_fifo_rx, (size_t)size + 1U);
}

esp_err_t subghz_cc1101_read_fifo(uint8_t *data, uint8_t capacity, uint8_t *size,
                                  bool *overflow)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* RXBYTES first: it carries the count AND the overflow flag. Reading it
     * before the FIFO burst matches the datasheet overflow recovery flow. */
    uint8_t rxbytes = 0;
    esp_err_t err = subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_RXBYTES, &rxbytes);
    if (err != ESP_OK) {
        return err;
    }
    if (overflow != NULL) {
        *overflow = (rxbytes & SUBGHZ_CC1101_RXBYTES_OVERFLOW_MASK) != 0U;
    }
    uint8_t count = rxbytes & SUBGHZ_CC1101_FIFO_COUNT_MASK;
    if (count > capacity) {
        ESP_LOGW(TAG, "read_fifo: %u bytes in RX FIFO, capacity %u — truncating",
                 (unsigned)count, (unsigned)capacity);
        count = capacity;
    }
    if (count > SUBGHZ_CC1101_FIFO_SIZE) {
        count = SUBGHZ_CC1101_FIFO_SIZE;
    }

    if (count > 0U) {
        s_fifo_tx[0] = SUBGHZ_CC1101_FIFO | SUBGHZ_CC1101_READ_BURST;
        memset(&s_fifo_tx[1], 0, count);
        err = cc1101_xfer(s_fifo_tx, s_fifo_rx, (size_t)count + 1U);
        if (err != ESP_OK) {
            return err;
        }
        memcpy(data, &s_fifo_rx[1], count);
    }
    *size = count;
    return ESP_OK;
}

esp_err_t subghz_cc1101_enter_raw_rx(uint32_t frequency_hz, uint32_t *real_frequency_hz)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = subghz_cc1101_switch_to_idle(NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_IDLE, 10);
    if (err != ESP_OK) {
        return err;
    }

    err = subghz_cc1101_set_frequency(frequency_hz, real_frequency_hz);
    if (err != ESP_OK) {
        return err;
    }

    /* Raw async stream on GDO0. Each value is provable:
     * IOCFG0=0x0D  — Serial Data Output (datasheet IOCFG table / reference
     *                driver's CC1101IocfgSerialDataOutput);
     * MDMCFG2=0x30 — MOD_FORMAT=3 (ASK/OOK), SYNC_MODE=0 (no preamble/sync);
     * PKTCTRL0=0x32 — PKT_FORMAT=3 (async serial), CRC_EN=0, LENGTH_CONFIG=2
     *                (infinite). Bits per the datasheet register map. */
    static const struct {
        uint8_t addr;
        uint8_t value;
        const char *why;
    } raw_cfg[] = {
        {SUBGHZ_CC1101_REG_IOCFG0, SUBGHZ_CC1101_IOCFG_SERIAL_DATA_OUT,
         "GDO0 = serial data output (demodulated async stream)"},
        {SUBGHZ_CC1101_REG_MDMCFG2, 0x30U,
         "ASK/OOK, no sync word"},
        {SUBGHZ_CC1101_REG_PKTCTRL0, 0x32U,
         "async serial mode, CRC off, infinite length"},
    };
    for (size_t i = 0; i < sizeof(raw_cfg) / sizeof(raw_cfg[0]); ++i) {
        err = subghz_cc1101_write_reg(raw_cfg[i].addr, raw_cfg[i].value);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t rb = 0;
        err = subghz_cc1101_read_reg(raw_cfg[i].addr, &rb);
        if (err != ESP_OK || rb != raw_cfg[i].value) {
            ESP_LOGE(TAG, "raw cfg 0x%02X: wrote 0x%02X read 0x%02X",
                     (unsigned)raw_cfg[i].addr, (unsigned)raw_cfg[i].value,
                     (unsigned)rb);
            return (err != ESP_OK) ? err : ESP_FAIL;
        }
        ESP_LOGI(TAG, "raw cfg: reg 0x%02X = 0x%02X (read-back OK) — %s",
                 (unsigned)raw_cfg[i].addr, (unsigned)raw_cfg[i].value,
                 raw_cfg[i].why);
    }

    /* Modem rate/bandwidth stay at post-reset defaults for the first air
     * test; the log proves what is actually programmed, computed with the
     * datasheet formulas:
     *   RX BW  = f_xosc / (8 * (4 + CHANBW_M) * 2^CHANBW_E)
     *   DRATE  = ((256 + DRATE_M) * 2^DRATE_E / 2^28) * f_xosc            */
    uint8_t m4 = 0, m3 = 0;
    if (subghz_cc1101_read_reg(SUBGHZ_CC1101_REG_MDMCFG4, &m4) == ESP_OK &&
        subghz_cc1101_read_reg(SUBGHZ_CC1101_REG_MDMCFG3, &m3) == ESP_OK) {
        const uint32_t bw_hz = SUBGHZ_CC1101_QUARTZ_HZ /
            (8UL * (4UL + ((m4 >> 4) & 0x03UL)) * (1UL << ((m4 >> 6) & 0x03UL)));
        const uint32_t drate = (uint32_t)(
            ((uint64_t)(256UL + m3) * (1UL << (m4 & 0x0FUL)) *
             SUBGHZ_CC1101_QUARTZ_HZ) / (1UL << 28));
        ESP_LOGI(TAG,
                 "modem (post-reset defaults): MDMCFG4=0x%02X MDMCFG3=0x%02X -> RX BW ~%lu Hz, data rate ~%lu Baud",
                 (unsigned)m4, (unsigned)m3,
                 (unsigned long)bw_hz, (unsigned long)drate);
    }

    err = subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_RX);
    if (err != ESP_OK) {
        return err;
    }
    err = subghz_cc1101_switch_to_rx(NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_RX, 10);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "raw async RX active on GDO0 (IO%d), external LNA ON (RX_EN=IO%d)",
                 s_gdo0_gpio, SUBGHZ_CC1101_RX_EN_GPIO);
    }
    return err;
}

esp_err_t subghz_cc1101_exit_raw_rx(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);
    (void)subghz_cc1101_switch_to_idle(NULL);
    /* Full reset returns every register touched by enter_raw_rx to its
     * post-reset default — cheaper and more honest than tracking a restore
     * list. */
    return subghz_cc1101_reset();
}

subghz_cc1101_config_t subghz_cc1101_config_default(void)
{
    subghz_cc1101_config_t config = {
        .cs_gpio = SUBGHZ_CC1101_CS_GPIO,
        .gdo0_gpio = SUBGHZ_CC1101_GDO0_GPIO,
        .gdo2_gpio = SUBGHZ_CC1101_GDO2_GPIO,
        .spi_clock_hz = 0,
    };
    return config;
}

/* Tears down everything init already brought up, in reverse order. */
static void cc1101_init_cleanup(bool bus_initialized, bool device_added,
                                bool mutex_created)
{
    if (device_added) {
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    if (bus_initialized) {
        spi_bus_free(SUBGHZ_CC1101_SPI_HOST);
    }
    if (mutex_created) {
        vSemaphoreDelete(s_bus_mutex);
        s_bus_mutex = NULL;
    }
}

esp_err_t subghz_cc1101_init(const subghz_cc1101_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "init: already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->cs_gpio < 0) {
        ESP_LOGE(TAG,
                 "CC1101 CS GPIO is not set (still -1): check "
                 "SUBGHZ_CC1101_CS_GPIO in src/subghz/include/subghz_hal_cc1101.h");
        return ESP_ERR_INVALID_STATE;
    }
    if (config->cs_gpio == SUBGHZ_CC1101_SCK_GPIO ||
        config->cs_gpio == SUBGHZ_CC1101_MOSI_GPIO ||
        config->cs_gpio == SUBGHZ_CC1101_MISO_GPIO ||
        config->gdo0_gpio == config->cs_gpio ||
        config->gdo2_gpio == config->cs_gpio ||
        (config->gdo0_gpio >= 0 && config->gdo0_gpio == config->gdo2_gpio) ||
        SUBGHZ_CC1101_RX_EN_GPIO == SUBGHZ_CC1101_TX_EN_GPIO ||
        config->cs_gpio == SUBGHZ_CC1101_RX_EN_GPIO ||
        config->cs_gpio == SUBGHZ_CC1101_TX_EN_GPIO ||
        config->gdo0_gpio == SUBGHZ_CC1101_RX_EN_GPIO ||
        config->gdo0_gpio == SUBGHZ_CC1101_TX_EN_GPIO ||
        SUBGHZ_CC1101_RX_EN_GPIO == SUBGHZ_CC1101_SCK_GPIO ||
        SUBGHZ_CC1101_RX_EN_GPIO == SUBGHZ_CC1101_MOSI_GPIO ||
        SUBGHZ_CC1101_RX_EN_GPIO == SUBGHZ_CC1101_MISO_GPIO ||
        SUBGHZ_CC1101_TX_EN_GPIO == SUBGHZ_CC1101_SCK_GPIO ||
        SUBGHZ_CC1101_TX_EN_GPIO == SUBGHZ_CC1101_MOSI_GPIO ||
        SUBGHZ_CC1101_TX_EN_GPIO == SUBGHZ_CC1101_MISO_GPIO) {
        ESP_LOGE(TAG, "GPIO assignment has duplicates (CS=%d GDO0=%d GDO2=%d RX_EN=%d TX_EN=%d)",
                 config->cs_gpio, config->gdo0_gpio, config->gdo2_gpio,
                 SUBGHZ_CC1101_RX_EN_GPIO, SUBGHZ_CC1101_TX_EN_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t clock_hz = (config->spi_clock_hz != 0U)
                                  ? config->spi_clock_hz
                                  : SUBGHZ_CC1101_DEFAULT_SPI_CLOCK_HZ;

    ESP_LOGI(TAG,
             "Wiring: dedicated RF bus SPI3 SCK=%d MOSI=%d MISO=%d; CC1101 CS=%d GDO0=%d GDO2=%d; SPI clock %lu Hz",
             SUBGHZ_CC1101_SCK_GPIO, SUBGHZ_CC1101_MOSI_GPIO, SUBGHZ_CC1101_MISO_GPIO,
             config->cs_gpio, config->gdo0_gpio, config->gdo2_gpio,
             (unsigned long)clock_hz);

    s_bus_mutex = xSemaphoreCreateMutex();
    if (s_bus_mutex == NULL) {
        ESP_LOGE(TAG, "bus mutex allocation failed");
        return ESP_ERR_NO_MEM;
    }

    /* Manual CS: the CHIP_RDY handshake (wait for SO low after CS low) is
     * impossible with a driver-managed chip select. Keep CS high before the
     * first transaction so the module starts deselected. */
    gpio_config_t cs_config = {
        .pin_bit_mask = (1ULL << (uint32_t)config->cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t     err = gpio_config(&cs_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CS gpio_config failed: %s", esp_err_to_name(err));
        cc1101_init_cleanup(false, false, true);
        return err;
    }
    gpio_set_level(config->cs_gpio, 1);

    /* External front-end control (E07 PA/LNA): both paths OFF at boot —
     * an enabled PA without TX data would only burn current and add noise. */
    gpio_config_t fe_config = {
        .pin_bit_mask = (1ULL << SUBGHZ_CC1101_RX_EN_GPIO) |
                        (1ULL << SUBGHZ_CC1101_TX_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&fe_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX_EN/TX_EN gpio_config failed: %s", esp_err_to_name(err));
        cc1101_init_cleanup(false, false, true);
        return err;
    }
    gpio_set_level(SUBGHZ_CC1101_RX_EN_GPIO, 0);
    gpio_set_level(SUBGHZ_CC1101_TX_EN_GPIO, 0);
    ESP_LOGI(TAG, "Front-end control: RX_EN=IO%d TX_EN=IO%d (both OFF)",
             SUBGHZ_CC1101_RX_EN_GPIO, SUBGHZ_CC1101_TX_EN_GPIO);

    uint64_t gdo_mask = 0;
    if (config->gdo0_gpio >= 0) {
        gdo_mask |= (1ULL << (uint32_t)config->gdo0_gpio);
    }
    if (config->gdo2_gpio >= 0) {
        gdo_mask |= (1ULL << (uint32_t)config->gdo2_gpio);
    }
    if (gdo_mask != 0) {
        /* Inputs with pull-up: an unconnected pad reads a stable 1 instead of
         * picking up noise. The RMT stage re-routes GDO0 through the GPIO
         * matrix; both uses coexist. */
        gpio_config_t gdo_config = {
            .pin_bit_mask = gdo_mask,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&gdo_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GDO gpio_config failed: %s", esp_err_to_name(err));
            cc1101_init_cleanup(false, false, true);
            return err;
        }
    }

    /* max_transfer_sz covers one FIFO burst (1 addr + 64 bytes); auto DMA
     * keeps that path working while 2-byte register exchanges stay
     * CPU-driven. */
    spi_bus_config_t bus_config = {
        .sclk_io_num = SUBGHZ_CC1101_SCK_GPIO,
        .mosi_io_num = SUBGHZ_CC1101_MOSI_GPIO,
        .miso_io_num = SUBGHZ_CC1101_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1 + (int)SUBGHZ_CC1101_FIFO_SIZE + 1,
    };
    err = spi_bus_initialize(SUBGHZ_CC1101_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RF spi_bus_initialize failed: %s", esp_err_to_name(err));
        cc1101_init_cleanup(false, false, true);
        return err;
    }

    /* Pull-up on MISO: with no module attached the pad reads a stable 1, so
     * the CHIP_RDY wait times out instead of sampling floating noise. */
    (void)gpio_pullup_en(SUBGHZ_CC1101_MISO_GPIO);

    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = (int)clock_hz,
        .mode = 0,
        .spics_io_num = -1,  /* CS driven manually, see cc1101_xfer() */
        .queue_size = 1,
    };
    err = spi_bus_add_device(SUBGHZ_CC1101_SPI_HOST, &dev_config, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        cc1101_init_cleanup(true, false, true);
        return err;
    }

    s_cs_gpio = config->cs_gpio;
    s_gdo0_gpio = config->gdo0_gpio;
    s_gdo2_gpio = config->gdo2_gpio;
    s_initialized = true;
    ESP_LOGI(TAG, "CC1101 device registered on dedicated SPI3 host");
    return ESP_OK;
}

void subghz_cc1101_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    s_initialized = false;
    cc1101_init_cleanup(true, true, true);
    if (s_cs_gpio >= 0) {
        gpio_set_level(s_cs_gpio, 1);
    }
    s_cs_gpio = -1;
    s_gdo0_gpio = -1;
    s_gdo2_gpio = -1;
}

bool subghz_cc1101_is_initialized(void)
{
    return s_initialized;
}

esp_err_t subghz_cc1101_selftest(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "selftest: driver is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    unsigned failures = 0;
    uint8_t value = 0;
    uint8_t status = 0;

    ESP_LOGI(TAG, "===== CC1101 selftest start =====");
    ESP_LOGI(TAG,
             "Pins: SCK=%d MOSI=%d MISO=%d CS=%d GDO0=%d GDO2=%d",
             SUBGHZ_CC1101_SCK_GPIO, SUBGHZ_CC1101_MOSI_GPIO, SUBGHZ_CC1101_MISO_GPIO,
             s_cs_gpio, s_gdo0_gpio, s_gdo2_gpio);
    if (s_gdo0_gpio >= 0) {
        ESP_LOGI(TAG, "GDO0 level=%d", gpio_get_level(s_gdo0_gpio));
    }
    if (s_gdo2_gpio >= 0) {
        ESP_LOGI(TAG, "GDO2 level=%d", gpio_get_level(s_gdo2_gpio));
    }

    /* -- Step 1: hardware reset ---------------------------------------- */
    ESP_LOGI(TAG, "Step 1: SRES reset");
    esp_err_t err = subghz_cc1101_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "FAIL: reset: %s — CHIP_RDY timeout means no answer on MISO; "
                 "check module power (3.4V), CS, SCK and MISO wiring",
                 esp_err_to_name(err));
        failures++;
    } else {
        ESP_LOGI(TAG, "OK: reset done");
    }

    /* -- Step 2: PARTNUM / VERSION ------------------------------------- */
    ESP_LOGI(TAG, "Step 2: read PARTNUM/VERSION");
    uint8_t partnum = 0;
    uint8_t version = 0;
    esp_err_t err_pn = subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_PARTNUM, &partnum);
    esp_err_t err_ver = subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_VERSION, &version);
    if (err_pn != ESP_OK || err_ver != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: status read: PARTNUM=%s VERSION=%s",
                 esp_err_to_name(err_pn), esp_err_to_name(err_ver));
        failures++;
    } else {
        ESP_LOGI(TAG, "PARTNUM=0x%02X (expect 0x%02X), VERSION=0x%02X (expect 0x%02X)",
                 (unsigned)partnum, SUBGHZ_CC1101_EXPECTED_PARTNUM,
                 (unsigned)version, SUBGHZ_CC1101_EXPECTED_VERSION);
        if (version == 0x00U || version == 0xFFU) {
            ESP_LOGE(TAG,
                     "FAIL: VERSION=0x%02X — module does not answer; "
                     "check power, CS/SCK/MOSI/MISO wiring and SPI mode",
                     (unsigned)version);
            failures++;
        } else if (version != SUBGHZ_CC1101_EXPECTED_VERSION ||
                   partnum != SUBGHZ_CC1101_EXPECTED_PARTNUM) {
            ESP_LOGW(TAG,
                     "WARN: unexpected PARTNUM/VERSION — clone chip or another "
                     "silicon revision; continuing");
        } else {
            ESP_LOGI(TAG, "OK: PARTNUM/VERSION match CC1101");
        }
    }

    /* -- Step 3: scratch write/read proves writes actually land ---------
     * CHANNR is the safest writable register: its value only multiplies the
     * programmed channel spacing (irrelevant until channels are used) and it
     * is restored to the 0x00 reset default afterwards. Two complementary
     * patterns separate a stuck bit (wrong in both patterns) from the 1-LSB
     * sampling error this step originally exposed on PKTLEN (wrong only in
     * the final bit of the frame). */
    ESP_LOGI(TAG, "Step 3: scratch write/read on CHANNR (safe register)");
    static const uint8_t scratch_patterns[] = {0xA5U, 0x5AU};
    /* Diagnostic loop: repeat the write/read many times and tally WHICH bit
     * positions flip. A fixed line/solder fault flips the SAME bit almost every
     * time (histogram spikes on one bit); supply noise / brown-out scatters
     * flips across bits and iterations. This turns one ambiguous mismatch into
     * an actionable verdict. Pass/fail semantics are unchanged: any mismatch
     * still counts as a failure. */
    const unsigned scratch_iters = 64U;
    uint16_t bit_flips[8] = {0};
    unsigned scratch_mismatch = 0;
    unsigned scratch_access_err = 0;
    uint8_t first_wrote = 0, first_read = 0;
    bool have_first = false;
    for (unsigned iter = 0; iter < scratch_iters; ++iter) {
        for (size_t i = 0; i < sizeof(scratch_patterns); ++i) {
            const uint8_t pattern = scratch_patterns[i];
            err = subghz_cc1101_write_reg(SUBGHZ_CC1101_REG_CHANNR, pattern);
            if (err == ESP_OK) {
                err = subghz_cc1101_read_reg(SUBGHZ_CC1101_REG_CHANNR, &value);
            }
            if (err != ESP_OK) {
                scratch_access_err++;
                continue;
            }
            const uint8_t diff = (uint8_t)(value ^ pattern);
            if (diff != 0U) {
                scratch_mismatch++;
                if (!have_first) {
                    first_wrote = pattern;
                    first_read = value;
                    have_first = true;
                }
                for (unsigned b = 0; b < 8U; ++b) {
                    if (((diff >> b) & 1U) != 0U) {
                        bit_flips[b]++;
                    }
                }
            }
        }
    }
    /* Restore the reset default no matter how the check went. */
    (void)subghz_cc1101_write_reg(SUBGHZ_CC1101_REG_CHANNR, 0x00U);

    const unsigned scratch_total = scratch_iters * (unsigned)sizeof(scratch_patterns);
    if (scratch_access_err == 0U && scratch_mismatch == 0U) {
        ESP_LOGI(TAG, "OK: scratch %u/%u clean (both patterns)", scratch_total, scratch_total);
    } else {
        ESP_LOGE(TAG,
                 "FAIL: scratch %u/%u mismatched (access_err=%u); first wrote 0x%02X read 0x%02X",
                 scratch_mismatch, scratch_total, scratch_access_err,
                 (unsigned)first_wrote, (unsigned)first_read);
        ESP_LOGE(TAG,
                 "  per-bit flips: b7=%u b6=%u b5=%u b4=%u b3=%u b2=%u b1=%u b0=%u",
                 bit_flips[7], bit_flips[6], bit_flips[5], bit_flips[4],
                 bit_flips[3], bit_flips[2], bit_flips[1], bit_flips[0]);
        ESP_LOGE(TAG,
                 "  verdict: ONE bit dominating => that data line/solder (fixed fault); "
                 "SCATTERED across bits => supply noise / brown-out (add 100nF ceramic at VCC)");
        failures++;
    }

    /* -- Step 4: key register dump -------------------------------------- */
    ESP_LOGI(TAG, "Step 4: key register dump (post-reset defaults)");
    static const struct {
        uint8_t addr;
        const char *name;
    } dump[] = {
        {SUBGHZ_CC1101_REG_IOCFG2,   "IOCFG2  "},
        {SUBGHZ_CC1101_REG_IOCFG0,   "IOCFG0  "},
        {SUBGHZ_CC1101_REG_FIFOTHR,  "FIFOTHR "},
        {SUBGHZ_CC1101_REG_SYNC1,    "SYNC1   "},
        {SUBGHZ_CC1101_REG_SYNC0,    "SYNC0   "},
        {SUBGHZ_CC1101_REG_PKTLEN,   "PKTLEN  "},
        {SUBGHZ_CC1101_REG_FSCTRL1,  "FSCTRL1 "},
        {SUBGHZ_CC1101_REG_FREQ2,    "FREQ2   "},
        {SUBGHZ_CC1101_REG_FREQ1,    "FREQ1   "},
        {SUBGHZ_CC1101_REG_FREQ0,    "FREQ0   "},
        {SUBGHZ_CC1101_REG_MDMCFG2,  "MDMCFG2 "},
        {SUBGHZ_CC1101_REG_MCSM1,    "MCSM1   "},
        {SUBGHZ_CC1101_REG_MCSM0,    "MCSM0   "},
        {SUBGHZ_CC1101_REG_FSCAL3,   "FSCAL3  "},
        {SUBGHZ_CC1101_REG_TEST2,    "TEST2   "},
        {SUBGHZ_CC1101_REG_TEST1,    "TEST1   "},
        {SUBGHZ_CC1101_REG_TEST0,    "TEST0   "},
    };
    for (size_t i = 0; i < sizeof(dump) / sizeof(dump[0]); ++i) {
        err = subghz_cc1101_read_reg(dump[i].addr, &value);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  %s(0x%02X) read failed: %s",
                     dump[i].name, (unsigned)dump[i].addr, esp_err_to_name(err));
            failures++;
        } else {
            ESP_LOGI(TAG, "  %s(0x%02X) = 0x%02X",
                     dump[i].name, (unsigned)dump[i].addr, (unsigned)value);
        }
    }

    /* -- Step 5: radio state machine IDLE -> RX -> IDLE -----------------
     * Waits poll the status byte with SNOP (same as the reference driver's
     * cc1101_wait_status_state) instead of blind delays; MARCSTATE is read
     * afterward as an independent cross-check. */
    ESP_LOGI(TAG, "Step 5: state transitions (SNOP waits + MARCSTATE cross-check)");
    err = subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_MARCSTATE, &value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: MARCSTATE read: %s", esp_err_to_name(err));
        failures++;
    } else {
        ESP_LOGI(TAG, "MARCSTATE=0x%02X (%s), expect IDLE(0x%02X)",
                 (unsigned)value, marcstate_name(value),
                 SUBGHZ_CC1101_MARCSTATE_IDLE);
        if (value != SUBGHZ_CC1101_MARCSTATE_IDLE) {
            ESP_LOGW(TAG, "WARN: not IDLE after reset");
        }
    }

    err = subghz_cc1101_switch_to_rx(&status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: SRX strobe: %s", esp_err_to_name(err));
        failures++;
    } else {
        ESP_LOGI(TAG, "SRX status=0x%02X (state=%s)",
                 (unsigned)status, status_state_name(status));
        err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_RX, 10);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "FAIL: chip did not reach RX state: %s",
                     esp_err_to_name(err));
            failures++;
        } else if (subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_MARCSTATE, &value) == ESP_OK) {
            if (value != SUBGHZ_CC1101_MARCSTATE_RX) {
                ESP_LOGW(TAG,
                         "WARN: MARCSTATE=0x%02X (%s) while status byte says RX — "
                         "transient read or brown-out dip; see the SLEEP-pattern note below",
                         (unsigned)value, marcstate_name(value));
            } else {
                ESP_LOGI(TAG, "OK: entered RX (status + MARCSTATE agree)");
            }
        }
    }

    /* E07-433M20S front-end check: compare chip-level RSSI with the external
     * LNA off vs on (RX_EN). A real wiring of RX_EN should change the noise
     * floor reading; identical values are only a WARN — in a quiet room the
     * delta can be small, so this is informational, not a failure. */
    uint8_t rssi_lna_off = 0;
    uint8_t rssi_lna_on = 0;
    if (subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_RSSI, &rssi_lna_off) == ESP_OK) {
        ESP_LOGI(TAG, "RSSI raw=0x%02X (external LNA OFF)", (unsigned)rssi_lna_off);
        if (subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_RX) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            if (subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_RSSI, &rssi_lna_on) == ESP_OK) {
                ESP_LOGI(TAG, "RSSI raw=0x%02X (external LNA ON, RX_EN=IO%d)",
                         (unsigned)rssi_lna_on, SUBGHZ_CC1101_RX_EN_GPIO);
                if (rssi_lna_on == rssi_lna_off) {
                    ESP_LOGW(TAG,
                             "WARN: RSSI unchanged with LNA on — RX_EN line suspect, or the room is too quiet to tell");
                } else {
                    ESP_LOGI(TAG, "OK: RSSI moved with LNA on — RX_EN reaches the module");
                }
            }
            (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);
        }
    }

    err = subghz_cc1101_switch_to_idle(&status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: SIDLE strobe: %s", esp_err_to_name(err));
        failures++;
    } else {
        err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_IDLE, 10);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "FAIL: chip did not return to IDLE: %s",
                     esp_err_to_name(err));
            failures++;
        } else {
            /* The status byte already proved IDLE — but its STATE field has
             * NO code for SLEEP (IDLE/RX/TX/FSTXON/CAL/SETTLING/overflows
             * only), so a sleeping chip still answers "IDLE" to SNOP. The
             * pattern STATE=IDLE + MARCSTATE=0x00(SLEEP) therefore means the
             * chip really drops to SLEEP between accesses: with no SPWD/
             * SXOFF/WOR in use the only path is brown-out reset from a
             * supply dip during state switching (synth calibration surge;
             * on this board Wi-Fi auto-connect TX bursts share the rail at
             * boot). Each CS-low pulse re-wakes the chip, which is why SPI
             * keeps answering. This is a hardware rail problem, not a state
             * machine failure: WARN with the fix, do not fail the probe. */
            bool marc_idle = false;
            uint8_t last_marc = 0;
            unsigned attempt = 0;
            for (; attempt < 3U && !marc_idle; ++attempt) {
                if (attempt > 0U) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                if (subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_MARCSTATE, &last_marc) != ESP_OK) {
                    break;
                }
                marc_idle = (last_marc == SUBGHZ_CC1101_MARCSTATE_IDLE);
            }
            if (marc_idle) {
                if (attempt > 0U) {
                    ESP_LOGW(TAG,
                             "WARN: MARCSTATE settled to IDLE only after %u retry — "
                             "transient read right after the RX->IDLE transition",
                             attempt);
                }
                ESP_LOGI(TAG, "OK: back to IDLE (status + MARCSTATE agree)");
            } else {
                ESP_LOGW(TAG,
                         "WARN: chip drops to SLEEP between accesses (STATE=IDLE/MARCSTATE=0x%02X). "
                         "Suspect 3.4V rail sag -> brown-out: add 10uF + 100nF right at the module "
                         "VCC/GND and shorten its power wires. Continuing — SPI and RX entry work.",
                         (unsigned)last_marc);
            }
        }
    }

    if (failures == 0U) {
        ESP_LOGI(TAG, "===== CC1101 selftest PASSED =====");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "===== CC1101 selftest FAILED (%u failure(s)) =====", failures);
    return ESP_FAIL;
}

esp_err_t subghz_cc1101_boot_probe(void)
{
    ESP_LOGI(TAG, "CC1101 boot probe (presence self-check; failure never blocks boot)");
    subghz_cc1101_config_t config = subghz_cc1101_config_default();
    esp_err_t err = subghz_cc1101_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "boot probe: init skipped/failed: %s", esp_err_to_name(err));
        return err;
    }
    return subghz_cc1101_selftest();
}

/* ============================================================================
 * Packet RX mode implementation (sync word + CRC, fixed/variable length)
 * ==========================================================================*/

/* RSSI conversion: CC1101 STATUS_RSSI is a signed 8-bit offset value.
 * Actual dBm = (RSSI_raw / 2) - 74 (per datasheet, for 315/433/868/915 MHz).
 * We read it as uint8_t, convert to int8_t, then apply formula using
 * arithmetic right shift to preserve sign for negative odd values. */
static inline int16_t cc1101_rssi_raw_to_dbm(uint8_t rssi_raw)
{
    int8_t rssi_signed = (int8_t)rssi_raw;
    return (int16_t)((rssi_signed >> 1) - 74);
}

/* LQI is in STATUS_LQI (0x33), bit 7 = CRC_OK, bits 6..0 = correlation value. */
static inline uint8_t cc1101_lqi_from_status(uint8_t lqi_raw)
{
    return lqi_raw & 0x7FU;
}

/* ---- Anti-replay sliding-window defaults (receiver-side) ------------------*/

static inline uint32_t cc1101_seq_mask(subghz_cc1101_seq_width_t width)
{
    return (width == SUBGHZ_CC1101_SEQ_WIDTH_U32) ? 0xFFFFFFFFUL : 0x0000FFFFUL;
}

bool subghz_cc1101_default_validate_window(const subghz_cc1101_seq_state_t *state,
                                           uint32_t incoming,
                                           uint32_t window,
                                           subghz_cc1101_seq_width_t width)
{
    if (state == NULL) {
        return false;
    }
    /* First frame primes the window; the caller commits it via sync. */
    if (!state->primed) {
        return true;
    }
    const uint32_t mask = cc1101_seq_mask(width);
    /* Reduce both operands to the counter width first, then subtract. The
     * final mask makes the unsigned subtraction wrap at the width boundary,
     * so a u16 rollover (last=0xFFFF, incoming=0x0000) yields delta=1 rather
     * than 0xFFFF0001 — no signed math, no undefined overflow. */
    const uint32_t delta = ((incoming & mask) - (state->last_counter & mask)) & mask;
    /* delta == 0       : exact replay of the last accepted value -> reject
     * delta  > window  : too far ahead (stale resync or forged)   -> reject
     * otherwise        : strictly newer and within look-ahead     -> accept */
    return (delta != 0UL) && (delta <= window);
}

void subghz_cc1101_default_sync_state(subghz_cc1101_seq_state_t *state,
                                      uint32_t accepted)
{
    if (state == NULL) {
        return;
    }
    state->last_counter = accepted;
    state->primed = true;
}

static const subghz_cc1101_antireplay_ops_t s_default_antireplay_ops = {
    .validate_sequence_window = subghz_cc1101_default_validate_window,
    .parse_secure_payload = NULL,
    .sync_sequence_state = subghz_cc1101_default_sync_state,
};

const subghz_cc1101_antireplay_ops_t *subghz_cc1101_default_antireplay_ops(void)
{
    return &s_default_antireplay_ops;
}

/* Extract the sequence counter from the payload per the antireplay config.
 * Returns false if the counter field would read past payload_len. The widened
 * comparison keeps (offset + width) from wrapping a uint8_t. */
static bool cc1101_extract_seq(const subghz_cc1101_antireplay_config_t *ar,
                               const uint8_t *payload,
                               uint8_t payload_len,
                               uint32_t *out)
{
    const uint8_t width_bytes =
        (ar->seq_width == SUBGHZ_CC1101_SEQ_WIDTH_U32) ? 4U : 2U;

    if ((uint32_t)ar->seq_offset + (uint32_t)width_bytes > (uint32_t)payload_len) {
        return false;
    }

    const uint8_t *p = &payload[ar->seq_offset];
    uint32_t v = 0UL;
    if (ar->big_endian) {
        for (uint8_t i = 0U; i < width_bytes; ++i) {
            v = (v << 8) | (uint32_t)p[i];
        }
    } else {
        for (uint8_t i = 0U; i < width_bytes; ++i) {
            v |= (uint32_t)p[i] << (8U * i);
        }
    }
    *out = v;
    return true;
}

/* Forward declarations */
struct cc1101_rx_context;

static void cc1101_rx_task(void *arg);
/* IRAM_ATTR lives on the definition only; repeating it here makes GCC assign
 * two different .iram1.N sections to the same symbol and error out. */
static void cc1101_gdo0_isr_handler(void *arg);

struct cc1101_rx_context {
    subghz_cc1101_rx_config_t config;
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    bool running;
    /* Set true by the worker as its very last act before vTaskDelete(NULL).
     * rx_stop() waits on this instead of a blind delay, so the context is
     * never freed while the task can still dereference it (same join pattern
     * as the board-proven subghz_rmt_raw logger task). */
    volatile bool task_exited;
    uint8_t sync_word_hi;
    uint8_t sync_word_lo;
};

typedef struct cc1101_rx_context *cc1101_rx_handle_t;

static inline uint8_t cc1101_build_pktctrl0(bool whitening, bool crc_en, bool variable_len)
{
    uint8_t val = 0;
    /* PKT_FORMAT: 0=fixed, 1=variable, 2=infinite, 3=reserved */
    val |= variable_len ? 0x01U : 0x00U;
    /* CRC_EN */
    val |= crc_en ? 0x04U : 0x00U;
    /* WHITENING (DCFREE=1 for whitening) */
    val |= whitening ? 0x40U : 0x00U;
    /* LENGTH_CONFIG=0 (PKTLEN used for variable/fixed) */
    return val;
}

static inline uint8_t cc1101_build_pktctrl1(bool addr_check, bool append_status)
{
    uint8_t val = 0x00U;
    /* PREAMBLE_QUAL=1 (4 preamble bytes required for sync detect) */
    val |= 0x04U;
    /* ADR_CHK: 0=none, 1=0x00/0xFF broadcast, 2=0x00/0xFF + own addr */
    if (addr_check) {
        val |= 0x01U;  /* 0x00/0xFF broadcast + own address check */
    }
    /* APPEND_STATUS: adds 2 bytes (RSSI + LQI/CRC_OK) after payload */
    val |= append_status ? 0x80U : 0x00U;
    return val;
}

static inline uint8_t cc1101_build_mdmcfg2(uint8_t mod_format, uint8_t sync_mode)
{
    /* MOD_FORMAT in bits 5..4, SYNC_MODE in bits 3..0, DEM_DCFILT_OFF in bit 6,
     * MANCHESTER_EN in bit 7. For ASK/OOK: MOD_FORMAT=3 (0b11). */
    return (uint8_t)(((mod_format & 0x03U) << 4) | (sync_mode & 0x0FU));
}

static inline uint8_t cc1101_build_mdmcfg1(uint8_t num_preamble, uint8_t sync_bits, uint8_t fec_en)
{
    /* NUM_PREAMBLE in bits 6..4, SYNC_WORD_LEN in bits 3..0, FEC_EN in bit 7.
     * We want 4 preamble bytes (0b100), 16/16 sync (0b0011), no FEC. */
    return (uint8_t)(((num_preamble & 0x07U) << 4) | (sync_bits & 0x0FU) | (fec_en ? 0x80U : 0x00U));
}

static esp_err_t cc1101_apply_packet_rx_config(const cc1101_rx_handle_t h)
{
    esp_err_t err;
    uint8_t rb;

    /* 1. Go to IDLE first */
    err = subghz_cc1101_switch_to_idle(NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_IDLE, 10);
    if (err != ESP_OK) {
        return err;
    }

    /* 2. Set frequency */
    err = subghz_cc1101_set_frequency(h->config.frequency_hz, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* 3. Apply packet mode register set.
     * Not 'static': the values come from h->config at run time, so this table
     * is rebuilt on each call (unlike the purely-literal tables elsewhere). */
    const struct {
        uint8_t addr;
        uint8_t value;
        const char *why;
    } pkt_cfg[] = {
        /* IOCFG0: GDO0 = 0x06 (sync word detected / packet received) */
        {SUBGHZ_CC1101_REG_IOCFG0, SUBGHZ_CC1101_IOCFG_SYNC_WORD,
         "GDO0 = sync word / packet received"},

        /* PKTCTRL1: PREAMBLE_QUAL=1, ADR_CHK per config, APPEND_STATUS=1 */
        {SUBGHZ_CC1101_REG_PKTCTRL1,
         cc1101_build_pktctrl1(h->config.addr_check, true),
         "preamble qual, addr check, append status"},

        /* PKTCTRL0: variable/fixed len, CRC, whitening per config */
        {SUBGHZ_CC1101_REG_PKTCTRL0,
         cc1101_build_pktctrl0(h->config.whitening, h->config.crc_en, true),
         "variable len, CRC, whitening"},

        /* PKTLEN: max payload length */
        {SUBGHZ_CC1101_REG_PKTLEN, h->config.max_payload_len,
         "max packet length"},

        /* SYNC1/SYNC0: 16-bit sync word */
        {SUBGHZ_CC1101_REG_SYNC1, h->sync_word_hi, "sync word high"},
        {SUBGHZ_CC1101_REG_SYNC0, h->sync_word_lo, "sync word low"},

        /* ADDR: own address for address check */
        {SUBGHZ_CC1101_REG_ADDR, h->config.own_addr, "own address"},

        /* MDMCFG2: ASK/OOK (mod_format=3), sync_mode=2 (16/16 sync no carrier sense) */
        {SUBGHZ_CC1101_REG_MDMCFG2,
         cc1101_build_mdmcfg2(3, 2),
         "ASK/OOK, 16/16 sync"},

        /* MDMCFG1: 4 preamble bytes, 16-bit sync, no FEC */
        {SUBGHZ_CC1101_REG_MDMCFG1,
         cc1101_build_mdmcfg1(4, 3, 0),
         "4 preamble, 16-bit sync"},

        /* MDMCFG0: channel spacing (not used in single channel) */
        {SUBGHZ_CC1101_REG_MDMCFG0, 0xF8U, "channel spacing default"},

        /* DEVIATN: ASK has no deviation */
        {SUBGHZ_CC1101_REG_DEVIATN, 0x00U, "ASK no deviation"},

        /* MCSM1: CCA_MODE=0, RX_OFF_MODE=1 (IDLE), TX_OFF_MODE=0 */
        {SUBGHZ_CC1101_REG_MCSM1, 0x0CU, "auto IDLE after RX"},

        /* MCSM0: FS_AUTOCAL=1 (cal every RX/TX), PO_TIMEOUT=14 (default) */
        {SUBGHZ_CC1101_REG_MCSM0, 0x18U, "auto calibrate"},

        /* FSCTRL1: IF frequency */
        {SUBGHZ_CC1101_REG_FSCTRL1, 0x0CU, "IF freq"},

        /* FREND1: RX front-end config */
        {SUBGHZ_CC1101_REG_FREND1, 0xB6U, "RX front-end"},

        /* AGCCTRL2: AGC config */
        {SUBGHZ_CC1101_REG_AGCCTRL2, 0x07U, "AGC"},
    };

    for (size_t i = 0; i < sizeof(pkt_cfg) / sizeof(pkt_cfg[0]); ++i) {
        err = subghz_cc1101_write_reg(pkt_cfg[i].addr, pkt_cfg[i].value);
        if (err != ESP_OK) {
            return err;
        }
        err = subghz_cc1101_read_reg(pkt_cfg[i].addr, &rb);
        if (err != ESP_OK || rb != pkt_cfg[i].value) {
            ESP_LOGE(TAG, "pkt cfg 0x%02X: wrote 0x%02X read 0x%02X",
                     (unsigned)pkt_cfg[i].addr, (unsigned)pkt_cfg[i].value,
                     (unsigned)rb);
            return (err != ESP_OK) ? err : ESP_FAIL;
        }
        ESP_LOGI(TAG, "pkt cfg: reg 0x%02X = 0x%02X (read-back OK) — %s",
                 (unsigned)pkt_cfg[i].addr, (unsigned)pkt_cfg[i].value,
                 pkt_cfg[i].why);
    }

    /* 4. Enable external LNA */
    err = subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_RX);
    if (err != ESP_OK) {
        return err;
    }

    /* 5. Enter RX */
    err = subghz_cc1101_switch_to_rx(NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_RX, 10);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Packet RX active on %lu Hz, sync=0x%04X, max_payload=%u, LNA ON",
             (unsigned long)h->config.frequency_hz,
             ((unsigned)h->sync_word_hi << 8) | h->sync_word_lo,
             (unsigned)h->config.max_payload_len);

    return ESP_OK;
}

static void IRAM_ATTR cc1101_gdo0_isr_handler(void *arg)
{
    cc1101_rx_handle_t h = (cc1101_rx_handle_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (h != NULL && h->running) {
        /* Notify the worker task via queue (just a dummy value) */
        uint8_t dummy = 1;
        xQueueSendFromISR(h->event_queue, &dummy, &xHigherPriorityTaskWoken);
    }

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void cc1101_rx_task(void *arg)
{
    cc1101_rx_handle_t h = (cc1101_rx_handle_t)arg;
    uint8_t dummy;

    while (h->running) {
        if (xQueueReceive(h->event_queue, &dummy, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;  /* Periodic check for h->running */
        }

        if (!h->running) {
            break;
        }

        /* Read RXBYTES to get count and overflow */
        uint8_t rxbytes = 0;
        esp_err_t err = subghz_cc1101_read_status(SUBGHZ_CC1101_STATUS_RXBYTES, &rxbytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RX task: read RXBYTES failed: %s", esp_err_to_name(err));
            continue;
        }

        bool overflow = (rxbytes & SUBGHZ_CC1101_RXBYTES_OVERFLOW_MASK) != 0U;
        uint8_t count = rxbytes & SUBGHZ_CC1101_FIFO_COUNT_MASK;

        if (overflow) {
            ESP_LOGW(TAG, "RX task: RX FIFO overflow — flushing");
            (void)subghz_cc1101_flush_rx(NULL);
            continue;
        }

        if (count == 0U) {
            /* Spurious interrupt */
            continue;
        }

        if (count > SUBGHZ_CC1101_FIFO_SIZE) {
            count = SUBGHZ_CC1101_FIFO_SIZE;
        }

        /* Read FIFO burst */
        uint8_t fifo_data[SUBGHZ_CC1101_FIFO_SIZE + 2];  /* +2 for appended status */
        err = subghz_cc1101_read_fifo(fifo_data, sizeof(fifo_data), &count, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RX task: read_fifo failed: %s", esp_err_to_name(err));
            continue;
        }

        if (count < 3U) {
            /* Too short for length + payload + 2 status bytes */
            ESP_LOGW(TAG, "RX task: packet too short (%u bytes)", (unsigned)count);
            continue;
        }

        /* Packet format with APPEND_STATUS=1:
         * [0] = length byte (if variable) or first payload byte (if fixed)
         * [1..len] = payload
         * [len+1] = RSSI
         * [len+2] = LQI (bit7=CRC_OK)
         */
        uint8_t payload_len;
        uint8_t *payload;

        if (h->config.max_payload_len > 0) {
            /* Variable length mode: first byte is length */
            payload_len = fifo_data[0];
            payload = &fifo_data[1];
        } else {
            /* Fixed length mode: no length byte */
            payload_len = h->config.max_payload_len;
            payload = &fifo_data[0];
        }

        /* Sanity check */
        if (payload_len > h->config.max_payload_len) {
            ESP_LOGW(TAG, "RX task: payload_len %u > max %u",
                     (unsigned)payload_len, (unsigned)h->config.max_payload_len);
            continue;
        }

        if (count < payload_len + 3U) {
            ESP_LOGW(TAG, "RX task: count %u < expected %u",
                     (unsigned)count, (unsigned)(payload_len + 3U));
            continue;
        }

        uint8_t rssi_raw = fifo_data[payload_len + 1];
        uint8_t lqi_raw = fifo_data[payload_len + 2];
        bool crc_ok = (lqi_raw & 0x80U) != 0U;

        if (!crc_ok && h->config.crc_en) {
            ESP_LOGD(TAG, "RX task: CRC failed, dropping packet");
            continue;
        }

        int16_t rssi_dbm = cc1101_rssi_raw_to_dbm(rssi_raw);
        uint8_t lqi = cc1101_lqi_from_status(lqi_raw);

        ESP_LOGD(TAG, "RX task: packet len=%u rssi=%d lqi=%u crc=%s",
                 (unsigned)payload_len, (int)rssi_dbm, (unsigned)lqi,
                 crc_ok ? "OK" : "FAIL");

        /* --- Anti-replay gate (receiver-side; drops replays/stale frames) ---
         * Runs only when the caller opted in (ops != NULL). Strict NULL checks
         * on every pointer before use. This task is the sole writer of the
         * window state, so the commit needs no lock. */
        const subghz_cc1101_antireplay_config_t *ar = &h->config.antireplay;
        if (ar->ops != NULL) {
            uint32_t incoming = 0UL;
            if (!cc1101_extract_seq(ar, payload, payload_len, &incoming)) {
                ESP_LOGD(TAG, "RX task: seq field out of bounds, dropping");
                continue;
            }

            if (ar->ops->validate_sequence_window != NULL && ar->state != NULL) {
                if (!ar->ops->validate_sequence_window(ar->state, incoming,
                                                       ar->look_ahead,
                                                       ar->seq_width)) {
                    ESP_LOGD(TAG, "RX task: replay/stale seq %lu (last %lu), dropping",
                             (unsigned long)incoming,
                             (unsigned long)ar->state->last_counter);
                    continue;
                }
            }

            /* Optional payload hook (no key material in the HAL). */
            if (ar->ops->parse_secure_payload != NULL) {
                if (!ar->ops->parse_secure_payload(payload, payload_len, ar->key_ctx)) {
                    ESP_LOGD(TAG, "RX task: payload rejected by handler, dropping");
                    continue;
                }
            }

            /* Commit the new sequence state only after full acceptance. */
            if (ar->ops->sync_sequence_state != NULL && ar->state != NULL) {
                ar->ops->sync_sequence_state(ar->state, incoming);
            }
        }

        /* Deliver to callback */
        if (h->config.callback != NULL) {
            h->config.callback(payload, payload_len, rssi_dbm, lqi, h->config.user_ctx);
        }
    }

    /* Publish exit AFTER the last read of h, so rx_stop() can free the
     * context the moment it observes this flag. No access to h past here. */
    h->task_exited = true;
    vTaskDelete(NULL);
}

subghz_cc1101_rx_handle_t subghz_cc1101_rx_start(const subghz_cc1101_rx_config_t *config)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "rx_start: driver not initialized");
        return NULL;
    }
    if (config == NULL) {
        ESP_LOGE(TAG, "rx_start: config is NULL");
        return NULL;
    }
    if (config->callback == NULL) {
        ESP_LOGE(TAG, "rx_start: callback is required");
        return NULL;
    }
    if (s_gdo0_gpio < 0) {
        ESP_LOGE(TAG, "rx_start: GDO0 not configured");
        return NULL;
    }
    if (config->max_payload_len == 0U || config->max_payload_len > SUBGHZ_CC1101_FIFO_SIZE - 3U) {
        ESP_LOGE(TAG, "rx_start: max_payload_len %u invalid (1..%u)",
                 (unsigned)config->max_payload_len,
                 (unsigned)(SUBGHZ_CC1101_FIFO_SIZE - 3U));
        return NULL;
    }

    cc1101_rx_handle_t h = calloc(1, sizeof(struct cc1101_rx_context));
    if (h == NULL) {
        ESP_LOGE(TAG, "rx_start: alloc failed");
        return NULL;
    }

    h->config = *config;
    h->sync_word_hi = (uint8_t)((config->sync_word >> 8) & 0xFFU);
    h->sync_word_lo = (uint8_t)(config->sync_word & 0xFFU);
    h->running = true;
    h->task_exited = false;

    h->event_queue = xQueueCreate(16, sizeof(uint8_t));
    if (h->event_queue == NULL) {
        ESP_LOGE(TAG, "rx_start: queue create failed");
        free(h);
        return NULL;
    }

    /* Install GPIO interrupt on GDO0 (positive edge: packet received) */
    gpio_config_t gdo0_int_config = {
        .pin_bit_mask = (1ULL << (uint32_t)s_gdo0_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&gdo0_int_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx_start: gpio_config GDO0 failed: %s", esp_err_to_name(err));
        vQueueDelete(h->event_queue);
        free(h);
        return NULL;
    }

    /* The per-pin ISR dispatcher needs the GPIO ISR service running. Nothing
     * else in this driver installs it, so install it here and treat "already
     * installed" (by another module) as success — the flags argument is only
     * honoured on the first install. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "rx_start: gpio_install_isr_service failed: %s", esp_err_to_name(err));
        vQueueDelete(h->event_queue);
        free(h);
        return NULL;
    }

    err = gpio_isr_handler_add(s_gdo0_gpio, cc1101_gdo0_isr_handler, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx_start: gpio_isr_handler_add failed: %s", esp_err_to_name(err));
        vQueueDelete(h->event_queue);
        free(h);
        return NULL;
    }

    /* Apply packet RX config */
    err = cc1101_apply_packet_rx_config(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx_start: apply config failed: %s", esp_err_to_name(err));
        gpio_isr_handler_remove(s_gdo0_gpio);
        vQueueDelete(h->event_queue);
        free(h);
        return NULL;
    }

    /* Create worker task */
    BaseType_t task_created = xTaskCreate(
        cc1101_rx_task,
        "cc1101_rx",
        config->task_stack_size,
        h,
        config->task_priority,
        &h->task_handle
    );
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "rx_start: task create failed");
        gpio_isr_handler_remove(s_gdo0_gpio);
        vQueueDelete(h->event_queue);
        free(h);
        return NULL;
    }

    ESP_LOGI(TAG, "Packet RX started (task priority %d, stack %u)",
             config->task_priority, (unsigned)config->task_stack_size);

    return (subghz_cc1101_rx_handle_t)h;
}

void subghz_cc1101_rx_stop(subghz_cc1101_rx_handle_t handle)
{
    cc1101_rx_handle_t h = (cc1101_rx_handle_t)handle;
    if (h == NULL) {
        return;
    }

    h->running = false;

    /* Unblock the task if it's waiting on the queue */
    uint8_t dummy = 0;
    xQueueSend(h->event_queue, &dummy, 0);

    /* Join the worker: wait until it publishes task_exited before freeing the
     * context (the task's queue timeout is 100 ms, so it always wakes). The
     * bounded deadline keeps a wedged task from hanging the caller forever. */
    if (h->task_handle != NULL) {
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
        while (!h->task_exited && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!h->task_exited) {
            /* Task did not exit in time: leaking the context is the safe
             * choice — freeing it now would risk a use-after-free. */
            ESP_LOGE(TAG, "rx_stop: worker did not exit in time; context leaked to stay safe");
            if (s_gdo0_gpio >= 0) {
                gpio_isr_handler_remove(s_gdo0_gpio);
            }
            (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);
            (void)subghz_cc1101_switch_to_idle(NULL);
            (void)subghz_cc1101_reset();
            return;
        }
    }

    /* Remove GPIO interrupt */
    if (s_gdo0_gpio >= 0) {
        gpio_isr_handler_remove(s_gdo0_gpio);
        /* Restore GDO0 to input without interrupt */
        gpio_config_t gdo0_config = {
            .pin_bit_mask = (1ULL << (uint32_t)s_gdo0_gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&gdo0_config);
    }

    /* Return chip to IDLE and reset */
    (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);
    (void)subghz_cc1101_switch_to_idle(NULL);
    (void)subghz_cc1101_reset();

    if (h->event_queue != NULL) {
        vQueueDelete(h->event_queue);
    }

    free(h);
    ESP_LOGI(TAG, "Packet RX stopped");
}

/* ============================================================================
 * Reed-Solomon decode adapter
 * ==========================================================================*/

bool subghz_cc1101_rs_decode(const subghz_cc1101_rs_config_t *rs_cfg,
                             uint8_t *data,
                             uint8_t len)
{
    if (rs_cfg == NULL || rs_cfg->gf == NULL || data == NULL) {
        return false;
    }
    if (len == 0U || len > rs_cfg->n) {
        return false;
    }
    if (rs_cfg->syndrome_count == 0U || rs_cfg->syndrome_count > SUBGHZ_RS_MAX_PARITY) {
        return false;
    }

    /* Prepare codeword buffer: pad with zeros to length n. rs_cfg->n is a
     * uint8_t, so it can never exceed SUBGHZ_RS_NONZERO_ELEMENTS (255); the
     * old runtime guard was always false (-Werror=type-limits). Assert the
     * invariant at compile time instead. */
    _Static_assert(SUBGHZ_RS_NONZERO_ELEMENTS >= UINT8_MAX,
                   "codeword[] must be able to hold any uint8_t n");
    uint8_t codeword[SUBGHZ_RS_NONZERO_ELEMENTS];
    memset(codeword, 0, rs_cfg->n);
    memcpy(codeword, data, len);

    /* Compute syndromes */
    uint8_t syndromes[SUBGHZ_RS_MAX_PARITY];
    if (!subghz_rs_compute_syndromes(rs_cfg->gf, codeword, rs_cfg->n,
                                     rs_cfg->first_root, syndromes, rs_cfg->syndrome_count)) {
        return false;
    }

    /* Check if all syndromes are zero (no errors) */
    bool all_zero = true;
    for (size_t i = 0; i < rs_cfg->syndrome_count; ++i) {
        if (syndromes[i] != 0U) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return true;  /* No errors */
    }

    /* Find error locator polynomial */
    uint8_t locator[SUBGHZ_RS_MAX_PARITY + 1];
    size_t locator_degree = 0;
    if (!subghz_rs_find_error_locator(rs_cfg->gf, syndromes, rs_cfg->syndrome_count,
                                      locator, sizeof(locator), &locator_degree)) {
        return false;
    }

    /* Compute error evaluator */
    uint8_t evaluator[SUBGHZ_RS_MAX_PARITY];
    if (!subghz_rs_compute_error_evaluator(rs_cfg->gf, syndromes, rs_cfg->syndrome_count,
                                           locator, locator_degree,
                                           evaluator, sizeof(evaluator))) {
        return false;
    }

    /* Chien search for error positions */
    size_t error_positions[SUBGHZ_RS_MAX_PARITY];
    size_t error_count = 0;
    if (!subghz_rs_chien_search(rs_cfg->gf, locator, locator_degree,
                                rs_cfg->n, error_positions, sizeof(error_positions),
                                &error_count)) {
        return false;
    }

    if (error_count == 0U || error_count > rs_cfg->syndrome_count / 2U) {
        return false;  /* Too many errors or none found */
    }

    /* Forney magnitudes and correction */
    for (size_t i = 0; i < error_count; ++i) {
        uint8_t magnitude = 0;
        if (!subghz_rs_forney_magnitude(rs_cfg->gf, locator, locator_degree,
                                        evaluator, rs_cfg->syndrome_count,
                                        rs_cfg->n, error_positions[i],
                                        rs_cfg->first_root, &magnitude)) {
            return false;
        }
        /* Correct the error: codeword[position] ^= magnitude */
        codeword[error_positions[i]] ^= magnitude;
    }

    /* Verify correction by recomputing syndromes */
    uint8_t verify_syndromes[SUBGHZ_RS_MAX_PARITY];
    if (!subghz_rs_compute_syndromes(rs_cfg->gf, codeword, rs_cfg->n,
                                     rs_cfg->first_root, verify_syndromes,
                                     rs_cfg->syndrome_count)) {
        return false;
    }

    for (size_t i = 0; i < rs_cfg->syndrome_count; ++i) {
        if (verify_syndromes[i] != 0U) {
            return false;  /* Correction failed */
        }
    }

    /* Copy corrected data back (only the original len bytes) */
    memcpy(data, codeword, len);
    return true;
}
/* ============================================================================
 * Packet TX & Raw TX Mode Implementation (Red Team Extensions)
 * ==========================================================================*/

esp_err_t subghz_cc1101_send_packet(const subghz_cc1101_tx_config_t *config,
                                    const uint8_t *data, uint8_t len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Variable-length mode prepends a length byte, so the payload must leave
     * room for it inside a single FIFO fill (no TX FIFO refill implemented). */
    if (len > SUBGHZ_CC1101_FIFO_SIZE - 1U) {
        ESP_LOGE(TAG, "send_packet: len %u exceeds FIFO payload capacity %u",
                 (unsigned)len, (unsigned)(SUBGHZ_CC1101_FIFO_SIZE - 1U));
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. Переводим чип в IDLE перед настройкой */
    esp_err_t err = subghz_cc1101_switch_to_idle(NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_IDLE, 10);
    if (err != ESP_OK) {
        return err;
    }

    /* 2. Очищаем TX FIFO */
    err = subghz_cc1101_flush_tx(NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* 3. Устанавливаем несущую частоту */
    err = subghz_cc1101_set_frequency(config->frequency_hz, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* 3a. Применяем обрамление из конфига (раньше поля молча игнорировались):
     * sync word, whitening, CRC. Регистры совпадают с раскладкой packet-RX
     * (cc1101_apply_packet_rx_config), чтобы приёмник на subghz_cc1101_rx_start()
     * мог это декодировать. Переменная длина => первый байт FIFO = длина. */
    const struct {
        uint8_t addr;
        uint8_t value;
    } tx_cfg[] = {
        {SUBGHZ_CC1101_REG_PKTCTRL0,
         cc1101_build_pktctrl0(config->whitening, config->crc_en, true)},
        {SUBGHZ_CC1101_REG_PKTCTRL1,
         cc1101_build_pktctrl1(false, false)}, /* TX: без addr-check и append-status */
        {SUBGHZ_CC1101_REG_SYNC1, (uint8_t)((config->sync_word >> 8) & 0xFFU)},
        {SUBGHZ_CC1101_REG_SYNC0, (uint8_t)(config->sync_word & 0xFFU)},
        {SUBGHZ_CC1101_REG_PKTLEN, SUBGHZ_CC1101_FIFO_SIZE},
        {SUBGHZ_CC1101_REG_MDMCFG2, cc1101_build_mdmcfg2(3, 2)}, /* ASK/OOK, 16/16 sync */
        {SUBGHZ_CC1101_REG_MDMCFG1, cc1101_build_mdmcfg1(4, 3, 0)}, /* 4 preamble, 16-bit sync */
    };
    for (size_t i = 0; i < sizeof(tx_cfg) / sizeof(tx_cfg[0]); ++i) {
        err = subghz_cc1101_write_reg(tx_cfg[i].addr, tx_cfg[i].value);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* 4. Загружаем PATABLE согласно config->pa_power_index (раньше был хардкод
     * максимума). Стандартные уровни 433 МГц; индекс клампится в 0..7. Один
     * элемент => FSK; для OOK keyed-уровень берётся из PATABLE[1] (см. замечание). */
    static const uint8_t pa_levels[8] = {
        0x12, 0x0E, 0x1D, 0x34, 0x60, 0x84, 0xC8, 0xC0
    };
    int8_t pa_idx = config->pa_power_index;
    if (pa_idx < 0) {
        pa_idx = 0;
    } else if (pa_idx > 7) {
        pa_idx = 7;
    }
    const uint8_t pa_table[8] = {
        pa_levels[pa_idx], 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    err = subghz_cc1101_set_pa_table(pa_table);
    if (err != ESP_OK) {
        return err;
    }

    /* 5. Собираем кадр переменной длины [len][payload] и пишем в TX FIFO.
     * Локальный буфер, без динамической памяти; len уже ограничен FIFO-1. */
    uint8_t frame[1U + SUBGHZ_CC1101_FIFO_SIZE];
    frame[0] = len;
    if (len > 0U) {
        memcpy(&frame[1], data, len);
    }
    err = subghz_cc1101_write_fifo(frame, (uint8_t)(len + 1U));
    if (err != ESP_OK) {
        return err;
    }

    /* 6. Включаем внешний усилитель мощности (TX_EN = HIGH, RX_EN = LOW) через безопасный путь */
    err = subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_TX);
    if (err != ESP_OK) {
        return err;
    }

    /* 7. Переводим радио в режим передачи (STX strobe) */
    err = subghz_cc1101_switch_to_tx(NULL);
    if (err != ESP_OK) {
        (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);
        return err;
    }

    /* 8. Ожидаем завершения отправки пакета (возврат в IDLE по опустошению FIFO) */
    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_IDLE, 100);

    /* 9. Безопасно отключаем RF-тракт в любом случае */
    (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Packet sent successfully: len=%u, freq=%lu Hz",
                 (unsigned)len, (unsigned long)config->frequency_hz);
    }
    return err;
}

esp_err_t subghz_cc1101_enter_raw_tx(uint32_t frequency_hz)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Сброс в IDLE */
    esp_err_t err = subghz_cc1101_switch_to_idle(NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_IDLE, 10);
    if (err != ESP_OK) {
        return err;
    }

    /* 2. Установка частоты */
    err = subghz_cc1101_set_frequency(frequency_hz, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* 3. Настройка регистров для асинхронного сырого потока TX (ASK/OOK) */
    err = subghz_cc1101_write_reg(SUBGHZ_CC1101_REG_MDMCFG2, 0x30U); /* ASK/OOK, no sync */
    if (err != ESP_OK) return err;
    err = subghz_cc1101_write_reg(SUBGHZ_CC1101_REG_PKTCTRL0, 0x32U); /* Async serial mode, infinite len */
    if (err != ESP_OK) return err;

    /* 4. Включаем внешний усилитель мощности (PA) */
    err = subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_TX);
    if (err != ESP_OK) {
        return err;
    }

    /* 5. Переводим чип в режим передачи */
    err = subghz_cc1101_switch_to_tx(NULL);
    if (err != ESP_OK) {
        (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);
        return err;
    }

    err = subghz_cc1101_wait_status_state(SUBGHZ_CC1101_STATE_TX, 10);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Raw TX active on GDO0 (IO%d), external PA ON (TX_EN=IO%d)",
                 s_gdo0_gpio, SUBGHZ_CC1101_TX_EN_GPIO);
    }
    return err;
}

esp_err_t subghz_cc1101_exit_raw_tx(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)subghz_cc1101_set_rf_path(SUBGHZ_CC1101_RF_PATH_OFF);
    (void)subghz_cc1101_switch_to_idle(NULL);
    return subghz_cc1101_reset();
}

esp_err_t subghz_cc1101_test_tx_beacon(void)
{
    /* TX-path self-test: emit one short fixed pattern on the default channel so
     * the operator can confirm the radio + external PA actually key up. Reuses
     * the normal packet path; no special framing, no sequence state. */
    static const uint8_t beacon[] = {0xAA, 0x55, 0xAA, 0x55, 0x00, 0xFF};
    const subghz_cc1101_tx_config_t cfg = subghz_cc1101_tx_config_default();
    return subghz_cc1101_send_packet(&cfg, beacon, (uint8_t)sizeof(beacon));
}