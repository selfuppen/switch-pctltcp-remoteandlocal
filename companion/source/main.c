#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GRANT_DIR          "sdmc:/switch/pctltcp-sysmodule"
#define GRANT_REQUEST_PATH GRANT_DIR "/grant_request.json"
#define GRANT_RESULT_PATH  GRANT_DIR "/grant_result.json"
#define SETTINGS_PATH      GRANT_DIR "/settings.conf"
#define DEFAULT_PASSWORD   "1234"

static char g_last_result[512] = "No result yet.";

static void set_last_result(const char *msg) {
    if (!msg) msg = "";
    strncpy(g_last_result, msg, sizeof(g_last_result) - 1);
    g_last_result[sizeof(g_last_result) - 1] = '\0';
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

static bool wait_result(void) {
    for (int i = 0; i < 25; i++) {
        if (read_text(GRANT_RESULT_PATH, g_last_result, sizeof(g_last_result))) {
            return true;
        }
        svcSleepThread(500000000ULL);
    }

    set_last_result("Timed out waiting for sysmodule result.");
    return false;
}

static bool show_keyboard(const char *header, const char *guide,
                          char *out, size_t out_size) {
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

static void request_offline_code(void) {
    char code[64] = {0};
    if (!show_keyboard("Offline Grant Code", "ABCD-EFGH-JKQ2-M7P9",
                       code, sizeof(code))) {
        set_last_result("Keyboard canceled.");
        return;
    }

    char req[160];
    char escaped[96];
    json_escape(code, escaped, sizeof(escaped));
    snprintf(req, sizeof(req), "{\"type\":\"offline_code\",\"code\":\"%s\"}", escaped);
    if (!write_request(req)) {
        set_last_result("Failed to write grant_request.json.");
        return;
    }

    wait_result();
}

static void open_settings(void) {
    char current[64];
    char entered[64] = {0};
    read_password(current, sizeof(current));

    if (!show_keyboard("Settings Password", "Enter current password",
                       entered, sizeof(entered))) {
        set_last_result("Settings canceled.");
        return;
    }
    trim_line(entered);

    if (strcmp(entered, current) != 0) {
        set_last_result("Wrong settings password.");
        return;
    }

    char next[64] = {0};
    if (!show_keyboard("New Settings Password", "Enter a new password",
                       next, sizeof(next))) {
        set_last_result("Password unchanged.");
        return;
    }
    trim_line(next);

    if (next[0] == '\0') {
        set_last_result("Password cannot be empty.");
        return;
    }

    if (!write_text_atomic(SETTINGS_PATH, next)) {
        set_last_result("Failed to write settings.conf.");
        return;
    }

    set_last_result("Settings password updated.");
}

static void draw_screen(void) {
    consoleClear();
    printf("Pctl Offline Grant\n");
    printf("==================\n\n");
    printf("A      Enter offline code\n");
    printf("X      Settings\n");
    printf("Y      Reload last result\n");
    printf("PLUS   Exit\n\n");
    printf("Last result:\n%s\n", g_last_result);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);
    fsdevMountSdmc();
    ensure_dirs();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    read_text(GRANT_RESULT_PATH, g_last_result, sizeof(g_last_result));

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus) {
            break;
        }
        if (down & HidNpadButton_A) {
            request_offline_code();
        }
        if (down & HidNpadButton_X) {
            open_settings();
        }
        if (down & HidNpadButton_Y) {
            if (!read_text(GRANT_RESULT_PATH, g_last_result, sizeof(g_last_result))) {
                set_last_result("No result file found.");
            }
        }

        draw_screen();
        consoleUpdate(NULL);
    }

    fsdevUnmountAll();
    consoleExit(NULL);
    return 0;
}
