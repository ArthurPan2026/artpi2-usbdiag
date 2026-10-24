#include <rtthread.h>
#include "wifi_profile_manager.h"

static wpm_core_t g_core;

static uint32_t wpm_example_now_ms(void)
{
    return (uint32_t)(((uint64_t)rt_tick_get() * 1000U) /
                      RT_TICK_PER_SECOND);
}

static int wifi_profile_manager_startup_example(void)
{
    wpm_core_job_t job;
    static const uint8_t ssid[] = "wpm-test";
    wpm_core_init(&g_core);
    wpm_core_load(&g_core, RT_NULL, WPM_STORAGE_EMPTY);
    if (wpm_core_connect(&g_core, ssid, sizeof(ssid) - 1U,
                         WPM_SECURITY_OPEN, RT_NULL, RT_FALSE,
                         wpm_example_now_ms()) != 0) {
        rt_kprintf("WIFI_PROFILE_MANAGER_TEST: FAIL connect\\n");
        return -RT_ERROR;
    }
    if (!wpm_core_next_job(&g_core, &job) || job.kind != WPM_CORE_JOB_JOIN) {
        rt_kprintf("WIFI_PROFILE_MANAGER_TEST: FAIL job\\n");
        return -RT_ERROR;
    }
    wpm_secure_zero(&job, sizeof(job));
    rt_kprintf("WIFI_PROFILE_MANAGER_TEST: PASS version=0.1.0 jobs=1\\n");
    return RT_EOK;
}
INIT_APP_EXPORT(wifi_profile_manager_startup_example);

#ifdef RT_USING_FINSH
static int wifi_profile_manager_example(int argc, char **argv)
{
    wpm_core_job_t job;
    if (argc < 2) {
        rt_kprintf("usage: wifi_profile_manager_example <ssid>\\n");
        return -RT_ERROR;
    }
    wpm_core_init(&g_core);
    wpm_core_load(&g_core, RT_NULL, WPM_STORAGE_EMPTY);
    if (wpm_core_connect(&g_core, (const uint8_t *)argv[1],
                         rt_strlen(argv[1]), WPM_SECURITY_OPEN,
                         RT_NULL, RT_FALSE, wpm_example_now_ms()) != 0) {
        rt_kprintf("profile request rejected\\n");
        return -RT_ERROR;
    }
    if (wpm_core_next_job(&g_core, &job) && job.kind == WPM_CORE_JOB_JOIN) {
        rt_kprintf("radio adapter should execute JOIN operation %u\\n",
                   (unsigned)job.operation_id);
        wpm_secure_zero(&job, sizeof(job));
        return RT_EOK;
    }
    return -RT_ERROR;
}
MSH_CMD_EXPORT(wifi_profile_manager_example, demonstrate the radio adapter boundary);
#endif
