#include "wifi_mgmt_parser.h"

#include <string.h>
#include "wifi_ie_iterator.h"

#define IE_SSID 0U
#define IE_DS_PARAMETER_SET 3U
#define IE_RSN 48U

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0U;
    for (unsigned i = 0; i < 8U; ++i) v |= (uint64_t)p[i] << (8U * i);
    return v;
}

static void capability(wifi_mgmt_info_t *out, const uint8_t *p)
{
    out->has_capability = true;
    out->capability = le16(p);
    out->capability_ess = (out->capability & 1U) != 0U;
    out->capability_ibss = (out->capability & 2U) != 0U;
    out->capability_privacy = (out->capability & 0x10U) != 0U;
}

wifi_parse_status_t wifi_mgmt_parse(const uint8_t *mpdu, size_t length,
                                    const wifi_frame_info_t *frame,
                                    wifi_mgmt_info_t *out)
{
    if (out == NULL) return WIFI_PARSE_MALFORMED;
    memset(out, 0, sizeof(*out));
    out->status = WIFI_PARSE_MALFORMED;
    if (mpdu == NULL || frame == NULL || frame->type != WIFI_FRAME_TYPE_MGMT ||
        frame->header_length > length) return out->status;
    out->subtype = frame->subtype;
    out->rx_channel = frame->channel;
    if (frame->protected_frame) {
        out->status = WIFI_PARSE_UNSUPPORTED;
        return out->status;
    }
    const uint8_t *body = mpdu + frame->header_length;
    size_t body_len = length - frame->header_length, fixed = 0U;
    switch (frame->subtype) {
        case 8: case 5: /* Beacon, Probe Response */
            fixed = 12U;
            if (body_len < fixed) goto partial;
            out->has_timestamp = true;
            out->timestamp = le64(body);
            out->has_beacon_interval = true;
            out->beacon_interval = le16(body + 8U);
            capability(out, body + 10U);
            break;
        case 4: fixed = 0U; break; /* Probe Request */
        case 0:
            fixed = 4U;
            if (body_len < fixed) goto partial;
            capability(out, body);
            break;
        case 2:
            fixed = 10U;
            if (body_len < fixed) goto partial;
            capability(out, body);
            break;
        case 1: case 3:
            fixed = 6U;
            if (body_len < fixed) goto partial;
            capability(out, body);
            out->status_code = le16(body + 2U);
            out->has_association_id = true;
            out->association_id = le16(body + 4U) & 0x3fffU;
            break;
        case 11:
            fixed = 6U;
            if (body_len < fixed) goto partial;
            out->has_auth_fields = true;
            out->auth_algorithm = le16(body);
            out->auth_sequence = le16(body + 2U);
            out->status_code = le16(body + 4U);
            break;
        case 10: case 12:
            if (body_len < 2U) goto partial;
            out->has_reason_code = true;
            out->reason_code = le16(body);
            out->status = WIFI_PARSE_VALID;
            return out->status;
        case 13:
            if (body_len < 2U) goto partial;
            out->has_action_fields = true;
            out->action_category = body[0];
            out->action_code = body[1];
            out->status = WIFI_PARSE_VALID;
            return out->status;
        default:
            out->status = WIFI_PARSE_UNSUPPORTED;
            return out->status;
    }
    wifi_ie_iterator_t it; wifi_ie_t ie;
    wifi_ie_iterator_init(&it, body + fixed, body_len - fixed);
    while (wifi_ie_iterator_next(&it, &ie)) {
        if (ie.id == IE_SSID && out->ssid_kind == WIFI_SSID_ABSENT) {
            out->ssid_raw_length = ie.length;
            if (ie.length == 0U) out->ssid_kind = WIFI_SSID_EMPTY;
            else if (ie.length > WIFI_MGMT_SSID_MAX_LEN) { out->ie_chain_malformed = true; break; }
            else {
                bool all_zero = true;
                for (uint8_t i = 0; i < ie.length; ++i) {
                    const uint8_t c = ie.data[i]; if (c != 0U) all_zero = false;
                    out->ssid[i] = c >= 0x20U && c <= 0x7eU ? (char)c : '.';
                }
                out->ssid_kind = all_zero ? WIFI_SSID_ALL_ZERO : WIFI_SSID_NORMAL;
            }
        } else if (ie.id == IE_DS_PARAMETER_SET) {
            if (ie.length != 1U) { out->ie_chain_malformed = true; break; }
            out->has_advertised_channel = true;
            out->advertised_channel = ie.data[0];
        } else if (ie.id == IE_RSN) {
            out->rsn_ie_seen = true;
            (void)wifi_rsn_parse(ie.data, ie.length, &out->rsn);
        }
    }
    if (it.status == WIFI_PARSE_MALFORMED) out->ie_chain_malformed = true;
    out->status = out->ie_chain_malformed ? WIFI_PARSE_MALFORMED : WIFI_PARSE_VALID;
    return out->status;
partial:
    out->status = WIFI_PARSE_PARTIAL;
    return out->status;
}
