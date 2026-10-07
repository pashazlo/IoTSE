#pragma once
#include <stdint.h>
#include "esp_err.h"
/* PRIVATE: called exclusively by wifi_worker_task, never UI or callback. */
esp_err_t wifi_channel_hopper_worker_start(uint32_t interval_ms);
void wifi_channel_hopper_worker_stop(void);
void wifi_channel_engine_worker_begin_fixed(uint8_t channel);
void wifi_channel_engine_worker_set_fixed(uint8_t channel);
void wifi_channel_hopper_worker_tick(void);
