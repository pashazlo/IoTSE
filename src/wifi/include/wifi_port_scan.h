#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi.h"
#include "wifi_host_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PORT_SCAN_MAX_OPEN_PORTS 64U
#define WIFI_PORT_SCAN_MAX_PROBES 64U
#define WIFI_PORT_SCAN_DEFAULT_TIMEOUT_MS 250U
#define WIFI_PORT_SCAN_MIN_TIMEOUT_MS 50U
#define WIFI_PORT_SCAN_MAX_TIMEOUT_MS 5000U
#define WIFI_PORT_SCAN_MAX_BANNERS 4U
#define WIFI_PORT_SCAN_BANNER_CAPACITY 96U
#define WIFI_PORT_SCAN_CONNECT_CONCURRENCY 3U

typedef enum {
    WIFI_PORT_SCAN_STATE_UNINITIALIZED = 0,
    WIFI_PORT_SCAN_STATE_IDLE,
    WIFI_PORT_SCAN_STATE_RUNNING,
} wifi_port_scan_state_t;

typedef enum {
    WIFI_PORT_SCAN_PROFILE_QUICK = 0,
    WIFI_PORT_SCAN_PROFILE_COMMON,
    WIFI_PORT_SCAN_PROFILE_IOT_LAN,
    WIFI_PORT_SCAN_PROFILE_CUSTOM,
} wifi_port_scan_profile_t;

typedef struct {
    /* Zero selects the current STA gateway. Values must be in the STA subnet. */
    uint32_t target_ipv4;
    wifi_port_scan_profile_t profile;
    uint16_t first_port;
    uint16_t last_port;
    uint16_t timeout_ms;
    /* Shuffle the bounded port array once before the immutable job is queued. */
    bool randomize_ports;
    /* Run one bounded local ICMP discovery pass before TCP probing. */
    bool discover_hosts;
} wifi_scan_job_t;

typedef enum {
    WIFI_PORT_SCAN_FINISH_COMPLETE = 0,
    WIFI_PORT_SCAN_FINISH_CANCELLED,
    WIFI_PORT_SCAN_FINISH_NETWORK_CHANGED,
    WIFI_PORT_SCAN_FINISH_INVALID_TARGET,
    WIFI_PORT_SCAN_FINISH_RESOURCE_ERROR,
} wifi_port_scan_finish_t;

typedef enum {
    WIFI_PROBE_OPEN = 0,
    WIFI_PROBE_CLOSED,
    WIFI_PROBE_FILTERED,
    WIFI_PROBE_ERROR,
} wifi_probe_state_t;

/*
 * Lightweight service hint derived only from the destination port.
 *
 * This is not banner detection and must never be presented as proof that a
 * particular daemon is running. A service can use a non-standard port, while
 * a standard port can host an unrelated protocol. Later banner probing may
 * raise confidence, but it must preserve this distinction in the result.
 */
typedef enum {
    WIFI_SERVICE_UNKNOWN = 0,
    WIFI_SERVICE_FTP,
    WIFI_SERVICE_SSH,
    WIFI_SERVICE_TELNET,
    WIFI_SERVICE_SMTP,
    WIFI_SERVICE_DNS,
    WIFI_SERVICE_HTTP,
    WIFI_SERVICE_POP3,
    WIFI_SERVICE_RPC,
    WIFI_SERVICE_NETBIOS,
    WIFI_SERVICE_IMAP,
    WIFI_SERVICE_LDAP,
    WIFI_SERVICE_HTTPS,
    WIFI_SERVICE_SMB,
    WIFI_SERVICE_PRINTER,
    WIFI_SERVICE_AFP,
    WIFI_SERVICE_RTSP,
    WIFI_SERVICE_RSYNC,
    WIFI_SERVICE_MQTT,
    WIFI_SERVICE_NFS,
    WIFI_SERVICE_MYSQL,
    WIFI_SERVICE_RDP,
    WIFI_SERVICE_POSTGRESQL,
    WIFI_SERVICE_VNC,
    WIFI_SERVICE_REDIS,
    WIFI_SERVICE_HTTP_ALT,
    WIFI_SERVICE_HTTPS_ALT,
    WIFI_SERVICE_MONGODB,
    WIFI_SERVICE_MODBUS,
} wifi_service_hint_t;

typedef struct {
    uint16_t port;
    uint16_t latency_ms;
    int32_t socket_error;
    wifi_probe_state_t state;
    wifi_service_hint_t service_hint;
} wifi_probe_result_t;

typedef enum {
    WIFI_BANNER_NONE = 0,
    /* Server spoke first, for example SSH, FTP or Telnet greeting. */
    WIFI_BANNER_PASSIVE,
    /* Response to a small protocol-safe request, for example HTTP HEAD. */
    WIFI_BANNER_ACTIVE,
} wifi_banner_source_t;

/*
 * Bounded service evidence for an open port.
 *
 * banner[] always contains printable ASCII and is always NUL-terminated.
 * banner_length excludes that terminator. Binary bytes and control sequences
 * must be sanitized by the future capture function before publication.
 *
 * Only WIFI_PORT_SCAN_MAX_BANNERS observations are retained per scan. This
 * keeps the result small and predictable even when many ports are open.
 */
typedef struct {
    uint16_t port;
    wifi_service_hint_t service_hint;
    wifi_banner_source_t source;
    uint8_t banner_length;
    bool truncated;
    char banner[WIFI_PORT_SCAN_BANNER_CAPACITY];
} wifi_banner_observation_t;

/*
 * A bounded field hint derived from observed open ports and captured banners.
 * It is not an OS fingerprint. confidence is 0..100 and evidence is a bitmask
 * so reports and the future Info popup can explain every classification.
 */
typedef enum {
    WIFI_HOST_HINT_UNKNOWN = 0,
    WIFI_HOST_HINT_WEB_DEVICE,
    WIFI_HOST_HINT_UNIX_LIKE,
    WIFI_HOST_HINT_WINDOWS_LIKE,
    WIFI_HOST_HINT_IP_CAMERA,
    WIFI_HOST_HINT_NETWORK_DEVICE,
    WIFI_HOST_HINT_PRINTER,
    WIFI_HOST_HINT_DATABASE,
    WIFI_HOST_HINT_IOT_CONTROLLER,
} wifi_host_hint_t;

typedef enum {
    WIFI_HOST_EVIDENCE_NONE          = 0,
    WIFI_HOST_EVIDENCE_HTTP          = 1U << 0,
    WIFI_HOST_EVIDENCE_SSH           = 1U << 1,
    WIFI_HOST_EVIDENCE_TELNET        = 1U << 2,
    WIFI_HOST_EVIDENCE_DNS           = 1U << 3,
    WIFI_HOST_EVIDENCE_SMB           = 1U << 4,
    WIFI_HOST_EVIDENCE_RDP           = 1U << 5,
    WIFI_HOST_EVIDENCE_RTSP          = 1U << 6,
    WIFI_HOST_EVIDENCE_PRINT_SERVICE = 1U << 7,
    WIFI_HOST_EVIDENCE_DATABASE      = 1U << 8,
    WIFI_HOST_EVIDENCE_MQTT          = 1U << 9,
    WIFI_HOST_EVIDENCE_MODBUS        = 1U << 10,
    WIFI_HOST_EVIDENCE_UNIX_SERVICE  = 1U << 11,
    WIFI_HOST_EVIDENCE_BANNER        = 1U << 12,
    WIFI_HOST_EVIDENCE_GATEWAY       = 1U << 13,
} wifi_host_evidence_t;

typedef struct {
    wifi_host_hint_t hint;
    uint16_t evidence;
    uint8_t confidence;
} wifi_host_classification_t;

typedef struct {
    esp_err_t error;
    wifi_port_scan_finish_t finish;
    wifi_port_scan_profile_t profile;
    uint32_t job_id;
    char ssid[WIFI_SSID_MAX_LEN + 1];
    uint32_t gateway;
    uint32_t target_ipv4;
    uint16_t timeout_ms;
    bool ports_randomized;
    uint8_t connect_concurrency;
    uint16_t total_count;
    uint16_t scanned_count;
    uint16_t closed_count;
    uint16_t filtered_count;
    uint16_t probe_error_count;
    uint8_t open_count;
    uint16_t open_ports[WIFI_PORT_SCAN_MAX_OPEN_PORTS];
    uint8_t probe_count;
    wifi_probe_result_t probes[WIFI_PORT_SCAN_MAX_PROBES];
    uint8_t banner_count;
    /* Eligible open ports skipped after the fixed banner array became full. */
    uint8_t banner_dropped;
    wifi_banner_observation_t banners[WIFI_PORT_SCAN_MAX_BANNERS];
    wifi_host_classification_t classification;
    wifi_host_discovery_result_t discovery;
} wifi_port_scan_result_t;

typedef struct {
    wifi_port_scan_state_t state;
    wifi_port_scan_profile_t profile;
    uint16_t total_count;
    uint16_t scanned_count;
    uint16_t current_port;
    uint8_t open_count;
} wifi_port_scan_progress_t;

esp_err_t wifi_port_scan_init(void);

/*
 * Starts a bounded one-shot scan of the connected network's gateway.
 * expected_ssid prevents a stale UI selection from targeting another network.
 */
esp_err_t wifi_port_scan_start(const char *expected_ssid);
esp_err_t wifi_port_scan_start_profile(
    const char *expected_ssid,
    wifi_port_scan_profile_t profile
);
/*
 * Enqueues an immutable bounded job. Custom ranges are inclusive and limited
 * to WIFI_PORT_SCAN_MAX_PROBES ports. target_ipv4=0 selects the STA gateway.
 */
esp_err_t wifi_port_scan_start_job(
    const char *expected_ssid,
    const wifi_scan_job_t *job,
    uint32_t *job_id
);
esp_err_t wifi_port_scan_cancel(void);
bool wifi_port_scan_receive_result(wifi_port_scan_result_t *result);
bool wifi_port_scan_is_running(void);
bool wifi_port_scan_get_progress(wifi_port_scan_progress_t *progress);

/* Stable short label for logs, reports and the future MAP details screen. */
const char *wifi_port_scan_service_name(wifi_service_hint_t service);
const char *wifi_port_scan_probe_state_name(wifi_probe_state_t state);

/* Stable short labels for logs, reports and the future MAP Info popup. */
const char *wifi_host_hint_name(wifi_host_hint_t hint);

#ifdef __cplusplus
}
#endif
