#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TIMESTAMP="$(date '+%Y-%m-%d_%H-%M-%S')"
OUT_NAME="pctltcp-offline-grant-sdmc-${TIMESTAMP}.zip"
KEEP_STAGING=0
INSTALL_BOOT2=0
CONTROL_MODE="observe"
MAX_ADD_MINUTES=""
ALLOW_UNLIMITED_TO_LIMITED=""

usage() {
    cat <<EOF
用法: $(basename "$0") [选项] [输出.zip]

根据当前构建产物生成可解压到 Switch SD 卡根目录的安装包。

选项:
  --keep-staging                  打包后保留临时目录 sdmc-pack。
  --boot2                         打包 Atmosphere boot2.flag。
  --control-mode M                设置 grant.conf 中的 control_mode。
                                  可选: disabled、observe、grant、enforce；默认 observe。
  --max-add-minutes N             设置 grant.conf 中的 max_add_minutes，便于测试分钟上限。
  --allow-unlimited-to-limited    允许把无限制/未配置的当天改成有限制，用于写入测试。
  --no-allow-unlimited-to-limited 禁止把无限制/未配置的当天改成有限制，安全测试默认值。
  --safe-test                     安全测试快捷参数：不打包 boot2.flag，control_mode=observe，
                                  allow_unlimited_to_limited=false。
  --strong-control                强控制快捷参数：打包 boot2.flag，control_mode=enforce。
  -h, --help                      显示帮助。
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --keep-staging)
            KEEP_STAGING=1
            shift
            ;;
        --boot2)
            INSTALL_BOOT2=1
            shift
            ;;
        --control-mode)
            if [ "$#" -lt 2 ]; then
                echo "错误：--control-mode 需要一个参数。" >&2
                usage >&2
                exit 2
            fi
            case "$2" in
                disabled|observe|grant|enforce)
                    CONTROL_MODE="$2"
                    ;;
                *)
                    echo "错误：无效的 control_mode：$2" >&2
                    usage >&2
                    exit 2
                    ;;
            esac
            shift 2
            ;;
        --max-add-minutes)
            if [ "$#" -lt 2 ]; then
                echo "错误：--max-add-minutes 需要一个参数。" >&2
                usage >&2
                exit 2
            fi
            case "$2" in
                ''|*[!0-9]*)
                    echo "错误：max_add_minutes 必须是 1 到 1440 之间的整数。" >&2
                    usage >&2
                    exit 2
                    ;;
                *)
                    if [ "$2" -lt 1 ] || [ "$2" -gt 1440 ]; then
                        echo "错误：max_add_minutes 必须是 1 到 1440 之间的整数。" >&2
                        usage >&2
                        exit 2
                    fi
                    MAX_ADD_MINUTES="$2"
                    ;;
            esac
            shift 2
            ;;
        --allow-unlimited-to-limited)
            ALLOW_UNLIMITED_TO_LIMITED="true"
            shift
            ;;
        --no-allow-unlimited-to-limited)
            ALLOW_UNLIMITED_TO_LIMITED="false"
            shift
            ;;
        --safe-test)
            INSTALL_BOOT2=0
            CONTROL_MODE="observe"
            ALLOW_UNLIMITED_TO_LIMITED="false"
            shift
            ;;
        --strong-control)
            INSTALL_BOOT2=1
            CONTROL_MODE="enforce"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "错误：未知选项：$1" >&2
            usage >&2
            exit 2
            ;;
        *)
            OUT_NAME="$1"
            shift
            ;;
    esac
done

PACK_DIR="$ROOT_DIR/sdmc-pack"
OUT_PATH="$ROOT_DIR/$OUT_NAME"

SYS_NSP="$ROOT_DIR/pctltcp-sysmodule.nsp"
GRANT_NRO="$ROOT_DIR/companion/pctltcp-grant.nro"
TOOLBOX="$ROOT_DIR/toolbox.json"
GRANT_CONF="$ROOT_DIR/grant.conf"
GRANT_CONF_EXAMPLE="$ROOT_DIR/grant.conf.example"
SETTINGS_CONF="$ROOT_DIR/settings.conf"
SETTINGS_CONF_EXAMPLE="$ROOT_DIR/settings.conf.example"

need_file() {
    if [ ! -f "$1" ]; then
        echo "缺少文件：$1" >&2
        return 1
    fi
}

missing=0
need_file "$SYS_NSP" || missing=1
need_file "$GRANT_NRO" || missing=1
need_file "$TOOLBOX" || missing=1
need_file "$GRANT_CONF_EXAMPLE" || missing=1
need_file "$SETTINGS_CONF_EXAMPLE" || missing=1

if [ "$missing" -ne 0 ]; then
    echo
    echo "缺少构建产物。请先运行 'make'，然后重新执行本脚本。" >&2
    exit 1
fi

find_python() {
    if command -v python3 >/dev/null 2>&1; then
        echo "python3"
    elif command -v python >/dev/null 2>&1; then
        echo "python"
    else
        return 1
    fi
}

cleanup_staging() {
    if [ "$KEEP_STAGING" -eq 0 ]; then
        rm -rf "$PACK_DIR"
    fi
}

update_grant_config() {
    local config_path="$1"
    local pybin

    if ! pybin="$(find_python)"; then
        echo "错误：需要 python 或 python3 来更新 grant.conf。" >&2
        exit 1
    fi

    "$pybin" - "$config_path" "$CONTROL_MODE" "$MAX_ADD_MINUTES" "$ALLOW_UNLIMITED_TO_LIMITED" <<'PY'
import json
import sys

path, mode, max_add_minutes, allow_unlimited_to_limited = sys.argv[1:5]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

data["control_mode"] = mode

if max_add_minutes:
    data["max_add_minutes"] = int(max_add_minutes)

if allow_unlimited_to_limited:
    data["allow_unlimited_to_limited"] = allow_unlimited_to_limited == "true"
else:
    data.setdefault("allow_unlimited_to_limited", False)

with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=4, ensure_ascii=False)
    f.write("\n")
PY
}

rm -rf "$PACK_DIR"
rm -f "$OUT_PATH"
trap cleanup_staging EXIT

mkdir -p "$PACK_DIR/atmosphere/contents/010000000000BD23"
mkdir -p "$PACK_DIR/switch/pctltcp-sysmodule"
mkdir -p "$PACK_DIR/switch"

cp "$SYS_NSP" "$PACK_DIR/atmosphere/contents/010000000000BD23/exefs.nsp"
cp "$TOOLBOX" "$PACK_DIR/atmosphere/contents/010000000000BD23/toolbox.json"
if [ "$INSTALL_BOOT2" -eq 1 ]; then
    mkdir -p "$PACK_DIR/atmosphere/contents/010000000000BD23/flags"
    : > "$PACK_DIR/atmosphere/contents/010000000000BD23/flags/boot2.flag"
fi

cp "$GRANT_NRO" "$PACK_DIR/switch/pctltcp-grant.nro"
if [ -f "$GRANT_CONF" ]; then
    cp "$GRANT_CONF" "$PACK_DIR/switch/pctltcp-sysmodule/grant.conf"
else
    cp "$GRANT_CONF_EXAMPLE" "$PACK_DIR/switch/pctltcp-sysmodule/grant.conf"
fi
update_grant_config "$PACK_DIR/switch/pctltcp-sysmodule/grant.conf"

if [ -f "$SETTINGS_CONF" ]; then
    cp "$SETTINGS_CONF" "$PACK_DIR/switch/pctltcp-sysmodule/settings.conf"
else
    cp "$SETTINGS_CONF_EXAMPLE" "$PACK_DIR/switch/pctltcp-sysmodule/settings.conf"
fi

(
    cd "$PACK_DIR"
    if command -v zip >/dev/null 2>&1; then
        zip -qr "$OUT_PATH" .
    else
        if ! pybin="$(find_python)"; then
            echo "错误：未找到 zip，也未找到 python/python3，无法创建 zip 包。" >&2
            exit 1
        fi
        "$pybin" -m zipfile -c "$OUT_PATH" .
    fi
)

echo "已生成安装包：$OUT_PATH"
if [ "$KEEP_STAGING" -eq 1 ]; then
    echo "已保留临时目录：$PACK_DIR"
else
    echo "已清理临时目录：$PACK_DIR"
fi
echo
echo "安装方式：将 zip 解压到 SD 卡根目录。"
echo "已打包 control_mode：$CONTROL_MODE"
if [ -n "$MAX_ADD_MINUTES" ]; then
    echo "已打包 max_add_minutes：$MAX_ADD_MINUTES"
else
    echo "已打包 max_add_minutes：沿用 grant.conf 当前值"
fi
if [ -n "$ALLOW_UNLIMITED_TO_LIMITED" ]; then
    echo "已打包 allow_unlimited_to_limited：$ALLOW_UNLIMITED_TO_LIMITED"
else
    echo "已打包 allow_unlimited_to_limited：沿用 grant.conf 当前值；缺省时为 false"
fi
if [ "$INSTALL_BOOT2" -eq 1 ]; then
    echo "已包含 boot2.flag：是"
else
    echo "已包含 boot2.flag：否（安全测试包）"
fi
echo "使用前请确认 switch/pctltcp-sysmodule/grant.conf 中的设备 ID 和密钥。"
echo "如需 fail-open，可在 SD 卡创建 switch/pctltcp-sysmodule/disable.flag。"
echo "设置页默认密码：1234。"
