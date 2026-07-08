#ifndef GRANT_MANAGER_H
#define GRANT_MANAGER_H

#include <switch.h>
#include <stdbool.h>

#define GRANT_DIR          "sdmc:/switch/pctltcp-sysmodule"
#define GRANT_CONFIG_PATH  GRANT_DIR "/grant.conf"
#define GRANT_REQUEST_PATH GRANT_DIR "/grant_request.json"
#define GRANT_RESULT_PATH  GRANT_DIR "/grant_result.json"
#define GRANT_USED_PATH    GRANT_DIR "/used_grants.dat"
#define GRANT_DISABLE_PATH GRANT_DIR "/disable.flag"
#define GRANT_BACKUP_PATH  GRANT_DIR "/last_pctl_backup.dat"
#define TIME_RULES_PATH    GRANT_DIR "/time_rules.json"
#define TIME_STATE_PATH    GRANT_DIR "/time_state.json"
#define EVENTS_PATH        GRANT_DIR "/events.jsonl"
#define MONTHLY_REPORT_PATH GRANT_DIR "/monthly_report.txt"

typedef enum {
    GRANT_CONTROL_DISABLED = 0,
    GRANT_CONTROL_OBSERVE,
    GRANT_CONTROL_GRANT,
    GRANT_CONTROL_ENFORCE,
} GrantControlMode;

void grant_manager_init(void);
void grant_manager_process(void);
GrantControlMode grant_manager_control_mode(void);
const char *grant_manager_control_mode_name(void);
bool grant_manager_is_disabled(void);
bool grant_manager_should_enforce_play_timer(void);
void grant_manager_log_pctl_status(const char *context);

#endif /* GRANT_MANAGER_H */
