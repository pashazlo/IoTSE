#include <assert.h>
#include <string.h>
#include "wifi_frame_parser.h"
#include "wifi_mgmt_parser.h"
#include "wifi_sta_tracker_internal.h"

static const uint8_t AP[6]={0x10,0,0,0,0,1}, STA[6]={0x22,0,0,0,0,2};
static wifi_sta_tracker_state_t state; static wifi_sta_tracker_snapshot_t snap;
static void hdr(uint8_t *p,uint8_t type,uint8_t sub,bool td,bool fd,const uint8_t*a1,const uint8_t*a2,const uint8_t*a3){memset(p,0,64);uint16_t fc=(uint16_t)(type<<2)|(uint16_t)(sub<<4)|(td?0x100:0)|(fd?0x200:0);p[0]=fc;p[1]=fc>>8;memcpy(p+4,a1,6);memcpy(p+10,a2,6);memcpy(p+16,a3,6);}
static bool chain(const uint8_t*p,uint16_t n,int64_t t){wifi_frame_info_t f; if(!wifi_frame_parse(p,n,-55,6,&f))return false;wifi_mgmt_info_t m,*mp=NULL;if(f.type==WIFI_FRAME_TYPE_MGMT){wifi_mgmt_parse(p,n,&f,&m);mp=&m;}wifi_sta_observation_t o={&f,mp,t};return wifi_sta_tracker_core_observe(&state,&o);}
static const wifi_tracked_sta_t *one(void){wifi_sta_tracker_core_snapshot(&state,&snap);assert(snap.count<=1);return snap.count?&snap.records[0]:NULL;}
static uint16_t mgmt(uint8_t*p,uint8_t sub,bool ap_tx,uint16_t status){hdr(p,0,sub,0,0,ap_tx?STA:AP,ap_tx?AP:STA,AP);uint16_t n=24;if(sub==4){p[n++]=0;p[n++]=0;}else if(sub==8||sub==5){n+=10;p[n++]=1;p[n++]=0;}else if(sub==11){p[n++]=0;p[n++]=0;p[n++]=ap_tx?2:1;p[n++]=0;p[n++]=status;p[n++]=status>>8;}else if(sub==0){n+=4;}else if(sub==2){n+=10;}else if(sub==1||sub==3){n+=2;p[n++]=status;p[n++]=status>>8;n+=2;}else if(sub==10||sub==12){p[n++]=1;p[n++]=0;}return n;}
static uint16_t data(uint8_t*p,bool td,bool fd,bool qos,bool multicast){uint8_t dst[6];memcpy(dst,STA,6);if(multicast)dst[0]|=1;if(td)hdr(p,2,qos?8:0,1,0,AP,STA,dst);else if(fd)hdr(p,2,qos?8:0,0,1,dst,AP,STA);else hdr(p,2,0,td,fd,dst,STA,AP);return qos?26:24;}
static void prefixes(const uint8_t*p,uint16_t n){for(uint16_t i=0;i<n;i++){wifi_sta_tracker_core_reset(&state);(void)chain(p,i,100+i);wifi_sta_tracker_core_snapshot(&state,&snap);assert(snap.count<=1);if(snap.count){assert((snap.records[0].mac[0]&1)==0);assert(snap.records[0].first_seen_us<=snap.records[0].last_seen_us);}}}
int main(void)
{
    uint8_t p[64]; uint16_t n;
    wifi_sta_tracker_core_init(&state);

    n=mgmt(p,4,false,0); assert(chain(p,n,1)); assert(one()->relationship_state==WIFI_STA_REL_PROBING); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,8,true,0); assert(!chain(p,n,2)&&!one()); prefixes(p,n);
    n=mgmt(p,5,true,0); assert(!chain(p,n,3)&&!one()); prefixes(p,n);

    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,11,false,0); assert(chain(p,n,4)); assert(one()->relationship_state==WIFI_STA_REL_AUTHENTICATING); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,11,true,0); assert(chain(p,n,5)); assert(one()->relationship_state==WIFI_STA_REL_AUTHENTICATED_OBSERVED); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,11,true,13); assert(chain(p,n,6)); assert(one()->relationship_state==WIFI_STA_REL_AUTH_FAILED_OBSERVED); prefixes(p,n);

    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,0,false,0); assert(chain(p,n,7)); assert(one()->relationship_state==WIFI_STA_REL_ASSOCIATING); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,1,true,0); assert(chain(p,n,8)); assert(one()->relationship_state==WIFI_STA_REL_ASSOCIATED_OBSERVED); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,1,true,17); assert(chain(p,n,9)); assert(one()->relationship_state==WIFI_STA_REL_ASSOC_FAILED_OBSERVED); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,2,false,0); assert(chain(p,n,10)); assert(one()->relationship_source==WIFI_STA_SOURCE_MGMT_REASSOC); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,3,true,0); assert(chain(p,n,11)); assert(one()->relationship_state==WIFI_STA_REL_ASSOCIATED_OBSERVED); prefixes(p,n);

    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,12,false,0); assert(chain(p,n,12)); assert(one()->relationship_state==WIFI_STA_REL_DISCONNECTED_OBSERVED); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,12,true,0); assert(chain(p,n,13)); assert(one()->relationship_state==WIFI_STA_REL_DISCONNECTED_OBSERVED); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,10,false,0); assert(chain(p,n,14)); assert(one()->relationship_state==WIFI_STA_REL_DISCONNECTED_OBSERVED); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=mgmt(p,10,true,0); assert(chain(p,n,15)); assert(one()->relationship_state==WIFI_STA_REL_DISCONNECTED_OBSERVED); prefixes(p,n);

    wifi_sta_tracker_core_reset(&state);
    n=data(p,true,false,false,false); assert(chain(p,n,16)); assert(one()->relationship_state==WIFI_STA_REL_DATA_RELATION_OBSERVED&&one()->relationship_source==WIFI_STA_SOURCE_DATA_TO_DS); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=data(p,false,true,false,false); assert(chain(p,n,17)); assert(one()->relationship_source==WIFI_STA_SOURCE_DATA_FROM_DS); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=data(p,true,false,true,false); assert(chain(p,n,18)); assert(one()->relationship_source==WIFI_STA_SOURCE_DATA_TO_DS); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=data(p,false,true,true,false); assert(chain(p,n,19)); assert(one()->relationship_source==WIFI_STA_SOURCE_DATA_FROM_DS); prefixes(p,n);

    wifi_sta_tracker_core_reset(&state);
    hdr(p,2,0,1,1,STA,AP,STA); n=30; assert(!chain(p,n,20)&&!one()); prefixes(p,n);
    wifi_sta_tracker_core_reset(&state);
    n=data(p,false,true,false,true); assert(!chain(p,n,21)&&!one()); prefixes(p,n);
    return 0;
}