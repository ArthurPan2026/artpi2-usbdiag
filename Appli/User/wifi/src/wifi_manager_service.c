#include "wifi_service.h"
#include "wifi_manager_backend.h"
#include "wifi_nv_flash.h"
#include "whd_stm32_sdio.h"
#include <finsh.h>
#include <string.h>
#include "startup_coordinator.h"
#include "watchdog_supervisor.h"
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
#include "usb_evidence.h"
#include "usb_perf.h"
#include "build_identity.h"
#endif

#define WIFI_CONTROL_DEPTH 8
#define WIFI_WORK_DEPTH 2
#define WIFI_RESULT_DEPTH 2
#define WIFI_VIEW_PUBLISH_INTERVAL_MS 5U
#define WIFI_SERVICE_RAM __attribute__((section(".wifi_web_ram"),aligned(32)))
typedef enum { CMD_SCAN, CMD_CONNECT, CMD_SAVED, CMD_PASSWORD, CMD_DISCONNECT,
    CMD_CANCEL, CMD_FORGET, CMD_CLEAR, CMD_AUTO, CMD_PROFILE_AUTO, CMD_RETRY_SAVE, CMD_TEMP } control_kind_t;
typedef struct {
    control_kind_t kind;
    uint32_t receipt, id;
    bool enabled, remember;
    uint8_t ssid_length;
    wifi_security_t security;
    uint8_t ssid[32];
    char password[64];
} control_t;
typedef enum { WORK_LOAD, WORK_INITIALIZE, WORK_RADIO, WORK_SCAN, WORK_STOP_SCAN } work_kind_t;
typedef struct { work_kind_t kind; uint32_t config_receipt; wifi_core_job_t radio; } work_t;
typedef struct {
    work_kind_t kind; wifi_core_job_kind_t radio_kind;
    uint32_t generation, config_revision, config_receipt;
    wifi_reason_t reason;
    int nv_status, radio_status;
    wifi_config_t config;
} result_t;
static struct rt_messagequeue s_control_queue,s_urgent_queue,s_work_queue,s_result_queue;
static rt_align(RT_ALIGN_SIZE) uint8_t s_control_pool[RT_MQ_BUF_SIZE(sizeof(control_t),WIFI_CONTROL_DEPTH)];
static rt_align(RT_ALIGN_SIZE) uint8_t s_urgent_pool[RT_MQ_BUF_SIZE(sizeof(control_t),4)];
static rt_align(RT_ALIGN_SIZE) uint8_t s_work_pool[RT_MQ_BUF_SIZE(sizeof(work_t),WIFI_WORK_DEPTH)] WIFI_SERVICE_RAM;
static rt_align(RT_ALIGN_SIZE) uint8_t s_result_pool[RT_MQ_BUF_SIZE(sizeof(result_t),WIFI_RESULT_DEPTH)] WIFI_SERVICE_RAM;
static struct rt_mutex s_view_lock;
static wifi_snapshot_t s_view WIFI_SERVICE_RAM;
static wifi_core_t s_core WIFI_SERVICE_RAM;
static bool s_ready,s_worker_busy,s_scan_start_pending,s_scan_stop_failed;
static bool s_config_loaded;
static bool s_view_publish_valid;
static uint32_t s_view_last_publish_ms;
static volatile bool s_radio_ready;
static volatile uint8_t s_radio_init_state = WIFI_RADIO_INIT_WAITING_DISPLAY;
static uint32_t s_radio_init_started_ms;
static volatile uint32_t s_next_receipt,s_radio_generation;
static uint32_t s_last_request,s_last_net_generation,s_link_generation,s_cancel_barrier;
static uint32_t s_connection_receipt,s_connection_generation,s_config_receipt;
static uint32_t s_saved_receipt,s_config_saved_receipt;
static uint32_t s_request_result_id;
static int s_request_result;
static uint32_t s_transport_drops;
static uint32_t s_watchdog_worker_progress;
static wpm_nv_journal_t s_nv_journal = WPM_NV_JOURNAL_INITIALIZER;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static volatile uint32_t s_scan_publish_copy_count;
static volatile uint32_t s_scan_publish_skip_count;
#endif

static wifi_core_adapter_state_t core_adapter_state(void)
{
    wifi_core_adapter_state_t state;
    wifi_core_adapter_snapshot(&s_core, &state);
    return state;
}

static uint32_t allocate_operation(void *unused)
{
    (void)unused;
    rt_base_t irq=rt_hw_interrupt_disable();
    uint32_t value=++s_next_receipt;
    if(!value)value=++s_next_receipt;
    rt_hw_interrupt_enable(irq);
    return value;
}
static bool join_cancelled(void *context)
{
    const uint32_t *generation=context;
    return s_radio_generation!=*generation;
}
static uint32_t milliseconds(void)
{ return (uint32_t)((uint64_t)rt_tick_get()*1000U/RT_TICK_PER_SECOND); }

/* Radio readiness and stage form one public status tuple shared by the owner
 * and worker. Keep every read/write in the same coarse critical section. */
static void radio_status_read(bool *ready, uint8_t *stage)
{
    rt_enter_critical();
    if(ready)*ready=s_radio_ready;
    if(stage)*stage=s_radio_init_state;
    rt_exit_critical();
}
static bool radio_is_ready(void)
{
    bool ready;
    radio_status_read(&ready,NULL);
    return ready;
}
static void radio_status_set(uint8_t stage, bool ready)
{
    rt_enter_critical();
    s_radio_ready=ready;
    s_radio_init_state=stage;
    if(stage!=WIFI_RADIO_INIT_INITIALIZING && stage!=WIFI_RADIO_INIT_SLOW)
        s_radio_init_started_ms=0U;
    rt_exit_critical();
}
static void radio_initialization_started(void)
{
    rt_enter_critical();
    if(s_radio_init_state==WIFI_RADIO_INIT_QUEUED) {
        s_radio_ready=false;
        s_radio_init_state=WIFI_RADIO_INIT_INITIALIZING;
        s_radio_init_started_ms=milliseconds();
    }
    rt_exit_critical();
}
static void radio_mark_slow_if_due(uint32_t now)
{
    rt_enter_critical();
    if(s_radio_init_state==WIFI_RADIO_INIT_INITIALIZING &&
       (uint32_t)(now-s_radio_init_started_ms)>=15000U)
        s_radio_init_state=WIFI_RADIO_INIT_SLOW;
    rt_exit_critical();
}
static const char *radio_stage_name(uint8_t stage)
{
    static const char *const names[]={"waiting_display","queued","initializing",
        "ready","failed","slow"};
    return stage<sizeof(names)/sizeof(names[0])?names[stage]:"failed";
}

static bool wifi_nv_status_is_reset_candidate(int status)
{
    return status==WPM_NV_JOURNAL_EFOREIGN ||
           status==WPM_NV_JOURNAL_ECORRUPT;
}

static int wifi_nv_load_config(wpm_nv_journal_t *journal,
                               wifi_config_t *config, uint8_t *payload)
{
    size_t length=0U;
    uint64_t sequence=0U;
    int status;

    memset(config,0,sizeof(*config));
    status=wpm_nv_journal_load(journal,payload,
        WPM_NV_JOURNAL_MAX_PAYLOAD,&length,&sequence);
    if((status==WPM_NV_JOURNAL_OK || status==WPM_NV_JOURNAL_ESCRUB) &&
       wifi_config_decode(config,payload,length)!=0)
        status=WPM_NV_JOURNAL_ECORRUPT;
    if(status!=WPM_NV_JOURNAL_OK && status!=WPM_NV_JOURNAL_ESCRUB)
        memset(config,0,sizeof(*config));
    wifi_secure_zero(payload,WPM_NV_JOURNAL_MAX_PAYLOAD);
    return status;
}

static int wifi_nv_reset_and_verify_empty(wpm_nv_journal_t *journal,
                                          const wpm_nv_journal_io_t *io,
                                          const wpm_nv_journal_geometry_t *geometry,
                                          wifi_config_t *config,
                                          uint8_t *payload)
{
    int status=wpm_nv_journal_reset(journal);
    if(status!=WPM_NV_JOURNAL_OK)
        return status;

    status=wpm_nv_journal_init(journal,io,geometry);
    if(status!=WPM_NV_JOURNAL_OK && status!=WPM_NV_JOURNAL_ESCRUB &&
       status!=WPM_NV_JOURNAL_EMPTY)
        return status;

    status=wifi_nv_load_config(journal,config,payload);
    if(status==WPM_NV_JOURNAL_EMPTY)
        return status;
    return status==WPM_NV_JOURNAL_OK || status==WPM_NV_JOURNAL_ESCRUB ?
           WPM_NV_JOURNAL_ECORRUPT : status;
}

/* One transaction, shared by the worker and deterministic IPC integration tests. */
static void execute_work(work_t *work, result_t *result)
{
    uint8_t payload[WPM_NV_JOURNAL_MAX_PAYLOAD];
    memset(result,0,sizeof(*result));result->kind=work->kind;
    result->radio_kind=work->radio.kind;result->generation=work->radio.operation_id;
    result->config_revision=work->radio.config_revision;
    result->config_receipt=work->config_receipt;
    if(work->kind==WORK_LOAD) {
#ifndef WORK_WIFI_HOST_TEST
        startup_mark(BOOT_CONFIG_BEGIN);
#endif
        const wpm_nv_journal_io_t *io=wifi_nv_flash_io();
        const wpm_nv_journal_geometry_t *geometry=wifi_nv_flash_geometry();
        int init_status=wpm_nv_journal_init(&s_nv_journal,io,geometry);
        bool did_reset=false;

        if(wifi_nv_status_is_reset_candidate(init_status)) {
            wpm_nv_journal_t confirmation=WPM_NV_JOURNAL_INITIALIZER;
            int confirmation_status=wpm_nv_journal_init(&confirmation,io,geometry);

            if(wifi_nv_status_is_reset_candidate(confirmation_status)) {
                init_status=wifi_nv_reset_and_verify_empty(&s_nv_journal,
                    io,geometry,&result->config,payload);
                did_reset=true;
            } else {
                if(confirmation_status==WPM_NV_JOURNAL_OK ||
                   confirmation_status==WPM_NV_JOURNAL_ESCRUB ||
                   confirmation_status==WPM_NV_JOURNAL_EMPTY)
                    s_nv_journal=confirmation;
                init_status=confirmation_status;
            }
        }

        result->nv_status=init_status;
        if(!did_reset && (init_status==WPM_NV_JOURNAL_OK ||
           init_status==WPM_NV_JOURNAL_ESCRUB ||
           init_status==WPM_NV_JOURNAL_EMPTY)) {
            result->nv_status=wifi_nv_load_config(&s_nv_journal,
                &result->config,payload);
            if(result->nv_status==WPM_NV_JOURNAL_ECORRUPT && !did_reset) {
                wifi_config_t confirmation_config;
                int confirmation_status;

                memset(&confirmation_config,0,sizeof(confirmation_config));
                confirmation_status=wifi_nv_load_config(&s_nv_journal,
                    &confirmation_config,payload);
                if(confirmation_status==WPM_NV_JOURNAL_ECORRUPT) {
                    result->nv_status=wifi_nv_reset_and_verify_empty(
                        &s_nv_journal,io,geometry,&result->config,payload);
                    did_reset=true;
                } else {
                    result->nv_status=confirmation_status;
                    if(confirmation_status==WPM_NV_JOURNAL_OK ||
                       confirmation_status==WPM_NV_JOURNAL_ESCRUB)
                        result->config=confirmation_config;
                    else
                        memset(&result->config,0,sizeof(result->config));
                }
                wifi_secure_zero(&confirmation_config,
                                 sizeof(confirmation_config));
            }
        }
        wifi_secure_zero(payload,sizeof(payload));
    } else if(work->kind==WORK_INITIALIZE) {
        radio_initialization_started();
        result->radio_status=wifi_backend_initialize();
    } else if(work->kind==WORK_SCAN) {
        int status=wifi_backend_scan();
        result->reason=status==RT_EOK?WIFI_REASON_NONE:status==-RT_EBUSY?WIFI_REASON_BUSY:WIFI_REASON_IO;
    } else if(work->kind==WORK_STOP_SCAN) {
        int status=wifi_backend_stop_scan();
        result->reason=status==RT_EOK?WIFI_REASON_NONE:status==-RT_EBUSY?WIFI_REASON_BUSY:WIFI_REASON_IO;
    } else if(work->radio.kind==WIFI_CORE_JOB_JOIN) {
        if(join_cancelled(&work->radio.operation_id))result->reason=WIFI_REASON_CANCELLED;
        else result->reason=wifi_backend_join(&work->radio.target,join_cancelled,&work->radio.operation_id);
    } else if(work->radio.kind==WIFI_CORE_JOB_LEAVE) {
        result->reason=wifi_backend_leave();
    } else if(work->radio.kind==WIFI_CORE_JOB_STORE) {
        size_t length=0;uint64_t sequence=0;
        result->nv_status=wifi_config_encode(&work->radio.config,payload,sizeof(payload),&length);
        if(!result->nv_status)
            result->nv_status=wpm_nv_journal_commit(&s_nv_journal,payload,
                length,work->radio.scrub_old,&sequence);
        wifi_secure_zero(payload,sizeof(payload));
    }
}
static void wireless_worker(void *unused)
{
    (void)unused;
    work_t work; result_t result;
    while(1) {
        rt_ssize_t got=rt_mq_recv(&s_work_queue,&work,sizeof(work),RT_WAITING_FOREVER);
        if(got!=(rt_ssize_t)sizeof(work))continue;
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_WIFI_WORKER, WATCHDOG_COMPONENT_STAGE_ACTIVE,
            (uint32_t)work.kind);
        execute_work(&work,&result);
        wifi_secure_zero(&work,sizeof(work));
        /* Result must not be lost. Only this worker produces these messages. */
        if(rt_mq_send_wait(&s_result_queue,&result,sizeof(result),RT_WAITING_FOREVER)!=RT_EOK)
            s_transport_drops++;
        wifi_secure_zero(&result,sizeof(result));
        watchdog_supervisor_component_progress(
            WATCHDOG_COMPONENT_WIFI_WORKER, WATCHDOG_COMPONENT_STAGE_IDLE,
            ++s_watchdog_worker_progress);
    }
}
int wifi_manager_service_init(void)
{
    if(s_ready)return RT_EOK;
    s_view_publish_valid=false;
    s_view_last_publish_ms=0U;
    s_config_loaded=false;
    s_worker_busy=false;
    s_radio_init_started_ms=0U;
    radio_status_set(WIFI_RADIO_INIT_WAITING_DISPLAY,false);
    wifi_core_init(&s_core);
    wifi_core_set_operation_allocator(&s_core,allocate_operation,NULL);
    if(rt_mutex_init(&s_view_lock,"wfview",RT_IPC_FLAG_PRIO)!=RT_EOK ||
       rt_mq_init(&s_control_queue,"wfctl",s_control_pool,sizeof(control_t),sizeof(s_control_pool),RT_IPC_FLAG_PRIO)!=RT_EOK ||
       rt_mq_init(&s_urgent_queue,"wfurgent",s_urgent_pool,sizeof(control_t),sizeof(s_urgent_pool),RT_IPC_FLAG_PRIO)!=RT_EOK ||
       rt_mq_init(&s_work_queue,"wfwork",s_work_pool,sizeof(work_t),sizeof(s_work_pool),RT_IPC_FLAG_FIFO)!=RT_EOK ||
       rt_mq_init(&s_result_queue,"wfdone",s_result_pool,sizeof(result_t),sizeof(s_result_pool),RT_IPC_FLAG_FIFO)!=RT_EOK)
        return -RT_ENOMEM;
    rt_thread_t worker=rt_thread_create("wifi_worker",wireless_worker,NULL,8192,12,2);
    if(!worker)return -RT_ENOMEM;
    if(rt_thread_startup(worker)!=RT_EOK)return -RT_ERROR;
    watchdog_supervisor_component_expect(WATCHDOG_COMPONENT_WIFI_WORKER, 0);
    work_t first;memset(&first,0,sizeof(first));first.kind=WORK_LOAD;
    if(rt_mq_send(&s_work_queue,&first,sizeof(first))!=RT_EOK)return -RT_EIO;
    s_worker_busy=true;s_ready=true;return RT_EOK;
}
static int submit(control_t *command,rt_uint32_t *receipt)
{
    if(!s_ready)return -RT_EIO;
    command->receipt=allocate_operation(NULL);
    bool urgent=command->kind==CMD_CANCEL||command->kind==CMD_DISCONNECT||
        command->kind==CMD_FORGET||command->kind==CMD_CLEAR;
    rt_err_t result=rt_mq_send_wait_prio(urgent?&s_urgent_queue:&s_control_queue,
        command,sizeof(*command),urgent?1:0,0,RT_UNINTERRUPTIBLE);
    if(result==RT_EOK && receipt)*receipt=command->receipt;
    wifi_secure_zero(command,sizeof(*command));
    return result;
}
int wifi_request_scan(void)
{if(!radio_is_ready())return -RT_EBUSY;control_t cmd={0};cmd.kind=CMD_SCAN;return submit(&cmd,NULL);}
int wifi_request_connect_network(const wifi_ap_record_t *ap,const char*password,
    rt_bool_t remember,rt_uint32_t*receipt)
{
    if(!radio_is_ready())return -RT_EBUSY;
    if(!ap)return -RT_EINVAL;
    size_t n=ap->ssid_length?ap->ssid_length:strlen(ap->ssid);
    size_t key=password?strlen(password):0;
    if(!n||n>32||key>63||(ap->flags&WIFI_AP_FLAG_SSID_TRUNCATED))return -RT_EINVAL;
    if(ap->security_mode==WIFI_SECURITY_UNSUPPORTED)return -RT_EINVAL;
    if(ap->security_mode!=WIFI_SECURITY_OPEN && key<8)return -RT_EINVAL;
    if(ap->security_mode==WIFI_SECURITY_OPEN && key)return -RT_EINVAL;
    for(size_t i=0;i<key;i++)if((uint8_t)password[i]<32||(uint8_t)password[i]>126)return -RT_EINVAL;
    control_t cmd={0};cmd.kind=CMD_CONNECT;cmd.remember=remember!=0;
    cmd.ssid_length=(uint8_t)n;memcpy(cmd.ssid,ap->ssid,n);cmd.security=ap->security_mode;
    if(key)memcpy(cmd.password,password,key);
    return submit(&cmd,receipt);
}
int wifi_request_connect_ap(const wifi_ap_record_t *ap,const char*password)
{return wifi_request_connect_network(ap,password,RT_TRUE,NULL);}
int wifi_request_connect(const char*ssid,const char*password)
{
    if(!ssid)return -RT_EINVAL;
    wifi_ap_record_t ap;memset(&ap,0,sizeof(ap));size_t n=strlen(ssid);
    if(!n||n>32)return -RT_EINVAL;
    memcpy(ap.ssid,ssid,n);ap.ssid_length=(uint8_t)n;
    ap.security_mode=password&&password[0]?WIFI_SECURITY_WPA2:WIFI_SECURITY_OPEN;
    return wifi_request_connect_network(&ap,password,RT_TRUE,NULL);
}
int wifi_request_connect_saved(rt_uint32_t id,rt_uint32_t*op)
{if(!radio_is_ready())return -RT_EBUSY;control_t c={0};c.kind=CMD_SAVED;c.id=id;return submit(&c,op);}
int wifi_request_update_password(rt_uint32_t id,const char*password,rt_uint32_t*op)
{
    if(!radio_is_ready())return -RT_EBUSY;
    size_t n=password?strlen(password):0;if(n>63)return -RT_EINVAL;
    control_t c={0};c.kind=CMD_PASSWORD;c.id=id;if(n)memcpy(c.password,password,n);return submit(&c,op);
}
int wifi_request_disconnect(rt_uint32_t*op)
{control_t c={0};c.kind=CMD_DISCONNECT;return submit(&c,op);}
int wifi_request_cancel(rt_uint32_t*op)
{control_t c={0};c.kind=CMD_CANCEL;return submit(&c,op);}
int wifi_request_forget(rt_uint32_t id,rt_uint32_t*op)
{control_t c={0};c.kind=CMD_FORGET;c.id=id;return submit(&c,op);}
int wifi_request_forget_all(rt_uint32_t*op)
{control_t c={0};c.kind=CMD_CLEAR;return submit(&c,op);}
int wifi_request_set_autoconnect(rt_bool_t on,rt_uint32_t*op)
{control_t c={0};c.kind=CMD_AUTO;c.enabled=on!=0;return submit(&c,op);}
int wifi_request_set_profile_autoconnect(rt_uint32_t id,rt_bool_t on,rt_uint32_t*op)
{control_t c={0};c.kind=CMD_PROFILE_AUTO;c.id=id;c.enabled=on!=0;return submit(&c,op);}
int wifi_request_retry_save(rt_uint32_t*op)
{control_t c={0};c.kind=CMD_RETRY_SAVE;return submit(&c,op);}
int wifi_request_use_temporary(rt_uint32_t*op)
{control_t c={0};c.kind=CMD_TEMP;return submit(&c,op);}
int wifi_get_snapshot(wifi_snapshot_t*out)
{
    if(!out||!s_ready)return -RT_EBUSY;
    if(rt_mutex_take(&s_view_lock,0)!=RT_EOK)return -RT_EBUSY;
    *out=s_view;rt_mutex_release(&s_view_lock);return RT_EOK;
}
uint32_t wifi_manager_radio_generation(void){return s_radio_generation;}
static uint32_t connection_receipt(void)
{
    wifi_core_adapter_state_t state=core_adapter_state();
    return s_connection_generation==state.operation_id?s_connection_receipt:state.operation_id;
}
void wifi_manager_notify_ip_ready(uint32_t generation,const char*ip)
{
    wifi_core_adapter_state_t before=core_adapter_state();
    wifi_core_ip_ready(&s_core,generation,ip,milliseconds());
    wifi_core_adapter_state_t after=core_adapter_state();
    if(before.phase==WPM_PHASE_DHCP&&after.phase==WPM_PHASE_ONLINE) {
#ifndef WORK_WIFI_HOST_TEST
        startup_mark(BOOT_NETWORK_READY);
#endif
        if(before.config_revision!=after.config_revision)s_config_receipt=connection_receipt();
        else if(after.storage==WIFI_STORAGE_OK&&after.active_target_is_durable)
            s_saved_receipt=connection_receipt();
    }
}
static void command_apply(control_t *cmd,uint32_t now)
{
    if(s_cancel_barrier && (int32_t)(cmd->receipt-s_cancel_barrier)<0 &&
       (cmd->kind==CMD_SCAN||cmd->kind==CMD_CONNECT||cmd->kind==CMD_SAVED||cmd->kind==CMD_PASSWORD)) {
        wifi_secure_zero(cmd,sizeof(*cmd));return;
    }
    if((int32_t)(cmd->receipt-s_last_request)>0)s_last_request=cmd->receipt;
    wifi_core_adapter_state_t previous=core_adapter_state();
    int result=0;
    switch(cmd->kind) {
    case CMD_SCAN:
        if(!radio_is_ready()||previous.radio_busy||previous.radio_pending||
           previous.phase==WPM_PHASE_ASSOCIATING||previous.phase==WPM_PHASE_DHCP||
           previous.phase==WPM_PHASE_CANCELLING||wifi_backend_scan_active()||
           s_worker_busy||s_scan_start_pending||s_scan_stop_failed)result=WIFI_EBUSY;
        else s_scan_start_pending=true;
        break;
    case CMD_CONNECT:result=radio_is_ready()?wifi_core_connect(&s_core,cmd->ssid,cmd->ssid_length,cmd->security,cmd->password,cmd->remember,now):WIFI_EBUSY;break;
    case CMD_SAVED:result=radio_is_ready()?wifi_core_connect_saved(&s_core,cmd->id,now):WIFI_EBUSY;break;
    case CMD_PASSWORD:result=radio_is_ready()?wifi_core_update_password(&s_core,cmd->id,cmd->password,now):WIFI_EBUSY;break;
    case CMD_DISCONNECT:wifi_core_disconnect(&s_core);break;
    case CMD_CANCEL:wifi_core_cancel(&s_core);break;
    case CMD_FORGET:result=wifi_core_forget(&s_core,cmd->id);break;
    case CMD_CLEAR:wifi_core_forget_all(&s_core);break;
    case CMD_AUTO:wifi_core_set_auto(&s_core,cmd->enabled);break;
    case CMD_PROFILE_AUTO:result=wifi_core_set_profile_auto(&s_core,cmd->id,cmd->enabled);break;
    case CMD_RETRY_SAVE:result=wifi_core_retry_save(&s_core);break;
    case CMD_TEMP:result=wifi_core_use_temporary(&s_core);break;
    }
    if(result < 0) {
        /* Scan refusal has its own request receipt. It is not a failed
         * association, and must not overwrite a healthy or pending link. */
        if(cmd->kind!=CMD_SCAN) {
            wifi_reason_t reason=result==WIFI_EFULL?WIFI_REASON_FULL:
                result==WIFI_ENOENT?WIFI_REASON_NOT_FOUND:
                result==WIFI_EFOREIGN?WIFI_REASON_FOREIGN:
                (result==WIFI_EBUSY||result==WIFI_ENOTINITIALIZED)?WIFI_REASON_BUSY:
                WIFI_REASON_INVALID;
            wifi_core_report_error(&s_core,reason);
        }
    } else if (result == 0) {
        if(cmd->kind==CMD_CONNECT||cmd->kind==CMD_SAVED||cmd->kind==CMD_PASSWORD||
           cmd->kind==CMD_DISCONNECT||cmd->kind==CMD_CANCEL||cmd->kind==CMD_TEMP)
            s_scan_stop_failed=false;
        wifi_core_adapter_state_t after=core_adapter_state();
        if(previous.operation_id!=after.operation_id||cmd->kind==CMD_TEMP) {
            if((int32_t)(cmd->receipt-s_connection_receipt)>0)s_connection_receipt=cmd->receipt;
            s_connection_generation=after.operation_id;
            s_saved_receipt=0;
        }
        if(previous.config_revision!=after.config_revision &&
           (int32_t)(cmd->receipt-s_config_receipt)>0)s_config_receipt=cmd->receipt;
    }
    if((int32_t)(cmd->receipt-s_request_result_id)>0) {
        s_request_result_id=cmd->receipt;s_request_result=result;
    }
    s_radio_generation=core_adapter_state().operation_id;
    wifi_secure_zero(cmd,sizeof(*cmd));
}
static void publish(uint32_t now)
{
    /* Keep the 1 ms manager/network poll responsive, but cap how often its
     * public snapshot and legacy g_wifi mirror are rebuilt. UI refreshes at
     * frame cadence; a bounded 5 ms snapshot age avoids repeating the same
     * locked copy on every owner loop. The first snapshot is always published. */
    if(s_view_publish_valid &&
       (uint32_t)(now-s_view_last_publish_ms)<WIFI_VIEW_PUBLISH_INTERVAL_MS)
        return;
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    rt_uint32_t publish_start=usb_perf_now();
    rt_uint32_t copy_start=0U,copy_end=0U;
    bool copied=false;
#endif
    if(rt_mutex_take(&s_view_lock,RT_WAITING_FOREVER)!=RT_EOK)return;
    s_view_last_publish_ms=now;
    s_view_publish_valid=true;
    wifi_public_snapshot_t previous=s_view.network;
    int previous_scan_status=s_view.scan_status;
    uint32_t previous_scan_generation=s_view.scan_generation;
    uint32_t previous_scan_dropped=s_view.scan_dropped;
    uint8_t previous_scan_count=s_view.scan_count;
    wifi_core_snapshot(&s_core,now,&s_view.network);
    radio_status_read(&s_view.network.radio_ready,&s_view.network.radio_init_state);
    if(s_last_request)s_view.network.operation_id=s_last_request;
    s_view.network.connection_operation_id=connection_receipt();
    s_view.network.config_operation_id=s_config_receipt;
    s_view.network.saved_operation_id=s_saved_receipt;
    s_view.network.config_saved_operation_id=s_config_saved_receipt;
    s_view.network.request_result_id=s_request_result_id;s_view.network.request_result=s_request_result;
    if(s_scan_stop_failed) {
        s_view.network.phase=WIFI_PHASE_FAILED;
        s_view.network.reason=WIFI_REASON_IO;
    }
    if(wifi_backend_scan_active()&&(s_view.network.phase==WIFI_PHASE_IDLE||
       s_view.network.phase==WIFI_PHASE_DISCONNECTED))s_view.network.phase=WIFI_PHASE_SCANNING;
    rt_enter_critical();
    uint32_t scan_generation=g_wifi.scan_updated;
    s_view.scan_status=g_wifi.scan_status;s_view.scan_generation=scan_generation;
    s_view.scan_dropped=g_wifi.scan_dropped_count;
    s_view.scan_count=(uint8_t)(g_wifi.scan_count>32?32:g_wifi.scan_count<0?0:g_wifi.scan_count);
    /* Every writer that changes scan_records must increment scan_updated in
     * the same critical section. Overflow can change metadata without records,
     * so status/count/drop fields are refreshed on every publish. */
    if(scan_generation!=previous_scan_generation) {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        copy_start=usb_perf_now();
#endif
        memcpy(s_view.access_points,g_wifi.scan_records,sizeof(s_view.access_points));
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        copy_end=usb_perf_now();
        s_scan_publish_copy_count++;
        copied=true;
#endif
    } else {
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
        s_scan_publish_skip_count++;
#endif
    }
    g_wifi.conn_status=s_view.network.phase==WIFI_PHASE_ONLINE?WIFI_STATE_CONNECTED:
        s_view.network.phase==WIFI_PHASE_ASSOCIATING||s_view.network.phase==WIFI_PHASE_DHCP||
        s_view.network.phase==WIFI_PHASE_CANCELLING?WIFI_STATE_CONNECTING:
        s_view.network.phase==WIFI_PHASE_FAILED?WIFI_STATE_FAILED:WIFI_STATE_DISCONNECTED;
    wifi_copy_text(g_wifi.ip,sizeof(g_wifi.ip),s_view.network.ip);
    wifi_copy_text(g_wifi.ssid,sizeof(g_wifi.ssid),s_view.network.ssid);
    wifi_copy_text(g_wifi.status_msg,sizeof(g_wifi.status_msg),
                   wifi_phase_name((wifi_phase_t)s_view.network.phase));
    rt_exit_critical();
    wifi_public_snapshot_t current=s_view.network;
    /* Retry countdown is sampled every owner poll; it must not wake the UI
     * every millisecond. The UI can refresh countdown text on its own timer. */
    previous.retry_in_ms=0U;
    current.retry_in_ms=0U;
    bool changed=memcmp(&previous,&current,sizeof(current))!=0||
        previous_scan_status!=s_view.scan_status||
        previous_scan_generation!=s_view.scan_generation||
        previous_scan_dropped!=s_view.scan_dropped||
        previous_scan_count!=s_view.scan_count;
    rt_mutex_release(&s_view_lock);
    if(changed)startup_notify_ui();
#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
    usb_perf_record(USB_PERF_WIFI_PUBLISH,publish_start,usb_perf_now());
    if(copied)usb_perf_record(USB_PERF_WIFI_SCAN_COPY,copy_start,copy_end);
#endif
}
void wifi_manager_poll(void)
{
    if(!s_ready)return;
    uint32_t now=milliseconds();result_t result;control_t cmd;
    while(rt_mq_recv(&s_result_queue,&result,sizeof(result),RT_WAITING_NO)==(rt_ssize_t)sizeof(result)) {
        s_worker_busy=false;
        if(result.kind==WORK_LOAD) {
            wifi_storage_status_t status=result.nv_status==WPM_NV_JOURNAL_OK?WIFI_STORAGE_OK:
                result.nv_status==WPM_NV_JOURNAL_EMPTY?WIFI_STORAGE_EMPTY:
                result.nv_status==WPM_NV_JOURNAL_EFOREIGN?WIFI_STORAGE_FOREIGN:
                result.nv_status==WPM_NV_JOURNAL_ESCRUB||result.nv_status==WPM_NV_JOURNAL_EIO?
                    WIFI_STORAGE_FAILED:WIFI_STORAGE_CORRUPT;
            wifi_core_load(&s_core,(result.nv_status==WPM_NV_JOURNAL_OK||result.nv_status==WPM_NV_JOURNAL_ESCRUB)?&result.config:NULL,status);
            s_config_loaded=true;
            startup_mark(BOOT_CONFIG_READY);
            if(result.nv_status==WPM_NV_JOURNAL_ESCRUB) {
                /* The committed new value is authoritative, even if retiring
                 * old credentials failed. Explicit retry must remain possible. */
                wifi_core_mark_cleanup_pending(&s_core);
            }
        } else if(result.kind==WORK_INITIALIZE) {
            uint8_t stage;
            radio_status_read(NULL,&stage);
            if(stage==WIFI_RADIO_INIT_INITIALIZING||stage==WIFI_RADIO_INIT_SLOW) {
                int finalized=result.radio_status==RT_EOK?wifi_backend_finalize():-RT_ERROR;
                if(finalized==RT_EOK) {
                    radio_status_set(WIFI_RADIO_INIT_READY,true);
                    startup_mark(BOOT_RADIO_READY);
                } else {
                    radio_status_set(WIFI_RADIO_INIT_FAILED,false);
                    startup_mark(BOOT_RADIO_FAILED);
                }
            }
        } else if(result.kind==WORK_RADIO) {
            if(result.radio_kind==WIFI_CORE_JOB_STORE) {
                wifi_core_adapter_state_t before=core_adapter_state();
                bool expected=before.store_busy&&before.store_revision==result.config_revision;
                wifi_store_outcome_t outcome=result.nv_status==WPM_NV_JOURNAL_OK?WIFI_STORE_CLEAN:
                    result.nv_status==WPM_NV_JOURNAL_ESCRUB?WIFI_STORE_COMMITTED_DIRTY:
                    result.nv_status==WPM_NV_JOURNAL_EFOREIGN?WPM_STORE_FOREIGN:WIFI_STORE_FAILED;
                wifi_core_store_result(&s_core,result.config_revision,outcome);
                wifi_core_adapter_state_t after=core_adapter_state();
                if(expected&&result.nv_status==WPM_NV_JOURNAL_OK) {
                    s_config_saved_receipt=result.config_receipt;
                    if(after.phase==WPM_PHASE_ONLINE&&after.active_target_is_durable)
                        s_saved_receipt=connection_receipt();
                }
            } else wifi_core_radio_result(&s_core,result.radio_kind,result.generation,result.reason,now);
        } else if(result.kind==WORK_STOP_SCAN) {
            if(result.reason!=WIFI_REASON_NONE) {
                /* Stop timed out or failed: keep the backend's active state
                 * authoritative and require a new explicit radio request. */
                s_scan_stop_failed=true;
            }
        }
        /* WORK_SCAN publishes its terminal state through the scan callback /
         * backend. Keep it independent of the association failure reason. */
        wifi_secure_zero(&result,sizeof(result));
    }
    if(s_config_loaded) {
        while(rt_mq_recv(&s_urgent_queue,&cmd,sizeof(cmd),RT_WAITING_NO)==(rt_ssize_t)sizeof(cmd)) {
            s_cancel_barrier=cmd.receipt;
            command_apply(&cmd,now);
        }
        bool suppressed=core_adapter_state().auto_suspended;
        while(rt_mq_recv(&s_control_queue,&cmd,sizeof(cmd),RT_WAITING_NO)==(rt_ssize_t)sizeof(cmd)) {
            bool old_preference=s_cancel_barrier && (int32_t)(cmd.receipt-s_cancel_barrier)<0;
            command_apply(&cmd,now);
            if(old_preference && suppressed)wifi_core_suspend_automatic(&s_core);
        }
    }
    bool radio_ready=radio_is_ready();
    wifi_core_adapter_state_t state=core_adapter_state();
    if(!radio_ready&&state.leave_pending&&!state.join_pending&&!state.radio_busy) {
        /* No connection request or automatic join can run before bootstrap.
         * Complete this logical leave locally so queued preferences can save. */
        wifi_core_job_t logical_leave;
        if(wifi_core_next_job(&s_core,&logical_leave)&&logical_leave.kind==WIFI_CORE_JOB_LEAVE)
            wifi_core_radio_result(&s_core,WIFI_CORE_JOB_LEAVE,logical_leave.operation_id,
                                   WIFI_REASON_NONE,now);
        wifi_secure_zero(&logical_leave,sizeof(logical_leave));
    }
    if(radio_ready&&state.link_ready && s_link_generation==state.operation_id && !wifi_backend_link_ready()) {
        if(state.phase==WPM_PHASE_ONLINE||state.phase==WPM_PHASE_DHCP)
            wifi_core_link_lost(&s_core,state.operation_id,now);
    }
    if(s_config_loaded&&radio_ready)wifi_core_tick(&s_core,now);
    state=core_adapter_state();
    s_radio_generation=state.operation_id;
    if(s_last_net_generation!=state.operation_id) {
        s_last_net_generation=state.operation_id;s_link_generation=0;
        if(radio_ready)wifi_backend_network_reset(state.operation_id);
    }
    if(radio_ready&&state.link_ready && s_link_generation!=state.operation_id &&
       (state.phase==WPM_PHASE_DHCP||state.phase==WPM_PHASE_ONLINE)) {
        wifi_backend_publish_link(true);s_link_generation=state.operation_id;
    }
    if(radio_ready&&wifi_core_take_dhcp_reset_request(&s_core))wifi_backend_dhcp_reset();
    radio_mark_slow_if_due(now);
    /* Never dispatch pending JOIN/LEAVE while scan stop is unconfirmed. */
    if(!s_worker_busy&&s_config_loaded&&!s_scan_stop_failed) {
        work_t work;memset(&work,0,sizeof(work));bool send=false;
        uint8_t init_stage;
        radio_status_read(NULL,&init_stage);
        if(init_stage==WIFI_RADIO_INIT_WAITING_DISPLAY&&startup_radio_allowed()) {
            work.kind=WORK_INITIALIZE;
            radio_status_set(WIFI_RADIO_INIT_QUEUED,false);
            s_worker_busy=true;
            if(rt_mq_send(&s_work_queue,&work,sizeof(work))!=RT_EOK) {
                s_worker_busy=false;
                radio_status_set(WIFI_RADIO_INIT_WAITING_DISPLAY,false);
                s_transport_drops++;
            }
            wifi_secure_zero(&work,sizeof(work));
            publish(now);
            return;
        }
        state=core_adapter_state();
        bool radio_pending=state.radio_pending;
        bool connection_waiting=state.phase==WPM_PHASE_ASSOCIATING||
            state.phase==WPM_PHASE_DHCP||state.phase==WPM_PHASE_CANCELLING;
        if(radio_pending&&!radio_ready) {
            /* Radio jobs remain pending until the TCP/IP owner finalizes WHD. */
        } else if(radio_pending&&wifi_backend_scan_active()) {
            work.kind=WORK_STOP_SCAN;send=true;
        } else if(radio_pending&&radio_ready) {
            work.kind=WORK_RADIO;send=wifi_core_next_job(&s_core,&work.radio);
            work.config_receipt=s_config_receipt;
        } else if(radio_ready&&s_scan_start_pending&&!state.radio_busy&&!connection_waiting&&
                  !wifi_backend_scan_active()) {
            work.kind=WORK_SCAN;s_scan_start_pending=false;send=true;
        } else {
            work.kind=WORK_RADIO;send=wifi_core_next_job(&s_core,&work.radio);
            work.config_receipt=s_config_receipt;
        }
        if(send) {
            if(rt_mq_send(&s_work_queue,&work,sizeof(work))==RT_EOK)s_worker_busy=true;
            else {
                s_transport_drops++;
                if(work.kind==WORK_RADIO) {
                    if(work.radio.kind==WIFI_CORE_JOB_STORE)
                        wifi_core_store_result(&s_core,work.radio.config_revision,false);
                    else wifi_core_radio_result(&s_core,work.radio.kind,work.radio.operation_id,WIFI_REASON_IO,now);
                } else if(work.kind==WORK_SCAN||work.kind==WORK_STOP_SCAN)
                    s_scan_stop_failed=true;
            }
            wifi_secure_zero(&work,sizeof(work));
        }
    }
    publish(now);
}

static size_t utf8_span(const uint8_t *p,size_t remaining)
{
    if(p[0]<0x80)return 1;
    size_t n=p[0]>=0xC2&&p[0]<=0xDF?2:p[0]>=0xE0&&p[0]<=0xEF?3:
        p[0]>=0xF0&&p[0]<=0xF4?4:0;
    if(!n||n>remaining)return 0;
    for(size_t i=1;i<n;i++)if((p[i]&0xC0)!=0x80)return 0;
    if((p[0]==0xE0&&p[1]<0xA0)||(p[0]==0xED&&p[1]>=0xA0)||
       (p[0]==0xF0&&p[1]<0x90)||(p[0]==0xF4&&p[1]>=0x90))return 0;
    return n;
}
static size_t json_text(char *out,size_t capacity,const uint8_t *text,size_t length)
{
    size_t used=0;
    for(size_t i=0;i<length && used+7<capacity;i++) {
        const uint8_t*p=text+i;
        if(*p=='"'||*p=='\\'){out[used++]='\\';out[used++]=(char)*p;}
        else if(*p<32){int n=rt_snprintf(out+used,capacity-used,"\\u%04X",*p);if(n<0)break;used+=(size_t)n;}
        else if(*p>=0x80) {
            size_t n=utf8_span(p,length-i);
            if(n){memcpy(out+used,p,n);used+=n;i+=n-1;}
            else {memcpy(out+used,"\\uFFFD",6);used+=6;}
        } else out[used++]=(char)*p;
    }
    out[used]=0;return used;
}
int wifi_status_json(char*out,size_t cap)
{
    /* Status-only. No persisted profile bodies or credential lengths. */
    if(!out||!cap)return -RT_EINVAL;
    wifi_public_snapshot_t network;
    int scan_status;
    if(!s_ready||rt_mutex_take(&s_view_lock,0)!=RT_EOK)return -RT_EBUSY;
    network=s_view.network;scan_status=s_view.scan_status;rt_mutex_release(&s_view_lock);
    wifi_backend_diagnostics_t diag;
    memset(&diag,0,sizeof(diag));(void)wifi_backend_get_diagnostics(&diag);
    whd_stm32_sdio_stats_t sdio;memset(&sdio,0,sizeof(sdio));whd_stm32_sdio_get_stats(&sdio);
    char name[224],raw_hex[65];
    size_t ssid_length=network.ssid_length;
    if(ssid_length>WIFI_SSID_BYTES_MAX)return -RT_EINVAL;
    json_text(name,sizeof(name),(const uint8_t*)network.ssid,ssid_length);
    static const char hex[]="0123456789ABCDEF";
    for(size_t i=0;i<ssid_length;i++) {
        uint8_t byte=(uint8_t)network.ssid[i];
        raw_hex[i*2]=hex[byte>>4];raw_hex[i*2+1]=hex[byte&15];
    }
    raw_hex[ssid_length*2]=0;
    int count=rt_snprintf(out,cap,
        "{\"phase\":\"%s\",\"reason\":\"%s\",\"storage\":\"%s\",\"ssid\":\"%s\","
        "\"ssid_raw_hex\":\"%s\",\"ip\":\"%s\",\"operation_id\":%lu,\"profile_id\":%lu,\"config_revision\":%lu,"
        "\"saved_networks\":%u,\"known_networks\":%u,\"pending_changes\":%s,\"auto_connect\":%s,\"auto_suspended\":%s,"
        "\"retry_in_ms\":%lu,\"queue_drops\":%lu,\"cleanup_pending\":%s,"
        "\"connection_operation_id\":%lu,\"config_operation_id\":%lu,"
        "\"saved_operation_id\":%lu,\"config_saved_operation_id\":%lu,"
        "\"request_result_id\":%lu,\"request_result\":%d,\"configuration_format\":1,"
        "\"scan_status\":%d,\"scan_raw\":%lu,\"scan_outcome\":%u,\"scan_calls\":%lu,"
        "\"join_raw\":%lu,\"join_calls\":%lu,\"leave_raw\":%lu,\"leave_calls\":%lu,"
        "\"sdio_errors\":%lu,\"sdio_timeouts\":%lu,\"sdio_recoveries\":%lu,\"sdio_recovery_failures\":%lu,"
        "\"sdio_last_code\":%lu,\"sdio_last_sta\":%lu,\"sdio_last_dcount\":%lu,\"sdio_last_abort\":%lu,"
        "\"radio_ready\":%s,\"radio_init_stage\":\"%s\"}",
        wifi_phase_name(network.phase),wifi_reason_name(network.reason),
        wifi_storage_name(network.storage),name,raw_hex,network.ip,
        (unsigned long)network.operation_id,(unsigned long)network.active_profile_id,
        (unsigned long)network.config_revision,network.saved_profile_count,
        network.profile_count,network.pending_changes?"true":"false",
        network.auto_connect?"true":"false",network.auto_suspended?"true":"false",
        (unsigned long)network.retry_in_ms,(unsigned long)s_transport_drops,
        network.cleanup_pending?"true":"false",
        (unsigned long)network.connection_operation_id,(unsigned long)network.config_operation_id,
        (unsigned long)network.saved_operation_id,(unsigned long)network.config_saved_operation_id,
        (unsigned long)network.request_result_id,network.request_result,
        scan_status,(unsigned long)diag.last_scan.raw_whd_result,diag.last_scan.outcome,
        (unsigned long)diag.scan_counters.attempts,
        (unsigned long)diag.last_join.raw_whd_result,(unsigned long)diag.join_counters.attempts,
        (unsigned long)diag.last_leave.raw_whd_result,(unsigned long)diag.leave_counters.attempts,
        (unsigned long)sdio.errors,(unsigned long)sdio.timeouts,(unsigned long)sdio.timeout_recoveries,
        (unsigned long)sdio.timeout_recovery_failures,(unsigned long)sdio.last_error_code,
        (unsigned long)sdio.last_error_status,(unsigned long)sdio.last_error_dcount,(unsigned long)sdio.last_abort_result,
        network.radio_ready?"true":"false",radio_stage_name(network.radio_init_state));
    return count<0||(size_t)count>=cap?-RT_ENOSPC:count;
}

#if defined(WORK_EVIDENCE_DIAGNOSTICS) && WORK_EVIDENCE_DIAGNOSTICS
static void wifi_status_packet_cmd(void)
{
    static char json[1536];static uint32_t sequence;
    uint32_t request=++sequence;
    int length=wifi_status_json(json,sizeof(json));
    if(length<0){rt_kprintf("[WIFI_STATUS] req=%lu FAIL error=%d\r\n",(unsigned long)request,length);return;}
    rt_kprintf("[WIFI_BUILD] source=%s\r\n",WORK_SOURCE_FINGERPRINT);
    rt_kprintf("[WIFI_STATUS] req=%lu bytes=%u crc32=%08lX\r\n",(unsigned long)request,
        (unsigned)length,(unsigned long)usb_evidence_bytes_crc32(json,(size_t)length));
    static const char digits[]="0123456789ABCDEF";
    for(size_t offset=0;offset<(size_t)length;offset+=16) {
        size_t count=(size_t)length-offset;if(count>16)count=16;
        char hex[33];
        for(size_t i=0;i<count;i++){uint8_t byte=(uint8_t)json[offset+i];hex[i*2]=digits[byte>>4];hex[i*2+1]=digits[byte&15];}
        hex[count*2]=0;
        rt_kprintf("[WIFI_STATUS_HEX] req=%lu offset=%u hex=%s\r\n",(unsigned long)request,(unsigned)offset,hex);
    }
    rt_kprintf("[WIFI_STATUS_END] req=%lu PASS\r\n",(unsigned long)request);
}
MSH_CMD_EXPORT(wifi_status_packet_cmd, debug-only CRC-checked status without credentials);

static void wifi_publish_copy_stats_cmd(int argc,char **argv)
{
    uint32_t copies,skips;

    if(argc==2&&rt_strcmp(argv[1],"reset")==0) {
        rt_enter_critical();
        s_scan_publish_copy_count=0U;
        s_scan_publish_skip_count=0U;
        rt_exit_critical();
        rt_kprintf("[wifi_publish_copy] counters reset\r\n");
        return;
    }
    if(argc!=1) {
        rt_kprintf("Usage: wifi_publish_copy_stats [reset]\r\n");
        return;
    }
    rt_enter_critical();
    copies=s_scan_publish_copy_count;
    skips=s_scan_publish_skip_count;
    rt_exit_critical();
    rt_kprintf("[wifi_publish_copy] copies=%lu bytes=%lu skips=%lu\r\n",
        (unsigned long)copies,
        (unsigned long)(copies*(uint32_t)sizeof(s_view.access_points)),
        (unsigned long)skips);
}
MSH_CMD_EXPORT(wifi_publish_copy_stats_cmd,
               report or reset measured WiFi snapshot table copies);
#endif
