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

typedef struct {
    char device_id[CFG_DEVICE_MAX];
    char grant_secret[CFG_SECRET_MAX];
    int max_add_minutes;
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

static GrantConfig s_cfg;
static Mutex s_pctl_mutex;

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

static void write_result(const char *status, const char *reason, int applied_minutes, int today_limit) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", GRANT_RESULT_PATH);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    if (status && strcmp(status, "ok") == 0) {
        fprintf(f, "{\"status\":\"ok\",\"applied_minutes\":%d,\"today_limit\":%d}\n",
                applied_minutes, today_limit);
    } else {
        fprintf(f, "{\"status\":\"error\",\"reason\":\"%s\"}\n", reason ? reason : "unknown");
    }

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

    s_cfg.loaded = true;
}

static bool get_current_date_key(uint16_t *date_key) {
    if (!date_key) return false;

    u64 now_posix = 0;
    Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
    if (R_FAILED(rc) || now_posix <= 946684800ULL) {
        rc = timeGetCurrentTime(TimeType_LocalSystemClock, &now_posix);
    }
    if (R_FAILED(rc) || now_posix <= 946684800ULL) {
        rc = timeGetCurrentTime(TimeType_UserSystemClock, &now_posix);
    }
    if (R_FAILED(rc) || now_posix <= 946684800ULL) {
        return false;
    }

    TimeCalendarTime cal;
    TimeCalendarAdditionalInfo additional;
    rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
    if (R_FAILED(rc)) {
        time_t t = (time_t)now_posix;
        struct tm *tm_info = gmtime(&t);
        if (!tm_info) return false;
        cal.year = tm_info->tm_year + 1900;
        cal.month = tm_info->tm_mon + 1;
        cal.day = tm_info->tm_mday;
    }

    if (cal.year < 2020 || cal.year > 2147 || cal.month < 1 || cal.month > 12 ||
        cal.day < 1 || cal.day > 31) {
        return false;
    }

    *date_key = (uint16_t)(((cal.year - 2020) << 9) | (cal.month << 5) | cal.day);
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

    u32 daily_limit = 0;
    rc = pctl_get_daily_limit_minutes(&daily_limit);
    if (R_FAILED(rc)) {
        pctl_exit();
        mutexUnlock(&s_pctl_mutex);
        *reason = "pctl_read_failed";
        return false;
    }

    int new_limit = (int)daily_limit + minutes;
    if (new_limit < 0) new_limit = 0;
    if (new_limit > 1440) new_limit = 1440;

    int today = pctl_get_today_day();
    rc = pctl_set_day_limit_minutes(today, (u32)new_limit);
    if (R_SUCCEEDED(rc)) {
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

    if (!apply_add_minutes(token.minutes, today_limit, reason)) {
        return false;
    }

    if (!used_nonce_append(token.date_key, token.nonce)) {
        log_msg("grant: warning, used ledger append failed");
    }

    if (applied_minutes) *applied_minutes = token.minutes;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void grant_manager_init(void) {
    mutexInit(&s_pctl_mutex);
    load_config();
    if (s_cfg.loaded) {
        log_msg("grant: config loaded");
    } else {
        log_msg("grant: config not loaded");
    }
}

void grant_manager_process(void) {
    char req[1024];
    if (!read_file_text(GRANT_REQUEST_PATH, req, sizeof(req))) {
        return;
    }

    remove(GRANT_REQUEST_PATH);
    load_config();

    const char *type_val = json_find_value(req, "type");
    char type[32];
    if (!type_val || !json_read_string(type_val, type, sizeof(type))) {
        write_result("error", "bad_request", 0, 0);
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
        reason = "unknown_request";
    }

    if (ok) {
        write_result("ok", NULL, applied, today_limit);
        log_msg("grant: applied successfully");
    } else {
        write_result("error", reason ? reason : "unknown", 0, 0);
        char msg[128];
        snprintf(msg, sizeof(msg), "grant: rejected (%s)", reason ? reason : "unknown");
        log_msg(msg);
    }
}
