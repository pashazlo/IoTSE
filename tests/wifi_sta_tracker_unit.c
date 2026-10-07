#include <assert.h>
#include <string.h>
#include "wifi_sta_tracker_internal.h"
static wifi_sta_tracker_state_t s; static wifi_sta_tracker_snapshot_t a,b;
static void mac(uint8_t *m,uint8_t id){uint8_t x[6]={0x02,0,0,0,0,id};memcpy(m,x,6);}
static bool obs(uint8_t id,int64_t t,int8_t r,uint8_t type,uint8_t sub,bool td,bool fd,uint8_t bid,bool eapol){
 wifi_frame_info_t f={.type=type,.subtype=sub,.rssi=r,.channel=6,.to_ds=td,.from_ds=fd,.has_bssid=true,.is_eapol=eapol};mac(f.bssid,bid);
 if(type==WIFI_FRAME_TYPE_DATA){if(td&&!fd){memcpy(f.addr1,f.bssid,6);mac(f.addr2,id);}else if(!td&&fd){mac(f.addr1,id);memcpy(f.addr2,f.bssid,6);}else mac(f.addr2,id);}
 else{mac(f.addr2,id);memcpy(f.addr3,f.bssid,6);if(sub==1||sub==3){mac(f.addr1,id);memcpy(f.addr2,f.bssid,6);}}
 wifi_mgmt_info_t m={.status=WIFI_PARSE_VALID,.subtype=sub,.rx_channel=6,.status_code=0};wifi_sta_observation_t o={&f,type==WIFI_FRAME_TYPE_MGMT?&m:NULL,t};return wifi_sta_tracker_core_observe(&s,&o);}
static const wifi_tracked_sta_t *get(const wifi_sta_tracker_snapshot_t *x,uint8_t id){uint8_t m[6];mac(m,id);for(uint16_t i=0;i<x->count;i++)if(!memcmp(x->records[i].mac,m,6))return &x->records[i];return NULL;}
int main(void){
 wifi_sta_tracker_core_init(&s);assert(obs(1,1,-70,WIFI_FRAME_TYPE_MGMT,WIFI_FRAME_SUBTYPE_PROBE_REQ,0,0,0,0));assert(obs(1,2,-50,WIFI_FRAME_TYPE_MGMT,WIFI_FRAME_SUBTYPE_PROBE_REQ,0,0,0,0));
 assert(obs(1,3,-60,WIFI_FRAME_TYPE_MGMT,0,0,0,9,0));assert(obs(2,4,-80,WIFI_FRAME_TYPE_MGMT,0,0,0,9,0));
 assert(obs(1,4,-58,WIFI_FRAME_TYPE_MGMT,0,0,0,10,0));
 assert(obs(1,5,-55,WIFI_FRAME_TYPE_DATA,0,1,0,10,1));assert(obs(2,6,-75,WIFI_FRAME_TYPE_DATA,0,0,1,9,0));assert(!obs(3,7,-40,WIFI_FRAME_TYPE_DATA,0,1,1,9,0));
 wifi_sta_tracker_core_snapshot(&s,&a);assert(a.count==2);const wifi_tracked_sta_t *x=get(&a,1);uint8_t b10[6];mac(b10,10);assert(x&&x->locally_administered&&x->has_related_bssid&&!memcmp(x->related_bssid,b10,6)&&x->eapol_observed_count==1);assert(x->rssi_min==-70&&x->rssi_max==-50&&wifi_sta_record_average_rssi(x)==-58);
 for(int i=3;i<=128;i++)
  assert(obs((uint8_t)i,(int64_t)i+10,-60,WIFI_FRAME_TYPE_MGMT,WIFI_FRAME_SUBTYPE_PROBE_REQ,0,0,0,0));
 wifi_sta_tracker_core_snapshot(&s,&a);assert(a.count==128);assert(obs(200,1000,-30,WIFI_FRAME_TYPE_MGMT,WIFI_FRAME_SUBTYPE_PROBE_REQ,0,0,0,0));wifi_sta_tracker_core_snapshot(&s,&b);assert(b.diagnostics.sta_evicted==1&&get(&a,1)&&!get(&b,1)&&get(&b,200));
 wifi_sta_tracker_core_reset(&s);wifi_sta_tracker_core_snapshot(&s,&b);assert(!b.count);
 wifi_frame_info_t bad={.type=WIFI_FRAME_TYPE_MGMT,.subtype=WIFI_FRAME_SUBTYPE_PROBE_REQ,.has_addr2=true};memset(bad.addr2,0xff,6);wifi_mgmt_info_t mm={.status=WIFI_PARSE_VALID,.subtype=WIFI_FRAME_SUBTYPE_PROBE_REQ};wifi_sta_observation_t oo={&bad,&mm,1};assert(!wifi_sta_tracker_core_observe(&s,&oo));memset(bad.addr2,0,6);assert(!wifi_sta_tracker_core_observe(&s,&oo));memset(bad.addr2,0,6);bad.addr2[0]=1;assert(!wifi_sta_tracker_core_observe(&s,&oo));mm.status=WIFI_PARSE_PARTIAL;mac(bad.addr2,1);assert(!wifi_sta_tracker_core_observe(&s,&oo));mm.status=WIFI_PARSE_VALID;bad.subtype=WIFI_FRAME_SUBTYPE_BEACON;mm.subtype=WIFI_FRAME_SUBTYPE_BEACON;assert(!wifi_sta_tracker_core_observe(&s,&oo));return 0;}
