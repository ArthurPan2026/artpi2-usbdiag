/* SPDX-License-Identifier: Apache-2.0 */
#include "wifi_profile_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#x);exit(2);}}while(0)
static wpm_core_t c;
static wpm_core_job_t job;
static wpm_config_t cfg, decoded;
static uint8_t wire[WPM_CONFIG_WIRE_MAX];
static void init(void){wpm_core_init(&c);wpm_core_load(&c,NULL,WPM_STORAGE_EMPTY);}
static void radio_join_ok(uint32_t now)
{
    while(wpm_core_next_job(&c,&job)) {
        if(job.kind==WPM_CORE_JOB_STORE){wpm_core_store_result(&c,job.config_revision,true);continue;}
        wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,now);
        if(job.kind==WPM_CORE_JOB_JOIN)return;
    }
    CHECK(0);
}
static uint32_t connect(const char*ssid,bool remember,uint32_t now)
{
    CHECK(wpm_core_connect(&c,(const uint8_t*)ssid,strlen(ssid),WPM_SECURITY_WPA2,
        "CoreSecret123!",remember,now)==0);
    uint32_t op=c.operation_id;radio_join_ok(now);
    CHECK(c.phase==WPM_PHASE_DHCP);
    wpm_core_ip_ready(&c,op,"192.168.1.20",now+1);
    CHECK(c.phase==WPM_PHASE_ONLINE);
    return op;
}
static void store_ok(void)
{
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_STORE);
    wpm_core_store_result(&c,job.config_revision,true);
}
static bool profile_blocked(uint32_t id)
{
    for(unsigned i=0;i<WPM_PROFILE_MAX;i++)if(c.blocked_ids[i]==id)return true;
    return false;
}
static void prepare_auto_profiles(void)
{
    init();connect("First",true,0);store_ok();
    connect("Second",true,30);store_ok();
    cfg=c.config;wpm_core_init(&c);wpm_core_load(&c,&cfg,WPM_STORAGE_OK);
}
static void test_profiles(void)
{
    init();connect("Home",true,0);CHECK(c.config.count==1);store_ok();
    uint32_t home=c.config.profiles[0].id;
    uint32_t oldrev=c.config_revision;
    uint32_t oldop=c.operation_id;
    CHECK(wpm_core_connect_saved(&c,home,20)==WPM_CORE_ALREADY_CONNECTED);
    CHECK(c.operation_id==oldop);
    CHECK(c.config_revision==oldrev);CHECK(!c.store_pending);
    CHECK(wpm_core_update_password(&c,home,"WrongButValid",30)==0);
    CHECK(!memcmp(c.config.profiles[0].password,"CoreSecret123!",13));
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,30);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_AUTH,31);
    CHECK(!memcmp(c.config.profiles[0].password,"CoreSecret123!",13));
    CHECK(c.phase==WPM_PHASE_FAILED);
    for(unsigned i=1;i<WPM_PROFILE_MAX;i++){char name[16];snprintf(name,sizeof(name),"Network%u",i);
        connect(name,true,100*i);store_ok();}
    CHECK(c.config.count==8);
    CHECK(wpm_core_connect(&c,(uint8_t*)"Ninth",5,WPM_SECURITY_WPA2,"CoreSecret123!",true,900)==WPM_EFULL);
    CHECK(c.config.count==8);
    connect("Temporary",false,901);CHECK(c.config.count==8);CHECK(!c.store_pending);
    wpm_core_link_lost(&c,c.operation_id,902);CHECK(c.auto_suspended);
}
static uint32_t saved_online(void)
{
    init();connect("Home",true,0);store_ok();
    CHECK(c.phase==WPM_PHASE_ONLINE);CHECK(!c.temporary);
    CHECK(!c.store_pending&&!c.store_busy&&!c.cleanup_pending);
    CHECK(c.config_revision==c.durable_revision);
    return c.config.profiles[0].id;
}
static void test_saved_connect_idempotent_and_reconnect(void)
{
    uint32_t id=saved_online();
    /* Preserve non-empty auto-candidate bookkeeping as well as the full core state. */
    c.candidates[0]=id;c.candidates[1]=id+1;c.candidate_count=2;c.candidate_next=1;
    wpm_core_t before=c;
    CHECK(wpm_core_connect_saved(&c,id,500)==WPM_CORE_ALREADY_CONNECTED);
    CHECK(memcmp(&c,&before,sizeof(c))==0);

    wpm_core_disconnect(&c);CHECK(c.phase==WPM_PHASE_CANCELLING);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,500);
    CHECK(c.phase==WPM_PHASE_DISCONNECTED&&c.reason==WPM_REASON_CANCELLED);
    CHECK(wpm_core_connect_saved(&c,id,501)==0);
    CHECK(c.reason==WPM_REASON_NONE);CHECK(c.join_pending);
    id=saved_online();before=c;

    /* A mismatching live key must submit a real reconnect to the durable key. */
    c.target.password[0]^=1;
    uint32_t old_operation=c.operation_id;
    CHECK(wpm_core_connect_saved(&c,id,501)==0);
    CHECK(c.operation_id!=old_operation);CHECK(c.phase==WPM_PHASE_CANCELLING);
    CHECK(c.leave_pending&&c.join_pending);CHECK(!c.temporary);
    CHECK(!memcmp(c.target.password,c.durable_config.profiles[0].password,
        c.target.password_length));

    id=saved_online();
    CHECK(wpm_core_use_temporary(&c)==0);CHECK(c.temporary);
    old_operation=c.operation_id;
    CHECK(wpm_core_connect_saved(&c,id,600)==0);
    CHECK(c.operation_id!=old_operation);CHECK(c.phase==WPM_PHASE_CANCELLING);
    CHECK(c.leave_pending&&c.join_pending);CHECK(!c.temporary);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,600);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    CHECK(job.target.id==id);
    CHECK(!memcmp(job.target.password,c.durable_config.profiles[0].password,
        job.target.password_length));

    id=saved_online();
    CHECK(wpm_core_update_password(&c,id,"ChangedPass",700)==0);
    CHECK(c.phase==WPM_PHASE_CANCELLING&&c.leave_pending&&c.join_pending);
    CHECK(!memcmp(c.target.password,"ChangedPass",11));
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,700);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    CHECK(!memcmp(job.target.password,"ChangedPass",11));
    uint32_t op=job.operation_id;
    wpm_core_radio_result(&c,job.kind,op,WPM_REASON_NONE,701);
    wpm_core_ip_ready(&c,op,"192.168.1.21",702);
    CHECK(c.phase==WPM_PHASE_ONLINE);CHECK(c.store_pending);
    CHECK(c.config_revision!=c.durable_revision);
    CHECK(!memcmp(c.durable_config.profiles[0].password,"CoreSecret123!",14));
    old_operation=c.operation_id;
    CHECK(wpm_core_connect_saved(&c,id,703)==0);
    CHECK(c.operation_id!=old_operation);CHECK(c.leave_pending&&c.join_pending);
}
static void test_stale_and_forget(void)
{
    init();uint32_t op=connect("Home",true,0);store_ok();
    uint32_t id=c.config.profiles[0].id;
    CHECK(wpm_core_update_password(&c,id,"ChangedPass",10)==0);
    CHECK(wpm_core_next_job(&c,&job));wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,10);
    CHECK(wpm_core_next_job(&c,&job));uint32_t joining=job.operation_id;
    CHECK(wpm_core_forget(&c,id)==0);CHECK(c.config.count==0);
    wpm_core_radio_result(&c,WPM_CORE_JOB_JOIN,joining,WPM_REASON_NONE,12);
    wpm_core_ip_ready(&c,joining,"192.168.1.9",13);
    wpm_core_ip_ready(&c,op,"192.168.1.9",13);
    CHECK(c.config.count==0);CHECK(c.phase!=WPM_PHASE_ONLINE);CHECK(c.auto_suspended);
    while(wpm_core_next_job(&c,&job)) {
        if(job.kind==WPM_CORE_JOB_STORE) {
            CHECK(job.scrub_old);wpm_core_store_result(&c,job.config_revision,true);
        } else wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,14);
    }
    CHECK(c.storage==WPM_STORAGE_OK);
    wpm_core_tick(&c,100000);CHECK(!c.join_pending);
    CHECK(c.config.next_id>id);
}
static void test_auto(void)
{
    prepare_auto_profiles();
    wpm_core_tick(&c,UINT32_MAX-1000);
    CHECK(c.target.id==cfg.last_success_id);
    uint32_t definitive_auth_id=c.target.id;
    CHECK(wpm_core_next_job(&c,&job));
    uint32_t op=job.operation_id;
    wpm_core_radio_result(&c,WPM_CORE_JOB_JOIN,op,WPM_REASON_AUTH,UINT32_MAX-990);
    CHECK(profile_blocked(definitive_auth_id));
    CHECK(c.target.id==cfg.profiles[0].id);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,UINT32_MAX-989);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NOT_FOUND,UINT32_MAX-980);
    CHECK(wpm_core_next_job(&c,&job));wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,UINT32_MAX-979);
    CHECK(c.phase==WPM_PHASE_RETRY_WAIT);
    wpm_core_tick(&c,100);CHECK(c.phase==WPM_PHASE_RETRY_WAIT);
    wpm_core_tick(&c,30000);CHECK(c.target.id==cfg.profiles[0].id);
    CHECK(c.candidate_count==1&&c.candidates[0]==cfg.profiles[0].id);
    CHECK(profile_blocked(definitive_auth_id));
    wpm_core_disconnect(&c);
    while(wpm_core_next_job(&c,&job))
        wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,31000);
    wpm_core_tick(&c,99999);CHECK(!c.join_pending);
    init();connect("Home",true,0);store_ok();
    wpm_core_set_auto(&c,false);store_ok();
    cfg=c.config;wpm_core_init(&c);wpm_core_load(&c,&cfg,WPM_STORAGE_OK);
    wpm_core_tick(&c,0);CHECK(!c.join_pending);
}
static void test_auth_incomplete_retry_and_reason(void)
{
    CHECK(WPM_REASON_NONE==0&&WPM_REASON_NOT_FOUND==1&&WPM_REASON_AUTH==2);
    CHECK(WPM_REASON_UNSUPPORTED==3&&WPM_REASON_DHCP==4&&WPM_REASON_STORAGE==5);
    CHECK(WPM_REASON_IO==6&&WPM_REASON_CANCELLED==7&&WPM_REASON_FULL==8);
    CHECK(WPM_REASON_BUSY==9&&WPM_REASON_CORRUPT==10&&WPM_REASON_FOREIGN==11);
    CHECK(WPM_REASON_INVALID==12&&WPM_REASON_AUTH_INCOMPLETE==13);
    CHECK(strcmp(wpm_reason_name(WPM_REASON_AUTH_INCOMPLETE),
        "authentication_incomplete")==0);
    CHECK(WPM_CONFIG_FORMAT_VERSION==1U);

    prepare_auto_profiles();
    const uint32_t first_id=cfg.profiles[0].id;
    const uint32_t most_recent_id=cfg.last_success_id;
    const uint32_t saved_revision=c.config_revision;
    CHECK(first_id!=most_recent_id);
    wpm_core_tick(&c,1000);
    CHECK(c.target.id==most_recent_id);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    uint32_t op=job.operation_id;
    wpm_core_radio_result(&c,job.kind,op,WPM_REASON_AUTH_INCOMPLETE,1001);
    CHECK(c.reason==WPM_REASON_AUTH_INCOMPLETE);
    CHECK(c.target.id==first_id&&c.candidate_count==2&&c.candidate_next==2);
    CHECK(c.leave_pending&&c.join_pending&&!c.auto_suspended);
    CHECK(!profile_blocked(first_id)&&!profile_blocked(most_recent_id));
    CHECK(memcmp(&c.config,&cfg,sizeof(cfg))==0);
    CHECK(memcmp(&c.durable_config,&cfg,sizeof(cfg))==0);
    CHECK(c.config_revision==saved_revision&&c.durable_revision==saved_revision);
    CHECK(!c.store_pending&&!c.store_busy);

    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,1002);
    CHECK(c.reason==WPM_REASON_AUTH_INCOMPLETE);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    CHECK(job.target.id==first_id);
    wpm_core_radio_result(&c,job.kind,job.operation_id,
        WPM_REASON_AUTH_INCOMPLETE,1003);
    CHECK(c.phase==WPM_PHASE_RETRY_WAIT&&c.retry_due==31003U);
    CHECK(c.reason==WPM_REASON_AUTH_INCOMPLETE);
    CHECK(!profile_blocked(first_id)&&!profile_blocked(most_recent_id));
    CHECK(c.leave_pending&&!c.join_pending);
    CHECK(memcmp(&c.config,&cfg,sizeof(cfg))==0);
    CHECK(memcmp(&c.durable_config,&cfg,sizeof(cfg))==0);
    CHECK(c.config_revision==saved_revision&&c.durable_revision==saved_revision);
    CHECK(!c.store_pending&&!c.store_busy);

    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,1004);
    CHECK(c.phase==WPM_PHASE_RETRY_WAIT);
    wpm_core_tick(&c,31002);CHECK(c.phase==WPM_PHASE_RETRY_WAIT);
    CHECK(!wpm_core_next_job(&c,&job));
    wpm_core_tick(&c,31003);
    CHECK(c.target.id==most_recent_id&&c.join_pending);
    CHECK(c.candidate_count==2&&c.candidate_next==1);
    CHECK(!profile_blocked(first_id)&&!profile_blocked(most_recent_id));
    CHECK(c.reason==WPM_REASON_AUTH_INCOMPLETE);

    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    op=job.operation_id;
    wpm_core_radio_result(&c,job.kind,op,WPM_REASON_NONE,31004);
    CHECK(c.phase==WPM_PHASE_DHCP);
    CHECK(c.reason==WPM_REASON_AUTH_INCOMPLETE);
    wpm_core_ip_ready(&c,op+1,"192.168.1.25",31005);
    CHECK(c.phase==WPM_PHASE_DHCP&&c.reason==WPM_REASON_AUTH_INCOMPLETE);
    wpm_core_ip_ready(&c,op,"",31005);
    CHECK(c.phase==WPM_PHASE_DHCP&&c.reason==WPM_REASON_AUTH_INCOMPLETE);
    wpm_core_ip_ready(&c,op,"192.168.1.25",31006);
    CHECK(c.phase==WPM_PHASE_ONLINE&&c.ip[0]);
    CHECK(c.reason==WPM_REASON_NONE);
}
static void test_manual_disconnect_auto_off_and_retry(void)
{
    prepare_auto_profiles();
    const uint32_t retry_id=cfg.last_success_id;
    CHECK(wpm_core_connect_saved(&c,retry_id,100)==0);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_AUTH,101);
    CHECK(c.auto_suspended&&profile_blocked(retry_id));
    CHECK(c.reason==WPM_REASON_AUTH);
    CHECK(wpm_core_connect_saved(&c,retry_id,102)==0);
    CHECK(!c.auto_suspended&&!profile_blocked(retry_id));
    CHECK(c.reason==WPM_REASON_AUTH);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,103);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,104);
    CHECK(c.phase==WPM_PHASE_DHCP&&c.reason==WPM_REASON_AUTH);
    wpm_core_ip_ready(&c,c.operation_id,"192.168.1.26",105);
    CHECK(c.phase==WPM_PHASE_ONLINE&&c.reason==WPM_REASON_NONE);

    prepare_auto_profiles();
    wpm_core_tick(&c,200);
    CHECK(c.from_auto&&c.join_pending);
    wpm_core_disconnect(&c);
    CHECK(c.auto_suspended&&c.config.auto_connect);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,201);
    CHECK(c.phase==WPM_PHASE_DISCONNECTED&&c.auto_suspended);
    wpm_core_tick(&c,100000);
    CHECK(!c.join_pending&&!wpm_core_next_job(&c,&job));

    prepare_auto_profiles();
    wpm_core_tick(&c,300);
    CHECK(c.from_auto&&c.join_pending);
    wpm_core_set_auto(&c,false);
    CHECK(!c.config.auto_connect&&c.auto_suspended);
    while(wpm_core_next_job(&c,&job)) {
        if(job.kind==WPM_CORE_JOB_LEAVE)
            wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,301);
        else if(job.kind==WPM_CORE_JOB_STORE)
            wpm_core_store_result(&c,job.config_revision,WPM_STORE_CLEAN);
        else CHECK(0);
    }
    CHECK(c.phase==WPM_PHASE_DISCONNECTED&&c.config.count==2);
    wpm_core_tick(&c,1000000);
    CHECK(!c.join_pending&&!wpm_core_next_job(&c,&job));
}
static void start_auto_leave_retry(uint32_t now)
{
    init();connect("First",true,0);store_ok();connect("Second",true,30);store_ok();
    cfg=c.config;wpm_core_init(&c);wpm_core_load(&c,&cfg,WPM_STORAGE_OK);
    wpm_core_tick(&c,now);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    uint32_t join_op=job.operation_id;
    wpm_core_radio_result(&c,WPM_CORE_JOB_JOIN,join_op,WPM_REASON_AUTH,now+1);
    CHECK(c.from_auto&&c.leave_pending&&c.join_pending);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,WPM_CORE_JOB_LEAVE,job.operation_id,WPM_REASON_IO,now+2);
    CHECK(c.phase==WPM_PHASE_RETRY_WAIT);CHECK(c.reason==WPM_REASON_IO);
    CHECK(c.retry_due==now+30002U);CHECK(!c.join_pending&&!c.leave_pending);
}
static void test_auto_leave_retry_and_suspension(void)
{
    start_auto_leave_retry(100);
    CHECK(!wpm_core_next_job(&c,&job));
    wpm_core_tick(&c,30101);CHECK(c.phase==WPM_PHASE_RETRY_WAIT);
    CHECK(!wpm_core_next_job(&c,&job));
    wpm_core_tick(&c,30102);
    CHECK(c.leave_pending&&c.join_pending);CHECK(c.phase==WPM_PHASE_CANCELLING);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_NONE,30102);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_JOIN);
    CHECK(job.target.id==cfg.profiles[0].id);

    start_auto_leave_retry(40000);
    wpm_core_cancel(&c);
    CHECK(c.auto_suspended&&c.config.auto_connect);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_IO,40003);
    CHECK(c.phase==WPM_PHASE_FAILED);CHECK(c.auto_suspended);
    wpm_core_tick(&c,1000000);CHECK(c.auto_suspended&&!c.join_pending);
    CHECK(!wpm_core_next_job(&c,&job));

    start_auto_leave_retry(110000);
    wpm_core_set_auto(&c,false);
    CHECK(!c.config.auto_connect&&c.auto_suspended);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_LEAVE);
    wpm_core_radio_result(&c,job.kind,job.operation_id,WPM_REASON_IO,110003);
    CHECK(c.phase==WPM_PHASE_FAILED);CHECK(c.auto_suspended);
    wpm_core_tick(&c,1000000);
    while(wpm_core_next_job(&c,&job)) {
        CHECK(job.kind!=WPM_CORE_JOB_JOIN);
        if(job.kind==WPM_CORE_JOB_STORE)
            wpm_core_store_result(&c,job.config_revision,WPM_STORE_CLEAN);
    }
    CHECK(c.auto_suspended&&!c.join_pending);
}
static void test_dhcp_store(void)
{
    init();wpm_core_connect(&c,(uint8_t*)"Home",4,WPM_SECURITY_WPA2,"CoreSecret123!",true,0);
    radio_join_ok(0);wpm_core_tick(&c,8000);CHECK(c.dhcp_reset_requested);
    CHECK(c.config.count==0);wpm_core_tick(&c,16000);
    CHECK(c.reason==WPM_REASON_DHCP);CHECK(c.config.count==0);
    init();connect("Home",true,0);
    CHECK(wpm_core_next_job(&c,&job));CHECK(job.kind==WPM_CORE_JOB_STORE);
    wpm_core_store_result(&c,job.config_revision,false);
    CHECK(c.phase==WPM_PHASE_ONLINE);CHECK(c.storage==WPM_STORAGE_FAILED);
    CHECK(wpm_core_retry_save(&c)==0);store_ok();
    CHECK(c.storage==WPM_STORAGE_OK);
    wpm_core_forget_all(&c);CHECK(c.config.count==0);
}
static void test_public_error_and_adapter_contracts(void)
{
    wpm_core_adapter_state_t adapter;
    wpm_core_job_t radio_job;
    uint32_t active_id;

    init();
    cfg=c.config;cfg.count=1U;cfg.profiles[0].id=1U;
    wpm_core_load(&c,&cfg,WPM_STORAGE_OK);
    wpm_core_adapter_snapshot(&c,&adapter);
    CHECK(adapter.storage==WPM_STORAGE_CORRUPT);
    CHECK(adapter.reason==WPM_REASON_CORRUPT);
    CHECK(adapter.phase==WPM_PHASE_IDLE&&adapter.store_revision==0U);
    CHECK(adapter.active_target_is_durable==false);

    init();
    CHECK(wpm_core_connect(&c,(const uint8_t*)"Pending",7U,
        WPM_SECURITY_WPA2,"CoreSecret123!",true,10U)==0);
    active_id=c.target.id;
    CHECK(c.config.count==0U&&c.target_valid);
    CHECK(wpm_core_forget(&c,active_id)==0);
    CHECK(!c.target_valid&&c.leave_pending&&!c.join_pending);

    init();
    CHECK(wpm_core_connect(&c,(const uint8_t*)"Radio",5U,
        WPM_SECURITY_WPA2,"CoreSecret123!",false,20U)==0);
    CHECK(wpm_core_next_job(&c,&radio_job)&&radio_job.kind==WPM_CORE_JOB_JOIN);
    wpm_core_t before=c;
    wpm_core_radio_result(&c,WPM_CORE_JOB_STORE,radio_job.operation_id,
        WPM_REASON_NONE,21U);
    CHECK(memcmp(&c,&before,sizeof(c))==0);
    wpm_core_radio_result(&c,WPM_CORE_JOB_LEAVE,radio_job.operation_id,
        WPM_REASON_NONE,21U);
    CHECK(memcmp(&c,&before,sizeof(c))==0);
    wpm_core_radio_result(&c,WPM_CORE_JOB_JOIN,radio_job.operation_id,
        WPM_REASON_NONE,21U);
    CHECK(c.phase==WPM_PHASE_DHCP&&!c.radio_busy);
    before=c;
    wpm_core_radio_result(&c,WPM_CORE_JOB_JOIN,radio_job.operation_id,
        WPM_REASON_NONE,22U);
    CHECK(memcmp(&c,&before,sizeof(c))==0);

    init();
    CHECK(wpm_core_connect(&c,(const uint8_t*)"Store",5U,
        WPM_SECURITY_WPA2,"CoreSecret123!",true,30U)==0);
    radio_join_ok(30U);
    wpm_core_ip_ready(&c,c.operation_id,"2001:db8::1",31U);
    CHECK(c.phase==WPM_PHASE_ONLINE);
    CHECK(strcmp(c.ip,"2001:db8::1")==0);
    CHECK(wpm_core_next_job(&c,&job)&&job.kind==WPM_CORE_JOB_STORE);
    wpm_core_store_result(&c,job.config_revision,(wpm_store_outcome_t)99);
    CHECK(!c.store_busy&&c.storage==WPM_STORAGE_FAILED);
    CHECK(c.reason==WPM_REASON_STORAGE&&c.durable_config.count==0U);
    CHECK(c.config.count==1U&&c.config_revision!=c.durable_revision);

    init();
    CHECK(wpm_core_connect(&c,(const uint8_t*)"DHCP",4U,
        WPM_SECURITY_WPA2,"CoreSecret123!",false,40U)==0);
    radio_join_ok(40U);
    wpm_core_link_lost(&c,c.operation_id,41U);
    CHECK(c.phase==WPM_PHASE_FAILED&&c.reason==WPM_REASON_IO);
}
static void test_wire(void)
{
    init();connect("Home",true,0);store_ok();cfg=c.config;
    size_t n=0;CHECK(wpm_config_encode(&cfg,wire,sizeof(wire),&n)==0);
    CHECK(wire[4]==1U);
    CHECK(wpm_config_decode(&decoded,wire,n)==0);
    CHECK(decoded.profiles[0].id==cfg.profiles[0].id);
    CHECK(!memcmp(decoded.profiles[0].password,cfg.profiles[0].password,13));
    for(size_t cut=0;cut<n;cut++)CHECK(wpm_config_decode(&decoded,wire,cut)!=0);
    uint32_t random=0x173b4891;
    for(unsigned iteration=0;iteration<10000;iteration++) {
        uint8_t test[WPM_CONFIG_WIRE_MAX];memcpy(test,wire,n);
        random=random*1664525U+1013904223U;
        test[random%n]^=(uint8_t)((random>>24)|1);
        int result=wpm_config_decode(&decoded,test,n);
        if(!result)CHECK(wpm_config_valid(&decoded));else CHECK(result==WPM_EINVAL);
    }
    CHECK(wpm_config_decode(&decoded,wire,n)==0);
    uint8_t bad[WPM_CONFIG_WIRE_MAX];memcpy(bad,wire,n);
    bad[20]=1U;CHECK(wpm_config_decode(&decoded,bad,n)==WPM_EINVAL);
    CHECK(decoded.count==0U&&decoded.profiles[0].password_length==0U);
    memcpy(bad,wire,n);bad[24U+12U+4U]=1U;
    CHECK(wpm_config_decode(&decoded,bad,n)==WPM_EINVAL);
    memcpy(bad,wire,n);bad[24U+44U+14U]=1U;
    CHECK(wpm_config_decode(&decoded,bad,n)==WPM_EINVAL);
    memcpy(bad,wire,n);bad[24U+107U]=1U;
    CHECK(wpm_config_decode(&decoded,bad,n)==WPM_EINVAL);
    wpm_public_snapshot_t public_state;wpm_core_snapshot(&c,0,&public_state);
    const uint8_t *p=(const uint8_t*)&public_state;
    for(size_t i=0;i+13<=sizeof(public_state);i++)
        CHECK(memcmp(p+i,"CoreSecret123!",13)!=0);
}
int main(void)
{
    test_profiles();test_saved_connect_idempotent_and_reconnect();
    test_stale_and_forget();test_auto();test_auto_leave_retry_and_suspension();
    test_auth_incomplete_retry_and_reason();
    test_manual_disconnect_auto_off_and_retry();
    test_dhcp_store();test_public_error_and_adapter_contracts();test_wire();
    printf("PASS wpm_manager_core checks=%u seeded_boundary_inputs=10000 NDEBUG=1\n",checks);
    return 0;
}
