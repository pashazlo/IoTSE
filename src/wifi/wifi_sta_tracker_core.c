#include "wifi_sta_tracker_internal.h"
#include <limits.h>
#include <string.h>

typedef struct {
    const uint8_t *mac;
    const uint8_t *bssid;
    wifi_sta_relationship_state_t state;
    wifi_sta_relationship_source_t source;
    bool transmitter, receiver, ambiguous;
} roles_t;

static void inc64(uint64_t *v) { if (*v != UINT64_MAX) ++*v; }
static uint32_t next_gen(uint32_t v) { ++v; return v == 0U ? 1U : v; }
static bool unicast(const uint8_t *m) {
    bool any = false; if (m == NULL || (m[0] & 1U)) return false;
    for (unsigned i=0;i<6;i++) any |= m[i] != 0;
    return any;
}
static int find_mac(const wifi_sta_tracker_state_t *s, const uint8_t *mac) {
    for (unsigned i=0;i<WIFI_STA_TRACKER_CAPACITY;i++)
        if (s->slots[i].occupied && !memcmp(s->slots[i].record.mac,mac,6)) return (int)i;
    return -1;
}
static unsigned slot_for(const wifi_sta_tracker_state_t *s, bool *evicted) {
    for (unsigned i=0;i<WIFI_STA_TRACKER_CAPACITY;i++) if(!s->slots[i].occupied){*evicted=false;return i;}
    unsigned oldest=0; for(unsigned i=1;i<WIFI_STA_TRACKER_CAPACITY;i++)
        if(s->slots[i].record.last_seen_us<s->slots[oldest].record.last_seen_us) oldest=i;
    *evicted=true; return oldest;
}
static bool same(const uint8_t *a,const uint8_t *b){return a&&b&&!memcmp(a,b,6);}

static roles_t roles(const wifi_frame_info_t *f,const wifi_mgmt_info_t *m)
{
    roles_t r={0};
    if(f->type==WIFI_FRAME_TYPE_DATA){
        if(f->to_ds&&!f->from_ds){r.mac=f->addr2;r.bssid=f->addr1;r.transmitter=true;r.state=WIFI_STA_REL_DATA_RELATION_OBSERVED;r.source=WIFI_STA_SOURCE_DATA_TO_DS;}
        else if(!f->to_ds&&f->from_ds){r.mac=f->addr1;r.bssid=f->addr2;r.receiver=true;r.state=WIFI_STA_REL_DATA_RELATION_OBSERVED;r.source=WIFI_STA_SOURCE_DATA_FROM_DS;}
        else r.ambiguous=true;
        return r;
    }
    if(f->type==WIFI_FRAME_TYPE_CTRL){r.ambiguous=true;return r;}
    if(f->type!=WIFI_FRAME_TYPE_MGMT||m==NULL){r.ambiguous=true;return r;}
    switch(f->subtype){
        case WIFI_FRAME_SUBTYPE_PROBE_REQ:r.mac=f->addr2;r.transmitter=true;r.state=WIFI_STA_REL_PROBING;break;
        case 0:r.mac=f->addr2;r.bssid=f->bssid;r.transmitter=true;r.state=WIFI_STA_REL_ASSOCIATING;r.source=WIFI_STA_SOURCE_MGMT_ASSOC;break;
        case 2:r.mac=f->addr2;r.bssid=f->bssid;r.transmitter=true;r.state=WIFI_STA_REL_ASSOCIATING;r.source=WIFI_STA_SOURCE_MGMT_REASSOC;break;
        case 1:case 3:r.mac=f->addr1;r.bssid=f->bssid;r.receiver=true;r.state=m->status_code==0?WIFI_STA_REL_ASSOCIATED_OBSERVED:WIFI_STA_REL_ASSOC_FAILED_OBSERVED;r.source=f->subtype==1?WIFI_STA_SOURCE_MGMT_ASSOC:WIFI_STA_SOURCE_MGMT_REASSOC;break;
        case 11:
            r.bssid=f->bssid;r.source=WIFI_STA_SOURCE_MGMT_AUTH;
            r.state=m->auth_sequence<=1?WIFI_STA_REL_AUTHENTICATING:
                (m->status_code==0?WIFI_STA_REL_AUTHENTICATED_OBSERVED:
                                   WIFI_STA_REL_AUTH_FAILED_OBSERVED);
            if(!same(f->addr2,f->bssid)){r.mac=f->addr2;r.transmitter=true;}else{r.mac=f->addr1;r.receiver=true;} break;
        case WIFI_FRAME_SUBTYPE_DISASSOC:case WIFI_FRAME_SUBTYPE_DEAUTH:
            r.bssid=f->bssid;r.state=WIFI_STA_REL_DISCONNECTED_OBSERVED;r.source=WIFI_STA_SOURCE_MGMT_DISCONNECT;
            if(!same(f->addr2,f->bssid)){r.mac=f->addr2;r.transmitter=true;}else{r.mac=f->addr1;r.receiver=true;} break;
        default:r.ambiguous=true;break;
    }
    return r;
}

void wifi_sta_tracker_core_init(wifi_sta_tracker_state_t *s){if(!s)return;memset(s,0,sizeof(*s));s->generation=1;s->diagnostics.table_capacity=WIFI_STA_TRACKER_CAPACITY;}
void wifi_sta_tracker_core_reset(wifi_sta_tracker_state_t *s){if(!s)return;uint32_t g=next_gen(s->generation);memset(s,0,sizeof(*s));s->generation=g;s->diagnostics.table_capacity=WIFI_STA_TRACKER_CAPACITY;}

bool wifi_sta_tracker_core_observe(wifi_sta_tracker_state_t *s,const wifi_sta_observation_t *o)
{
    if(!s||!o||!o->frame)return false;
    inc64(&s->diagnostics.observations_received);
    const wifi_frame_info_t *f=o->frame; const wifi_mgmt_info_t *m=o->management;
    if(o->timestamp_us<=0 ||
       (f->type==WIFI_FRAME_TYPE_MGMT && (!m || m->status!=WIFI_PARSE_VALID))){
        inc64(&s->diagnostics.invalid_mac_ignored);
        return false;
    }
    roles_t r=roles(f,m);
    if(r.ambiguous){inc64(&s->diagnostics.ambiguous_relation_count);return false;}
    if(!unicast(r.mac)){inc64(&s->diagnostics.invalid_mac_ignored);return false;}
    if(r.bssid&&!unicast(r.bssid))r.bssid=NULL;
    int found=find_mac(s,r.mac); bool evicted=false; unsigned idx;
    if(found<0){idx=slot_for(s,&evicted);if(evicted)inc64(&s->diagnostics.sta_evicted);else{s->diagnostics.table_count++;if(s->diagnostics.table_count>s->diagnostics.table_high_watermark)s->diagnostics.table_high_watermark=s->diagnostics.table_count;}memset(&s->slots[idx],0,sizeof(s->slots[idx]));s->slots[idx].occupied=true;memcpy(s->slots[idx].record.mac,r.mac,6);s->slots[idx].record.locally_administered=(r.mac[0]&2U)!=0; s->slots[idx].record.first_seen_us=o->timestamp_us;inc64(&s->diagnostics.sta_created);}
    else{idx=(unsigned)found;inc64(&s->diagnostics.existing_sta_observations);}
    wifi_tracked_sta_t *x=&s->slots[idx].record;
    if(o->timestamp_us>x->last_seen_us)x->last_seen_us=o->timestamp_us;
    x->rx_channel=f->channel; x->rssi_last=f->rssi;
    if(!x->rssi_sample_count||f->rssi<x->rssi_min)x->rssi_min=f->rssi;
    if(!x->rssi_sample_count||f->rssi>x->rssi_max)x->rssi_max=f->rssi;
    if(x->rssi_sample_count!=UINT32_MAX){x->rssi_sum+=f->rssi;x->rssi_sample_count++;}
    inc64(&x->frame_count);if(f->type==WIFI_FRAME_TYPE_MGMT)inc64(&x->management_count);else if(f->type==WIFI_FRAME_TYPE_DATA)inc64(&x->data_count);else inc64(&x->control_count);
    if(r.transmitter)inc64(&x->tx_observed_count);
    if(r.receiver)inc64(&x->rx_observed_count);
    if(f->is_eapol)inc64(&x->eapol_observed_count);
    if(r.state!=WIFI_STA_REL_UNKNOWN){
        x->relationship_state=r.state;
        x->relationship_source=r.source;
        /* A relation-less Probe Request must not erase a previously observed
         * infrastructure BSSID. Reset/eviction owns relation destruction. */
        if(r.bssid){x->has_related_bssid=true;memcpy(x->related_bssid,r.bssid,6);}
        inc64(&s->diagnostics.relationship_observations);
    }
    s->generation=next_gen(s->generation);x->update_generation=s->generation;return true;
}

void wifi_sta_tracker_core_snapshot(const wifi_sta_tracker_state_t *s,wifi_sta_tracker_snapshot_t *out){if(!s||!out)return;memset(out,0,sizeof(*out));out->generation=s->generation;out->diagnostics=s->diagnostics;for(unsigned i=0;i<WIFI_STA_TRACKER_CAPACITY;i++)if(s->slots[i].occupied)out->records[out->count++]=s->slots[i].record;}
int8_t wifi_sta_record_average_rssi(const wifi_tracked_sta_t *r){return !r||!r->rssi_sample_count?0:(int8_t)(r->rssi_sum/(int64_t)r->rssi_sample_count);}
