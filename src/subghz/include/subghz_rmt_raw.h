#pragma once

#include <stdbool.h>
#include <stddef.h>            /* size_t                         */
#include <stdint.h>

#include "esp_err.h"           /* esp_err_t                      */
#include "freertos/FreeRTOS.h" /* TickType_t (базовые типы ОС)   */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t subghz_rmt_tx_init(int tx_gpio);
esp_err_t subghz_rmt_tx_send(const void *symbols, size_t num_symbols, TickType_t timeout_ticks);
void subghz_rmt_tx_deinit(void);
/*
 * subghz_rmt_raw — first-stage raw RX transport for the Sub-GHz pipeline
 * (docs/subghz/README.md "subghz_rmt_transport").
 *
 * Captures the demodulated async stream that the CC1101 outputs on GDO0
 * (IO10, SUBGHZ_CC1101_GDO0_GPIO) with the ESP32-S3 RMT RX peripheral and
 * logs bursts: this is the hardware proof that later becomes .sub files.
 *
 * Pipeline ownership on this stage:
 *   - RMT ISR callback only moves a buffer index into a queue. No malloc,
 *     no logging, no SPI, no UI — per the project rules.
 *   - A low-priority logger task copies the symbols, restarts rmt_receive()
 *     and prints. A buffer is never in-flight and queued at the same time,
 *     so there is no ping-pong race at this stage.
 *   - Bursts are separated by >10 ms of line silence (RMT idle threshold),
 *     matching typical 433 MHz remote retransmission gaps.
 *
 * The chip side (frequency, ASK/OOK, GDO0 = serial data out, SRX) is owned
 * by subghz_hal_cc1101; this module never touches the SPI bus.
 */

/*
 * Full one-call bring-up for the boot proof: enters raw async RX on the chip
 * at frequency_hz, creates the RMT channel on GDO0 and starts capture.
 * Fail-soft: any error is logged and returned; the firmware continues.
 */
esp_err_t subghz_rmt_raw_capture_start(uint32_t frequency_hz);

/*
 * Record mode: same capture pipeline, but every frame-sized burst is dumped
 * to the log as raw timing halves ("+660 -340 ..." = level + microseconds),
 * line by line — the manual "press the keyfob and watch raw data" tool.
 * This is the exact raw material that later becomes .sub files.
 *
 * UART is slower than a burst storm: if frames arrive faster than the log
 * prints, queue_dropped in the stats line grows — that counter is the honest
 * overflow indicator, bursts are never silently truncated.
 */
esp_err_t subghz_rmt_raw_record_start(uint32_t frequency_hz);

/* Stops capture, returns the chip to post-reset defaults, frees the RMT
 * channel, the queue and the logger task. Safe when never started. */
void subghz_rmt_raw_capture_stop(void);

bool subghz_rmt_raw_is_running(void);

#ifdef __cplusplus
}
#endif
