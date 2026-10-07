#include "wifi_port_scan.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wifi_banner.h"
#include "wifi_host_classifier.h"
#include "wifi_host_discovery.h"
#include "wifi_map_store.h"
#include "wifi_worker.h"

#define WIFI_PORT_SCAN_TASK_STACK       4096
#define WIFI_PORT_SCAN_TASK_PRIORITY    2
#define WIFI_PORT_SCAN_QUICK_TIMEOUT_MS  WIFI_PORT_SCAN_DEFAULT_TIMEOUT_MS
#define WIFI_PORT_SCAN_COMMON_TIMEOUT_MS WIFI_PORT_SCAN_DEFAULT_TIMEOUT_MS
#define WIFI_PORT_SCAN_IOT_TIMEOUT_MS    WIFI_PORT_SCAN_DEFAULT_TIMEOUT_MS
#define WIFI_PORT_SCAN_RESULT_BUDGET     4096U
#define WIFI_PORT_SCAN_BANNER_TIMEOUT_MS 150U
#define WIFI_PORT_SCAN_HTTP_TIMEOUT_MS   200U
#define WIFI_PORT_SCAN_BANNER_RAW_BYTES  128U
#define WIFI_PORT_SCAN_SELECT_SLICE_MS   25U

_Static_assert(sizeof(wifi_port_scan_result_t) <= WIFI_PORT_SCAN_RESULT_BUDGET,
               "Wi-Fi MAP result exceeded its fixed memory budget");

typedef struct {
    char expected_ssid[WIFI_SSID_MAX_LEN + 1];
    wifi_scan_job_t job;
    uint32_t job_id;
    uint16_t port_count;
    uint16_t ports[WIFI_PORT_SCAN_MAX_PROBES];
} wifi_port_scan_request_t;

typedef struct {
    uint32_t generation;
} wifi_port_scan_event_t;

typedef struct {
    int sock;
    int64_t started_us;
    int64_t deadline_us;
    bool active;
    wifi_probe_result_t probe;
} wifi_connect_slot_t;

static const char *TAG = "WIFI_PORT_SCAN";

static const uint16_t s_quick_ports[] = {
    21, 22, 23, 53, 80, 443, 445, 554, 1883, 3000, 8080, 8443,
};

static const uint16_t s_common_ports[] = {
    21, 22, 23, 25, 53, 80, 110, 111,
    135, 139, 143, 389, 443, 445, 465, 515,
    548, 554, 587, 631, 873, 993, 995, 1883,
    2049, 3000, 3306, 3389, 5900, 8080, 8443, 9100,
};

static const uint16_t s_iot_lan_ports[] = {
    21, 22, 23, 53, 80, 139, 443, 445,
    502, 554, 631, 1883, 2049, 3000, 3306, 3389,
    5432, 5900, 6379, 8000, 8080, 8443, 8888, 9100, 27017,
};

static QueueHandle_t s_result_queue;
static QueueHandle_t s_request_queue;
static TaskHandle_t s_worker_task;
/* Mutable scratch owned by the single scanner worker. */
static wifi_port_scan_result_t *s_work_result;
static atomic_int s_state = WIFI_PORT_SCAN_STATE_UNINITIALIZED;
static atomic_bool s_cancel_requested;
static atomic_uint_fast16_t s_progress_total;
static atomic_uint_fast16_t s_progress_scanned;
static atomic_uint_fast16_t s_progress_current_port;
static atomic_uchar s_progress_open;
static atomic_int s_progress_profile = WIFI_PORT_SCAN_PROFILE_QUICK;
static atomic_uint_fast32_t s_next_job_id = 1U;

static void publish_progress(const wifi_port_scan_result_t *result,
                             uint16_t current_port);
static bool connection_still_matches(const char *expected_ssid,
                                     uint32_t gateway);

typedef struct {
    const char *ssid;
    uint32_t gateway;
} discovery_abort_context_t;

static bool discovery_should_abort(void *context)
{
    const discovery_abort_context_t *abort = context;
    return atomic_load_explicit(&s_cancel_requested, memory_order_acquire) ||
           abort == NULL ||
           !connection_still_matches(abort->ssid, abort->gateway);
}

static void log_memory_status(const char *phase)
{
    ESP_LOGI(TAG,
             "%s: internal_free=%u internal_min=%u largest_internal=%u "
             "largest_dma=%u psram_free=%u stack_hwm=%u",
             phase,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(s_worker_task));
}

static bool get_connection_snapshot_retry(
    wifi_connection_snapshot_t *connection
)
{
    if (connection == NULL) return false;
    for (unsigned attempt = 0; attempt < 3U; ++attempt) {
        if (wifi_worker_get_connection_snapshot(connection)) return true;
        vTaskDelay(1);
    }
    return false;
}

static bool connection_still_matches(
    const char *expected_ssid,
    uint32_t gateway
)
{
    wifi_connection_snapshot_t connection = {0};
    return get_connection_snapshot_retry(&connection) &&
           connection.info.connected &&
           connection.info.gateway == gateway &&
           strcmp(connection.info.ssid, expected_ssid) == 0;
}

static bool target_is_in_local_subnet(
    const wifi_connection_info_t *connection,
    uint32_t target
)
{
    if (connection == NULL || target == 0 || connection->netmask == 0) {
        return false;
    }
    uint32_t network = connection->ip & connection->netmask;
    uint32_t broadcast = network | ~connection->netmask;
    return (target & connection->netmask) == network &&
           target != network && target != broadcast &&
           target != connection->ip;
}

static wifi_probe_state_t classify_socket_error(int socket_error)
{
    if (socket_error == 0) return WIFI_PROBE_OPEN;
    if (socket_error == ECONNREFUSED) return WIFI_PROBE_CLOSED;
    if (socket_error == ETIMEDOUT || socket_error == EHOSTUNREACH ||
        socket_error == ENETUNREACH) {
        return WIFI_PROBE_FILTERED;
    }
    return WIFI_PROBE_ERROR;
}

static uint16_t elapsed_ms_since(int64_t started_us)
{
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (elapsed_us <= 0) return 0;
    uint64_t rounded_ms = ((uint64_t)elapsed_us + 999U) / 1000U;
    return rounded_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)rounded_ms;
}

static bool socket_error_is_resource_fatal(int socket_error)
{
    return socket_error == ENOMEM || socket_error == ENFILE ||
           socket_error == EMFILE;
}

static uint32_t random_bounded(uint32_t bound)
{
    if (bound <= 1U) return 0U;

    /* Rejection sampling avoids modulo bias without allocating state. */
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
    uint32_t value;
    do {
        value = esp_random();
    } while (value >= limit);
    return value % bound;
}

static void shuffle_ports(uint16_t *ports, uint16_t count)
{
    if (ports == NULL || count < 2U) return;
    for (uint16_t i = count - 1U; i > 0U; --i) {
        const uint16_t other = (uint16_t)random_bounded((uint32_t)i + 1U);
        const uint16_t saved = ports[i];
        ports[i] = ports[other];
        ports[other] = saved;
    }
}

static wifi_service_hint_t service_hint_for_port(uint16_t port)
{
    switch (port) {
    case 21: return WIFI_SERVICE_FTP;
    case 22: return WIFI_SERVICE_SSH;
    case 23: return WIFI_SERVICE_TELNET;
    case 25:
    case 465:
    case 587: return WIFI_SERVICE_SMTP;
    case 53: return WIFI_SERVICE_DNS;
    case 80: return WIFI_SERVICE_HTTP;
    case 110:
    case 995: return WIFI_SERVICE_POP3;
    case 111: return WIFI_SERVICE_RPC;
    case 135: return WIFI_SERVICE_RPC;
    case 139: return WIFI_SERVICE_NETBIOS;
    case 143:
    case 993: return WIFI_SERVICE_IMAP;
    case 389: return WIFI_SERVICE_LDAP;
    case 443: return WIFI_SERVICE_HTTPS;
    case 445: return WIFI_SERVICE_SMB;
    case 515:
    case 631:
    case 9100: return WIFI_SERVICE_PRINTER;
    case 502: return WIFI_SERVICE_MODBUS;
    case 548: return WIFI_SERVICE_AFP;
    case 554: return WIFI_SERVICE_RTSP;
    case 873: return WIFI_SERVICE_RSYNC;
    case 1883: return WIFI_SERVICE_MQTT;
    case 2049: return WIFI_SERVICE_NFS;
    case 3306: return WIFI_SERVICE_MYSQL;
    case 3389: return WIFI_SERVICE_RDP;
    case 5432: return WIFI_SERVICE_POSTGRESQL;
    case 5900: return WIFI_SERVICE_VNC;
    case 6379: return WIFI_SERVICE_REDIS;
    case 3000:
    case 8000:
    case 8080:
    case 8888: return WIFI_SERVICE_HTTP_ALT;
    case 8443: return WIFI_SERVICE_HTTPS_ALT;
    case 27017: return WIFI_SERVICE_MONGODB;
    default: return WIFI_SERVICE_UNKNOWN;
    }
}

static bool service_has_passive_banner(wifi_service_hint_t service)
{
    return service == WIFI_SERVICE_SSH || service == WIFI_SERVICE_FTP ||
           service == WIFI_SERVICE_TELNET;
}

static bool service_supports_http_head(wifi_service_hint_t service)
{
    return service == WIFI_SERVICE_HTTP ||
           service == WIFI_SERVICE_HTTP_ALT;
}

static bool service_supports_rtsp_options(wifi_service_hint_t service)
{
    return service == WIFI_SERVICE_RTSP;
}

static bool wait_for_socket(int sock, bool write_ready, uint32_t timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000U,
        .tv_usec = (timeout_ms % 1000U) * 1000U,
    };
    int selected = select(sock + 1,
                          write_ready ? NULL : &set,
                          write_ready ? &set : NULL,
                          NULL,
                          &timeout);
    return selected > 0 && FD_ISSET(sock, &set);
}

static bool send_bounded(int sock,
                         const char *data,
                         size_t data_length,
                         uint32_t timeout_ms)
{
    if (sock < 0 || data == NULL || data_length == 0U) return false;

    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)timeout_ms * 1000;
    size_t sent = 0U;
    while (sent < data_length) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) return false;
        uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
        if (!wait_for_socket(sock, true, remaining_ms)) return false;

        int written = send(sock, data + sent, data_length - sent, 0);
        if (written > 0) {
            sent += (size_t)written;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return false;
    }
    return true;
}

static bool capture_passive_banner(int sock,
                                   const wifi_probe_result_t *probe,
                                   wifi_banner_observation_t *observation)
{
    if (sock < 0 || probe == NULL || observation == NULL ||
        probe->state != WIFI_PROBE_OPEN ||
        !service_has_passive_banner(probe->service_hint)) {
        return false;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(sock, &read_set);
    struct timeval timeout = {
        .tv_sec = WIFI_PORT_SCAN_BANNER_TIMEOUT_MS / 1000U,
        .tv_usec = (WIFI_PORT_SCAN_BANNER_TIMEOUT_MS % 1000U) * 1000U,
    };
    int selected = select(sock + 1, &read_set, NULL, NULL, &timeout);
    if (selected <= 0 || !FD_ISSET(sock, &read_set)) return false;

    uint8_t raw[WIFI_PORT_SCAN_BANNER_RAW_BYTES];
    int received = recv(sock, raw, sizeof(raw), 0);
    if (received <= 0) return false;

    bool sanitized_truncated = false;
    size_t visible_length = wifi_banner_sanitize(
        raw,
        (size_t)received,
        observation->banner,
        sizeof(observation->banner),
        &sanitized_truncated
    );
    if (visible_length == 0U) return false;

    observation->port = probe->port;
    observation->service_hint = probe->service_hint;
    observation->source = WIFI_BANNER_PASSIVE;
    observation->banner_length = (uint8_t)visible_length;
    observation->truncated = sanitized_truncated ||
                             received == (int)sizeof(raw);
    return true;
}

static bool capture_http_banner(int sock,
                                uint32_t target_ipv4,
                                const wifi_probe_result_t *probe,
                                wifi_banner_observation_t *observation)
{
    if (sock < 0 || probe == NULL || observation == NULL ||
        probe->state != WIFI_PROBE_OPEN ||
        !service_supports_http_head(probe->service_hint)) {
        return false;
    }

    esp_ip4_addr_t target = {.addr = target_ipv4};
    char request[96];
    int request_length = snprintf(
        request,
        sizeof(request),
        "HEAD / HTTP/1.0\r\nHost: " IPSTR "\r\nConnection: close\r\n\r\n",
        IP2STR(&target)
    );
    if (request_length <= 0 || (size_t)request_length >= sizeof(request) ||
        !send_bounded(sock, request, (size_t)request_length,
                      WIFI_PORT_SCAN_HTTP_TIMEOUT_MS) ||
        !wait_for_socket(sock, false, WIFI_PORT_SCAN_HTTP_TIMEOUT_MS)) {
        return false;
    }

    uint8_t raw[WIFI_PORT_SCAN_BANNER_RAW_BYTES];
    int received = recv(sock, raw, sizeof(raw), 0);
    if (received <= 0) return false;

    bool sanitized_truncated = false;
    size_t visible_length = wifi_banner_sanitize(
        raw,
        (size_t)received,
        observation->banner,
        sizeof(observation->banner),
        &sanitized_truncated
    );
    if (visible_length == 0U) return false;

    observation->port = probe->port;
    observation->service_hint = probe->service_hint;
    observation->source = WIFI_BANNER_ACTIVE;
    observation->banner_length = (uint8_t)visible_length;
    observation->truncated = sanitized_truncated ||
                             received == (int)sizeof(raw);
    return true;
}

static bool capture_rtsp_banner(int sock,
                                const wifi_probe_result_t *probe,
                                wifi_banner_observation_t *observation)
{
    if (sock < 0 || probe == NULL || observation == NULL ||
        probe->state != WIFI_PROBE_OPEN ||
        !service_supports_rtsp_options(probe->service_hint)) {
        return false;
    }

    static const char request[] =
        "OPTIONS * RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "\r\n";
    if (!send_bounded(sock, request, sizeof(request) - 1U,
                      WIFI_PORT_SCAN_HTTP_TIMEOUT_MS) ||
        !wait_for_socket(sock, false, WIFI_PORT_SCAN_HTTP_TIMEOUT_MS)) {
        return false;
    }

    uint8_t raw[WIFI_PORT_SCAN_BANNER_RAW_BYTES];
    int received = recv(sock, raw, sizeof(raw), 0);
    if (received <= 0) return false;

    bool sanitized_truncated = false;
    size_t visible_length = wifi_banner_sanitize(
        raw,
        (size_t)received,
        observation->banner,
        sizeof(observation->banner),
        &sanitized_truncated
    );
    if (visible_length == 0U) return false;

    observation->port = probe->port;
    observation->service_hint = probe->service_hint;
    observation->source = WIFI_BANNER_ACTIVE;
    observation->banner_length = (uint8_t)visible_length;
    observation->truncated = sanitized_truncated ||
                             received == (int)sizeof(raw);
    return true;
}

static bool capture_service_banner(int sock,
                                   uint32_t target_ipv4,
                                   const wifi_probe_result_t *probe,
                                   wifi_banner_observation_t *observation)
{
    if (probe == NULL) return false;
    if (service_has_passive_banner(probe->service_hint)) {
        return capture_passive_banner(sock, probe, observation);
    }
    if (service_supports_http_head(probe->service_hint)) {
        return capture_http_banner(
            sock, target_ipv4, probe, observation);
    }
    if (service_supports_rtsp_options(probe->service_hint)) {
        return capture_rtsp_banner(sock, probe, observation);
    }
    return false;
}

static bool probe_tcp_port(
    uint32_t gateway,
    uint16_t port,
    uint16_t timeout_ms,
    wifi_probe_result_t *probe,
    int *connected_socket
)
{
    if (probe == NULL) return true;
    if (connected_socket != NULL) *connected_socket = -1;
    memset(probe, 0, sizeof(*probe));
    probe->port = port;
    probe->state = WIFI_PROBE_ERROR;
    probe->service_hint = service_hint_for_port(port);
    int64_t started_us = esp_timer_get_time();

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        probe->socket_error = errno;
        probe->latency_ms = elapsed_ms_since(started_us);
        return socket_error_is_resource_fatal(probe->socket_error);
    }

    bool fatal = false;
    if (sock >= FD_SETSIZE) {
        ESP_LOGE(TAG, "Socket fd=%d exceeds FD_SETSIZE=%d", sock, FD_SETSIZE);
        probe->socket_error = EMFILE;
        fatal = true;
        goto cleanup;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        probe->socket_error = errno;
        goto cleanup;
    }

    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = gateway,
    };
    int connect_result = connect(
        sock,
        (const struct sockaddr *)&destination,
        sizeof(destination)
    );
    if (connect_result == 0) {
        probe->state = WIFI_PROBE_OPEN;
        goto cleanup;
    }
    if (errno != EINPROGRESS) {
        probe->socket_error = errno;
        probe->state = classify_socket_error(probe->socket_error);
        goto cleanup;
    }

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(sock, &write_set);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000U,
        .tv_usec = (timeout_ms % 1000U) * 1000U,
    };
    int selected = select(sock + 1, NULL, &write_set, NULL, &timeout);
    if (selected == 0) {
        probe->socket_error = ETIMEDOUT;
        probe->state = WIFI_PROBE_FILTERED;
    } else if (selected > 0 && FD_ISSET(sock, &write_set)) {
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error,
                       &error_length) == 0) {
            probe->socket_error = socket_error;
            probe->state = classify_socket_error(socket_error);
        } else {
            probe->socket_error = errno;
        }
    } else {
        probe->socket_error = errno;
    }

cleanup:
    probe->latency_ms = elapsed_ms_since(started_us);
    if (probe->state == WIFI_PROBE_OPEN && connected_socket != NULL) {
        /* Ownership moves to the caller, which must close this descriptor. */
        *connected_socket = sock;
        sock = -1;
    }
    if (sock >= 0) close(sock);
    return fatal;
}

static void record_probe_result(wifi_port_scan_result_t *result,
                                const wifi_probe_result_t *probe)
{
    if (result == NULL || probe == NULL) return;
    if (result->probe_count < WIFI_PORT_SCAN_MAX_PROBES) {
        result->probes[result->probe_count++] = *probe;
    }

    ++result->scanned_count;
    switch (probe->state) {
    case WIFI_PROBE_OPEN:
        if (result->open_count < WIFI_PORT_SCAN_MAX_OPEN_PORTS) {
            result->open_ports[result->open_count++] = probe->port;
        }
        break;
    case WIFI_PROBE_CLOSED: ++result->closed_count; break;
    case WIFI_PROBE_FILTERED: ++result->filtered_count; break;
    case WIFI_PROBE_ERROR: ++result->probe_error_count; break;
    default: break;
    }
    publish_progress(result, probe->port);
}

static void close_connect_slot(wifi_connect_slot_t *slot)
{
    if (slot == NULL) return;
    if (slot->sock >= 0) close(slot->sock);
    slot->sock = -1;
    slot->active = false;
}

static void close_all_connect_slots(wifi_connect_slot_t *slots)
{
    if (slots == NULL) return;
    for (size_t i = 0U; i < WIFI_PORT_SCAN_CONNECT_CONCURRENCY; ++i) {
        close_connect_slot(&slots[i]);
    }
}

/*
 * Starts one non-blocking connect attempt.
 * Returns true while select() must continue watching this slot. A false return
 * means probe already contains a terminal result and no descriptor is owned.
 */
static bool start_connect_slot(wifi_connect_slot_t *slot,
                               uint32_t target_ipv4,
                               uint16_t port,
                               uint16_t timeout_ms,
                               bool *resource_fatal)
{
    if (slot == NULL || resource_fatal == NULL) return false;
    memset(slot, 0, sizeof(*slot));
    slot->sock = -1;
    slot->probe.port = port;
    slot->probe.state = WIFI_PROBE_ERROR;
    slot->probe.service_hint = service_hint_for_port(port);
    slot->started_us = esp_timer_get_time();
    slot->deadline_us = slot->started_us + (int64_t)timeout_ms * 1000;
    *resource_fatal = false;

    slot->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (slot->sock < 0) {
        slot->probe.socket_error = errno;
        slot->probe.latency_ms = elapsed_ms_since(slot->started_us);
        *resource_fatal = socket_error_is_resource_fatal(
            slot->probe.socket_error);
        return false;
    }
    if (slot->sock >= FD_SETSIZE) {
        slot->probe.socket_error = EMFILE;
        slot->probe.latency_ms = elapsed_ms_since(slot->started_us);
        *resource_fatal = true;
        close_connect_slot(slot);
        return false;
    }

    int flags = fcntl(slot->sock, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(slot->sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        slot->probe.socket_error = errno;
        slot->probe.latency_ms = elapsed_ms_since(slot->started_us);
        *resource_fatal = socket_error_is_resource_fatal(
            slot->probe.socket_error);
        close_connect_slot(slot);
        return false;
    }

    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = target_ipv4,
    };
    int connected = connect(slot->sock,
                            (const struct sockaddr *)&destination,
                            sizeof(destination));
    if (connected == 0) {
        slot->probe.state = WIFI_PROBE_OPEN;
        slot->probe.latency_ms = elapsed_ms_since(slot->started_us);
        close_connect_slot(slot);
        return false;
    }
    if (errno != EINPROGRESS) {
        slot->probe.socket_error = errno;
        slot->probe.state = classify_socket_error(errno);
        slot->probe.latency_ms = elapsed_ms_since(slot->started_us);
        *resource_fatal = socket_error_is_resource_fatal(errno);
        close_connect_slot(slot);
        return false;
    }

    slot->active = true;
    return true;
}

static void finish_connect_slot(wifi_connect_slot_t *slot,
                                bool ready,
                                int64_t now_us)
{
    if (slot == NULL || !slot->active) return;

    if (ready) {
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(slot->sock, SOL_SOCKET, SO_ERROR, &socket_error,
                       &error_length) == 0) {
            slot->probe.socket_error = socket_error;
            slot->probe.state = classify_socket_error(socket_error);
        } else {
            slot->probe.socket_error = errno;
            slot->probe.state = WIFI_PROBE_ERROR;
        }
    } else if (now_us >= slot->deadline_us) {
        slot->probe.socket_error = ETIMEDOUT;
        slot->probe.state = WIFI_PROBE_FILTERED;
    } else {
        return;
    }

    slot->probe.latency_ms = elapsed_ms_since(slot->started_us);
    close_connect_slot(slot);
}

static void run_connect_engine(const wifi_port_scan_request_t *request,
                               wifi_port_scan_result_t *result)
{
    wifi_connect_slot_t slots[WIFI_PORT_SCAN_CONNECT_CONCURRENCY];
    for (size_t i = 0U; i < WIFI_PORT_SCAN_CONNECT_CONCURRENCY; ++i) {
        slots[i].sock = -1;
        slots[i].active = false;
    }

    uint16_t next_port = 0U;
    uint8_t active_count = 0U;
    bool stop = false;

    while (!stop &&
           (next_port < request->port_count || active_count > 0U)) {
        if (atomic_load_explicit(&s_cancel_requested,
                                 memory_order_acquire)) {
            result->error = ESP_ERR_INVALID_STATE;
            result->finish = WIFI_PORT_SCAN_FINISH_CANCELLED;
            break;
        }
        if (!connection_still_matches(request->expected_ssid,
                                      result->gateway)) {
            result->error = ESP_ERR_INVALID_STATE;
            result->finish = WIFI_PORT_SCAN_FINISH_NETWORK_CHANGED;
            break;
        }

        for (size_t i = 0U;
             i < WIFI_PORT_SCAN_CONNECT_CONCURRENCY &&
             next_port < request->port_count;
             ++i) {
            if (slots[i].active) continue;

            const uint16_t port = request->ports[next_port++];
            publish_progress(result, port);
            bool resource_fatal = false;
            bool active = start_connect_slot(
                &slots[i], result->target_ipv4, port,
                request->job.timeout_ms, &resource_fatal);
            if (active) {
                ++active_count;
            } else {
                record_probe_result(result, &slots[i].probe);
            }
            if (resource_fatal) {
                result->error = ESP_ERR_NO_MEM;
                result->finish = WIFI_PORT_SCAN_FINISH_RESOURCE_ERROR;
                stop = true;
                break;
            }
        }
        if (stop || active_count == 0U) continue;

        fd_set write_set;
        FD_ZERO(&write_set);
        int max_sock = -1;
        int64_t now_us = esp_timer_get_time();
        int64_t nearest_deadline_us = now_us +
            (int64_t)WIFI_PORT_SCAN_SELECT_SLICE_MS * 1000;
        for (size_t i = 0U; i < WIFI_PORT_SCAN_CONNECT_CONCURRENCY; ++i) {
            if (!slots[i].active) continue;
            FD_SET(slots[i].sock, &write_set);
            if (slots[i].sock > max_sock) max_sock = slots[i].sock;
            if (slots[i].deadline_us < nearest_deadline_us) {
                nearest_deadline_us = slots[i].deadline_us;
            }
        }

        int64_t wait_us = nearest_deadline_us - now_us;
        if (wait_us < 0) wait_us = 0;
        struct timeval timeout = {
            .tv_sec = (time_t)(wait_us / 1000000),
            .tv_usec = (suseconds_t)(wait_us % 1000000),
        };
        int selected = select(max_sock + 1, NULL, &write_set, NULL, &timeout);
        const int select_error = selected < 0 ? errno : 0;
        now_us = esp_timer_get_time();

        for (size_t i = 0U; i < WIFI_PORT_SCAN_CONNECT_CONCURRENCY; ++i) {
            if (!slots[i].active) continue;
            if (selected < 0 && select_error != EINTR) {
                slots[i].probe.socket_error = select_error;
                slots[i].probe.state = WIFI_PROBE_ERROR;
                slots[i].probe.latency_ms = elapsed_ms_since(
                    slots[i].started_us);
                close_connect_slot(&slots[i]);
            } else {
                const bool ready = selected > 0 &&
                                   FD_ISSET(slots[i].sock, &write_set);
                finish_connect_slot(&slots[i], ready, now_us);
            }

            if (!slots[i].active) {
                --active_count;
                record_probe_result(result, &slots[i].probe);
            }
        }
    }

    close_all_connect_slots(slots);
}

static void capture_result_banners(wifi_port_scan_result_t *result)
{
    if (result == NULL || result->finish != WIFI_PORT_SCAN_FINISH_COMPLETE) {
        return;
    }

    for (uint8_t i = 0U; i < result->probe_count; ++i) {
        if (atomic_load_explicit(&s_cancel_requested, memory_order_acquire)) {
            result->error = ESP_ERR_INVALID_STATE;
            result->finish = WIFI_PORT_SCAN_FINISH_CANCELLED;
            return;
        }
        if (!connection_still_matches(result->ssid, result->gateway)) {
            result->error = ESP_ERR_INVALID_STATE;
            result->finish = WIFI_PORT_SCAN_FINISH_NETWORK_CHANGED;
            return;
        }

        const wifi_probe_result_t *original = &result->probes[i];
        const bool eligible = original->state == WIFI_PROBE_OPEN &&
            (service_has_passive_banner(original->service_hint) ||
             service_supports_http_head(original->service_hint) ||
             service_supports_rtsp_options(original->service_hint));
        if (!eligible) continue;

        if (result->banner_count >= WIFI_PORT_SCAN_MAX_BANNERS) {
            if (result->banner_dropped < UINT8_MAX) {
                ++result->banner_dropped;
            }
            continue;
        }

        /*
         * Service detection is a separate bounded phase. Reconnecting here
         * keeps banner waits out of the main connect engine and lets the next
         * stage manage several connect slots with one select() call.
         */
        wifi_probe_result_t verification = {0};
        int sock = -1;
        (void)probe_tcp_port(
            result->target_ipv4,
            original->port,
            result->timeout_ms,
            &verification,
            &sock
        );
        if (sock < 0 || verification.state != WIFI_PROBE_OPEN) {
            if (sock >= 0) close(sock);
            continue;
        }

        wifi_banner_observation_t observation = {0};
        bool captured = capture_service_banner(
            sock, result->target_ipv4, &verification, &observation);
        close(sock);
        if (captured) {
            result->banners[result->banner_count++] = observation;
        }
    }
}

static void publish_progress(const wifi_port_scan_result_t *result,
                             uint16_t current_port)
{
    atomic_store_explicit(&s_progress_scanned, result->scanned_count,
                          memory_order_release);
    atomic_store_explicit(&s_progress_current_port, current_port,
                          memory_order_release);
    atomic_store_explicit(&s_progress_open, result->open_count,
                          memory_order_release);
}

static void finish_scan(wifi_port_scan_result_t *result)
{
    publish_progress(result, 0);
    uint32_t generation = 0;
    esp_err_t publish_error = wifi_map_store_publish_port_scan(
        result,
        &generation
    );
    if (publish_error == ESP_OK && s_result_queue != NULL) {
        wifi_port_scan_event_t event = {.generation = generation};
        (void)xQueueOverwrite(s_result_queue, &event);
    } else if (publish_error != ESP_OK) {
        ESP_LOGE(TAG, "Result publish failed: %s",
                 esp_err_to_name(publish_error));
    }
    ESP_LOGI(TAG,
             "Scan done: profile=%u checked=%u open=%u closed=%u "
             "filtered=%u errors=%u finish=%u error=%s "
             "host=%s confidence=%u evidence=0x%04x "
             "discovery=%u/%u dropped=%u discovery_error=%s",
             (unsigned)result->profile,
             (unsigned)result->scanned_count,
             (unsigned)result->open_count,
             (unsigned)result->closed_count,
             (unsigned)result->filtered_count,
             (unsigned)result->probe_error_count,
             (unsigned)result->finish,
             esp_err_to_name(result->error),
             wifi_host_hint_name(result->classification.hint),
             (unsigned)result->classification.confidence,
             (unsigned)result->classification.evidence,
             (unsigned)result->discovery.host_count,
             (unsigned)result->discovery.request_count,
             (unsigned)result->discovery.dropped_count,
             esp_err_to_name(result->discovery.error));
    log_memory_status("scan finished");
    atomic_store_explicit(&s_cancel_requested, false, memory_order_release);
    atomic_store_explicit(&s_state, WIFI_PORT_SCAN_STATE_IDLE,
                          memory_order_release);
}

static void run_scan(const wifi_port_scan_request_t *request)
{
    wifi_port_scan_result_t *result = s_work_result;
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    *result = (wifi_port_scan_result_t){
        .profile = request->job.profile,
        .job_id = request->job_id,
        .timeout_ms = request->job.timeout_ms,
        .ports_randomized = request->job.randomize_ports,
        .connect_concurrency = WIFI_PORT_SCAN_CONNECT_CONCURRENCY,
        .total_count = request->port_count,
        .finish = WIFI_PORT_SCAN_FINISH_COMPLETE,
    };
    snprintf(result->ssid, sizeof(result->ssid), "%s", request->expected_ssid);
    wifi_connection_snapshot_t connection = {0};

    if (!get_connection_snapshot_retry(&connection) ||
        !connection.info.connected || connection.info.gateway == 0 ||
        strcmp(connection.info.ssid, request->expected_ssid) != 0) {
        result->error = ESP_ERR_INVALID_STATE;
        result->finish = WIFI_PORT_SCAN_FINISH_NETWORK_CHANGED;
        finish_scan(result);
        return;
    }

    result->gateway = connection.info.gateway;
    result->target_ipv4 = request->job.target_ipv4 == 0
        ? connection.info.gateway : request->job.target_ipv4;
    if (!target_is_in_local_subnet(&connection.info, result->target_ipv4)) {
        result->error = ESP_ERR_INVALID_ARG;
        result->finish = WIFI_PORT_SCAN_FINISH_INVALID_TARGET;
        finish_scan(result);
        return;
    }
    if (request->job.discover_hosts) {
        const discovery_abort_context_t abort_context = {
            .ssid = result->ssid,
            .gateway = result->gateway,
        };
        esp_err_t discovery_error = wifi_host_discovery_run(
            &connection.info,
            &result->discovery,
            discovery_should_abort,
            (void *)&abort_context
        );
        if (discovery_error == ESP_ERR_INVALID_STATE) {
            result->error = ESP_ERR_INVALID_STATE;
            result->finish = atomic_load_explicit(
                &s_cancel_requested, memory_order_acquire)
                ? WIFI_PORT_SCAN_FINISH_CANCELLED
                : WIFI_PORT_SCAN_FINISH_NETWORK_CHANGED;
            finish_scan(result);
            return;
        }
        /* ICMP can be unavailable or filtered; TCP scanning remains useful. */
    }
    run_connect_engine(request, result);
    capture_result_banners(result);
    result->classification = wifi_host_classify(result);
    finish_scan(result);
}

static void port_scan_worker_task(void *arg)
{
    (void)arg;
    wifi_port_scan_request_t request;

    for (;;) {
        if (xQueueReceive(s_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        run_scan(&request);
        memset(&request, 0, sizeof(request));
    }
}

static bool configure_request(
    wifi_port_scan_request_t *request,
    const wifi_scan_job_t *job
)
{
    if (request == NULL || job == NULL) return false;
    request->job = *job;
    if (request->job.timeout_ms == 0) {
        request->job.timeout_ms = WIFI_PORT_SCAN_DEFAULT_TIMEOUT_MS;
    }
    if (request->job.timeout_ms < WIFI_PORT_SCAN_MIN_TIMEOUT_MS ||
        request->job.timeout_ms > WIFI_PORT_SCAN_MAX_TIMEOUT_MS) {
        return false;
    }

    const uint16_t *ports = NULL;
    uint16_t port_count = 0;
    switch (job->profile) {
    case WIFI_PORT_SCAN_PROFILE_QUICK:
        ports = s_quick_ports;
        port_count = (uint16_t)(sizeof(s_quick_ports) /
                                sizeof(s_quick_ports[0]));
        break;
    case WIFI_PORT_SCAN_PROFILE_COMMON:
        ports = s_common_ports;
        port_count = (uint16_t)(sizeof(s_common_ports) /
                                sizeof(s_common_ports[0]));
        break;
    case WIFI_PORT_SCAN_PROFILE_IOT_LAN:
        ports = s_iot_lan_ports;
        port_count = (uint16_t)(sizeof(s_iot_lan_ports) /
                                sizeof(s_iot_lan_ports[0]));
        break;
    case WIFI_PORT_SCAN_PROFILE_CUSTOM: {
        if (job->first_port == 0 || job->last_port < job->first_port) {
            return false;
        }
        uint32_t count = (uint32_t)job->last_port - job->first_port + 1U;
        if (count > WIFI_PORT_SCAN_MAX_PROBES) return false;
        request->port_count = (uint16_t)count;
        for (uint16_t i = 0; i < request->port_count; ++i) {
            request->ports[i] = (uint16_t)(job->first_port + i);
        }
        if (request->job.randomize_ports) {
            shuffle_ports(request->ports, request->port_count);
        }
        return true;
    }
    default:
        return false;
    }

    if (port_count > WIFI_PORT_SCAN_MAX_PROBES) return false;
    request->port_count = port_count;
    memcpy(request->ports, ports, (size_t)port_count * sizeof(ports[0]));
    if (request->job.randomize_ports) {
        shuffle_ports(request->ports, request->port_count);
    }
    return true;
}

esp_err_t wifi_port_scan_init(void)
{
    if (s_worker_task != NULL) return ESP_OK;

    esp_err_t store_error = wifi_map_store_init();
    if (store_error != ESP_OK) {
        ESP_LOGE(TAG, "MAP store init failed: %s",
                 esp_err_to_name(store_error));
        return store_error;
    }

    if (s_work_result == NULL) {
        s_work_result = heap_caps_calloc(
            1U,
            sizeof(*s_work_result),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (s_work_result == NULL) {
            ESP_LOGE(TAG, "Could not allocate scanner scratch in PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }

    s_result_queue = xQueueCreate(1, sizeof(wifi_port_scan_event_t));
    s_request_queue = xQueueCreate(1, sizeof(wifi_port_scan_request_t));
    if (s_result_queue == NULL || s_request_queue == NULL) {
        if (s_request_queue != NULL) vQueueDelete(s_request_queue);
        if (s_result_queue != NULL) vQueueDelete(s_result_queue);
        s_request_queue = NULL;
        s_result_queue = NULL;
        heap_caps_free(s_work_result);
        s_work_result = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(
        port_scan_worker_task,
        "wifi_port_scan",
        WIFI_PORT_SCAN_TASK_STACK,
        NULL,
        WIFI_PORT_SCAN_TASK_PRIORITY,
        &s_worker_task
    );
    if (created != pdPASS) {
        vQueueDelete(s_request_queue);
        vQueueDelete(s_result_queue);
        s_request_queue = NULL;
        s_result_queue = NULL;
        s_worker_task = NULL;
        heap_caps_free(s_work_result);
        s_work_result = NULL;
        return ESP_ERR_NO_MEM;
    }

    atomic_store_explicit(&s_state, WIFI_PORT_SCAN_STATE_IDLE,
                          memory_order_release);
    log_memory_status("worker initialized");
    return ESP_OK;
}

esp_err_t wifi_port_scan_start(const char *expected_ssid)
{
    return wifi_port_scan_start_profile(
        expected_ssid,
        WIFI_PORT_SCAN_PROFILE_QUICK
    );
}

esp_err_t wifi_port_scan_start_profile(
    const char *expected_ssid,
    wifi_port_scan_profile_t profile
)
{
    const wifi_scan_job_t job = {
        .profile = profile,
        .randomize_ports = true,
        /*
         * Quick/Common currently target one selected host (the gateway), so a
         * /24 ICMP sweep only adds radio load before their TCP probes. Keep
         * discovery for the IoT/LAN map workflow that consumes its host list.
         */
        .discover_hosts = profile == WIFI_PORT_SCAN_PROFILE_IOT_LAN,
        .timeout_ms = profile == WIFI_PORT_SCAN_PROFILE_IOT_LAN
            ? WIFI_PORT_SCAN_IOT_TIMEOUT_MS
            : (profile == WIFI_PORT_SCAN_PROFILE_COMMON
                ? WIFI_PORT_SCAN_COMMON_TIMEOUT_MS
                : WIFI_PORT_SCAN_QUICK_TIMEOUT_MS),
    };
    return wifi_port_scan_start_job(expected_ssid, &job, NULL);
}

esp_err_t wifi_port_scan_start_job(
    const char *expected_ssid,
    const wifi_scan_job_t *job,
    uint32_t *job_id
)
{
    if (expected_ssid == NULL) return ESP_ERR_INVALID_ARG;
    if (job == NULL) return ESP_ERR_INVALID_ARG;
    if (s_worker_task == NULL || s_request_queue == NULL ||
        s_result_queue == NULL) {
        ESP_LOGE(TAG, "Start rejected: scanner is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    size_t ssid_length = strnlen(expected_ssid, WIFI_SSID_MAX_LEN + 1U);
    if (ssid_length == 0 || ssid_length > WIFI_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_port_scan_request_t request = {0};
    if (!configure_request(&request, job)) return ESP_ERR_INVALID_ARG;
    memcpy(request.expected_ssid, expected_ssid, ssid_length);
    uint32_t next_job_id = (uint32_t)atomic_fetch_add_explicit(
        &s_next_job_id, 1U, memory_order_relaxed);
    if (next_job_id == 0U) {
        next_job_id = (uint32_t)atomic_fetch_add_explicit(
            &s_next_job_id, 1U, memory_order_relaxed);
    }
    request.job_id = next_job_id;

    int expected = WIFI_PORT_SCAN_STATE_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &s_state, &expected, WIFI_PORT_SCAN_STATE_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&s_cancel_requested, false, memory_order_release);
    atomic_store_explicit(&s_progress_profile, job->profile,
                          memory_order_release);
    atomic_store_explicit(&s_progress_total, request.port_count,
                          memory_order_release);
    atomic_store_explicit(&s_progress_scanned, 0, memory_order_release);
    atomic_store_explicit(&s_progress_current_port, 0, memory_order_release);
    atomic_store_explicit(&s_progress_open, 0, memory_order_release);
    (void)xQueueReset(s_result_queue);

    if (xQueueSend(s_request_queue, &request, 0) != pdTRUE) {
        atomic_store_explicit(&s_cancel_requested, false,
                              memory_order_release);
        atomic_store_explicit(&s_state, WIFI_PORT_SCAN_STATE_IDLE,
                              memory_order_release);
        ESP_LOGE(TAG, "Start rejected: request queue is full");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Scan queued: profile=%u ports=%u timeout=%u ms",
             (unsigned)job->profile, (unsigned)request.port_count,
             (unsigned)request.job.timeout_ms);
    if (job_id != NULL) *job_id = next_job_id;
    return ESP_OK;
}

esp_err_t wifi_port_scan_cancel(void)
{
    if (!wifi_port_scan_is_running()) return ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&s_cancel_requested, true, memory_order_release);
    return ESP_OK;
}

bool wifi_port_scan_receive_result(wifi_port_scan_result_t *result)
{
    if (result == NULL || s_result_queue == NULL) return false;
    wifi_port_scan_event_t event = {0};
    if (xQueuePeek(s_result_queue, &event, 0) != pdTRUE) return false;
    if (!wifi_map_store_get_port_scan(event.generation, result)) return false;
    wifi_port_scan_event_t consumed = {0};
    if (xQueueReceive(s_result_queue, &consumed, 0) != pdTRUE) return false;
    if (consumed.generation != event.generation) {
        /* Preserve a newer completion that replaced the event after peek. */
        (void)xQueueOverwrite(s_result_queue, &consumed);
    }
    return true;
}

bool wifi_port_scan_is_running(void)
{
    return atomic_load_explicit(&s_state, memory_order_acquire) ==
           WIFI_PORT_SCAN_STATE_RUNNING;
}

bool wifi_port_scan_get_progress(wifi_port_scan_progress_t *progress)
{
    if (progress == NULL || s_result_queue == NULL) return false;
    progress->state = (wifi_port_scan_state_t)atomic_load_explicit(
        &s_state, memory_order_acquire);
    progress->profile = (wifi_port_scan_profile_t)atomic_load_explicit(
        &s_progress_profile, memory_order_acquire);
    progress->total_count = (uint16_t)atomic_load_explicit(
        &s_progress_total, memory_order_acquire);
    progress->scanned_count = (uint16_t)atomic_load_explicit(
        &s_progress_scanned, memory_order_acquire);
    progress->current_port = (uint16_t)atomic_load_explicit(
        &s_progress_current_port, memory_order_acquire);
    progress->open_count = atomic_load_explicit(
        &s_progress_open, memory_order_acquire);
    return true;
}

const char *wifi_port_scan_service_name(wifi_service_hint_t service)
{
    switch (service) {
    case WIFI_SERVICE_FTP: return "ftp";
    case WIFI_SERVICE_SSH: return "ssh";
    case WIFI_SERVICE_TELNET: return "telnet";
    case WIFI_SERVICE_SMTP: return "smtp";
    case WIFI_SERVICE_DNS: return "dns";
    case WIFI_SERVICE_HTTP: return "http";
    case WIFI_SERVICE_POP3: return "pop3";
    case WIFI_SERVICE_RPC: return "rpc";
    case WIFI_SERVICE_NETBIOS: return "netbios";
    case WIFI_SERVICE_IMAP: return "imap";
    case WIFI_SERVICE_LDAP: return "ldap";
    case WIFI_SERVICE_HTTPS: return "https";
    case WIFI_SERVICE_SMB: return "smb";
    case WIFI_SERVICE_PRINTER: return "printer";
    case WIFI_SERVICE_AFP: return "afp";
    case WIFI_SERVICE_RTSP: return "rtsp";
    case WIFI_SERVICE_RSYNC: return "rsync";
    case WIFI_SERVICE_MQTT: return "mqtt";
    case WIFI_SERVICE_NFS: return "nfs";
    case WIFI_SERVICE_MYSQL: return "mysql";
    case WIFI_SERVICE_RDP: return "rdp";
    case WIFI_SERVICE_POSTGRESQL: return "postgresql";
    case WIFI_SERVICE_VNC: return "vnc";
    case WIFI_SERVICE_REDIS: return "redis";
    case WIFI_SERVICE_HTTP_ALT: return "http-alt";
    case WIFI_SERVICE_HTTPS_ALT: return "https-alt";
    case WIFI_SERVICE_MONGODB: return "mongodb";
    case WIFI_SERVICE_MODBUS: return "modbus";
    case WIFI_SERVICE_UNKNOWN:
    default: return "unknown";
    }
}

const char *wifi_port_scan_probe_state_name(wifi_probe_state_t state)
{
    switch (state) {
    case WIFI_PROBE_OPEN: return "OPEN";
    case WIFI_PROBE_CLOSED: return "CLOSED";
    case WIFI_PROBE_FILTERED: return "FILTERED";
    case WIFI_PROBE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}
