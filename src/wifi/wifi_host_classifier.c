#include "wifi_host_classifier.h"

#include <stddef.h>

typedef struct {
    uint16_t evidence;
    uint16_t banner_services;
} observed_host_t;

static uint16_t evidence_for_service(wifi_service_hint_t service)
{
    switch (service) {
    case WIFI_SERVICE_HTTP:
    case WIFI_SERVICE_HTTPS:
    case WIFI_SERVICE_HTTP_ALT:
    case WIFI_SERVICE_HTTPS_ALT:
        return WIFI_HOST_EVIDENCE_HTTP;
    case WIFI_SERVICE_SSH:
        return WIFI_HOST_EVIDENCE_SSH;
    case WIFI_SERVICE_TELNET:
        return WIFI_HOST_EVIDENCE_TELNET;
    case WIFI_SERVICE_DNS:
        return WIFI_HOST_EVIDENCE_DNS;
    case WIFI_SERVICE_NETBIOS:
    case WIFI_SERVICE_SMB:
        return WIFI_HOST_EVIDENCE_SMB;
    case WIFI_SERVICE_RDP:
        return WIFI_HOST_EVIDENCE_RDP;
    case WIFI_SERVICE_RTSP:
        return WIFI_HOST_EVIDENCE_RTSP;
    case WIFI_SERVICE_PRINTER:
        return WIFI_HOST_EVIDENCE_PRINT_SERVICE;
    case WIFI_SERVICE_MYSQL:
    case WIFI_SERVICE_POSTGRESQL:
    case WIFI_SERVICE_REDIS:
    case WIFI_SERVICE_MONGODB:
        return WIFI_HOST_EVIDENCE_DATABASE;
    case WIFI_SERVICE_MQTT:
        return WIFI_HOST_EVIDENCE_MQTT;
    case WIFI_SERVICE_MODBUS:
        return WIFI_HOST_EVIDENCE_MODBUS;
    case WIFI_SERVICE_RPC:
    case WIFI_SERVICE_NFS:
    case WIFI_SERVICE_AFP:
    case WIFI_SERVICE_RSYNC:
        return WIFI_HOST_EVIDENCE_UNIX_SERVICE;
    default:
        return WIFI_HOST_EVIDENCE_NONE;
    }
}

static bool has_all(uint16_t value, uint16_t bits)
{
    return (value & bits) == bits;
}

static bool has_any(uint16_t value, uint16_t bits)
{
    return (value & bits) != 0U;
}

static uint8_t banner_bonus(const observed_host_t *observed,
                            uint16_t relevant_evidence)
{
    return has_any(observed->banner_services, relevant_evidence) ? 15U : 0U;
}

static wifi_host_classification_t make_hint(wifi_host_hint_t hint,
                                            uint8_t confidence,
                                            const observed_host_t *observed)
{
    wifi_host_classification_t classification = {
        .hint = hint,
        .evidence = observed->evidence,
        .confidence = confidence > 100U ? 100U : confidence,
    };
    return classification;
}

wifi_host_classification_t wifi_host_classify(
    const wifi_port_scan_result_t *result
)
{
    if (result == NULL || result->finish != WIFI_PORT_SCAN_FINISH_COMPLETE) {
        return (wifi_host_classification_t){0};
    }

    observed_host_t observed = {0};
    if (result->target_ipv4 != 0U && result->target_ipv4 == result->gateway) {
        observed.evidence |= WIFI_HOST_EVIDENCE_GATEWAY;
    }

    for (uint8_t i = 0U; i < result->probe_count; ++i) {
        if (result->probes[i].state == WIFI_PROBE_OPEN) {
            observed.evidence |= evidence_for_service(
                result->probes[i].service_hint);
        }
    }
    for (uint8_t i = 0U; i < result->banner_count; ++i) {
        if (result->banners[i].banner_length == 0U) continue;
        observed.evidence |= WIFI_HOST_EVIDENCE_BANNER;
        observed.banner_services |= evidence_for_service(
            result->banners[i].service_hint);
    }

    const uint16_t e = observed.evidence;
    if (has_any(e, WIFI_HOST_EVIDENCE_RTSP)) {
        return make_hint(WIFI_HOST_HINT_IP_CAMERA,
                         75U + banner_bonus(&observed,
                                            WIFI_HOST_EVIDENCE_RTSP),
                         &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_MODBUS)) {
        return make_hint(WIFI_HOST_HINT_IOT_CONTROLLER, 90U, &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_PRINT_SERVICE)) {
        return make_hint(WIFI_HOST_HINT_PRINTER, 80U, &observed);
    }
    if (has_all(e, WIFI_HOST_EVIDENCE_SMB | WIFI_HOST_EVIDENCE_RDP)) {
        return make_hint(WIFI_HOST_HINT_WINDOWS_LIKE, 95U, &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_RDP)) {
        return make_hint(WIFI_HOST_HINT_WINDOWS_LIKE, 85U, &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_SMB)) {
        return make_hint(WIFI_HOST_HINT_WINDOWS_LIKE, 70U, &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_DATABASE)) {
        return make_hint(WIFI_HOST_HINT_DATABASE, 80U, &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_MQTT)) {
        return make_hint(WIFI_HOST_HINT_IOT_CONTROLLER, 75U, &observed);
    }
    if (has_all(e, WIFI_HOST_EVIDENCE_GATEWAY | WIFI_HOST_EVIDENCE_DNS) &&
        has_any(e, WIFI_HOST_EVIDENCE_HTTP | WIFI_HOST_EVIDENCE_SSH |
                   WIFI_HOST_EVIDENCE_TELNET)) {
        return make_hint(WIFI_HOST_HINT_NETWORK_DEVICE, 90U, &observed);
    }
    if (has_all(e, WIFI_HOST_EVIDENCE_SSH |
                   WIFI_HOST_EVIDENCE_UNIX_SERVICE)) {
        return make_hint(WIFI_HOST_HINT_UNIX_LIKE, 85U, &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_SSH)) {
        return make_hint(WIFI_HOST_HINT_UNIX_LIKE,
                         55U + banner_bonus(&observed,
                                            WIFI_HOST_EVIDENCE_SSH),
                         &observed);
    }
    if (has_any(e, WIFI_HOST_EVIDENCE_HTTP)) {
        return make_hint(WIFI_HOST_HINT_WEB_DEVICE,
                         50U + banner_bonus(&observed,
                                            WIFI_HOST_EVIDENCE_HTTP),
                         &observed);
    }
    return make_hint(WIFI_HOST_HINT_UNKNOWN, 0U, &observed);
}

const char *wifi_host_hint_name(wifi_host_hint_t hint)
{
    switch (hint) {
    case WIFI_HOST_HINT_WEB_DEVICE: return "WEB";
    case WIFI_HOST_HINT_UNIX_LIKE: return "UNIX_HINT";
    case WIFI_HOST_HINT_WINDOWS_LIKE: return "WINDOWS_HINT";
    case WIFI_HOST_HINT_IP_CAMERA: return "IP_CAMERA_HINT";
    case WIFI_HOST_HINT_NETWORK_DEVICE: return "NETWORK_DEVICE_HINT";
    case WIFI_HOST_HINT_PRINTER: return "PRINTER_HINT";
    case WIFI_HOST_HINT_DATABASE: return "DATABASE_HINT";
    case WIFI_HOST_HINT_IOT_CONTROLLER: return "IOT_CONTROLLER_HINT";
    case WIFI_HOST_HINT_UNKNOWN:
    default: return "UNKNOWN";
    }
}
