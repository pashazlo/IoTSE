#include "wifi_host_discovery.h"

#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/sockets.h"

#define DISCOVERY_SEND_BATCH       8U
#define DISCOVERY_SELECT_SLICE_MS 25U
#define DISCOVERY_PACKET_BYTES    16U

static bool result_has_host(const wifi_host_discovery_result_t *result,
                            uint32_t ipv4)
{
    for (uint8_t i = 0U; i < result->host_count; ++i) {
        if (result->hosts[i].ipv4 == ipv4) return true;
    }
    return false;
}

static void retain_host(wifi_host_discovery_result_t *result,
                        uint32_t ipv4,
                        uint8_t ttl,
                        wifi_host_discovery_source_t source)
{
    if (result_has_host(result, ipv4)) return;
    if (result->host_count >= WIFI_HOST_DISCOVERY_MAX_HOSTS) {
        result->truncated = true;
        if (result->dropped_count < UINT16_MAX) ++result->dropped_count;
        return;
    }
    result->hosts[result->host_count++] = (wifi_discovered_host_t){
        .ipv4 = ipv4,
        .ttl = ttl,
        .source = source,
    };
}

bool wifi_host_discovery_bounds(uint32_t local_ipv4,
                                uint32_t netmask,
                                uint32_t *first_host,
                                uint32_t *last_host)
{
    if (local_ipv4 == 0U || netmask == 0U || first_host == NULL ||
        last_host == NULL) {
        return false;
    }

    const uint32_t local = lwip_ntohl(local_ipv4);
    uint32_t mask = lwip_ntohl(netmask);
    /* Never sweep more than the /24 window containing the station. */
    if (mask < 0xFFFFFF00U) mask = 0xFFFFFF00U;
    const uint32_t network = local & mask;
    const uint32_t broadcast = network | ~mask;
    if (broadcast <= network + 1U) return false;

    *first_host = lwip_htonl(network + 1U);
    *last_host = lwip_htonl(broadcast - 1U);
    return true;
}

static void drain_replies(int sock,
                          uint16_t identifier,
                          uint32_t first_host,
                          uint32_t last_host,
                          uint32_t local_ipv4,
                          wifi_host_discovery_result_t *result)
{
    uint8_t packet[64];
    for (;;) {
        struct sockaddr_in from = {0};
        socklen_t from_length = sizeof(from);
        int received = recvfrom(sock, packet, sizeof(packet), 0,
                                (struct sockaddr *)&from, &from_length);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (received <= 0) return;
        if ((size_t)received < sizeof(struct ip_hdr) +
                               sizeof(struct icmp_echo_hdr)) {
            continue;
        }

        const struct ip_hdr *ip = (const struct ip_hdr *)packet;
        const uint8_t header_bytes = IPH_HL_BYTES(ip);
        if (header_bytes < sizeof(struct ip_hdr) ||
            (size_t)received < (size_t)header_bytes +
                               sizeof(struct icmp_echo_hdr)) {
            continue;
        }
        const struct icmp_echo_hdr *icmp =
            (const struct icmp_echo_hdr *)(packet + header_bytes);
        if (icmp->type != ICMP_ER || icmp->code != 0U ||
            icmp->id != identifier) {
            continue;
        }

        const uint32_t address = from.sin_addr.s_addr;
        const uint32_t host = lwip_ntohl(address);
        if (address == local_ipv4 || host < lwip_ntohl(first_host) ||
            host > lwip_ntohl(last_host)) {
            continue;
        }
        if (result->reply_count < UINT16_MAX) ++result->reply_count;
        retain_host(result, address, IPH_TTL(ip),
                    WIFI_HOST_DISCOVERY_SOURCE_ICMP);
    }
}

static bool wait_and_drain(int sock,
                           uint32_t wait_ms,
                           uint16_t identifier,
                           uint32_t first_host,
                           uint32_t last_host,
                           uint32_t local_ipv4,
                           wifi_host_discovery_result_t *result)
{
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(sock, &read_set);
    struct timeval timeout = {
        .tv_sec = wait_ms / 1000U,
        .tv_usec = (wait_ms % 1000U) * 1000U,
    };
    int selected = select(sock + 1, &read_set, NULL, NULL, &timeout);
    if (selected > 0 && FD_ISSET(sock, &read_set)) {
        drain_replies(sock, identifier, first_host, last_host, local_ipv4,
                      result);
    }
    return selected >= 0 || errno == EINTR;
}

esp_err_t wifi_host_discovery_run(
    const wifi_connection_info_t *connection,
    wifi_host_discovery_result_t *result,
    wifi_host_discovery_abort_fn should_abort,
    void *abort_context
)
{
    if (connection == NULL || result == NULL || !connection->connected) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->attempted = true;

    uint32_t first_host = 0U;
    uint32_t last_host = 0U;
    if (!wifi_host_discovery_bounds(connection->ip, connection->netmask,
                                    &first_host, &last_host)) {
        result->error = ESP_ERR_INVALID_ARG;
        return result->error;
    }
    const uint32_t first = lwip_ntohl(first_host);
    const uint32_t last = lwip_ntohl(last_host);
    result->candidate_count = (uint16_t)(last - first + 1U);
    if (connection->gateway != 0U && connection->gateway != connection->ip) {
        retain_host(result, connection->gateway, 0U,
                    WIFI_HOST_DISCOVERY_SOURCE_GATEWAY);
    }

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        result->error = ESP_FAIL;
        return result->error;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0 ||
        sock >= FD_SETSIZE) {
        close(sock);
        result->error = ESP_FAIL;
        return result->error;
    }

    uint8_t packet[DISCOVERY_PACKET_BYTES] = {0};
    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)packet;
    icmp->type = ICMP_ECHO;
    const uint16_t identifier = lwip_htons((uint16_t)(esp_random() | 1U));
    icmp->id = identifier;

    uint16_t sequence = 1U;
    uint8_t batch_count = 0U;
    for (uint32_t host = first; host <= last; ++host) {
        if (should_abort != NULL && should_abort(abort_context)) {
            close(sock);
            result->error = ESP_ERR_INVALID_STATE;
            return result->error;
        }
        const uint32_t target_ipv4 = lwip_htonl(host);
        if (target_ipv4 == connection->ip) continue;

        icmp->seqno = lwip_htons(sequence++);
        icmp->chksum = 0U;
        icmp->chksum = inet_chksum(packet, sizeof(packet));
        struct sockaddr_in target = {
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr.s_addr = target_ipv4,
        };
        int sent = sendto(sock, packet, sizeof(packet), 0,
                          (const struct sockaddr *)&target, sizeof(target));
        if (sent == (int)sizeof(packet) && result->request_count < UINT16_MAX) {
            ++result->request_count;
        }

        if (++batch_count >= DISCOVERY_SEND_BATCH) {
            batch_count = 0U;
            drain_replies(sock, identifier, first_host, last_host,
                          connection->ip, result);
            vTaskDelay(1);
        }
    }

    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)WIFI_HOST_DISCOVERY_REPLY_WINDOW_MS * 1000;
    while (esp_timer_get_time() < deadline_us) {
        if (should_abort != NULL && should_abort(abort_context)) {
            close(sock);
            result->error = ESP_ERR_INVALID_STATE;
            return result->error;
        }
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        uint32_t wait_ms = (uint32_t)((remaining_us + 999) / 1000);
        if (wait_ms > DISCOVERY_SELECT_SLICE_MS) {
            wait_ms = DISCOVERY_SELECT_SLICE_MS;
        }
        if (!wait_and_drain(sock, wait_ms, identifier, first_host, last_host,
                            connection->ip, result)) {
            close(sock);
            result->error = ESP_FAIL;
            return result->error;
        }
    }
    drain_replies(sock, identifier, first_host, last_host, connection->ip,
                  result);
    close(sock);
    result->complete = true;
    result->error = ESP_OK;
    return ESP_OK;
}
