#include <switch.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GRANT_DIR          "sdmc:/switch/pctltcp-sysmodule"
#define GRANT_REQUEST_PATH GRANT_DIR "/grant_request.json"
#define GRANT_RESULT_PATH  GRANT_DIR "/grant_result.json"
#define SETTINGS_PATH      GRANT_DIR "/settings.conf"
#define SYSMODULE_LOG_PATH GRANT_DIR "/sysmodule.log"
#define SYSMODULE_OLD_LOG_PATH GRANT_DIR "/sysmodule.log.old"
#define GRANT_CONFIG_PATH  GRANT_DIR "/grant.conf"
#define TIME_RULES_PATH    GRANT_DIR "/time_rules.json"
#define TIME_STATE_PATH    GRANT_DIR "/time_state.json"
#define EVENTS_PATH        GRANT_DIR "/events.jsonl"
#define MONTHLY_REPORT_PATH GRANT_DIR "/monthly_report.txt"
#define DEFAULT_PASSWORD   "1234"

#define SCREEN_W 1280
#define SCREEN_H 720
#define PARENT_HOLD_FRAMES 120
#define PREVIEW_MAX_BYTES 8192
#define PREVIEW_LINE_COUNT 15
#define PREVIEW_LINE_MAX 104

#define RGBA(r, g, b, a) ((uint32_t)((r) | ((g) << 8) | ((b) << 16) | ((a) << 24)))

typedef enum {
    UI_STATUS_IDLE,
    UI_STATUS_WAITING,
    UI_STATUS_SUCCESS,
    UI_STATUS_ERROR,
} UiStatus;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

typedef struct {
    UiStatus status;
    char message[512];
    bool button_pressed;
    bool preview_button_pressed;
    int parent_hold_frames;
    bool parent_triggered;
    bool raw_probe_pending;
    bool touch_was_down;
} UiState;

typedef struct {
    bool loaded;
    bool read_failed;
    bool limited_today;
    bool blocked_today;
    bool unrestricted_today;
    int today_limit;
    bool remaining_available;
    int remaining_minutes;
    bool play_timer_enabled;
    bool restricted_now;
    bool bedtime_active;
    bool parent_unlock_active;
    bool raw_block_verified;
    bool suspend_verified;
    char mode[32];
    char active_rule[32];
    int active_rule_minutes;
    char limit_action[32];
    char message[128];
} TimeStatus;

typedef struct {
    Framebuffer fb;
    FT_Library ft;
    FT_Face face;
    bool fb_ready;
    bool font_ready;
} UiRuntime;

typedef struct {
    const char *name;
    const char *path;
} PreviewFile;

typedef struct {
    int file_index;
    int scroll_line;
    int line_count;
    bool exists;
    bool truncated;
    char status[128];
    char text[PREVIEW_MAX_BYTES + 1];
    const char *lines[PREVIEW_MAX_BYTES + 1];
} PreviewState;

static UiRuntime g_ui;
static UiState g_state = {
    .status = UI_STATUS_IDLE,
    .message = "暂无结果。",
};
static TimeStatus g_time = {
    .loaded = false,
    .message = "正在读取时间信息...",
};
static PreviewState g_preview;

static const Rect PRIMARY_BUTTON = {340, 220, 600, 104};
static const Rect PREVIEW_BUTTON = {440, 350, 400, 62};
static const PreviewFile PREVIEW_FILES[] = {
    {"grant_result.json", GRANT_RESULT_PATH},
    {"grant_request.json", GRANT_REQUEST_PATH},
    {"sysmodule.log", SYSMODULE_LOG_PATH},
    {"sysmodule.log.old", SYSMODULE_OLD_LOG_PATH},
    {"grant.conf", GRANT_CONFIG_PATH},
    {"settings.conf", SETTINGS_PATH},
    {"time_rules.json", TIME_RULES_PATH},
    {"time_state.json", TIME_STATE_PATH},
    {"events.jsonl", EVENTS_PATH},
    {"monthly_report.txt", MONTHLY_REPORT_PATH},
};
static const int PREVIEW_FILE_COUNT = (int)(sizeof(PREVIEW_FILES) / sizeof(PREVIEW_FILES[0]));

static void set_status(UiStatus status, const char *msg) {
    g_state.status = status;
    if (!msg) msg = "";
    strncpy(g_state.message, msg, sizeof(g_state.message) - 1);
    g_state.message[sizeof(g_state.message) - 1] = '\0';
}

static void ensure_dirs(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir(GRANT_DIR, 0777);
}

static void trim_line(char *text) {
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n' ||
                       text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }
}

static bool read_text(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    return true;
}

static bool read_preview_text(const char *path, PreviewState *state) {
    if (!path || !state) return false;

    FILE *f = fopen(path, "r");
    if (!f) {
        state->text[0] = '\0';
        state->line_count = 0;
        state->exists = false;
        state->truncated = false;
        snprintf(state->status, sizeof(state->status), "文件不存在或无法读取。");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) size = 0;

    long offset = 0;
    state->truncated = false;
    if (size > PREVIEW_MAX_BYTES) {
        offset = size - PREVIEW_MAX_BYTES;
        state->truncated = true;
    }
    fseek(f, offset, SEEK_SET);

    size_t n = fread(state->text, 1, PREVIEW_MAX_BYTES, f);
    fclose(f);
    state->text[n] = '\0';

    state->exists = true;
    if (n == 0) {
        state->line_count = 0;
        snprintf(state->status, sizeof(state->status), "文件为空。");
        return true;
    }

    state->line_count = 0;
    char *p = state->text;
    while (*p && state->line_count < (int)(sizeof(state->lines) / sizeof(state->lines[0]))) {
        state->lines[state->line_count++] = p;
        while (*p && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p == '\r') {
            *p++ = '\0';
            if (*p == '\n') p++;
        } else if (*p == '\n') {
            *p++ = '\0';
        }
    }

    if (state->truncated) {
        snprintf(state->status, sizeof(state->status), "仅显示最后 %d 字节。", PREVIEW_MAX_BYTES);
    } else {
        snprintf(state->status, sizeof(state->status), "显示完整文件。");
    }
    return true;
}

static void load_preview_file(void) {
    if (g_preview.file_index < 0) g_preview.file_index = 0;
    if (g_preview.file_index >= PREVIEW_FILE_COUNT) g_preview.file_index = PREVIEW_FILE_COUNT - 1;
    g_preview.scroll_line = 0;
    read_preview_text(PREVIEW_FILES[g_preview.file_index].path, &g_preview);
}

static void copy_preview_line(const char *src, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t j = 0;
    bool truncated = false;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\t') {
            int spaces = 4 - (int)(j % 4);
            while (spaces-- > 0 && j + 1 < dst_size) dst[j++] = ' ';
        } else if (c < 0x20) {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
        if (src[i + 1] && j + 1 >= dst_size) truncated = true;
    }

    if (truncated && dst_size > 4) {
        dst[dst_size - 4] = '.';
        dst[dst_size - 3] = '.';
        dst[dst_size - 2] = '.';
        dst[dst_size - 1] = '\0';
    } else {
        dst[j] = '\0';
    }
}

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

static bool update_time_status_from_json(const char *text) {
    if (!text) return false;

    bool limited_today = false;
    bool blocked_today = false;
    bool unrestricted_today = false;
    bool remaining_available = false;
    bool play_timer_enabled = false;
    bool restricted_now = false;
    bool bedtime_active = false;
    bool parent_unlock_active = false;
    bool raw_block_verified = false;
    bool suspend_verified = false;
    int today_limit = 0;
    int remaining_minutes = 0;
    int active_rule_minutes = 0;
    char mode[32] = {0};
    char active_rule[32] = {0};
    char limit_action[32] = {0};

    const char *val = json_find_value(text, "limited_today");
    if (!val || !json_read_bool(val, &limited_today)) return false;

    val = json_find_value(text, "blocked_today");
    if (val) json_read_bool(val, &blocked_today);

    val = json_find_value(text, "unrestricted_today");
    if (val) json_read_bool(val, &unrestricted_today);

    val = json_find_value(text, "today_limit");
    if (!val || !json_read_int(val, &today_limit)) return false;

    val = json_find_value(text, "remaining_available");
    if (!val || !json_read_bool(val, &remaining_available)) return false;

    val = json_find_value(text, "remaining_minutes");
    if (val) json_read_int(val, &remaining_minutes);

    val = json_find_value(text, "play_timer_enabled");
    if (val) json_read_bool(val, &play_timer_enabled);

    val = json_find_value(text, "restricted_now");
    if (val) json_read_bool(val, &restricted_now);

    val = json_find_value(text, "bedtime_active");
    if (val) json_read_bool(val, &bedtime_active);

    val = json_find_value(text, "parent_unlock_active");
    if (val) json_read_bool(val, &parent_unlock_active);

    val = json_find_value(text, "raw_block_verified");
    if (val) json_read_bool(val, &raw_block_verified);

    val = json_find_value(text, "suspend_verified");
    if (val) json_read_bool(val, &suspend_verified);

    val = json_find_value(text, "active_rule_minutes");
    if (val) json_read_int(val, &active_rule_minutes);

    val = json_find_value(text, "mode");
    if (val) json_read_string(val, mode, sizeof(mode));

    val = json_find_value(text, "active_rule");
    if (val) json_read_string(val, active_rule, sizeof(active_rule));

    val = json_find_value(text, "limit_action");
    if (val) json_read_string(val, limit_action, sizeof(limit_action));

    g_time.loaded = true;
    g_time.read_failed = false;
    g_time.limited_today = limited_today;
    g_time.blocked_today = blocked_today;
    g_time.unrestricted_today = unrestricted_today;
    g_time.today_limit = today_limit;
    g_time.remaining_available = remaining_available;
    g_time.remaining_minutes = remaining_minutes;
    g_time.play_timer_enabled = play_timer_enabled;
    g_time.restricted_now = restricted_now;
    g_time.bedtime_active = bedtime_active;
    g_time.parent_unlock_active = parent_unlock_active;
    g_time.raw_block_verified = raw_block_verified;
    g_time.suspend_verified = suspend_verified;
    g_time.active_rule_minutes = active_rule_minutes;
    snprintf(g_time.mode, sizeof(g_time.mode), "%s", mode[0] ? mode : "-");
    snprintf(g_time.active_rule, sizeof(g_time.active_rule), "%s", active_rule[0] ? active_rule : "-");
    snprintf(g_time.limit_action, sizeof(g_time.limit_action), "%s", limit_action[0] ? limit_action : "remind");
    snprintf(g_time.message, sizeof(g_time.message), "时间信息已刷新。");
    return true;
}

static void set_time_status_error(const char *msg) {
    g_time.loaded = false;
    g_time.read_failed = true;
    g_time.limited_today = false;
    g_time.blocked_today = false;
    g_time.unrestricted_today = false;
    g_time.today_limit = 0;
    g_time.remaining_available = false;
    g_time.remaining_minutes = 0;
    g_time.play_timer_enabled = false;
    g_time.restricted_now = false;
    g_time.bedtime_active = false;
    g_time.parent_unlock_active = false;
    g_time.raw_block_verified = false;
    g_time.suspend_verified = false;
    g_time.mode[0] = '\0';
    g_time.active_rule[0] = '\0';
    g_time.limit_action[0] = '\0';
    g_time.active_rule_minutes = 0;
    snprintf(g_time.message, sizeof(g_time.message), "%s", msg ? msg : "无法读取时间信息。");
}

static const char *reason_to_zh(const char *reason) {
    if (!reason) return "未知错误";
    if (strcmp(reason, "config_missing") == 0) return "缺少授权配置，请检查 grant.conf。";
    if (strcmp(reason, "bad_code") == 0) return "授权码格式不正确，请重新输入。";
    if (strcmp(reason, "bad_version") == 0) return "授权码版本不支持。";
    if (strcmp(reason, "unsupported_action") == 0) return "授权码操作不支持。";
    if (strcmp(reason, "bad_signature") == 0) return "授权码签名校验失败，请确认设备和密钥。";
    if (strcmp(reason, "bad_clock") == 0) return "无法读取主机日期，请先校准时间。";
    if (strcmp(reason, "wrong_date") == 0) return "授权码不是今天的码，已过期或日期不匹配。";
    if (strcmp(reason, "used_token") == 0) return "这个授权码已经使用过。";
    if (strcmp(reason, "minutes_exceed_limit") == 0) return "加时时长超过允许上限。";
    if (strcmp(reason, "pctl_init_failed") == 0) return "家长控制服务初始化失败。";
    if (strcmp(reason, "pctl_read_failed") == 0) return "读取今日游玩限制失败。";
    if (strcmp(reason, "pctl_backup_failed") == 0) return "写入备份失败，已取消本次授权以便保持可恢复。";
    if (strcmp(reason, "pctl_write_failed") == 0) return "写入今日游玩限制失败。";
    if (strcmp(reason, "unlimited_not_allowed") == 0) return "当前未设置今日限制，安全模式下不会把无限制改成有限制。";
    if (strcmp(reason, "disabled") == 0) return "后台控制已禁用，未执行授权。";
    if (strcmp(reason, "bad_request") == 0) return "请求格式错误。";
    if (strcmp(reason, "missing_code") == 0) return "请求中没有授权码。";
    if (strcmp(reason, "unknown_request") == 0) return "未知请求类型。";
    if (strcmp(reason, "invalid_minutes") == 0) return "加时时长无效。";
    if (strcmp(reason, "raw_block_not_verified") == 0) return "raw 0 禁玩尚未真机验证，已拒绝写入。";
    if (strcmp(reason, "remaining_unavailable") == 0) return "无法读取剩余时间，不能安全降低今日额度。";
    if (strcmp(reason, "write_rules_failed") == 0) return "写入本地时间规则失败，请检查 SD 卡。";
    if (strcmp(reason, "bad_bedtime") == 0) return "bedtime 时间格式无效。";
    if (strcmp(reason, "bad_limit_action") == 0) return "限制动作设置无效。";
    return "未知错误";
}

static void set_last_result_from_sysmodule(const char *text) {
    if (!text || text[0] == '\0') {
        set_status(UI_STATUS_IDLE, "暂无结果。");
        return;
    }

    char status[32] = {0};
    const char *val = json_find_value(text, "status");
    if (!val || !json_read_string(val, status, sizeof(status))) {
        set_status(UI_STATUS_IDLE, text);
        return;
    }

    if (strcmp(status, "ok") == 0) {
        int applied_minutes = 0;
        int today_limit = 0;
        bool dry_run = false;

        val = json_find_value(text, "applied_minutes");
        if (val) json_read_int(val, &applied_minutes);
        val = json_find_value(text, "today_limit");
        if (val) json_read_int(val, &today_limit);
        val = json_find_value(text, "dry_run");
        if (val) json_read_bool(val, &dry_run);
        update_time_status_from_json(text);

        char msg[256];
        if (dry_run) {
            snprintf(msg, sizeof(msg),
                     "观察模式：授权码有效，预计增加 %d 分钟。\n今日限制预计调整为 %d 分钟；系统设置未被修改。",
                     applied_minutes, today_limit);
        } else {
            snprintf(msg, sizeof(msg),
                     "授权成功：已增加 %d 分钟。\n今日限制已调整为 %d 分钟。",
                     applied_minutes, today_limit);
        }
        set_status(UI_STATUS_SUCCESS, msg);
        return;
    }

    if (strcmp(status, "status") == 0) {
        if (update_time_status_from_json(text)) {
            set_status(UI_STATUS_IDLE, "时间信息已刷新。");
        } else {
            set_time_status_error("时间结果格式不完整。");
            set_status(UI_STATUS_ERROR, "时间结果格式不完整。");
        }
        return;
    }

    if (strcmp(status, "error") == 0) {
        char reason[96] = {0};
        val = json_find_value(text, "reason");
        if (val) json_read_string(val, reason, sizeof(reason));
        char msg[256];
        snprintf(msg, sizeof(msg), "授权失败：%s", reason_to_zh(reason));
        set_status(UI_STATUS_ERROR, msg);
        return;
    }

    set_status(UI_STATUS_IDLE, text);
}

static bool write_text_atomic(const char *path, const char *text) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;
    fputs(text ? text : "", f);
    fputc('\n', f);
    fclose(f);

    remove(path);
    return rename(tmp_path, path) == 0;
}

static void read_password(char *password, size_t size) {
    if (!password || size == 0) return;
    if (!read_text(SETTINGS_PATH, password, size)) {
        strncpy(password, DEFAULT_PASSWORD, size - 1);
        password[size - 1] = '\0';
        return;
    }
    trim_line(password);
    if (password[0] == '\0') {
        strncpy(password, DEFAULT_PASSWORD, size - 1);
        password[size - 1] = '\0';
    }
}

static bool write_request(const char *json) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", GRANT_REQUEST_PATH);

    remove(GRANT_RESULT_PATH);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;
    fputs(json, f);
    fputc('\n', f);
    fclose(f);

    remove(GRANT_REQUEST_PATH);
    return rename(tmp_path, GRANT_REQUEST_PATH) == 0;
}

static void json_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        if ((src[i] == '"' || src[i] == '\\') && j + 2 < dst_size) {
            dst[j++] = '\\';
            dst[j++] = src[i];
        } else if ((unsigned char)src[i] >= 0x20) {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static uint32_t decode_utf8_char(const char **text) {
    const unsigned char *s = (const unsigned char *)*text;
    if (s[0] < 0x80) {
        (*text)++;
        return s[0];
    }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        *text += 2;
        return ((uint32_t)(s[0] & 0x1f) << 6) | (uint32_t)(s[1] & 0x3f);
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *text += 3;
        return ((uint32_t)(s[0] & 0x0f) << 12) |
               ((uint32_t)(s[1] & 0x3f) << 6) |
               (uint32_t)(s[2] & 0x3f);
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *text += 4;
        return ((uint32_t)(s[0] & 0x07) << 18) |
               ((uint32_t)(s[1] & 0x3f) << 12) |
               ((uint32_t)(s[2] & 0x3f) << 6) |
               (uint32_t)(s[3] & 0x3f);
    }

    (*text)++;
    return '?';
}

static void set_pixel(uint32_t *fb, uint32_t stride, int x, int y, uint32_t color) {
    if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H) return;
    fb[(uint32_t)y * stride + (uint32_t)x] = color;
}

static void blend_pixel(uint32_t *fb, uint32_t stride, int x, int y, uint32_t color, uint8_t alpha) {
    if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H || alpha == 0) return;
    uint32_t *dst = &fb[(uint32_t)y * stride + (uint32_t)x];

    uint32_t sr = color & 0xff;
    uint32_t sg = (color >> 8) & 0xff;
    uint32_t sb = (color >> 16) & 0xff;
    uint32_t dr = *dst & 0xff;
    uint32_t dg = (*dst >> 8) & 0xff;
    uint32_t db = (*dst >> 16) & 0xff;

    uint32_t a = alpha;
    uint32_t r = (sr * a + dr * (255 - a)) / 255;
    uint32_t g = (sg * a + dg * (255 - a)) / 255;
    uint32_t b = (sb * a + db * (255 - a)) / 255;
    *dst = RGBA(r, g, b, 255);
}

static void fill_rect(uint32_t *fb, uint32_t stride, Rect rect, uint32_t color) {
    int x0 = rect.x < 0 ? 0 : rect.x;
    int y0 = rect.y < 0 ? 0 : rect.y;
    int x1 = rect.x + rect.w > SCREEN_W ? SCREEN_W : rect.x + rect.w;
    int y1 = rect.y + rect.h > SCREEN_H ? SCREEN_H : rect.y + rect.h;

    for (int y = y0; y < y1; y++) {
        uint32_t *row = fb + (uint32_t)y * stride;
        for (int x = x0; x < x1; x++) {
            row[x] = color;
        }
    }
}

static void fill_round_rect(uint32_t *fb, uint32_t stride, Rect rect, int radius, uint32_t color) {
    int rr = radius * radius;
    for (int y = rect.y; y < rect.y + rect.h; y++) {
        for (int x = rect.x; x < rect.x + rect.w; x++) {
            int dx = 0;
            int dy = 0;
            if (x < rect.x + radius) dx = rect.x + radius - x;
            else if (x >= rect.x + rect.w - radius) dx = x - (rect.x + rect.w - radius - 1);
            if (y < rect.y + radius) dy = rect.y + radius - y;
            else if (y >= rect.y + rect.h - radius) dy = y - (rect.y + rect.h - radius - 1);
            if (dx == 0 || dy == 0 || dx * dx + dy * dy <= rr) {
                set_pixel(fb, stride, x, y, color);
            }
        }
    }
}

static void draw_border(uint32_t *fb, uint32_t stride, Rect rect, uint32_t color) {
    fill_rect(fb, stride, (Rect){rect.x, rect.y, rect.w, 2}, color);
    fill_rect(fb, stride, (Rect){rect.x, rect.y + rect.h - 2, rect.w, 2}, color);
    fill_rect(fb, stride, (Rect){rect.x, rect.y, 2, rect.h}, color);
    fill_rect(fb, stride, (Rect){rect.x + rect.w - 2, rect.y, 2, rect.h}, color);
}

static bool set_font_size(int size_px) {
    return g_ui.font_ready && FT_Set_Pixel_Sizes(g_ui.face, 0, (FT_UInt)size_px) == 0;
}

static int measure_text_width(const char *text, int size_px) {
    if (!set_font_size(size_px) || !text) return 0;

    int x = 0;
    int max_x = 0;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            if (x > max_x) max_x = x;
            x = 0;
            p++;
            continue;
        }
        uint32_t codepoint = decode_utf8_char(&p);
        if (FT_Load_Char(g_ui.face, codepoint, FT_LOAD_DEFAULT) == 0) {
            x += (int)(g_ui.face->glyph->advance.x >> 6);
        }
    }
    return x > max_x ? x : max_x;
}

static void draw_text(uint32_t *fb, uint32_t stride, int x, int y, const char *text,
                      int size_px, uint32_t color) {
    if (!set_font_size(size_px) || !text) return;

    int pen_x = x;
    int pen_y = y;
    int line_height = size_px + 10;
    const char *p = text;

    while (*p) {
        if (*p == '\n') {
            pen_x = x;
            pen_y += line_height;
            p++;
            continue;
        }

        uint32_t codepoint = decode_utf8_char(&p);
        if (FT_Load_Char(g_ui.face, codepoint, FT_LOAD_RENDER) != 0) continue;

        FT_GlyphSlot glyph = g_ui.face->glyph;
        FT_Bitmap *bitmap = &glyph->bitmap;
        if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) {
            pen_x += (int)(glyph->advance.x >> 6);
            continue;
        }
        int glyph_x = pen_x + glyph->bitmap_left;
        int glyph_y = pen_y - glyph->bitmap_top;

        for (int row = 0; row < (int)bitmap->rows; row++) {
            const unsigned char *src = bitmap->buffer + row * bitmap->pitch;
            for (int col = 0; col < (int)bitmap->width; col++) {
                blend_pixel(fb, stride, glyph_x + col, glyph_y + row, color, src[col]);
            }
        }

        pen_x += (int)(glyph->advance.x >> 6);
    }
}

static void draw_text_center(uint32_t *fb, uint32_t stride, Rect area, const char *text,
                             int size_px, uint32_t color) {
    int width = measure_text_width(text, size_px);
    int x = area.x + (area.w - width) / 2;
    int y = area.y + (area.h + size_px) / 2 - 8;
    draw_text(fb, stride, x, y, text, size_px, color);
}

static void draw_key_hint(uint32_t *fb, uint32_t stride, int x, const char *key, const char *label) {
    int key_w = key && strlen(key) > 2 ? 82 : 42;
    Rect key_rect = {x, 654, key_w, 36};
    fill_round_rect(fb, stride, key_rect, 8, RGBA(235, 240, 248, 255));
    draw_text_center(fb, stride, key_rect, key, key_w > 42 ? 18 : 22, RGBA(37, 49, 67, 255));
    draw_text(fb, stride, x + key_w + 10, 680, label, 24, RGBA(73, 86, 105, 255));
}

static void draw_button(uint32_t *fb, uint32_t stride, Rect rect, const char *title, const char *subtitle,
                        bool pressed, uint32_t color, uint32_t pressed_color) {
    Rect shadow = {rect.x, rect.y + 8, rect.w, rect.h};
    fill_round_rect(fb, stride, shadow, 24, RGBA(203, 216, 235, 255));
    fill_round_rect(fb, stride, rect, 24, pressed ? pressed_color : color);
    if (subtitle) {
        draw_text_center(fb, stride, (Rect){rect.x, rect.y, rect.w, rect.h / 2 + 10},
                         title, rect.h > 80 ? 38 : 26, RGBA(255, 255, 255, 255));
        draw_text_center(fb, stride, (Rect){rect.x, rect.y + rect.h / 2, rect.w, rect.h / 2},
                         subtitle, 22, RGBA(224, 237, 255, 255));
    } else {
        draw_text_center(fb, stride, rect, title, rect.h > 80 ? 40 : 28, RGBA(255, 255, 255, 255));
    }
}

static void format_limit_text(char *buf, size_t size) {
    if (!buf || size == 0) return;

    if (g_time.read_failed) {
        snprintf(buf, size, "无法读取");
    } else if (!g_time.loaded) {
        snprintf(buf, size, "正在读取");
    } else if (g_time.blocked_today) {
        snprintf(buf, size, "今日禁玩");
    } else if (!g_time.limited_today) {
        snprintf(buf, size, "未设置限制");
    } else {
        snprintf(buf, size, "%d 分钟", g_time.today_limit);
    }
}

static void format_remaining_text(char *buf, size_t size) {
    if (!buf || size == 0) return;

    if (g_time.read_failed) {
        snprintf(buf, size, "无法读取");
    } else if (!g_time.loaded) {
        snprintf(buf, size, "正在读取");
    } else if (!g_time.remaining_available) {
        snprintf(buf, size, "无法读取");
    } else {
        snprintf(buf, size, "%d 分钟", g_time.remaining_minutes);
    }
}

static void draw_time_tile(uint32_t *fb, uint32_t stride, Rect rect, const char *label,
                           const char *value, uint32_t accent) {
    fill_round_rect(fb, stride, rect, 16, RGBA(255, 255, 255, 255));
    draw_border(fb, stride, rect, RGBA(219, 226, 236, 255));
    fill_round_rect(fb, stride, (Rect){rect.x + 20, rect.y + 16, 8, rect.h - 32}, 4, accent);
    draw_text(fb, stride, rect.x + 44, rect.y + 26, label, 20, RGBA(83, 96, 114, 255));
    draw_text(fb, stride, rect.x + 44, rect.y + 56, value, 28, RGBA(31, 42, 58, 255));
}

static void draw_time_status(uint32_t *fb, uint32_t stride) {
    char limit[64];
    char remaining[64];
    char state[64];
    char mode[64];
    format_limit_text(limit, sizeof(limit));
    format_remaining_text(remaining, sizeof(remaining));
    if (g_time.read_failed || !g_time.loaded) {
        snprintf(state, sizeof(state), "-");
    } else if (g_time.parent_unlock_active) {
        snprintf(state, sizeof(state), "Parent unlock");
    } else if (g_time.bedtime_active) {
        snprintf(state, sizeof(state), "Bedtime");
    } else if (g_time.restricted_now || g_time.blocked_today) {
        snprintf(state, sizeof(state), "Restricted");
    } else {
        snprintf(state, sizeof(state), "Allowed");
    }
    snprintf(mode, sizeof(mode), "%s/%s", g_time.mode[0] ? g_time.mode : "-", g_time.limit_action[0] ? g_time.limit_action : "remind");

    draw_time_tile(fb, stride, (Rect){76, 146, 260, 62}, "今日额度", limit, RGBA(48, 113, 204, 255));
    draw_time_tile(fb, stride, (Rect){362, 146, 260, 62}, "剩余时间", remaining, RGBA(43, 153, 95, 255));
    draw_time_tile(fb, stride, (Rect){648, 146, 260, 62}, "当前状态", state, RGBA(207, 76, 76, 255));
    draw_time_tile(fb, stride, (Rect){934, 146, 270, 62}, "模式", mode, RGBA(111, 126, 147, 255));
}

static void draw_status_card(uint32_t *fb, uint32_t stride) {
    Rect card = {220, 444, 840, 142};
    uint32_t accent = RGBA(111, 126, 147, 255);
    const char *title = "最近结果";

    if (g_state.status == UI_STATUS_WAITING) {
        accent = RGBA(224, 151, 44, 255);
        title = "正在处理";
    } else if (g_state.status == UI_STATUS_SUCCESS) {
        accent = RGBA(43, 153, 95, 255);
        title = "授权成功";
    } else if (g_state.status == UI_STATUS_ERROR) {
        accent = RGBA(207, 76, 76, 255);
        title = "授权失败";
    }

    fill_round_rect(fb, stride, card, 18, RGBA(255, 255, 255, 255));
    draw_border(fb, stride, card, RGBA(219, 226, 236, 255));
    fill_round_rect(fb, stride, (Rect){card.x + 28, card.y + 28, 12, 86}, 6, accent);
    draw_text(fb, stride, card.x + 62, card.y + 52, title, 30, accent);
    draw_text(fb, stride, card.x + 62, card.y + 94, g_state.message, 24, RGBA(37, 49, 67, 255));
}

static void draw_ui(void) {
    uint32_t stride_bytes = 0;
    uint32_t *fb = (uint32_t *)framebufferBegin(&g_ui.fb, &stride_bytes);
    if (!fb) return;
    uint32_t stride = stride_bytes / sizeof(uint32_t);

    fill_rect(fb, stride, (Rect){0, 0, SCREEN_W, SCREEN_H}, RGBA(242, 246, 252, 255));
    fill_rect(fb, stride, (Rect){0, 0, SCREEN_W, 104}, RGBA(255, 255, 255, 255));
    fill_rect(fb, stride, (Rect){0, 102, SCREEN_W, 2}, RGBA(222, 229, 239, 255));

    draw_text(fb, stride, 76, 68, "家长控制离线授权", 36, RGBA(31, 42, 58, 255));
    draw_text(fb, stride, 78, 118, "输入家长给你的离线授权码，就能临时增加今天的游玩时间。", 25,
              RGBA(83, 96, 114, 255));

    draw_time_status(fb, stride);

    draw_button(fb, stride, PRIMARY_BUTTON, "输入授权码", "触摸这里或按 A", g_state.button_pressed,
                RGBA(48, 113, 204, 255), RGBA(37, 96, 174, 255));
    draw_button(fb, stride, PREVIEW_BUTTON, "最近结果", "按 B 重新读取", g_state.preview_button_pressed,
                RGBA(73, 86, 105, 255), RGBA(52, 64, 82, 255));

    draw_status_card(fb, stride);

    draw_key_hint(fb, stride, 80, "A", "输入授权码");
    draw_key_hint(fb, stride, 302, "B", "最近结果");
    draw_key_hint(fb, stride, 502, "Y", "刷新时间");
    draw_key_hint(fb, stride, 654, "L+R+X", "家长");
    draw_key_hint(fb, stride, 882, "+", "退出");

    framebufferEnd(&g_ui.fb);
}

static void draw_preview_ui(void) {
    uint32_t stride_bytes = 0;
    uint32_t *fb = (uint32_t *)framebufferBegin(&g_ui.fb, &stride_bytes);
    if (!fb) return;
    uint32_t stride = stride_bytes / sizeof(uint32_t);

    fill_rect(fb, stride, (Rect){0, 0, SCREEN_W, SCREEN_H}, RGBA(242, 246, 252, 255));
    fill_rect(fb, stride, (Rect){0, 0, SCREEN_W, 104}, RGBA(255, 255, 255, 255));
    fill_rect(fb, stride, (Rect){0, 102, SCREEN_W, 2}, RGBA(222, 229, 239, 255));

    const PreviewFile *file = &PREVIEW_FILES[g_preview.file_index];
    char header[128];
    snprintf(header, sizeof(header), "文件预览：%s", file->name);
    draw_text(fb, stride, 76, 66, header, 34, RGBA(31, 42, 58, 255));

    char meta[160];
    snprintf(meta, sizeof(meta), "%d/%d  %s", g_preview.file_index + 1, PREVIEW_FILE_COUNT, file->path);
    draw_text(fb, stride, 78, 118, meta, 22, RGBA(83, 96, 114, 255));

    Rect panel = {76, 150, 1128, 452};
    fill_round_rect(fb, stride, panel, 14, RGBA(255, 255, 255, 255));
    draw_border(fb, stride, panel, RGBA(219, 226, 236, 255));

    draw_text(fb, stride, panel.x + 24, panel.y + 38, g_preview.status, 22, RGBA(73, 86, 105, 255));

    if (!g_preview.exists || g_preview.line_count == 0) {
        const char *empty = g_preview.exists ? "没有可显示的内容。" : "请确认文件已生成并且 SD 卡可读。";
        draw_text(fb, stride, panel.x + 24, panel.y + 100, empty, 28, RGBA(37, 49, 67, 255));
    } else {
        int max_scroll = g_preview.line_count > PREVIEW_LINE_COUNT ?
                         g_preview.line_count - PREVIEW_LINE_COUNT : 0;
        if (g_preview.scroll_line > max_scroll) g_preview.scroll_line = max_scroll;
        if (g_preview.scroll_line < 0) g_preview.scroll_line = 0;

        char scroll[64];
        snprintf(scroll, sizeof(scroll), "行 %d-%d / %d",
                 g_preview.scroll_line + 1,
                 g_preview.scroll_line + PREVIEW_LINE_COUNT < g_preview.line_count ?
                     g_preview.scroll_line + PREVIEW_LINE_COUNT : g_preview.line_count,
                 g_preview.line_count);
        draw_text(fb, stride, panel.x + panel.w - 180, panel.y + 38, scroll, 20, RGBA(111, 126, 147, 255));

        for (int i = 0; i < PREVIEW_LINE_COUNT; i++) {
            int line_index = g_preview.scroll_line + i;
            if (line_index >= g_preview.line_count) break;

            char line[PREVIEW_LINE_MAX];
            char prefix[16];
            copy_preview_line(g_preview.lines[line_index], line, sizeof(line));
            snprintf(prefix, sizeof(prefix), "%4d", line_index + 1);
            int y = panel.y + 82 + i * 24;
            draw_text(fb, stride, panel.x + 24, y, prefix, 18, RGBA(144, 156, 174, 255));
            draw_text(fb, stride, panel.x + 86, y, line, 18, RGBA(37, 49, 67, 255));
        }
    }

    draw_key_hint(fb, stride, 80, "L", "上个文件");
    draw_key_hint(fb, stride, 260, "R", "下个文件");
    draw_key_hint(fb, stride, 440, "↑↓", "滚动");
    draw_key_hint(fb, stride, 602, "Y", "重读");
    draw_key_hint(fb, stride, 744, "B", "返回");
    draw_key_hint(fb, stride, 886, "+", "退出");

    framebufferEnd(&g_ui.fb);
}

static void draw_parent_ui(void) {
    uint32_t stride_bytes = 0;
    uint32_t *fb = (uint32_t *)framebufferBegin(&g_ui.fb, &stride_bytes);
    if (!fb) return;
    uint32_t stride = stride_bytes / sizeof(uint32_t);

    fill_rect(fb, stride, (Rect){0, 0, SCREEN_W, SCREEN_H}, RGBA(238, 243, 249, 255));
    fill_rect(fb, stride, (Rect){0, 0, SCREEN_W, 104}, RGBA(255, 255, 255, 255));
    fill_rect(fb, stride, (Rect){0, 102, SCREEN_W, 2}, RGBA(222, 229, 239, 255));

    draw_text(fb, stride, 76, 66, "Parent Time Manager", 34, RGBA(31, 42, 58, 255));
    draw_text(fb, stride, 78, 118, "Local rules, today overrides, bedtime, reports, and offline grants.", 22,
              RGBA(83, 96, 114, 255));

    draw_time_status(fb, stride);

    Rect panel = {76, 236, 1128, 350};
    fill_round_rect(fb, stride, panel, 14, RGBA(255, 255, 255, 255));
    draw_border(fb, stride, panel, RGBA(219, 226, 236, 255));

    draw_text(fb, stride, panel.x + 26, panel.y + 44, "Today", 28, RGBA(48, 113, 204, 255));
    draw_text(fb, stride, panel.x + 26, panel.y + 90,
              "A +15   Up +30   Left +60   X fixed   Y unlimited   R block   L restore",
              22, RGBA(37, 49, 67, 255));
    draw_text(fb, stride, panel.x + 26, panel.y + 138, "Rules", 28, RGBA(43, 153, 95, 255));
    draw_text(fb, stride, panel.x + 26, panel.y + 184,
              "ZR bedtime   ZL weekly template   Right limit action/raw verify",
              22, RGBA(37, 49, 67, 255));
    draw_text(fb, stride, panel.x + 26, panel.y + 232, "Records and settings", 28, RGBA(111, 126, 147, 255));
    draw_text(fb, stride, panel.x + 26, panel.y + 278,
              "B files/events   Minus parent unlock   Plus exit parent area   Down password",
              22, RGBA(37, 49, 67, 255));

    Rect status = {76, 610, 1128, 54};
    fill_round_rect(fb, stride, status, 10, RGBA(255, 255, 255, 255));
    draw_border(fb, stride, status, RGBA(219, 226, 236, 255));
    draw_text(fb, stride, status.x + 20, status.y + 36, g_state.message, 20, RGBA(37, 49, 67, 255));

    framebufferEnd(&g_ui.fb);
}

static bool point_in_rect(int x, int y, Rect rect) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static void update_touch_buttons(bool *primary_pressed, bool *preview_pressed) {
    if (primary_pressed) *primary_pressed = false;
    if (preview_pressed) *preview_pressed = false;

    HidTouchScreenState touch_state = {0};
    if (hidGetTouchScreenStates(&touch_state, 1) < 1 || touch_state.count < 1) {
        g_state.button_pressed = false;
        g_state.preview_button_pressed = false;
        g_state.touch_was_down = false;
        return;
    }

    int x = (int)touch_state.touches[0].x;
    int y = (int)touch_state.touches[0].y;
    bool primary_inside = point_in_rect(x, y, PRIMARY_BUTTON);
    bool preview_inside = point_in_rect(x, y, PREVIEW_BUTTON);
    bool just_pressed = !g_state.touch_was_down;

    g_state.button_pressed = primary_inside;
    g_state.preview_button_pressed = preview_inside;
    g_state.touch_was_down = true;

    if (primary_pressed) *primary_pressed = primary_inside && just_pressed;
    if (preview_pressed) *preview_pressed = preview_inside && just_pressed;
}

static bool init_ui(void) {
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) return false;

    PlFontData font_data;
    rc = plGetSharedFontByType(&font_data, PlSharedFontType_ChineseSimplified);
    if (R_FAILED(rc)) {
        plExit();
        return false;
    }

    if (FT_Init_FreeType(&g_ui.ft) != 0) {
        plExit();
        return false;
    }
    if (FT_New_Memory_Face(g_ui.ft, (const FT_Byte *)font_data.address, (FT_Long)font_data.size, 0,
                           &g_ui.face) != 0) {
        FT_Done_FreeType(g_ui.ft);
        plExit();
        return false;
    }
    g_ui.font_ready = true;

    rc = framebufferCreate(&g_ui.fb, nwindowGetDefault(), SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) {
        FT_Done_Face(g_ui.face);
        FT_Done_FreeType(g_ui.ft);
        plExit();
        memset(&g_ui, 0, sizeof(g_ui));
        return false;
    }

    rc = framebufferMakeLinear(&g_ui.fb);
    if (R_FAILED(rc)) {
        framebufferClose(&g_ui.fb);
        FT_Done_Face(g_ui.face);
        FT_Done_FreeType(g_ui.ft);
        plExit();
        memset(&g_ui, 0, sizeof(g_ui));
        return false;
    }

    g_ui.fb_ready = true;
    return true;
}

static void exit_ui(void) {
    if (g_ui.fb_ready) framebufferClose(&g_ui.fb);
    if (g_ui.font_ready) FT_Done_Face(g_ui.face);
    if (g_ui.ft) FT_Done_FreeType(g_ui.ft);
    plExit();
}

static void run_console_fallback(const char *error) {
    consoleInit(NULL);
    printf("家长控制离线授权\n");
    printf("================\n\n");
    printf("图形界面初始化失败。\n");
    if (error) printf("%s\n", error);
    printf("\n请确认已安装 switch-freetype，并且系统共享中文字体可用。\n");
    printf("按 + 退出。\n");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
}

static bool show_keyboard(const char *header, const char *guide, char *out, size_t out_size) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return false;

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header ? header : "");
    swkbdConfigSetGuideText(&kbd, guide ? guide : "");
    swkbdConfigSetInitialText(&kbd, "");

    rc = swkbdShow(&kbd, out, out_size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0] != '\0';
}

static void wait_result(void) {
    char result[512];
    set_status(UI_STATUS_WAITING, "正在等待后台授权结果...");

    for (int i = 0; i < 25; i++) {
        if (read_text(GRANT_RESULT_PATH, result, sizeof(result))) {
            set_last_result_from_sysmodule(result);
            return;
        }
        draw_ui();
        svcSleepThread(500000000ULL);
    }

    set_status(UI_STATUS_ERROR, "等待后台授权结果超时，请稍后重试。");
}

static bool apply_time_status_response(const char *text, bool show_message) {
    char status[32] = {0};
    const char *val = json_find_value(text, "status");
    if (!val || !json_read_string(val, status, sizeof(status))) {
        set_time_status_error("时间结果格式错误。");
        if (show_message) set_status(UI_STATUS_ERROR, "时间结果格式错误。");
        return false;
    }

    if (strcmp(status, "status") == 0 || strcmp(status, "ok") == 0) {
        if (!update_time_status_from_json(text)) {
            set_time_status_error("时间结果格式不完整。");
            if (show_message) set_status(UI_STATUS_ERROR, "时间结果格式不完整。");
            return false;
        }
        if (show_message) set_status(UI_STATUS_IDLE, "时间信息已刷新。");
        return true;
    }

    if (strcmp(status, "error") == 0) {
        char reason[96] = {0};
        val = json_find_value(text, "reason");
        if (val) json_read_string(val, reason, sizeof(reason));

        char msg[192];
        snprintf(msg, sizeof(msg), "读取时间失败：%s", reason_to_zh(reason));
        set_time_status_error(msg);
        if (show_message) set_status(UI_STATUS_ERROR, msg);
        return false;
    }

    set_time_status_error("未知时间结果。");
    if (show_message) set_status(UI_STATUS_ERROR, "未知时间结果。");
    return false;
}

static void request_time_status(bool show_message) {
    if (!write_request("{\"type\":\"status\"}")) {
        set_time_status_error("写入时间查询请求失败。");
        if (show_message) set_status(UI_STATUS_ERROR, "写入时间查询请求失败，请检查 SD 卡。");
        return;
    }

    g_time.loaded = false;
    g_time.read_failed = false;
    snprintf(g_time.message, sizeof(g_time.message), "正在刷新时间信息...");
    if (show_message) set_status(UI_STATUS_WAITING, "正在刷新时间信息...");

    char result[512];
    for (int i = 0; i < 25; i++) {
        if (read_text(GRANT_RESULT_PATH, result, sizeof(result))) {
            apply_time_status_response(result, show_message);
            return;
        }
        draw_ui();
        svcSleepThread(500000000ULL);
    }

    set_time_status_error("刷新时间超时。");
    if (show_message) set_status(UI_STATUS_ERROR, "刷新时间超时，请稍后重试。");
}

static void wait_result_parent(void) {
    char result[1024];
    set_status(UI_STATUS_WAITING, "Waiting for sysmodule...");

    for (int i = 0; i < 25; i++) {
        if (read_text(GRANT_RESULT_PATH, result, sizeof(result))) {
            set_last_result_from_sysmodule(result);
            return;
        }
        draw_parent_ui();
        svcSleepThread(500000000ULL);
    }

    set_status(UI_STATUS_ERROR, "Timed out waiting for sysmodule.");
}

static void write_parent_request_and_wait(const char *req) {
    if (!write_request(req)) {
        set_status(UI_STATUS_ERROR, "Failed to write request to SD card.");
        return;
    }
    wait_result_parent();
    request_time_status(false);
}

static bool read_number_keyboard(const char *header, const char *guide, int *out) {
    char text[32] = {0};
    if (!show_keyboard(header, guide, text, sizeof(text))) return false;
    trim_line(text);
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || value < 0 || value > 1440) return false;
    *out = (int)value;
    return true;
}

static void parent_set_fixed_limit(void) {
    int minutes = 0;
    if (!read_number_keyboard("Today fixed limit", "Minutes 1-1440", &minutes) || minutes <= 0) {
        set_status(UI_STATUS_IDLE, "Canceled or invalid minutes.");
        return;
    }
    char req[96];
    snprintf(req, sizeof(req), "{\"type\":\"set_today_limit\",\"minutes\":%d}", minutes);
    write_parent_request_and_wait(req);
}

static void parent_add_minutes(int minutes) {
    char req[96];
    snprintf(req, sizeof(req), "{\"type\":\"add_today_minutes\",\"minutes\":%d}", minutes);
    write_parent_request_and_wait(req);
}

static void parent_set_bedtime(void) {
    int start = 0;
    int end = 0;
    if (!read_number_keyboard("Bedtime start", "Minute of day, e.g. 1260 for 21:00", &start)) {
        set_status(UI_STATUS_IDLE, "Canceled bedtime.");
        return;
    }
    if (!read_number_keyboard("Morning allow", "Minute of day, e.g. 480 for 08:00", &end)) {
        set_status(UI_STATUS_IDLE, "Canceled bedtime.");
        return;
    }
    char req[128];
    snprintf(req, sizeof(req), "{\"type\":\"set_bedtime\",\"enabled\":true,\"start_min\":%d,\"end_min\":%d}", start, end);
    write_parent_request_and_wait(req);
}

static void parent_set_weekly_template(void) {
    int weekday = 0;
    int weekend = 0;
    if (!read_number_keyboard("Weekday limit", "Mon-Fri minutes, 0 means unlimited", &weekday)) {
        set_status(UI_STATUS_IDLE, "Canceled weekly template.");
        return;
    }
    if (!read_number_keyboard("Weekend limit", "Sat-Sun minutes, 0 means unlimited", &weekend)) {
        set_status(UI_STATUS_IDLE, "Canceled weekly template.");
        return;
    }

    char req[640];
    snprintf(req, sizeof(req),
             "{\"type\":\"set_weekly_template\","
             "\"day0_mode\":\"%s\",\"day0_minutes\":%d,"
             "\"day1_mode\":\"%s\",\"day1_minutes\":%d,"
             "\"day2_mode\":\"%s\",\"day2_minutes\":%d,"
             "\"day3_mode\":\"%s\",\"day3_minutes\":%d,"
             "\"day4_mode\":\"%s\",\"day4_minutes\":%d,"
             "\"day5_mode\":\"%s\",\"day5_minutes\":%d,"
             "\"day6_mode\":\"%s\",\"day6_minutes\":%d}",
             weekend > 0 ? "limit" : "unlimited", weekend > 0 ? weekend : 120,
             weekday > 0 ? "limit" : "unlimited", weekday > 0 ? weekday : 120,
             weekday > 0 ? "limit" : "unlimited", weekday > 0 ? weekday : 120,
             weekday > 0 ? "limit" : "unlimited", weekday > 0 ? weekday : 120,
             weekday > 0 ? "limit" : "unlimited", weekday > 0 ? weekday : 120,
             weekday > 0 ? "limit" : "unlimited", weekday > 0 ? weekday : 120,
             weekend > 0 ? "limit" : "unlimited", weekend > 0 ? weekend : 120);
    write_parent_request_and_wait(req);
}

static void parent_set_limit_action(void) {
    if (!g_time.raw_block_verified && !g_state.raw_probe_pending) {
        g_state.raw_probe_pending = true;
        write_parent_request_and_wait("{\"type\":\"probe_raw_block\"}");
        set_status(UI_STATUS_WAITING, "Raw 0 probe sent. Verify console behavior, then press Right again to mark verified.");
        return;
    }

    if (!g_time.raw_block_verified && g_state.raw_probe_pending) {
        char req[160];
        snprintf(req, sizeof(req),
                 "{\"type\":\"set_limit_action\",\"action\":\"%s\",\"raw_block_verified\":true,\"suspend_verified\":%s}",
                 g_time.limit_action[0] ? g_time.limit_action : "remind",
                 g_time.suspend_verified ? "true" : "false");
        g_state.raw_probe_pending = false;
        write_parent_request_and_wait(req);
        return;
    }

    const char *next_action = strcmp(g_time.limit_action, "suspend") == 0 ? "remind" : "suspend";
    char req[160];
    snprintf(req, sizeof(req),
             "{\"type\":\"set_limit_action\",\"action\":\"%s\",\"raw_block_verified\":true,\"suspend_verified\":%s}",
             next_action,
             g_time.suspend_verified ? "true" : "false");
    write_parent_request_and_wait(req);
}

static void parent_unlock_start(void) {
    int minutes = 0;
    if (!read_number_keyboard("Parent unlock", "Minutes to pause play timer", &minutes) || minutes <= 0) {
        set_status(UI_STATUS_IDLE, "Canceled parent unlock.");
        return;
    }
    char req[96];
    snprintf(req, sizeof(req), "{\"type\":\"parent_unlock_start\",\"minutes\":%d}", minutes);
    write_parent_request_and_wait(req);
}

static void request_offline_code(void) {
    char code[64] = {0};
    if (!show_keyboard("输入离线授权码", "示例：ABCD-EFGH-JKQ2-M7P9", code, sizeof(code))) {
        set_status(UI_STATUS_IDLE, "已取消输入。");
        return;
    }

    char req[160];
    char escaped[96];
    json_escape(code, escaped, sizeof(escaped));
    snprintf(req, sizeof(req), "{\"type\":\"offline_code\",\"code\":\"%s\"}", escaped);
    if (!write_request(req)) {
        set_status(UI_STATUS_ERROR, "写入授权请求失败，请检查 SD 卡。");
        return;
    }

    wait_result();
}

static void open_settings(void) {
    char current[64];
    char entered[64] = {0};
    read_password(current, sizeof(current));

    if (!show_keyboard("设置密码", "请输入当前设置密码", entered, sizeof(entered))) {
        set_status(UI_STATUS_IDLE, "已取消设置。");
        return;
    }
    trim_line(entered);

    if (strcmp(entered, current) != 0) {
        set_status(UI_STATUS_ERROR, "设置密码错误。");
        return;
    }

    char next[64] = {0};
    if (!show_keyboard("新设置密码", "请输入新的设置密码", next, sizeof(next))) {
        set_status(UI_STATUS_IDLE, "密码未修改。");
        return;
    }
    trim_line(next);

    if (next[0] == '\0') {
        set_status(UI_STATUS_ERROR, "密码不能为空。");
        return;
    }

    if (!write_text_atomic(SETTINGS_PATH, next)) {
        set_status(UI_STATUS_ERROR, "写入设置失败，请检查 SD 卡。");
        return;
    }

    set_status(UI_STATUS_SUCCESS, "设置密码已更新。");
}

static bool check_settings_password(const char *cancel_msg) {
    char current[64];
    char entered[64] = {0};
    read_password(current, sizeof(current));

    if (!show_keyboard("设置密码", "请输入当前设置密码", entered, sizeof(entered))) {
        set_status(UI_STATUS_IDLE, cancel_msg ? cancel_msg : "已取消。");
        return false;
    }
    trim_line(entered);

    if (strcmp(entered, current) != 0) {
        set_status(UI_STATUS_ERROR, "设置密码错误。");
        return false;
    }

    return true;
}

static bool open_file_preview(bool require_password) {
    if (require_password && !check_settings_password("已取消查看文件。")) {
        return true;
    }

    load_preview_file();
    g_state.button_pressed = false;
    g_state.preview_button_pressed = false;
    g_state.touch_was_down = false;

    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
            return false;
        }
        if (down & HidNpadButton_B) {
            return true;
        }
        if (down & HidNpadButton_L) {
            g_preview.file_index--;
            if (g_preview.file_index < 0) g_preview.file_index = PREVIEW_FILE_COUNT - 1;
            load_preview_file();
        }
        if (down & HidNpadButton_R) {
            g_preview.file_index++;
            if (g_preview.file_index >= PREVIEW_FILE_COUNT) g_preview.file_index = 0;
            load_preview_file();
        }
        if (down & HidNpadButton_Up) {
            if (g_preview.scroll_line > 0) g_preview.scroll_line--;
        }
        if (down & HidNpadButton_Down) {
            int max_scroll = g_preview.line_count > PREVIEW_LINE_COUNT ?
                             g_preview.line_count - PREVIEW_LINE_COUNT : 0;
            if (g_preview.scroll_line < max_scroll) g_preview.scroll_line++;
        }
        if (down & HidNpadButton_Y) {
            load_preview_file();
        }

        draw_preview_ui();
    }

    return false;
}

static bool open_parent_area(void) {
    if (!check_settings_password("Parent area canceled.")) {
        return true;
    }

    request_time_status(false);

    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
            return true;
        }
        if (down & HidNpadButton_A) {
            parent_add_minutes(15);
        }
        if (down & HidNpadButton_Up) {
            parent_add_minutes(30);
        }
        if (down & HidNpadButton_Left) {
            parent_add_minutes(60);
        }
        if (down & HidNpadButton_X) {
            parent_set_fixed_limit();
        }
        if (down & HidNpadButton_Y) {
            write_parent_request_and_wait("{\"type\":\"disable_today_limit\"}");
        }
        if (down & HidNpadButton_R) {
            write_parent_request_and_wait("{\"type\":\"block_today\"}");
        }
        if (down & HidNpadButton_L) {
            write_parent_request_and_wait("{\"type\":\"restore_today_policy\"}");
        }
        if (down & HidNpadButton_ZR) {
            parent_set_bedtime();
        }
        if (down & HidNpadButton_ZL) {
            parent_set_weekly_template();
        }
        if (down & HidNpadButton_Right) {
            parent_set_limit_action();
        }
        if (down & HidNpadButton_Minus) {
            if (g_time.parent_unlock_active) {
                write_parent_request_and_wait("{\"type\":\"parent_unlock_end\"}");
            } else {
                parent_unlock_start();
            }
        }
        if (down & HidNpadButton_Down) {
            open_settings();
        }
        if (down & HidNpadButton_B) {
            if (!open_file_preview(false)) {
                return false;
            }
        }

        draw_parent_ui();
    }

    return false;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    fsdevMountSdmc();
    ensure_dirs();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    hidInitializeTouchScreen();

    if (!init_ui()) {
        run_console_fallback("无法创建 framebuffer 或加载简体中文字体。");
        fsdevUnmountAll();
        return 1;
    }

    PadState pad;
    padInitializeDefault(&pad);

    char result[512];
    if (read_text(GRANT_RESULT_PATH, result, sizeof(result))) {
        set_last_result_from_sysmodule(result);
    }
    request_time_status(false);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad);

        if (down & HidNpadButton_Plus) {
            break;
        }

        bool touch_primary = false;
        bool touch_preview = false;
        update_touch_buttons(&touch_primary, &touch_preview);
        if ((down & HidNpadButton_A) || touch_primary) {
            request_offline_code();
        }

        if ((down & HidNpadButton_B) || touch_preview) {
            if (read_text(GRANT_RESULT_PATH, result, sizeof(result))) {
                set_last_result_from_sysmodule(result);
            } else {
                set_status(UI_STATUS_IDLE, "没有找到最近结果。");
            }
        }

        if ((held & HidNpadButton_L) && (held & HidNpadButton_R) && (held & HidNpadButton_X)) {
            if (g_state.parent_hold_frames < PARENT_HOLD_FRAMES) g_state.parent_hold_frames++;
            if (g_state.parent_hold_frames >= PARENT_HOLD_FRAMES && !g_state.parent_triggered) {
                g_state.parent_triggered = true;
                if (!open_parent_area()) {
                    break;
                }
            }
        } else {
            g_state.parent_hold_frames = 0;
            g_state.parent_triggered = false;
        }

        if (down & HidNpadButton_Y) {
            request_time_status(true);
        }

        draw_ui();
    }

    exit_ui();
    fsdevUnmountAll();
    return 0;
}
