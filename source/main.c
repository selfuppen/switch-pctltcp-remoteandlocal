// pctltcp-sysmodule - Switch Parental Control Offline Grant
// Build: make -> pctltcp-sysmodule.nsp (with APP_JSON)
// Install: sd:/atmosphere/contents/010000000000BD23/exefs.nsp + flags/boot2.flag

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "grant_manager.h"

/* ================================================================
 * Sysmodule CRT0 overrides - CRITICAL for boot survival
 * ================================================================ */

#define INNER_HEAP_SIZE 0x80000  /* 512 KiB */

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

Result __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) return rc;

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) return rc;

    rc = timeInitialize();
    if (R_FAILED(rc)) {
        /* Non-fatal: timestamps may be wrong but module can run. */
    }

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) return rc;

    return 0;
}

void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    timeExit();
    smExit();
}

/* ---- Constants ---- */
#define PROGRAM_ID  0x010000000000BD23ULL
#define LOG_FILE    "sdmc:/switch/pctltcp-sysmodule/sysmodule.log"
#define MAX_LOG_SIZE (100 * 1024)

/* ---- Logging ---- */
static void rotate_log_if_needed(void) {
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size > MAX_LOG_SIZE) {
            char old[256];
            snprintf(old, sizeof(old), "%s.old", LOG_FILE);
            rename(LOG_FILE, old);
        }
    }
}

void log_msg(const char *msg) {
    rotate_log_if_needed();
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        u64 now_posix = 0;
        Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_LocalSystemClock, &now_posix);
        }
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_UserSystemClock, &now_posix);
        }

        if (R_SUCCEEDED(rc) && now_posix > 946684800ULL) {
            TimeCalendarTime cal;
            TimeCalendarAdditionalInfo additional;

            rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            if (R_FAILED(rc)) {
                rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            }

            if (R_SUCCEEDED(rc)) {
                fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                        cal.year, cal.month, cal.day,
                        cal.hour, cal.minute, cal.second, msg);
                fclose(f);
                return;
            }
        }
        fprintf(f, "[?] %s\n", msg);
        fclose(f);
    }
}

static void log_result(const char *ctx, Result rc) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s (0x%08X)",
             ctx, R_SUCCEEDED(rc) ? "OK" : "FAILED", (unsigned)rc);
    log_msg(buf);
}

static void ensure_play_timer_enabled(void) {
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        log_result("pctl_init for play timer", rc);
        return;
    }

    bool enabled = false;
    rc = pctl_is_enabled(&enabled);
    if (R_FAILED(rc)) {
        log_result("pctl_is_enabled", rc);
    }

    if (R_FAILED(rc) || !enabled) {
        rc = pctl_start_play_timer();
        log_result("pctl_start_play_timer", rc);
    } else {
        log_msg("PCTL play timer already enabled.");
    }

    pctl_exit();
}

static Result init_services(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting (offline grant mode)...");

    grant_manager_init();

    Result tz_rc = pctl_load_timezone();
    if (R_FAILED(tz_rc)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "WARNING: timezone load failed (0x%08X)", (unsigned)tz_rc);
        log_msg(buf);
    } else {
        log_msg("Timezone rule loaded successfully.");
    }

    u64 test_time = 0;
    Result time_rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &test_time);
    if (R_FAILED(time_rc)) {
        time_rc = timeGetCurrentTime(TimeType_UserSystemClock, &test_time);
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "Time service: %s (time=%llu, rc=0x%08X)",
             R_SUCCEEDED(time_rc) ? "OK" : "FAILED",
             (unsigned long long)test_time, (unsigned)time_rc);
    log_msg(buf);

    grant_manager_log_pctl_status("boot PCTL status");

    if (grant_manager_should_enforce_play_timer()) {
        ensure_play_timer_enabled();
    } else {
        char mode_msg[128];
        snprintf(mode_msg, sizeof(mode_msg),
                 "Skipping boot play timer enforcement (mode=%s, disabled=%s).",
                 grant_manager_control_mode_name(),
                 grant_manager_is_disabled() ? "true" : "false");
        log_msg(mode_msg);
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    (void)PROGRAM_ID;

    /* At boot2, many services are not yet registered with SM. */
    svcSleepThread(15000000000ULL);

    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: service initialization failed, entering request loop anyway.");
    }

    log_msg("pctltcp-sysmodule initialization complete (offline grant mode).");

    u64 loop = 0;
    while (1) {
        svcSleepThread(1000000000ULL);
        loop++;

        grant_manager_process();

        if ((loop % 60) == 0) {
            char hb[128];
            snprintf(hb, sizeof(hb),
                     "main loop status (loop=%llu, mode=offline)",
                     (unsigned long long)loop);
            log_msg(hb);
        }
    }

    return 0;
}
