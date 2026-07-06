#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TIMESTAMP="$(date '+%Y-%m-%d_%H-%M-%S')"
OUT_NAME="pctltcp-offline-grant-sdmc-${TIMESTAMP}.zip"
KEEP_STAGING=0
INSTALL_BOOT2=0
CONTROL_MODE="observe"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--keep-staging] [output.zip]

Build a Switch SD-card install zip from the latest build outputs.

Options:
  --keep-staging   Keep the temporary sdmc-pack directory after zipping.
  --boot2          Include the Atmosphere boot2 flag.
  --control-mode M Set packaged grant.conf control_mode
                   (disabled, observe, grant, enforce). Defaults to observe.
  --strong-control Shortcut for --boot2 --control-mode enforce.
  -h, --help       Show this help.
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
                echo "--control-mode requires a value" >&2
                usage >&2
                exit 2
            fi
            case "$2" in
                disabled|observe|grant|enforce)
                    CONTROL_MODE="$2"
                    ;;
                *)
                    echo "invalid control mode: $2" >&2
                    usage >&2
                    exit 2
                    ;;
            esac
            shift 2
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
            echo "unknown option: $1" >&2
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
        echo "missing: $1" >&2
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
    echo "Build outputs are missing. Run 'make' first, then rerun this script." >&2
    exit 1
fi

cleanup_staging() {
    if [ "$KEEP_STAGING" -eq 0 ]; then
        rm -rf "$PACK_DIR"
    fi
}

update_control_mode() {
    local config_path="$1"
    local pybin=""

    if command -v python3 >/dev/null 2>&1; then
        pybin="python3"
    elif command -v python >/dev/null 2>&1; then
        pybin="python"
    else
        echo "python is required to stamp control_mode into grant.conf" >&2
        exit 1
    fi

    "$pybin" - "$config_path" "$CONTROL_MODE" <<'PY'
import json
import sys

path, mode = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

data["control_mode"] = mode
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
update_control_mode "$PACK_DIR/switch/pctltcp-sysmodule/grant.conf"

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
        python3 -m zipfile -c "$OUT_PATH" .
    fi
)

echo "Created: $OUT_PATH"
if [ "$KEEP_STAGING" -eq 1 ]; then
    echo "Kept staging directory: $PACK_DIR"
else
    echo "Cleaned staging directory: $PACK_DIR"
fi
echo
echo "Install by extracting the zip into the SD card root."
echo "Packaged control_mode: $CONTROL_MODE"
if [ "$INSTALL_BOOT2" -eq 1 ]; then
    echo "boot2.flag included: yes"
else
    echo "boot2.flag included: no (safe test package)"
fi
echo "Edit switch/pctltcp-sysmodule/grant.conf before use."
echo "Create switch/pctltcp-sysmodule/disable.flag on the SD card to fail open."
echo "The default settings password is 1234."
