/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_profile_manager.h"
#include <string.h>

void wpm_secure_zero(void *pointer, size_t length)
{
    volatile uint8_t *p = pointer;
    while (length--) *p++ = 0;
}
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void put32(uint8_t *p, uint32_t x)
{ p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); p[2]=(uint8_t)(x>>16); p[3]=(uint8_t)(x>>24); }
static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    size_t i;
    for (i = 0U; i < length; i++)
        if (bytes[i] != 0U) return false;
    return true;
}
void wpm_config_default(wpm_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->auto_connect = true; cfg->next_id = 1;
}
static bool password_valid(wpm_security_t security, const uint8_t *p, size_t n)
{
    if (security == WPM_SECURITY_OPEN) return n == 0;
    if (security != WPM_SECURITY_WPA2 && security != WPM_SECURITY_WPA_MIXED) return false;
    if (n < 8 || n > WPM_PASSWORD_BYTES_MAX) return false;
    for (size_t i=0;i<n;i++) if (p[i]<32 || p[i]>126) return false;
    return true;
}
static bool same_network(const wpm_profile_t *a, const wpm_profile_t *b)
{
    return a->security == b->security && a->ssid_length == b->ssid_length &&
        !memcmp(a->ssid, b->ssid, a->ssid_length);
}
static bool same_profile_key(const wpm_profile_t *a, const wpm_profile_t *b)
{
    return same_network(a,b) && a->password_length == b->password_length &&
        !memcmp(a->password,b->password,a->password_length);
}
bool wpm_config_valid(const wpm_config_t *cfg)
{
    if (!cfg || cfg->count > WPM_PROFILE_MAX || !cfg->next_id) return false;
    bool last_found = cfg->last_success_id == 0;
    for (unsigned i=0;i<cfg->count;i++) {
        const wpm_profile_t *p = &cfg->profiles[i];
        if (!p->id || p->id >= cfg->next_id || !p->ssid_length ||
            p->ssid_length > WPM_SSID_BYTES_MAX ||
            !password_valid(p->security, p->password, p->password_length) ||
            p->last_success > cfg->success_counter) return false;
        if (p->id == cfg->last_success_id) last_found = true;
        for (unsigned j=0;j<i;j++)
            if (p->id == cfg->profiles[j].id || same_network(p, &cfg->profiles[j])) return false;
    }
    return last_found;
}
int wpm_config_encode(const wpm_config_t *cfg, uint8_t *out, size_t cap, size_t *length)
{
    if (!wpm_config_valid(cfg) || !out || !length) return WPM_EINVAL;
    size_t n = 24 + 108U * cfg->count;
    if (cap < n) return WPM_ECAPACITY;
    memset(out, 0, n); memcpy(out, "WN01", 4);
    out[4] = WPM_CONFIG_FORMAT_VERSION; out[6] = cfg->count;
    out[7] = cfg->auto_connect ? 1 : 0;
    put32(out+8,cfg->next_id); put32(out+12,cfg->success_counter);
    put32(out+16,cfg->last_success_id);
    for (unsigned i=0;i<cfg->count;i++) {
        const wpm_profile_t *p=&cfg->profiles[i]; uint8_t *r=out+24+108*i;
        put32(r,p->id); put32(r+4,p->last_success);
        r[8]=(uint8_t)p->security; r[9]=p->auto_connect?1:0;
        r[10]=p->ssid_length; r[11]=p->password_length;
        memcpy(r+12,p->ssid,p->ssid_length); memcpy(r+44,p->password,p->password_length);
    }
    *length=n; return 0;
}
int wpm_config_decode(wpm_config_t *cfg, const uint8_t *in, size_t length)
{
    if (!cfg) return WPM_EINVAL;
    wpm_secure_zero(cfg, sizeof(*cfg));
    wpm_config_default(cfg);
    if (!in || length < 24U || memcmp(in,"WN01",4) ||
        in[4] != WPM_CONFIG_FORMAT_VERSION || in[5] != 0U ||
        in[6] > WPM_PROFILE_MAX || in[7] > 1U ||
        !bytes_are_zero(in + 20U, 4U) || length != 24U + 108U * in[6])
        return WPM_EINVAL;
    wpm_config_t decoded; wpm_config_default(&decoded);
    decoded.count=in[6]; decoded.auto_connect=in[7]!=0;
    decoded.next_id=get32(in+8); decoded.success_counter=get32(in+12);
    decoded.last_success_id=get32(in+16);
    for (unsigned i=0;i<decoded.count;i++) {
        const uint8_t *r=in+24+108*i; wpm_profile_t *p=&decoded.profiles[i];
        p->id=get32(r); p->last_success=get32(r+4);
        if (r[8]>WPM_SECURITY_WPA_MIXED || r[9]>1 ||
            !r[10] || r[10]>WPM_SSID_BYTES_MAX || r[11]>WPM_PASSWORD_BYTES_MAX) {
            wpm_secure_zero(&decoded,sizeof(decoded)); return WPM_EINVAL;
        }
        if (!bytes_are_zero(r + 12U + r[10], WPM_SSID_BYTES_MAX - r[10]) ||
            !bytes_are_zero(r + 44U + r[11], WPM_PASSWORD_BYTES_MAX - r[11]) ||
            r[107] != 0U) {
            wpm_secure_zero(&decoded,sizeof(decoded)); return WPM_EINVAL;
        }
        p->security=(wpm_security_t)r[8]; p->auto_connect=r[9]!=0;
        p->ssid_length=r[10]; p->password_length=r[11];
        memcpy(p->ssid,r+12,p->ssid_length); memcpy(p->password,r+44,p->password_length);
    }
    if (!wpm_config_valid(&decoded)) {
        wpm_secure_zero(&decoded,sizeof(decoded)); return WPM_EINVAL;
    }
    *cfg=decoded; wpm_secure_zero(&decoded,sizeof(decoded));return 0;
}
static int profile_index(const wpm_config_t *cfg, uint32_t id)
{
    for(unsigned i=0;i<cfg->count;i++) if(cfg->profiles[i].id==id) return (int)i;
    return -1;
}
static int target_index(const wpm_config_t *cfg, const wpm_profile_t *target)
{
    for(unsigned i=0;i<cfg->count;i++) if(same_network(&cfg->profiles[i],target)) return (int)i;
    return -1;
}
static bool already_connected_saved(const wpm_core_t *c, uint32_t id)
{
    if(c->phase!=WPM_PHASE_ONLINE || !c->target_valid || !c->remember || c->temporary ||
       !c->join_ready || c->radio_busy || c->join_pending || c->leave_pending ||
       c->store_pending || c->store_busy || c->scrub_pending || c->cleanup_pending ||
       c->config_revision!=c->durable_revision || !c->ip[0] || c->target.id!=id) return false;
    int working=profile_index(&c->config,id);
    int durable=profile_index(&c->durable_config,id);
    return working>=0 && durable>=0 &&
        same_profile_key(&c->target,&c->config.profiles[working]) &&
        same_profile_key(&c->target,&c->durable_config.profiles[durable]);
}
static uint32_t next_operation(wpm_core_t *c)
{
    uint32_t value = c->allocate_operation ? c->allocate_operation(c->operation_context) : c->operation_id+1;
    c->operation_id=value?value:1; return c->operation_id;
}
static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now-deadline)>=0; }
static void changed(wpm_core_t *c, bool scrub)
{
    c->config_revision++;
    if (!c->config_revision) c->config_revision++;
    c->store_pending=true; c->scrub_pending |= scrub;
    if (c->storage!=WPM_STORAGE_FOREIGN && c->storage!=WPM_STORAGE_LOADING)
        c->storage=WPM_STORAGE_SAVING;
}
void wpm_core_init(wpm_core_t *c)
{
    memset(c,0,sizeof(*c)); wpm_config_default(&c->config);
    c->durable_config=c->config; c->phase=WPM_PHASE_INITIALIZING;
    c->storage=WPM_STORAGE_LOADING;
}
void wpm_core_set_operation_allocator(wpm_core_t *c,
    wpm_operation_id_allocator_t allocator, void *context)
{
    if (!c) return;
    c->allocate_operation = allocator;
    c->operation_context = context;
}
void wpm_core_load(wpm_core_t *c, const wpm_config_t *cfg, wpm_storage_status_t status)
{
    bool valid_config = cfg && wpm_config_valid(cfg);
    if (valid_config) c->config=*cfg;
    else {
        wpm_config_default(&c->config);
        if (cfg || status == WPM_STORAGE_OK || status == WPM_STORAGE_SAVING)
            status = WPM_STORAGE_CORRUPT;
    }
    c->durable_config=c->config; c->config_revision=c->durable_revision=1;
    c->storage=status; c->initialized=true; c->phase=WPM_PHASE_IDLE;
    c->reason = status == WPM_STORAGE_CORRUPT ? WPM_REASON_CORRUPT :
        status == WPM_STORAGE_FOREIGN ? WPM_REASON_FOREIGN :
        status == WPM_STORAGE_FAILED ? WPM_REASON_STORAGE : WPM_REASON_NONE;
}
static void begin_target(wpm_core_t *c, const wpm_profile_t *target, bool remember,
    bool automatic, uint32_t now)
{
    (void)now;
    bool occupied=c->target_valid || c->radio_busy || c->join_ready;
    next_operation(c); c->target=*target; c->target_valid=true;
    c->remember=remember; c->temporary=!remember; c->from_auto=automatic;
    c->join_pending=true; c->leave_pending |= occupied;
    c->join_ready=false; c->dhcp_retry=0; c->dhcp_reset_requested=false;
    /* A retry is not recovery; wpm_core_ip_ready clears the failure reason. */
    /* A previous user cancellation is not a failed attempt at this target. */
    if(c->reason==WPM_REASON_CANCELLED)c->reason=WPM_REASON_NONE;
    c->ip[0]=0;
    c->phase=occupied?WPM_PHASE_CANCELLING:WPM_PHASE_ASSOCIATING;
    c->leave_destination=WPM_PHASE_ASSOCIATING;
}
int wpm_core_connect(wpm_core_t *c,const uint8_t *ssid,size_t n,
    wpm_security_t security,const char *password,bool remember,uint32_t now)
{
    if(!c->initialized) return WPM_ENOTINITIALIZED;
    size_t key_length=password?strlen(password):0;
    if(!ssid || !n || n>WPM_SSID_BYTES_MAX || key_length>WPM_PASSWORD_BYTES_MAX ||
       !password_valid(security,(const uint8_t*)password,key_length)) return WPM_EINVAL;
    wpm_profile_t target; memset(&target,0,sizeof(target));
    target.ssid_length=(uint8_t)n; memcpy(target.ssid,ssid,n);
    target.security=security; target.password_length=(uint8_t)key_length;
    if(key_length)memcpy(target.password,password,key_length);
    target.auto_connect=true;
    int index=target_index(&c->config,&target);
    if(remember && index<0 && c->config.count>=WPM_PROFILE_MAX) {wpm_secure_zero(&target,sizeof(target));return WPM_EFULL;}
    if(index>=0) {target.id=c->config.profiles[index].id;target.auto_connect=c->config.profiles[index].auto_connect;}
    else if(remember) {
        if(c->config.next_id==UINT32_MAX){wpm_secure_zero(&target,sizeof(target));return WPM_EFULL;}
        target.id=c->config.next_id++;
    }
    c->auto_suspended=false; c->candidate_count=c->candidate_next=0;
    begin_target(c,&target,remember,false,now);
    wpm_secure_zero(&target,sizeof(target));return 0;
}
int wpm_core_connect_saved(wpm_core_t *c,uint32_t id,uint32_t now)
{
    int i=profile_index(&c->config,id);if(i<0)return WPM_ENOENT;
    if(already_connected_saved(c,id))return WPM_CORE_ALREADY_CONNECTED;
    wpm_profile_t target=c->config.profiles[i];
    for(unsigned b=0;b<WPM_PROFILE_MAX;b++)if(c->blocked_ids[b]==id)c->blocked_ids[b]=0;
    c->auto_suspended=false;c->candidate_count=c->candidate_next=0;
    begin_target(c,&target,true,false,now);wpm_secure_zero(&target,sizeof(target));return 0;
}
int wpm_core_update_password(wpm_core_t*c,uint32_t id,const char*password,uint32_t now)
{
    int i=profile_index(&c->config,id);if(i<0)return WPM_ENOENT;
    wpm_profile_t p=c->config.profiles[i];
    int result=wpm_core_connect(c,p.ssid,p.ssid_length,p.security,password,true,now);
    wpm_secure_zero(&p,sizeof(p));return result;
}
void wpm_core_disconnect(wpm_core_t *c)
{
    next_operation(c);c->auto_suspended=true;c->join_pending=false;
    c->leave_pending=true;c->join_ready=false;c->target_valid=false;
    c->phase=WPM_PHASE_CANCELLING;c->reason=WPM_REASON_NONE;c->ip[0]=0;
    c->leave_destination=WPM_PHASE_DISCONNECTED;
    wpm_secure_zero(&c->target,sizeof(c->target));
}
void wpm_core_cancel(wpm_core_t*c){wpm_core_disconnect(c);}
int wpm_core_forget(wpm_core_t*c,uint32_t id)
{
    int i=profile_index(&c->config,id);
    /* A pending remembered profile is cancellable even before DHCP persists it. */
    if (i < 0) {
        if (c->target_valid && c->remember && c->target.id == id) {
            wpm_core_disconnect(c);
            return 0;
        }
        return WPM_ENOENT;
    }
    if(c->target_valid && c->target.id==id)wpm_core_disconnect(c);
    wpm_secure_zero(&c->config.profiles[i],sizeof(wpm_profile_t));
    for(unsigned j=(unsigned)i;j+1<c->config.count;j++)c->config.profiles[j]=c->config.profiles[j+1];
    c->config.count--;wpm_secure_zero(&c->config.profiles[c->config.count],sizeof(wpm_profile_t));
    if(c->config.last_success_id==id)c->config.last_success_id=0;
    changed(c,true);return 0;
}
void wpm_core_forget_all(wpm_core_t*c)
{
    wpm_core_disconnect(c);
    uint32_t next=c->config.next_id;
    wpm_secure_zero(&c->config,sizeof(c->config));wpm_config_default(&c->config);
    c->config.next_id=next;changed(c,true);
}
void wpm_core_set_auto(wpm_core_t*c,bool value)
{
    if(c->config.auto_connect==value)return;
    c->config.auto_connect=value;changed(c,false);
    if(!value && c->from_auto && c->phase!=WPM_PHASE_ONLINE)wpm_core_disconnect(c);
    if(value)c->auto_suspended=false;
}
int wpm_core_set_profile_auto(wpm_core_t*c,uint32_t id,bool value)
{
    int i=profile_index(&c->config,id);if(i<0)return WPM_ENOENT;
    if(c->config.profiles[i].auto_connect!=value) {
        c->config.profiles[i].auto_connect=value;changed(c,false);
    }
    if(c->target_valid && c->target.id==id) {
        c->target.auto_connect=value;
        if(!value && c->from_auto && c->phase!=WPM_PHASE_ONLINE)wpm_core_disconnect(c);
    }
    return 0;
}
int wpm_core_retry_save(wpm_core_t*c)
{
    if(c->storage==WPM_STORAGE_FOREIGN)return WPM_EFOREIGN;
    if(c->config_revision==c->durable_revision&&!c->cleanup_pending)return 0;
    c->store_pending=true;c->storage=WPM_STORAGE_SAVING;return 0;
}
int wpm_core_use_temporary(wpm_core_t*c)
{
    if(c->phase!=WPM_PHASE_ONLINE||!c->target_valid)return WPM_ESTATE;
    if(c->store_busy||c->radio_busy||c->cleanup_pending)return WPM_EBUSY;
    if(c->temporary)return 0;
    int current=profile_index(&c->config,c->target.id);
    int durable=profile_index(&c->durable_config,c->target.id);
    bool modified=false;
    if(current>=0&&durable<0) {
        unsigned index=(unsigned)current;
        for(unsigned j=index;j+1<c->config.count;j++)c->config.profiles[j]=c->config.profiles[j+1];
        c->config.count--;wpm_secure_zero(&c->config.profiles[c->config.count],sizeof(wpm_profile_t));
        if(c->config.last_success_id==c->target.id) {
            c->config.last_success_id=0;
            uint32_t recent=0;
            for(unsigned j=0;j<c->config.count;j++)if(c->config.profiles[j].last_success>recent) {
                recent=c->config.profiles[j].last_success;c->config.last_success_id=c->config.profiles[j].id;
            }
        }
        modified=true;
    } else if(current>=0&&durable>=0) {
        wpm_profile_t *working=&c->config.profiles[current];
        const wpm_profile_t *stored=&c->durable_config.profiles[durable];
        if(working->password_length!=stored->password_length||
            memcmp(working->password,stored->password,stored->password_length)) {
            wpm_secure_zero(working->password,sizeof(working->password));
            memcpy(working->password,stored->password,stored->password_length);
            working->password_length=stored->password_length;modified=true;
        }
    }
    if(modified)changed(c,true);
    c->remember=false;c->temporary=true;c->auto_suspended=true;
    wpm_secure_zero(c->target.password,sizeof(c->target.password));c->target.password_length=0;
    /* The live association/IP are retained. A pending metadata/deletion write
     * is still reported separately and may fail; this is not a save success. */
    return 0;
}
static bool blocked(const wpm_core_t*c,uint32_t id)
{ for(unsigned i=0;i<WPM_PROFILE_MAX;i++)if(c->blocked_ids[i]==id)return true;return false; }
static void block_profile(wpm_core_t*c,uint32_t id)
{
    if(!id||blocked(c,id))return;
    for(unsigned i=0;i<WPM_PROFILE_MAX;i++)if(!c->blocked_ids[i]){c->blocked_ids[i]=id;return;}
}
static void build_candidates(wpm_core_t*c)
{
    c->candidate_count=c->candidate_next=c->attempt=0;
    for(unsigned i=0;i<c->config.count;i++) {
        const wpm_profile_t*p=&c->config.profiles[i];
        if(!p->auto_connect||blocked(c,p->id))continue;
        unsigned at=c->candidate_count++;
        while(at>0) {
            int prev=profile_index(&c->config,c->candidates[at-1]);
            bool better=p->id==c->config.last_success_id ||
                (c->candidates[at-1]!=c->config.last_success_id && prev>=0 &&
                 p->last_success>c->config.profiles[prev].last_success);
            if(!better)break;
            c->candidates[at]=c->candidates[at-1];at--;
        }
        c->candidates[at]=p->id;
    }
}
static void try_next(wpm_core_t*c,uint32_t now)
{
    while(c->candidate_next<c->candidate_count) {
        uint32_t id=c->candidates[c->candidate_next++];
        int i=profile_index(&c->config,id);
        if(i<0 || !c->config.profiles[i].auto_connect || blocked(c,id))continue;
        c->attempt++;begin_target(c,&c->config.profiles[i],true,true,now);return;
    }
    c->phase=WPM_PHASE_RETRY_WAIT;c->retry_due=now+30000U;
    c->target_valid=false;c->join_pending=false;
    wpm_secure_zero(&c->target,sizeof(c->target));
}
static void failed(wpm_core_t*c,wpm_reason_t reason,uint32_t now)
{
    c->reason=reason;c->join_ready=false;c->ip[0]=0;
    if(reason==WPM_REASON_AUTH||reason==WPM_REASON_UNSUPPORTED)block_profile(c,c->target.id);
    if(c->from_auto && c->config.auto_connect && !c->auto_suspended) {
        c->leave_pending=true;try_next(c,now);
        c->leave_destination=c->join_pending?WPM_PHASE_ASSOCIATING:WPM_PHASE_RETRY_WAIT;
    } else {
        c->auto_suspended=true;c->phase=WPM_PHASE_FAILED;
        c->leave_pending=true;c->join_pending=false;
        c->leave_destination=WPM_PHASE_FAILED;
    }
}
void wpm_core_tick(wpm_core_t*c,uint32_t now)
{
    if(!c->initialized)return;
    if(c->phase==WPM_PHASE_DHCP && due(now,c->dhcp_due)) {
        if(!c->dhcp_retry++) {c->dhcp_reset_requested=true;c->dhcp_due=now+8000U;}
        else failed(c,WPM_REASON_DHCP,now);
    }
    bool start=c->phase==WPM_PHASE_IDLE||c->phase==WPM_PHASE_DISCONNECTED;
    bool repeat=c->phase==WPM_PHASE_RETRY_WAIT&&due(now,c->retry_due);
    if((start||repeat)&&c->config.auto_connect&&!c->auto_suspended&&!c->radio_busy) {
        build_candidates(c);
        if(c->candidate_count)try_next(c,now);
        else if(repeat){c->retry_due=now+30000U;}
    }
}
bool wpm_core_next_job(wpm_core_t*c,wpm_core_job_t*out)
{
    memset(out,0,sizeof(*out));
    if(!c->radio_busy && c->leave_pending) {
        out->kind=WPM_CORE_JOB_LEAVE;out->operation_id=c->operation_id;
        c->leave_pending=false;c->radio_busy=true;
        c->radio_job_kind=WPM_CORE_JOB_LEAVE;return true;
    }
    if(!c->radio_busy && c->join_pending) {
        out->kind=WPM_CORE_JOB_JOIN;out->operation_id=c->operation_id;out->target=c->target;
        c->join_pending=false;c->radio_busy=true;c->radio_job_kind=WPM_CORE_JOB_JOIN;
        c->phase=WPM_PHASE_ASSOCIATING;return true;
    }
    if(c->store_pending&&!c->store_busy) {
        out->kind=WPM_CORE_JOB_STORE;out->operation_id=c->operation_id;
        out->config_revision=c->config_revision;out->config=c->config;
        out->scrub_old=c->scrub_pending;c->inflight_config=c->config;
        c->store_revision=c->config_revision;c->scrub_pending=false;
        c->store_pending=false;c->store_busy=true;return true;
    }
    return false;
}
void wpm_core_radio_result(wpm_core_t*c,wpm_core_job_kind_t kind,uint32_t op,
    wpm_reason_t result,uint32_t now)
{
    if (!c || (kind != WPM_CORE_JOB_JOIN && kind != WPM_CORE_JOB_LEAVE) ||
        !c->radio_busy || c->radio_job_kind != kind)
        return;
    c->radio_busy=false;c->radio_job_kind=WPM_CORE_JOB_NONE;
    if ((int)result < (int)WPM_REASON_NONE ||
        (int)result > (int)WPM_REASON_AUTH_INCOMPLETE)
        result=WPM_REASON_IO;
    if(op!=c->operation_id) {
        c->leave_pending=true;return;
    }
    if(kind==WPM_CORE_JOB_LEAVE) {
        if(result!=WPM_REASON_NONE) {
            c->reason=result;c->join_ready=false;c->ip[0]=0;
            c->leave_pending=false;c->join_pending=false;
            if(c->from_auto && c->config.auto_connect && !c->auto_suspended) {
                c->phase=WPM_PHASE_RETRY_WAIT;c->retry_due=now+30000U;
            } else c->phase=WPM_PHASE_FAILED;
            return;
        }
        c->phase=c->join_pending?WPM_PHASE_ASSOCIATING:c->leave_destination;
        if(c->phase==WPM_PHASE_DISCONNECTED)c->reason=WPM_REASON_CANCELLED;
        return;
    }
    if(result!=WPM_REASON_NONE){failed(c,result,now);return;}
    c->join_ready=true;c->phase=WPM_PHASE_DHCP;c->dhcp_due=now+8000U;c->dhcp_retry=0;
}
void wpm_core_ip_ready(wpm_core_t*c,uint32_t op,const char*ip,uint32_t now)
{
    (void)now;
    if(op!=c->operation_id||c->phase!=WPM_PHASE_DHCP||!c->join_ready||
       !c->target_valid||!ip||!ip[0])return;
    size_t n=strlen(ip);if(n>=sizeof(c->ip))return;
    memcpy(c->ip,ip,n+1);c->phase=WPM_PHASE_ONLINE;c->reason=WPM_REASON_NONE;
    if(c->remember) {
        int i=target_index(&c->config,&c->target);
        if(i<0) {
            if(c->config.count>=WPM_PROFILE_MAX){c->storage=WPM_STORAGE_FAILED;return;}
            i=c->config.count++;c->config.profiles[i]=c->target;
        }
        bool credentials_changed=c->config.profiles[i].password_length!=c->target.password_length ||
            memcmp(c->config.profiles[i].password,c->target.password,c->target.password_length)!=0;
        bool new_profile=c->config.profiles[i].last_success==0;
        bool order_changed=c->config.last_success_id!=c->target.id;
        if(credentials_changed||new_profile||order_changed) {
            c->target.last_success=++c->config.success_counter;
            c->config.profiles[i]=c->target;c->config.last_success_id=c->target.id;
            changed(c,credentials_changed);
        }
    } else wpm_secure_zero(c->target.password,sizeof(c->target.password));
}
void wpm_core_link_lost(wpm_core_t*c,uint32_t op,uint32_t now)
{
    if(op!=c->operation_id)return;
    if(c->phase==WPM_PHASE_DHCP&&c->join_ready) {
        failed(c,WPM_REASON_IO,now);
        return;
    }
    if(c->phase!=WPM_PHASE_ONLINE)return;
    c->join_ready=false;c->phase=WPM_PHASE_DISCONNECTED;c->reason=WPM_REASON_IO;
    c->ip[0]=0;c->target_valid=false;
    if(c->temporary)c->auto_suspended=true;
    wpm_core_tick(c,now);
}
void wpm_core_store_result(wpm_core_t*c,uint32_t revision,wpm_store_outcome_t outcome)
{
    if(!c->store_busy || revision!=c->store_revision)return;
    c->store_busy=false;
    if (outcome != WPM_STORE_FAILED && outcome != WPM_STORE_CLEAN &&
        outcome != WPM_STORE_COMMITTED_DIRTY && outcome != WPM_STORE_FOREIGN)
        outcome = WPM_STORE_FAILED;
    if(outcome==WPM_STORE_CLEAN || outcome==WPM_STORE_COMMITTED_DIRTY) {
        c->durable_config=c->inflight_config;c->durable_revision=revision;
        c->cleanup_pending=outcome==WPM_STORE_COMMITTED_DIRTY;
        if(c->cleanup_pending) {
            c->storage=WPM_STORAGE_FAILED;c->reason=WPM_REASON_STORAGE;
            c->scrub_pending=true;c->store_pending=false;
        } else if(c->config_revision==revision) {
            c->storage=WPM_STORAGE_OK;
            if (c->reason == WPM_REASON_STORAGE || c->reason == WPM_REASON_FOREIGN)
                c->reason = WPM_REASON_NONE;
        } else {c->store_pending=true;c->storage=WPM_STORAGE_SAVING;}
    } else if (outcome == WPM_STORE_FOREIGN) {
        c->storage=WPM_STORAGE_FOREIGN;c->reason=WPM_REASON_FOREIGN;
        c->store_pending=false;c->scrub_pending=false;
    } else {
        c->storage=WPM_STORAGE_FAILED;c->reason=WPM_REASON_STORAGE;
        c->scrub_pending=true;
        /* Keep explicit unsaved state for retry; never report deletion/saving complete. */
        c->store_pending=false;
    }
    wpm_secure_zero(&c->inflight_config,sizeof(c->inflight_config));
}
void wpm_core_adapter_snapshot(const wpm_core_t *c, wpm_core_adapter_state_t *s)
{
    int durable_index;
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (!c) return;
    s->phase=c->phase;s->reason=c->reason;s->storage=c->storage;
    s->operation_id=c->operation_id;s->config_revision=c->config_revision;
    s->durable_revision=c->durable_revision;s->store_revision=c->store_revision;
    s->radio_pending=c->join_pending||c->leave_pending;
    s->join_pending=c->join_pending;s->leave_pending=c->leave_pending;
    s->radio_busy=c->radio_busy;s->link_ready=c->join_ready;
    s->store_pending=c->store_pending;s->store_busy=c->store_busy;
    s->cleanup_pending=c->cleanup_pending;s->auto_suspended=c->auto_suspended;
    s->dhcp_reset_pending=c->dhcp_reset_requested;
    durable_index = c->target_valid && c->remember ?
        profile_index(&c->durable_config, c->target.id) : -1;
    s->active_target_is_durable = durable_index >= 0 &&
        same_profile_key(&c->target, &c->durable_config.profiles[durable_index]);
}
void wpm_core_report_error(wpm_core_t *c, wpm_reason_t reason)
{
    if (!c) return;
    if (reason == WPM_REASON_NONE || (int)reason < (int)WPM_REASON_NONE ||
        (int)reason > (int)WPM_REASON_AUTH_INCOMPLETE)
        reason = WPM_REASON_INVALID;
    c->reason=reason;
    if (c->phase==WPM_PHASE_IDLE || c->phase==WPM_PHASE_DISCONNECTED ||
        c->phase==WPM_PHASE_FAILED)
        c->phase=WPM_PHASE_FAILED;
}
void wpm_core_mark_cleanup_pending(wpm_core_t *c)
{
    if (!c) return;
    c->cleanup_pending=true;c->scrub_pending=true;
    c->storage=WPM_STORAGE_FAILED;c->reason=WPM_REASON_STORAGE;
}
void wpm_core_suspend_automatic(wpm_core_t *c)
{
    if (c) c->auto_suspended=true;
}
bool wpm_core_take_dhcp_reset_request(wpm_core_t *c)
{
    bool requested;
    if (!c) return false;
    requested=c->dhcp_reset_requested;c->dhcp_reset_requested=false;
    return requested;
}
void wpm_core_snapshot(const wpm_core_t*c,uint32_t now,wpm_public_snapshot_t*s)
{
    memset(s,0,sizeof(*s));s->phase=c->phase;s->reason=c->reason;s->storage=c->storage;
    s->operation_id=c->operation_id;s->config_revision=c->config_revision;
    s->auto_connect=c->config.auto_connect;s->auto_suspended=c->auto_suspended;
    s->temporary=c->temporary;s->initialized=c->initialized;
    s->attempt=c->attempt;s->candidate_count=c->candidate_count;s->profile_count=c->config.count;
    s->saved_profile_count=c->durable_config.count;
    s->pending_changes=c->config_revision!=c->durable_revision||c->cleanup_pending;
    s->cleanup_pending=c->cleanup_pending;
    if(c->phase==WPM_PHASE_RETRY_WAIT&&!due(now,c->retry_due))s->retry_in_ms=c->retry_due-now;
    if(c->target_valid) {
        s->ssid_length=c->target.ssid_length;
        memcpy(s->ssid,c->target.ssid,c->target.ssid_length);
        if(c->remember)s->active_profile_id=c->target.id;
    }
    memcpy(s->ip,c->ip,sizeof(s->ip));
    for(unsigned i=0;i<c->config.count;i++) {
        const wpm_profile_t*p=&c->config.profiles[i];wpm_profile_summary_t*r=&s->profiles[i];
        r->id=p->id;r->last_success=p->last_success;r->security=p->security;r->auto_connect=p->auto_connect;
        r->persisted=profile_index(&c->durable_config,p->id)>=0;
        r->ssid_length=p->ssid_length;memcpy(r->ssid,p->ssid,p->ssid_length);
    }
}
const char*wpm_phase_name(wpm_phase_t p)
{
    static const char*const names[]={"initializing","idle","associating","dhcp",
        "online","retry_wait","cancelling","disconnected","failed"};
    return (unsigned)p<sizeof(names)/sizeof(names[0])?names[p]:"unknown";
}
const char*wpm_reason_name(wpm_reason_t p)
{
    static const char*const names[]={"none","network_not_found","authentication_failed",
        "unsupported_security","dhcp_timeout","storage_error","driver_error","cancelled",
        "profiles_full","busy","config_corrupt","foreign_config","invalid_input",
        "authentication_incomplete"};
    return (unsigned)p<sizeof(names)/sizeof(names[0])?names[p]:"unknown";
}
const char*wpm_storage_name(wpm_storage_status_t p)
{
    static const char*const names[]={"loading","empty","saved","saving","save_failed","corrupt","foreign"};
    return (unsigned)p<sizeof(names)/sizeof(names[0])?names[p]:"unknown";
}
