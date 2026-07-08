#include "grant_manager.h"

#include "pctl_handler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void log_msg(const char *msg);

#define GRANT_VERSION          1
#define GRANT_ACTION_ADD_TODAY 0
#define GRANT_CODE_SYMBOLS     16
#define GRANT_TOKEN_BYTES      10
#define GRANT_PAYLOAD_BYTES    6
#define GRANT_DEFAULT_MAX_MIN  120

#define CFG_DEVICE_MAX 64
#define CFG_SECRET_MAX 128
#define RULE_DAYS 7
#define RULE_DEFAULT_LIMIT 120
#define RULE_UNSET_DATE 0

typedef enum {
    LOCAL_RULE_LIMIT = 0,
    LOCAL_RULE_UNLIMITED = 1,
    LOCAL_RULE_BLOCKED = 2,
} LocalRuleMode;

typedef enum {
    LIMIT_ACTION_REMIND = 0,
    LIMIT_ACTION_SUSPEND = 1,
} LimitAction;

typedef struct {
    char device_id[CFG_DEVICE_MAX];
    char grant_secret[CFG_SECRET_MAX];
    int max_add_minutes;
    GrantControlMode control_mode;
    bool allow_unlimited_to_limited;
    bool loaded;
} GrantConfig;

typedef struct {
    int version;
    int action;
    int minutes;
    uint16_t date_key;
    uint16_t nonce;
    uint32_t mac;
} GrantToken;

typedef struct {
    LocalRuleMode mode;
    int minutes;
} DayRule;

typedef struct {
    DayRule week[RULE_DAYS];
    bool today_override;
    uint16_t today_date_key;
    DayRule today;
    bool bedtime_enabled;
    int bedtime_start_min;
    int bedtime_end_min;
    LimitAction limit_action;
    bool raw_block_verified;
    bool suspend_verified;
    bool loaded;
} TimeRules;

typedef struct {
    uint16_t date_key;
    bool parent_unlock_active;
    u64 parent_unlock_until;
    bool bedtime_active;
    int last_applied_limit;
} TimeState;

static GrantConfig s_cfg;
static TimeRules s_rules;
static TimeState s_time_state;
static Mutex s_pctl_mutex;

static bool get_current_date_key(uint16_t *date_key);

static const char *control_mode_name(GrantControlMode mode) {
    switch (mode) {
        case GRANT_CONTROL_DISABLED: return "disabled";
        case GRANT_CONTROL_OBSERVE:  return "observe";
        case GRANT_CONTROL_GRANT:    return "grant";
        case GRANT_CONTROL_ENFORCE:  return "enforce";
        default:                     return "observe";
    }
}

static GrantControlMode parse_control_mode(const char *text) {
    if (!text) return GRANT_CONTROL_OBSERVE;
    if (strcmp(text, "disabled") == 0) return GRANT_CONTROL_DISABLED;
    if (strcmp(text, "observe") == 0) return GRANT_CONTROL_OBSERVE;
    if (strcmp(text, "grant") == 0) return GRANT_CONTROL_GRANT;
    if (strcmp(text, "enforce") == 0) return GRANT_CONTROL_ENFORCE;
    return GRANT_CONTROL_OBSERVE;
}

static const char *local_rule_mode_name(LocalRuleMode mode) {
    switch (mode) {
        case LOCAL_RULE_LIMIT: return "limit";
        case LOCAL_RULE_UNLIMITED: return "unlimited";
        case LOCAL_RULE_BLOCKED: return "blocked";
        default: return "limit";
    }
}

static LocalRuleMode parse_local_rule_mode(const char *text) {
    if (!text) return LOCAL_RULE_LIMIT;
    if (strcmp(text, "limit") == 0) return LOCAL_RULE_LIMIT;
    if (strcmp(text, "unlimited") == 0) return LOCAL_RULE_UNLIMITED;
    if (strcmp(text, "blocked") == 0) return LOCAL_RULE_BLOCKED;
    return LOCAL_RULE_LIMIT;
}

static const char *limit_action_name(LimitAction action) {
    return action == LIMIT_ACTION_SUSPEND ? "suspend" : "remind";
}

static LimitAction parse_limit_action(const char *text) {
    if (text && strcmp(text, "suspend") == 0) return LIMIT_ACTION_SUSPEND;
    return LIMIT_ACTION_REMIND;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Minimal JSON helpers                                                      */
/* ------------------------------------------------------------------------- */

static const char *json_find_value(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':') {
        pos++;
    }
    return pos;
}

static bool json_read_string(const char *value, char *buf, size_t bufsize) {
    if (!value || *value != '"' || !buf || bufsize == 0) return false;
    value++;
    size_t i = 0;
    while (*value && *value != '"' && i < bufsize - 1) {
        if (*value == '\\' && *(value + 1)) value++;
        buf[i++] = *value++;
    }
    buf[i] = '\0';
    return (*value == '"');
}

static bool json_read_int(const char *value, int *out) {
    if (!value || !out) return false;
    char *end = NULL;
    long val = strtol(value, &end, 10);
    if (end == value) return false;
    *out = (int)val;
    return true;
}

static bool json_read_bool(const char *value, bool *out) {
    if (!value || !out) return false;
    if (strncmp(value, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_read_named_int(const char *json, const char *key, int *out) {
    const char *val = json_find_value(json, key);
    return val && json_read_int(val, out);
}

static bool json_read_named_bool(const char *json, const char *key, bool *out) {
    const char *val = json_find_value(json, key);
    return val && json_read_bool(val, out);
}

static bool json_read_named_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *val = json_find_value(json, key);
    return val && json_read_string(val, out, out_size);
}

static bool read_file_text(const char *path, char *buf, size_t bufsize) {
    if (!path || !buf || bufsize == 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, bufsize - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    return true;
}

static void default_time_rules(TimeRules *rules) {
    if (!rules) return;
    memset(rules, 0, sizeof(*rules));
    for (int i = 0; i < RULE_DAYS; i++) {
        rules->week[i].mode = LOCAL_RULE_UNLIMITED;
        rules->week[i].minutes = RULE_DEFAULT_LIMIT;
    }
    rules->today.mode = LOCAL_RULE_LIMIT;
    rules->today.minutes = RULE_DEFAULT_LIMIT;
    rules->bedtime_start_min = 21 * 60;
    rules->bedtime_end_min = 8 * 60;
    rules->limit_action = LIMIT_ACTION_REMIND;
    rules->loaded = true;
}

static bool write_time_rules(const TimeRules *rules) {
    if (!rules) return false;

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", TIME_RULES_PATH);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"today_override\": %s,\n", rules->today_override ? "true" : "false");
    fprintf(f, "  \"today_date_key\": %u,\n", (unsigned)rules->today_date_key);
    fprintf(f, "  \"today_mode\": \"%s\",\n", local_rule_mode_name(rules->today.mode));
    fprintf(f, "  \"today_minutes\": %d,\n", rules->today.minutes);
    for (int i = 0; i < RULE_DAYS; i++) {
        fprintf(f, "  \"day%d_mode\": \"%s\",\n", i, local_rule_mode_name(rules->week[i].mode));
        fprintf(f, "  \"day%d_minutes\": %d,\n", i, rules->week[i].minutes);
    }
    fprintf(f, "  \"bedtime_enabled\": %s,\n", rules->bedtime_enabled ? "true" : "false");
    fprintf(f, "  \"bedtime_start_min\": %d,\n", rules->bedtime_start_min);
    fprintf(f, "  \"bedtime_end_min\": %d,\n", rules->bedtime_end_min);
    fprintf(f, "  \"limit_action\": \"%s\",\n", limit_action_name(rules->limit_action));
    fprintf(f, "  \"raw_block_verified\": %s,\n", rules->raw_block_verified ? "true" : "false");
    fprintf(f, "  \"suspend_verified\": %s\n", rules->suspend_verified ? "true" : "false");
    fprintf(f, "}\n");

    fclose(f);
    remove(TIME_RULES_PATH);
    return rename(tmp_path, TIME_RULES_PATH) == 0;
}

static void load_time_rules(void) {
    default_time_rules(&s_rules);

    char buf[4096];
    if (!read_file_text(TIME_RULES_PATH, buf, sizeof(buf))) {
        write_time_rules(&s_rules);
        return;
    }

    bool b = false;
    int n = 0;
    char text[32];

    if (json_read_named_bool(buf, "today_override", &b)) s_rules.today_override = b;
    if (json_read_named_int(buf, "today_date_key", &n)) s_rules.today_date_key = (uint16_t)n;
    if (json_read_named_string(buf, "today_mode", text, sizeof(text))) s_rules.today.mode = parse_local_rule_mode(text);
    if (json_read_named_int(buf, "today_minutes", &n)) s_rules.today.minutes = n;

    for (int i = 0; i < RULE_DAYS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "day%d_mode", i);
        if (json_read_named_string(buf, key, text, sizeof(text))) {
            s_rules.week[i].mode = parse_local_rule_mode(text);
        }
        snprintf(key, sizeof(key), "day%d_minutes", i);
        if (json_read_named_int(buf, key, &n)) {
            s_rules.week[i].minutes = n;
        }
        if (s_rules.week[i].minutes < 1 || s_rules.week[i].minutes > 1440) {
            s_rules.week[i].minutes = RULE_DEFAULT_LIMIT;
        }
    }

    if (json_read_named_bool(buf, "bedtime_enabled", &b)) s_rules.bedtime_enabled = b;
    if (json_read_named_int(buf, "bedtime_start_min", &n)) s_rules.bedtime_start_min = n;
    if (json_read_named_int(buf, "bedtime_end_min", &n)) s_rules.bedtime_end_min = n;
    if (json_read_named_string(buf, "limit_action", text, sizeof(text))) s_rules.limit_action = parse_limit_action(text);
    if (json_read_named_bool(buf, "raw_block_verified", &b)) s_rules.raw_block_verified = b;
    if (json_read_named_bool(buf, "suspend_verified", &b)) s_rules.suspend_verified = b;

    if (s_rules.today.minutes < 1 || s_rules.today.minutes > 1440) s_rules.today.minutes = RULE_DEFAULT_LIMIT;
    if (s_rules.bedtime_start_min < 0 || s_rules.bedtime_start_min >= 1440) s_rules.bedtime_start_min = 21 * 60;
    if (s_rules.bedtime_end_min < 0 || s_rules.bedtime_end_min >= 1440) s_rules.bedtime_end_min = 8 * 60;
    s_rules.loaded = true;
}

static bool write_time_state(void) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", TIME_STATE_PATH);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"date_key\": %u,\n", (unsigned)s_time_state.date_key);
    fprintf(f, "  \"parent_unlock_active\": %s,\n", s_time_state.parent_unlock_active ? "true" : "false");
    fprintf(f, "  \"parent_unlock_until\": %llu,\n", (unsigned long long)s_time_state.parent_unlock_until);
    fprintf(f, "  \"bedtime_active\": %s,\n", s_time_state.bedtime_active ? "true" : "false");
    fprintf(f, "  \"last_applied_limit\": %d\n", s_time_state.last_applied_limit);
    fprintf(f, "}\n");

    fclose(f);
    remove(TIME_STATE_PATH);
    return rename(tmp_path, TIME_STATE_PATH) == 0;
}

static void load_time_state(void) {
    memset(&s_time_state, 0, sizeof(s_time_state));

    char buf[1024];
    if (!read_file_text(TIME_STATE_PATH, buf, sizeof(buf))) return;

    int n = 0;
    bool b = false;
    if (json_read_named_int(buf, "date_key", &n)) s_time_state.date_key = (uint16_t)n;
    if (json_read_named_bool(buf, "parent_unlock_active", &b)) s_time_state.parent_unlock_active = b;
    if (json_read_named_int(buf, "parent_unlock_until", &n)) s_time_state.parent_unlock_until = (u64)n;
    if (json_read_named_bool(buf, "bedtime_active", &b)) s_time_state.bedtime_active = b;
    if (json_read_named_int(buf, "last_applied_limit", &n)) s_time_state.last_applied_limit = n;
}

static bool get_now_posix(u64 *now_posix) {
    if (!now_posix) return false;
    Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, now_posix);
    if (R_FAILED(rc) || *now_posix <= 946684800ULL) {
        rc = timeGetCurrentTime(TimeType_LocalSystemClock, now_posix);
    }
    if (R_FAILED(rc) || *now_posix <= 946684800ULL) {
        rc = timeGetCurrentTime(TimeType_UserSystemClock, now_posix);
    }
    return R_SUCCEEDED(rc) && *now_posix > 946684800ULL;
}

static void append_event(const char *event, const char *status, const char *detail,
                         int old_limit, int new_limit) {
    FILE *f = fopen(EVENTS_PATH, "a");
    if (!f) return;

    u64 now_posix = 0;
    get_now_posix(&now_posix);
    uint16_t date_key = 0;
    get_current_date_key(&date_key);

    fprintf(f,
            "{\"ts\":%llu,\"date_key\":%u,\"event\":\"%s\",\"status\":\"%s\","
            "\"detail\":\"%s\",\"old_limit\":%d,\"new_limit\":%d,\"mode\":\"%s\"}\n",
            (unsigned long long)now_posix,
            (unsigned)date_key,
            event ? event : "unknown",
            status ? status : "unknown",
            detail ? detail : "",
            old_limit,
            new_limit,
            control_mode_name(s_cfg.control_mode));
    fclose(f);
}

static bool is_bedtime_active_now(void) {
    if (!s_rules.bedtime_enabled) return false;

    time_t now = 0;
    u64 now_posix = 0;
    if (!get_now_posix(&now_posix)) return false;
    now = (time_t)now_posix;

    struct tm *tm_info = localtime(&now);
    if (!tm_info) return false;

    int minute = tm_info->tm_hour * 60 + tm_info->tm_min;
    int start = s_rules.bedtime_start_min;
    int end = s_rules.bedtime_end_min;
    if (start == end) return false;
    if (start < end) return minute >= start && minute < end;
    return minute >= start || minute < end;
}

static void refresh_time_state(void) {
    if (s_time_state.parent_unlock_active && s_time_state.parent_unlock_until > 0) {
        u64 now = 0;
        if (get_now_posix(&now) && now >= s_time_state.parent_unlock_until) {
            s_time_state.parent_unlock_active = false;
            s_time_state.parent_unlock_until = 0;
            write_time_state();
            append_event("parent_unlock_expired", "ok", "timer_auto_resume_pending", -1, -1);
            mutexLock(&s_pctl_mutex);
            if (R_SUCCEEDED(pctl_init())) {
                pctl_start_play_timer();
                pctl_exit();
            }
            mutexUnlock(&s_pctl_mutex);
        }
    }
}

static DayRule active_rule_for_today(int today, uint16_t date_key) {
    if (today < 0 || today >= RULE_DAYS) today = 0;
    if (s_rules.today_override && s_rules.today_date_key == date_key) {
        return s_rules.today;
    }
    return s_rules.week[today];
}

typedef struct {
    bool limited_today;
    bool blocked_today;
    bool unrestricted_today;
    int today_limit;
    bool remaining_available;
    u32 remaining_minutes;
    bool play_timer_enabled;
    bool restricted_now;
    int today;
    uint16_t date_key;
    LocalRuleMode active_rule_mode;
    int active_rule_minutes;
    bool bedtime_active;
    bool parent_unlock_active;
} PctlStatusResult;

static void write_monthly_report(const PctlStatusResult *status) {
    int total_events = 0;
    int offline_ok = 0;
    int manual_ops = 0;
    int blocked_ops = 0;
    int errors = 0;

    FILE *events = fopen(EVENTS_PATH, "r");
    if (events) {
        char line[512];
        while (fgets(line, sizeof(line), events)) {
            total_events++;
            if (strstr(line, "\"event\":\"offline_code\"") && strstr(line, "\"status\":\"ok\"")) offline_ok++;
            if (strstr(line, "set_today_limit") || strstr(line, "add_today_minutes") ||
                strstr(line, "disable_today_limit") || strstr(line, "restore_today_policy")) manual_ops++;
            if (strstr(line, "block_today") || strstr(line, "bedtime_block") || strstr(line, "probe_raw_block")) blocked_ops++;
            if (strstr(line, "\"status\":\"error\"") || strstr(line, "\"status\":\"rejected\"")) errors++;
        }
        fclose(events);
    }

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", MONTHLY_REPORT_PATH);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    fprintf(f, "Local Time Manager Monthly Report\n");
    fprintf(f, "=================================\n\n");
    fprintf(f, "This report is estimated from pctltcp local events and PCTL remaining-time snapshots.\n");
    fprintf(f, "It is not Nintendo's official per-game monthly report.\n\n");
    fprintf(f, "Current mode: %s\n", control_mode_name(s_cfg.control_mode));
    fprintf(f, "Limit action: %s\n", limit_action_name(s_rules.limit_action));
    fprintf(f, "Raw block verified: %s\n", s_rules.raw_block_verified ? "true" : "false");
    fprintf(f, "Suspend verified: %s\n\n", s_rules.suspend_verified ? "true" : "false");
    fprintf(f, "Today limit: %d\n", status ? status->today_limit : 0);
    fprintf(f, "Today blocked: %s\n", status && status->blocked_today ? "true" : "false");
    fprintf(f, "Remaining available: %s\n", status && status->remaining_available ? "true" : "false");
    fprintf(f, "Remaining minutes: %u\n\n",
            status && status->remaining_available ? (unsigned)status->remaining_minutes : 0);
    fprintf(f, "Event totals in local log:\n");
    fprintf(f, "- all events: %d\n", total_events);
    fprintf(f, "- successful offline grants: %d\n", offline_ok);
    fprintf(f, "- manual time operations: %d\n", manual_ops);
    fprintf(f, "- block/probe operations: %d\n", blocked_ops);
    fprintf(f, "- rejected/error events: %d\n", errors);

    fclose(f);
    remove(MONTHLY_REPORT_PATH);
    rename(tmp_path, MONTHLY_REPORT_PATH);
}

static void write_result_ok(int applied_minutes, int today_limit, bool dry_run,
                            const PctlStatusResult *status) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", GRANT_RESULT_PATH);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    fprintf(f,
            "{\"status\":\"ok\",\"applied_minutes\":%d,\"today_limit\":%d,"
            "\"dry_run\":%s,\"mode\":\"%s\","
            "\"limited_today\":%s,\"blocked_today\":%s,\"unrestricted_today\":%s,"
            "\"remaining_available\":%s,\"remaining_minutes\":%u,"
            "\"play_timer_enabled\":%s,\"restricted_now\":%s,"
            "\"today\":%d,\"date_key\":%u,"
            "\"active_rule\":\"%s\",\"active_rule_minutes\":%d,"
            "\"bedtime_active\":%s,\"parent_unlock_active\":%s,"
            "\"limit_action\":\"%s\",\"raw_block_verified\":%s,"
            "\"suspend_verified\":%s}\n",
            applied_minutes, today_limit, dry_run ? "true" : "false",
            control_mode_name(s_cfg.control_mode),
            status && status->limited_today ? "true" : "false",
            status && status->blocked_today ? "true" : "false",
            status && status->unrestricted_today ? "true" : "false",
            status && status->remaining_available ? "true" : "false",
            status && status->remaining_available ? (unsigned)status->remaining_minutes : 0,
            status && status->play_timer_enabled ? "true" : "false",
            status && status->restricted_now ? "true" : "false",
            status ? status->today : -1,
            status ? (unsigned)status->date_key : 0,
            status ? local_rule_mode_name(status->active_rule_mode) : "limit",
            status ? status->active_rule_minutes : 0,
            status && status->bedtime_active ? "true" : "false",
            status && status->parent_unlock_active ? "true" : "false",
            limit_action_name(s_rules.limit_action),
            s_rules.raw_block_verified ? "true" : "false",
            s_rules.suspend_verified ? "true" : "false");

    fclose(f);
    remove(GRANT_RESULT_PATH);
    rename(tmp_path, GRANT_RESULT_PATH);
}

static void write_result_error(const char *reason) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", GRANT_RESULT_PATH);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    fprintf(f, "{\"status\":\"error\",\"reason\":\"%s\",\"mode\":\"%s\"}\n",
            reason ? reason : "unknown", control_mode_name(s_cfg.control_mode));

    fclose(f);
    remove(GRANT_RESULT_PATH);
    rename(tmp_path, GRANT_RESULT_PATH);
}

static void write_result_status(const PctlStatusResult *status) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", GRANT_RESULT_PATH);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    fprintf(f,
            "{\"status\":\"status\",\"today_limit\":%d,"
            "\"limited_today\":%s,\"blocked_today\":%s,\"unrestricted_today\":%s,"
            "\"remaining_available\":%s,\"remaining_minutes\":%u,"
            "\"mode\":\"%s\",\"play_timer_enabled\":%s,\"restricted_now\":%s,"
            "\"today\":%d,\"date_key\":%u,"
            "\"active_rule\":\"%s\",\"active_rule_minutes\":%d,"
            "\"bedtime_active\":%s,\"parent_unlock_active\":%s,"
            "\"limit_action\":\"%s\",\"raw_block_verified\":%s,"
            "\"suspend_verified\":%s}\n",
            status ? status->today_limit : 0,
            status && status->limited_today ? "true" : "false",
            status && status->blocked_today ? "true" : "false",
            status && status->unrestricted_today ? "true" : "false",
            status && status->remaining_available ? "true" : "false",
            status && status->remaining_available ? (unsigned)status->remaining_minutes : 0,
            control_mode_name(s_cfg.control_mode),
            status && status->play_timer_enabled ? "true" : "false",
            status && status->restricted_now ? "true" : "false",
            status ? status->today : -1,
            status ? (unsigned)status->date_key : 0,
            status ? local_rule_mode_name(status->active_rule_mode) : "limit",
            status ? status->active_rule_minutes : 0,
            status && status->bedtime_active ? "true" : "false",
            status && status->parent_unlock_active ? "true" : "false",
            limit_action_name(s_rules.limit_action),
            s_rules.raw_block_verified ? "true" : "false",
            s_rules.suspend_verified ? "true" : "false");

    fclose(f);
    remove(GRANT_RESULT_PATH);
    rename(tmp_path, GRANT_RESULT_PATH);
}

/* ------------------------------------------------------------------------- */
/* SHA-256 + HMAC-SHA256                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} Sha256Ctx;

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32 - (b))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

static void sha256_transform(Sha256Ctx *ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(Sha256Ctx *ctx, const uint8_t data[], size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t hash[]) {
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4]  = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8]  = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg1, size_t msg1_len,
                        const uint8_t *msg2, size_t msg2_len,
                        uint8_t out[32]) {
    uint8_t kopad[64];
    uint8_t kipad[64];
    uint8_t khash[32];
    uint8_t inner[32];

    if (key_len > 64) {
        Sha256Ctx key_ctx;
        sha256_init(&key_ctx);
        sha256_update(&key_ctx, key, key_len);
        sha256_final(&key_ctx, khash);
        key = khash;
        key_len = 32;
    }

    memset(kopad, 0x5c, sizeof(kopad));
    memset(kipad, 0x36, sizeof(kipad));
    for (size_t i = 0; i < key_len; i++) {
        kopad[i] ^= key[i];
        kipad[i] ^= key[i];
    }

    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, kipad, sizeof(kipad));
    sha256_update(&ctx, msg1, msg1_len);
    sha256_update(&ctx, (const uint8_t *)"\0", 1);
    sha256_update(&ctx, msg2, msg2_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, kopad, sizeof(kopad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------------- */
/* Grant token codec                                                         */
/* ------------------------------------------------------------------------- */

static int crockford_value(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9') return c - '0';
    if (c == 'O') return 0;
    if (c == 'I' || c == 'L') return 1;
    if (c >= 'A' && c <= 'H') return 10 + (c - 'A');
    if (c >= 'J' && c <= 'K') return 18 + (c - 'J');
    if (c >= 'M' && c <= 'N') return 20 + (c - 'M');
    if (c >= 'P' && c <= 'T') return 22 + (c - 'P');
    if (c >= 'V' && c <= 'Z') return 27 + (c - 'V');
    return -1;
}

static bool decode_code_bytes(const char *code, uint8_t out[GRANT_TOKEN_BYTES]) {
    memset(out, 0, GRANT_TOKEN_BYTES);
    int symbols = 0;
    int bitpos = 0;

    for (const char *p = code; p && *p; p++) {
        if (*p == '-' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        int val = crockford_value(*p);
        if (val < 0 || val > 31) return false;
        if (symbols >= GRANT_CODE_SYMBOLS) return false;

        for (int b = 4; b >= 0; b--) {
            if (val & (1 << b)) {
                int byte_index = bitpos / 8;
                int bit_index = 7 - (bitpos % 8);
                out[byte_index] |= (uint8_t)(1u << bit_index);
            }
            bitpos++;
        }
        symbols++;
    }

    return symbols == GRANT_CODE_SYMBOLS;
}

static void unpack_token(const uint8_t bytes[GRANT_TOKEN_BYTES], GrantToken *token) {
    uint64_t payload = ((uint64_t)bytes[0] << 40) | ((uint64_t)bytes[1] << 32) |
                       ((uint64_t)bytes[2] << 24) | ((uint64_t)bytes[3] << 16) |
                       ((uint64_t)bytes[4] << 8) | (uint64_t)bytes[5];

    token->version = (int)((payload >> 44) & 0x0f);
    token->action = (int)((payload >> 42) & 0x03);
    token->minutes = (int)((payload >> 32) & 0x03ff);
    token->date_key = (uint16_t)((payload >> 16) & 0xffff);
    token->nonce = (uint16_t)(payload & 0xffff);
    token->mac = ((uint32_t)bytes[6] << 24) | ((uint32_t)bytes[7] << 16) |
                 ((uint32_t)bytes[8] << 8) | (uint32_t)bytes[9];
}

static uint32_t expected_mac(const uint8_t payload[GRANT_PAYLOAD_BYTES]) {
    uint8_t digest[32];
    hmac_sha256((const uint8_t *)s_cfg.grant_secret, strlen(s_cfg.grant_secret),
                (const uint8_t *)s_cfg.device_id, strlen(s_cfg.device_id),
                payload, GRANT_PAYLOAD_BYTES, digest);
    return ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) |
           ((uint32_t)digest[2] << 8) | (uint32_t)digest[3];
}

/* ------------------------------------------------------------------------- */
/* Config, date, replay ledger                                               */
/* ------------------------------------------------------------------------- */

static void load_config(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.max_add_minutes = GRANT_DEFAULT_MAX_MIN;
    s_cfg.control_mode = GRANT_CONTROL_OBSERVE;
    s_cfg.allow_unlimited_to_limited = false;

    char buf[2048];
    if (!read_file_text(GRANT_CONFIG_PATH, buf, sizeof(buf))) {
        return;
    }

    const char *val = json_find_value(buf, "device_id");
    if (!val || !json_read_string(val, s_cfg.device_id, sizeof(s_cfg.device_id))) return;

    val = json_find_value(buf, "grant_secret");
    if (!val || !json_read_string(val, s_cfg.grant_secret, sizeof(s_cfg.grant_secret))) return;

    val = json_find_value(buf, "max_add_minutes");
    if (val) json_read_int(val, &s_cfg.max_add_minutes);
    if (s_cfg.max_add_minutes <= 0 || s_cfg.max_add_minutes > 1440) {
        s_cfg.max_add_minutes = GRANT_DEFAULT_MAX_MIN;
    }

    val = json_find_value(buf, "control_mode");
    if (val) {
        char mode[32];
        if (json_read_string(val, mode, sizeof(mode))) {
            s_cfg.control_mode = parse_control_mode(mode);
        }
    }

    val = json_find_value(buf, "allow_unlimited_to_limited");
    if (val) {
        json_read_bool(val, &s_cfg.allow_unlimited_to_limited);
    }

    s_cfg.loaded = true;
}

static bool get_current_date_key(uint16_t *date_key) {
    if (!date_key) return false;
    u16 today_key = 0;
    Result rc = pctl_get_today_info(NULL, &today_key);
    if (R_FAILED(rc)) return false;
    *date_key = today_key;
    return true;
}

static bool used_nonce_contains(uint16_t date_key, uint16_t nonce) {
    FILE *f = fopen(GRANT_USED_PATH, "r");
    if (!f) return false;

    unsigned int d = 0;
    unsigned int n = 0;
    while (fscanf(f, "%x %x", &d, &n) == 2) {
        if ((uint16_t)d == date_key && (uint16_t)n == nonce) {
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

static bool used_nonce_append(uint16_t date_key, uint16_t nonce) {
    FILE *f = fopen(GRANT_USED_PATH, "a");
    if (!f) return false;
    fprintf(f, "%04X %04X\n", (unsigned)date_key, (unsigned)nonce);
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------------- */
/* PCTL apply                                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    PlayTimerSettings settings;
    int today;
    uint16_t date_key;
    bool has_limited_today;
    bool blocked_today;
    bool unrestricted_today;
    u32 today_limit;
    u16 raw_flag;
    u16 raw_enable;
    u16 raw_minutes;
} PctlTodayState;

static bool read_pctl_today_state(PctlTodayState *state, const char **reason) {
    if (!state) {
        if (reason) *reason = "bad_request";
        return false;
    }
    memset(state, 0, sizeof(*state));

    Result rc = pctl_get_today_info(&state->today, &state->date_key);
    if (R_FAILED(rc)) {
        if (reason) *reason = "bad_clock";
        return false;
    }

    rc = pctl_get_settings(&state->settings);
    if (R_FAILED(rc)) {
        if (reason) *reason = "pctl_read_failed";
        return false;
    }

    state->raw_flag = state->settings.raw[PCTL_DAY_FLAG_OFFSET(state->today)];
    state->raw_enable = state->settings.raw[PCTL_DAY_ENABLE_OFFSET(state->today)];
    state->raw_minutes = state->settings.raw[PCTL_DAY_MINUTES_OFFSET(state->today)];

    state->has_limited_today =
        state->settings.raw[0] != 0 &&
        state->settings.raw[1] != 0 &&
        state->raw_flag == PCTL_DAY_CONFIGURED &&
        state->raw_enable == PCTL_DAY_RESTRICTED &&
        state->raw_minutes != PT_DAY_NOLIMIT &&
        state->raw_minutes != 0;

    state->blocked_today =
        state->settings.raw[0] != 0 &&
        state->settings.raw[1] != 0 &&
        state->raw_flag == PCTL_DAY_CONFIGURED &&
        state->raw_enable == PCTL_DAY_RESTRICTED &&
        state->raw_minutes == 0;

    state->unrestricted_today = !state->has_limited_today && !state->blocked_today;

    state->today_limit = state->has_limited_today ? (u32)state->raw_minutes : 0;
    return true;
}

static bool read_current_pctl_status(PctlStatusResult *status, const char **reason) {
    if (!status) {
        if (reason) *reason = "bad_request";
        return false;
    }
    memset(status, 0, sizeof(*status));

    mutexLock(&s_pctl_mutex);
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        mutexUnlock(&s_pctl_mutex);
        if (reason) *reason = "pctl_init_failed";
        return false;
    }

    PctlTodayState state;
    bool ok = read_pctl_today_state(&state, reason);
    if (ok) {
        DayRule active = active_rule_for_today(state.today, state.date_key);
        status->limited_today = state.has_limited_today;
        status->blocked_today = state.blocked_today;
        status->unrestricted_today = state.unrestricted_today;
        status->today_limit = state.has_limited_today ? (int)state.today_limit : 0;
        status->today = state.today;
        status->date_key = state.date_key;
        status->active_rule_mode = active.mode;
        status->active_rule_minutes = active.minutes;
        status->bedtime_active = is_bedtime_active_now();
        status->parent_unlock_active = s_time_state.parent_unlock_active;

        u64 remaining_ns = 0;
        rc = pctl_get_remaining_time(&remaining_ns);
        if (R_SUCCEEDED(rc)) {
            status->remaining_available = true;
            status->remaining_minutes = NS_TO_MINUTES(remaining_ns);
        }

        bool enabled = false;
        if (R_SUCCEEDED(pctl_is_enabled(&enabled))) status->play_timer_enabled = enabled;

        bool restricted = false;
        if (R_SUCCEEDED(pctl_is_restricted(&restricted))) status->restricted_now = restricted;
    }

    pctl_exit();
    mutexUnlock(&s_pctl_mutex);
    return ok;
}

static bool write_pctl_backup(const PctlTodayState *state, int new_limit) {
    if (!state) return false;

    FILE *f = fopen(GRANT_BACKUP_PATH, "w");
    if (!f) return false;

    u64 now_posix = 0;
    Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
    if (R_FAILED(rc) || now_posix <= 946684800ULL) {
        rc = timeGetCurrentTime(TimeType_LocalSystemClock, &now_posix);
    }
    if (R_FAILED(rc) || now_posix <= 946684800ULL) {
        rc = timeGetCurrentTime(TimeType_UserSystemClock, &now_posix);
    }

    fprintf(f, "version=1\n");
    fprintf(f, "posix_time=%llu\n", (unsigned long long)(R_SUCCEEDED(rc) ? now_posix : 0));
    fprintf(f, "control_mode=%s\n", control_mode_name(s_cfg.control_mode));
    fprintf(f, "today=%d\n", state->today);
    fprintf(f, "old_limited=%s\n", state->has_limited_today ? "true" : "false");
    fprintf(f, "old_limit=%u\n", (unsigned)state->today_limit);
    fprintf(f, "new_limit=%d\n", new_limit);
    fprintf(f, "raw_hex=");
    const uint8_t *raw = (const uint8_t *)state->settings.raw;
    for (size_t i = 0; i < sizeof(state->settings.raw); i++) {
        fprintf(f, "%02X", raw[i]);
    }
    fprintf(f, "\n");

    fclose(f);
    return true;
}

static bool control_mode_writes_pctl(void) {
    return s_cfg.control_mode == GRANT_CONTROL_GRANT || s_cfg.control_mode == GRANT_CONTROL_ENFORCE;
}

static int clamp_minutes(int minutes) {
    if (minutes < 1) return 1;
    if (minutes > 1440) return 1440;
    return minutes;
}

static int estimate_used_minutes(const PctlTodayState *state, bool remaining_available, u32 remaining_minutes) {
    if (!state || !state->has_limited_today || !remaining_available) return -1;
    int used = (int)state->today_limit - (int)remaining_minutes;
    return used < 0 ? 0 : used;
}

static bool apply_today_rule(LocalRuleMode mode, int minutes, const char *event_name,
                             int *today_limit_out, const char **reason) {
    if (mode == LOCAL_RULE_LIMIT) {
        if (minutes <= 0 || minutes > 1440) {
            if (reason) *reason = "invalid_minutes";
            return false;
        }
        minutes = clamp_minutes(minutes);
    }
    if (mode == LOCAL_RULE_BLOCKED && !s_rules.raw_block_verified) {
        if (reason) *reason = "raw_block_not_verified";
        append_event(event_name, "rejected", "raw_block_not_verified", -1, 0);
        return false;
    }

    mutexLock(&s_pctl_mutex);
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        mutexUnlock(&s_pctl_mutex);
        if (reason) *reason = "pctl_init_failed";
        append_event(event_name, "error", "pctl_init_failed", -1, minutes);
        return false;
    }

    PctlTodayState state;
    if (!read_pctl_today_state(&state, reason)) {
        pctl_exit();
        mutexUnlock(&s_pctl_mutex);
        append_event(event_name, "error", reason ? *reason : "read_failed", -1, minutes);
        return false;
    }

    u64 remaining_ns = 0;
    bool remaining_available = R_SUCCEEDED(pctl_get_remaining_time(&remaining_ns));
    u32 remaining_minutes = remaining_available ? NS_TO_MINUTES(remaining_ns) : 0;
    int old_limit = state.has_limited_today ? (int)state.today_limit : (state.blocked_today ? 0 : -1);
    int new_limit = (mode == LOCAL_RULE_LIMIT) ? minutes : (mode == LOCAL_RULE_BLOCKED ? 0 : -1);

    if (mode == LOCAL_RULE_LIMIT) {
        int used = estimate_used_minutes(&state, remaining_available, remaining_minutes);
        if (used >= 0 && minutes <= used) {
            minutes = used > 0 ? used : 1;
            new_limit = minutes;
        } else if (used < 0 && state.has_limited_today && minutes < (int)state.today_limit) {
            pctl_exit();
            mutexUnlock(&s_pctl_mutex);
            if (reason) *reason = "remaining_unavailable";
            append_event(event_name, "rejected", "remaining_unavailable", old_limit, minutes);
            return false;
        }
    }

    bool dry_run = !control_mode_writes_pctl();
    if (!dry_run) {
        if (!write_pctl_backup(&state, new_limit)) {
            pctl_exit();
            mutexUnlock(&s_pctl_mutex);
            if (reason) *reason = "pctl_backup_failed";
            append_event(event_name, "error", "pctl_backup_failed", old_limit, new_limit);
            return false;
        }

        if (mode == LOCAL_RULE_LIMIT) {
            rc = pctl_set_day_restricted(state.today, (u32)minutes);
        } else if (mode == LOCAL_RULE_UNLIMITED) {
            rc = pctl_set_day_unlimited(state.today);
        } else {
            rc = pctl_set_day_blocked(state.today);
        }

        if (R_SUCCEEDED(rc) && s_cfg.control_mode == GRANT_CONTROL_ENFORCE) {
            pctl_stop_play_timer();
            pctl_start_play_timer();
        }
    }

    pctl_exit();
    mutexUnlock(&s_pctl_mutex);

    if (R_FAILED(rc)) {
        if (reason) *reason = "pctl_write_failed";
        append_event(event_name, "error", "pctl_write_failed", old_limit, new_limit);
        return false;
    }

    if (today_limit_out) *today_limit_out = new_limit < 0 ? 0 : new_limit;
    s_time_state.date_key = state.date_key;
    s_time_state.last_applied_limit = new_limit;
    write_time_state();
    append_event(event_name, dry_run ? "dry_run" : "ok", local_rule_mode_name(mode), old_limit, new_limit);
    return true;
}

static bool save_today_override(LocalRuleMode mode, int minutes, uint16_t date_key) {
    s_rules.today_override = true;
    s_rules.today_date_key = date_key;
    s_rules.today.mode = mode;
    s_rules.today.minutes = clamp_minutes(minutes > 0 ? minutes : RULE_DEFAULT_LIMIT);
    return write_time_rules(&s_rules);
}

static bool clear_today_override(void) {
    s_rules.today_override = false;
    s_rules.today_date_key = RULE_UNSET_DATE;
    return write_time_rules(&s_rules);
}

static void enforce_bedtime_transition(void) {
    bool active = is_bedtime_active_now();
    if (active == s_time_state.bedtime_active) return;

    uint16_t date_key = 0;
    int today = 0;
    pctl_get_today_info(&today, &date_key);

    if (active) {
        s_time_state.bedtime_active = true;
        write_time_state();
        if (s_rules.limit_action == LIMIT_ACTION_SUSPEND &&
            s_rules.raw_block_verified &&
            s_cfg.control_mode == GRANT_CONTROL_ENFORCE &&
            !s_time_state.parent_unlock_active) {
            const char *reason = NULL;
            int ignored = 0;
            apply_today_rule(LOCAL_RULE_BLOCKED, 0, "bedtime_block", &ignored, &reason);
        } else {
            append_event("bedtime_active", "remind", "no_pctl_write", -1, -1);
        }
        return;
    }

    s_time_state.bedtime_active = false;
    write_time_state();
    if (s_cfg.control_mode == GRANT_CONTROL_ENFORCE && !s_time_state.parent_unlock_active) {
        DayRule rule = active_rule_for_today(today, date_key);
        const char *reason = NULL;
        int ignored = 0;
        apply_today_rule(rule.mode, rule.minutes, "bedtime_restore", &ignored, &reason);
    } else {
        append_event("bedtime_inactive", "ok", "restore_pending_or_observe", -1, -1);
    }
}

static bool observe_add_minutes(int minutes, int *today_limit_out, const char **reason) {
    mutexLock(&s_pctl_mutex);
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        mutexUnlock(&s_pctl_mutex);
        *reason = "pctl_init_failed";
        return false;
    }

    PctlTodayState state;
    bool ok = read_pctl_today_state(&state, reason);
    pctl_exit();
    mutexUnlock(&s_pctl_mutex);

    if (!ok) return false;

    if (!state.has_limited_today && !s_cfg.allow_unlimited_to_limited) {
        *reason = "unlimited_not_allowed";
        return false;
    }

    int current = state.has_limited_today ? (int)state.today_limit : 0;
    int new_limit = current + minutes;
    if (new_limit > 1440) new_limit = 1440;
    if (today_limit_out) *today_limit_out = new_limit;
    return true;
}

static bool apply_add_minutes(int minutes, int *today_limit_out, const char **reason) {
    if (minutes <= 0) {
        *reason = "invalid_minutes";
        return false;
    }
    if (minutes > s_cfg.max_add_minutes) {
        *reason = "minutes_exceed_limit";
        return false;
    }

    mutexLock(&s_pctl_mutex);
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        mutexUnlock(&s_pctl_mutex);
        *reason = "pctl_init_failed";
        return false;
    }

    PctlTodayState state;
    if (!read_pctl_today_state(&state, reason)) {
        pctl_exit();
        mutexUnlock(&s_pctl_mutex);
        return false;
    }

    if (!state.has_limited_today && !s_cfg.allow_unlimited_to_limited) {
        pctl_exit();
        mutexUnlock(&s_pctl_mutex);
        *reason = "unlimited_not_allowed";
        return false;
    }

    int current_limit = state.has_limited_today ? (int)state.today_limit : 0;
    int new_limit = current_limit + minutes;
    if (new_limit < 0) new_limit = 0;
    if (new_limit > 1440) new_limit = 1440;

    if (!write_pctl_backup(&state, new_limit)) {
        pctl_exit();
        mutexUnlock(&s_pctl_mutex);
        *reason = "pctl_backup_failed";
        return false;
    }

    PlayTimerSettings next = state.settings;
    u16 next_val = (u16)new_limit;
    if (next.raw[0] == 0) {
        next.raw[0] = 0x0101;
        next.raw[1] = 0x0001;
    }
    next.raw[PCTL_DAY_FLAG_OFFSET(state.today)] = PCTL_DAY_CONFIGURED;
    next.raw[PCTL_DAY_ENABLE_OFFSET(state.today)] = PCTL_DAY_RESTRICTED;
    next.raw[PCTL_DAY_MINUTES_OFFSET(state.today)] = next_val;

    rc = pctl_set_settings(&next);
    if (R_SUCCEEDED(rc) && s_cfg.control_mode == GRANT_CONTROL_ENFORCE) {
        pctl_stop_play_timer();
        pctl_start_play_timer();
    }

    pctl_exit();
    mutexUnlock(&s_pctl_mutex);

    if (R_FAILED(rc)) {
        *reason = "pctl_write_failed";
        return false;
    }

    if (today_limit_out) *today_limit_out = new_limit;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Verification and execution                                                */
/* ------------------------------------------------------------------------- */

static bool verify_code(const char *code, GrantToken *token, const char **reason) {
    if (!s_cfg.loaded) {
        *reason = "config_missing";
        return false;
    }

    uint8_t bytes[GRANT_TOKEN_BYTES];
    if (!decode_code_bytes(code, bytes)) {
        *reason = "bad_code";
        return false;
    }

    unpack_token(bytes, token);

    if (token->version != GRANT_VERSION) {
        *reason = "bad_version";
        return false;
    }
    if (token->action != GRANT_ACTION_ADD_TODAY) {
        *reason = "unsupported_action";
        return false;
    }

    uint32_t mac = expected_mac(bytes);
    if (mac != token->mac) {
        *reason = "bad_signature";
        return false;
    }

    uint16_t today_key = 0;
    if (!get_current_date_key(&today_key)) {
        *reason = "bad_clock";
        return false;
    }
    if (token->date_key != today_key) {
        *reason = "wrong_date";
        return false;
    }

    if (used_nonce_contains(token->date_key, token->nonce)) {
        *reason = "used_token";
        return false;
    }

    if (token->minutes <= 0 || token->minutes > s_cfg.max_add_minutes) {
        *reason = "minutes_exceed_limit";
        return false;
    }

    return true;
}

static bool apply_code(const char *code, int *applied_minutes, int *today_limit, const char **reason) {
    GrantToken token;
    memset(&token, 0, sizeof(token));

    if (!verify_code(code, &token, reason)) {
        return false;
    }

    if (s_cfg.control_mode == GRANT_CONTROL_OBSERVE) {
        if (!observe_add_minutes(token.minutes, today_limit, reason)) {
            return false;
        }
    } else {
        if (!apply_add_minutes(token.minutes, today_limit, reason)) {
            return false;
        }

        if (!used_nonce_append(token.date_key, token.nonce)) {
            log_msg("grant: warning, used ledger append failed");
        }
    }

    if (applied_minutes) *applied_minutes = token.minutes;
    append_event("offline_code", "ok", "add_today", *today_limit - token.minutes, *today_limit);
    return true;
}

static bool handle_management_request(const char *type, const char *req,
                                      int *applied, int *today_limit, const char **reason) {
    uint16_t date_key = 0;
    get_current_date_key(&date_key);

    if (strcmp(type, "set_today_limit") == 0) {
        int minutes = 0;
        if (!json_read_named_int(req, "minutes", &minutes)) {
            *reason = "invalid_minutes";
            return true;
        }
        if (!save_today_override(LOCAL_RULE_LIMIT, minutes, date_key)) {
            *reason = "write_rules_failed";
            return true;
        }
        if (apply_today_rule(LOCAL_RULE_LIMIT, minutes, "set_today_limit", today_limit, reason)) {
            *applied = 0;
            return true;
        }
        return true;
    }

    if (strcmp(type, "add_today_minutes") == 0) {
        int minutes = 0;
        if (!json_read_named_int(req, "minutes", &minutes) || minutes <= 0 || minutes > s_cfg.max_add_minutes) {
            *reason = "minutes_exceed_limit";
            return true;
        }
        int base = 0;
        PctlStatusResult status;
        if (read_current_pctl_status(&status, NULL) && status.limited_today) base = status.today_limit;
        int next = base + minutes;
        if (next > 1440) next = 1440;
        if (!save_today_override(LOCAL_RULE_LIMIT, next, date_key)) {
            *reason = "write_rules_failed";
            return true;
        }
        if (apply_today_rule(LOCAL_RULE_LIMIT, next, "add_today_minutes", today_limit, reason)) {
            *applied = minutes;
            return true;
        }
        return true;
    }

    if (strcmp(type, "disable_today_limit") == 0) {
        if (!save_today_override(LOCAL_RULE_UNLIMITED, RULE_DEFAULT_LIMIT, date_key)) {
            *reason = "write_rules_failed";
            return true;
        }
        if (apply_today_rule(LOCAL_RULE_UNLIMITED, 0, "disable_today_limit", today_limit, reason)) {
            *applied = 0;
            return true;
        }
        return true;
    }

    if (strcmp(type, "block_today") == 0) {
        if (!s_rules.raw_block_verified) {
            *reason = "raw_block_not_verified";
            append_event("block_today", "rejected", "raw_block_not_verified", -1, 0);
            return true;
        }
        if (!save_today_override(LOCAL_RULE_BLOCKED, RULE_DEFAULT_LIMIT, date_key)) {
            *reason = "write_rules_failed";
            return true;
        }
        if (apply_today_rule(LOCAL_RULE_BLOCKED, 0, "block_today", today_limit, reason)) {
            *applied = 0;
            return true;
        }
        return true;
    }

    if (strcmp(type, "probe_raw_block") == 0) {
        bool old_verified = s_rules.raw_block_verified;
        s_rules.raw_block_verified = true;
        bool ok = apply_today_rule(LOCAL_RULE_BLOCKED, 0, "probe_raw_block", today_limit, reason);
        s_rules.raw_block_verified = old_verified;
        write_time_rules(&s_rules);
        if (ok) {
            *applied = 0;
        }
        return true;
    }

    if (strcmp(type, "restore_today_policy") == 0) {
        clear_today_override();
        int today = 0;
        pctl_get_today_info(&today, &date_key);
        DayRule rule = active_rule_for_today(today, date_key);
        if (apply_today_rule(rule.mode, rule.minutes, "restore_today_policy", today_limit, reason)) {
            *applied = 0;
            return true;
        }
        return true;
    }

    if (strcmp(type, "set_weekly_template") == 0) {
        for (int i = 0; i < RULE_DAYS; i++) {
            char key[32];
            char text[32];
            int minutes = 0;
            snprintf(key, sizeof(key), "day%d_mode", i);
            if (json_read_named_string(req, key, text, sizeof(text))) {
                s_rules.week[i].mode = parse_local_rule_mode(text);
            }
            snprintf(key, sizeof(key), "day%d_minutes", i);
            if (json_read_named_int(req, key, &minutes)) {
                s_rules.week[i].minutes = clamp_minutes(minutes);
            }
        }
        if (!write_time_rules(&s_rules)) {
            *reason = "write_rules_failed";
            return true;
        }
        append_event("set_weekly_template", "ok", "saved", -1, -1);
        *today_limit = 0;
        return true;
    }

    if (strcmp(type, "set_bedtime") == 0) {
        bool enabled = false;
        int start = 0;
        int end = 0;
        json_read_named_bool(req, "enabled", &enabled);
        if (!json_read_named_int(req, "start_min", &start) ||
            !json_read_named_int(req, "end_min", &end) ||
            start < 0 || start >= 1440 || end < 0 || end >= 1440) {
            *reason = "bad_bedtime";
            return true;
        }
        s_rules.bedtime_enabled = enabled;
        s_rules.bedtime_start_min = start;
        s_rules.bedtime_end_min = end;
        if (!write_time_rules(&s_rules)) {
            *reason = "write_rules_failed";
            return true;
        }
        append_event("set_bedtime", "ok", enabled ? "enabled" : "disabled", start, end);
        *today_limit = 0;
        return true;
    }

    if (strcmp(type, "set_limit_action") == 0) {
        char action[32];
        if (!json_read_named_string(req, "action", action, sizeof(action))) {
            *reason = "bad_limit_action";
            return true;
        }
        s_rules.limit_action = parse_limit_action(action);
        bool raw_verified = false;
        bool suspend_verified = false;
        if (json_read_named_bool(req, "raw_block_verified", &raw_verified)) {
            s_rules.raw_block_verified = raw_verified;
        }
        if (json_read_named_bool(req, "suspend_verified", &suspend_verified)) {
            s_rules.suspend_verified = suspend_verified;
        }
        if (!write_time_rules(&s_rules)) {
            *reason = "write_rules_failed";
            return true;
        }
        append_event("set_limit_action", "ok", limit_action_name(s_rules.limit_action), -1, -1);
        *today_limit = 0;
        return true;
    }

    if (strcmp(type, "parent_unlock_start") == 0) {
        int minutes = 0;
        if (!json_read_named_int(req, "minutes", &minutes) || minutes <= 0 || minutes > 1440) {
            *reason = "invalid_minutes";
            return true;
        }
        u64 now = 0;
        get_now_posix(&now);
        s_time_state.parent_unlock_active = true;
        s_time_state.parent_unlock_until = now + (u64)minutes * 60ULL;
        write_time_state();
        append_event("parent_unlock_start", "ok", "timer_paused", -1, minutes);
        mutexLock(&s_pctl_mutex);
        if (R_SUCCEEDED(pctl_init())) {
            pctl_stop_play_timer();
            pctl_exit();
        }
        mutexUnlock(&s_pctl_mutex);
        *today_limit = 0;
        return true;
    }

    if (strcmp(type, "parent_unlock_end") == 0) {
        s_time_state.parent_unlock_active = false;
        s_time_state.parent_unlock_until = 0;
        write_time_state();
        append_event("parent_unlock_end", "ok", "timer_resumed", -1, -1);
        mutexLock(&s_pctl_mutex);
        if (R_SUCCEEDED(pctl_init())) {
            pctl_start_play_timer();
            pctl_exit();
        }
        mutexUnlock(&s_pctl_mutex);
        *today_limit = 0;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void grant_manager_init(void) {
    mutexInit(&s_pctl_mutex);
    load_config();
    load_time_rules();
    load_time_state();
    refresh_time_state();
    enforce_bedtime_transition();
    if (s_cfg.loaded) {
        char msg[160];
        snprintf(msg, sizeof(msg), "grant: config loaded (mode=%s, allow_unlimited_to_limited=%s)",
                 control_mode_name(s_cfg.control_mode),
                 s_cfg.allow_unlimited_to_limited ? "true" : "false");
        log_msg(msg);
    } else {
        log_msg("grant: config not loaded");
    }
    if (grant_manager_is_disabled()) {
        log_msg("grant: fail-open disabled state active");
    }
}

void grant_manager_process(void) {
    char req[1024];
    if (!read_file_text(GRANT_REQUEST_PATH, req, sizeof(req))) {
        return;
    }

    remove(GRANT_REQUEST_PATH);
    load_config();
    load_time_rules();
    load_time_state();
    refresh_time_state();
    enforce_bedtime_transition();

    if (grant_manager_is_disabled()) {
        write_result_error("disabled");
        log_msg("grant: request ignored because control is disabled");
        return;
    }

    const char *type_val = json_find_value(req, "type");
    char type[32];
    if (!type_val || !json_read_string(type_val, type, sizeof(type))) {
        write_result_error("bad_request");
        return;
    }

    if (strcmp(type, "status") == 0) {
        PctlStatusResult status;
        const char *reason = NULL;
        if (read_current_pctl_status(&status, &reason)) {
            write_monthly_report(&status);
            write_result_status(&status);
            log_msg("grant: status refreshed");
        } else {
            write_result_error(reason ? reason : "unknown");
            char msg[128];
            snprintf(msg, sizeof(msg), "grant: status refresh failed (%s)", reason ? reason : "unknown");
            log_msg(msg);
        }
        return;
    }

    int applied = 0;
    int today_limit = 0;
    const char *reason = NULL;
    bool ok = false;

    if (strcmp(type, "offline_code") == 0) {
        const char *code_val = json_find_value(req, "code");
        char code[64];
        if (!code_val || !json_read_string(code_val, code, sizeof(code))) {
            reason = "missing_code";
        } else {
            ok = apply_code(code, &applied, &today_limit, &reason);
        }
    } else {
        bool handled = handle_management_request(type, req, &applied, &today_limit, &reason);
        if (handled) {
            ok = (reason == NULL);
        } else {
            reason = "unknown_request";
        }
    }

    if (ok) {
        bool dry_run = !control_mode_writes_pctl();
        PctlStatusResult status;
        if (!read_current_pctl_status(&status, NULL)) {
            memset(&status, 0, sizeof(status));
            status.limited_today = today_limit > 0;
            status.today_limit = today_limit;
        }
        write_monthly_report(&status);
        write_result_ok(applied, today_limit, dry_run, &status);
        log_msg(dry_run ? "grant: validated successfully (dry run)" : "grant: applied successfully");
    } else {
        write_result_error(reason ? reason : "unknown");
        append_event(type, "rejected", reason ? reason : "unknown", -1, -1);
        char msg[128];
        snprintf(msg, sizeof(msg), "grant: rejected (%s)", reason ? reason : "unknown");
        log_msg(msg);
    }
}

GrantControlMode grant_manager_control_mode(void) {
    return s_cfg.control_mode;
}

const char *grant_manager_control_mode_name(void) {
    return control_mode_name(s_cfg.control_mode);
}

bool grant_manager_is_disabled(void) {
    return file_exists(GRANT_DISABLE_PATH) || s_cfg.control_mode == GRANT_CONTROL_DISABLED;
}

bool grant_manager_should_enforce_play_timer(void) {
    return s_cfg.loaded && !grant_manager_is_disabled() && s_cfg.control_mode == GRANT_CONTROL_ENFORCE;
}

void grant_manager_log_pctl_status(const char *context) {
    if (!s_cfg.loaded) {
        log_msg("PCTL status skipped because config is not loaded");
        return;
    }
    if (grant_manager_is_disabled()) {
        log_msg("PCTL status skipped because control is disabled");
        return;
    }

    mutexLock(&s_pctl_mutex);
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        mutexUnlock(&s_pctl_mutex);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: pctl_init failed (0x%08X)",
                 context ? context : "PCTL status", (unsigned)rc);
        log_msg(msg);
        return;
    }

    PctlTodayState state;
    const char *reason = NULL;
    bool ok = read_pctl_today_state(&state, &reason);

    bool enabled = false;
    Result enabled_rc = pctl_is_enabled(&enabled);
    bool restricted = false;
    Result restricted_rc = pctl_is_restricted(&restricted);
    u64 remaining_ns = 0;
    Result remaining_rc = pctl_get_remaining_time(&remaining_ns);

    pctl_exit();
    mutexUnlock(&s_pctl_mutex);

    char msg[256];
    if (ok) {
        snprintf(msg, sizeof(msg),
                 "%s: mode=%s today=%d limited=%s limit=%u raw=%04X/%04X/%04X enabled=%s(rc=0x%08X) restricted=%s(rc=0x%08X) remaining_min=%u(rc=0x%08X)",
                 context ? context : "PCTL status",
                 control_mode_name(s_cfg.control_mode),
                 state.today,
                 state.has_limited_today ? "true" : "false",
                 (unsigned)state.today_limit,
                 (unsigned)state.raw_flag,
                 (unsigned)state.raw_enable,
                 (unsigned)state.raw_minutes,
                 enabled ? "true" : "false",
                 (unsigned)enabled_rc,
                 restricted ? "true" : "false",
                 (unsigned)restricted_rc,
                 R_SUCCEEDED(remaining_rc) ? (unsigned)NS_TO_MINUTES(remaining_ns) : 0,
                 (unsigned)remaining_rc);
    } else {
        snprintf(msg, sizeof(msg), "%s: read failed (%s)",
                 context ? context : "PCTL status",
                 reason ? reason : "unknown");
    }
    log_msg(msg);
}
