#pragma once

#include "wifi_port_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure bounded classification: no sockets, allocation, logging or mutation. */
wifi_host_classification_t wifi_host_classify(
    const wifi_port_scan_result_t *result
);

#ifdef __cplusplus
}
#endif
