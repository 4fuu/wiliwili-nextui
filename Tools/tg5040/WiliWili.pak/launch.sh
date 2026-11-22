#!/bin/sh
# WiliWili for NextUI (tg5040 / TrimUI Brick Pro)
#
# Runs the bundled binary with the pak's own libraries and returns to the
# NextUI desktop when it exits. HOME handling is left to NextUI's system
# overlay: this pak does not declare the "overlay" capability, so the desktop's
# HOME panel owns exit/resume.
#
# No sleep integration is needed: NextUI only runs its idle-sleep loop in the
# desktop process, and that process exits while an app is in the foreground.

PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
PAK_NAME=$(basename "$PAK_DIR")
PAK_NAME=${PAK_NAME%.pak}

SDCARD_PATH=${SDCARD_PATH:-/mnt/SDCARD}
USERDATA_PATH=${USERDATA_PATH:-$SDCARD_PATH/.userdata/tg5040}
SHARED_USERDATA_PATH=${SHARED_USERDATA_PATH:-$SDCARD_PATH/.userdata/shared}
LOGS_PATH=${LOGS_PATH:-$USERDATA_PATH/logs}

mkdir -p "$LOGS_PATH" "$SHARED_USERDATA_PATH/wiliwili" 2>/dev/null

# Everything the app prints goes to the NextUI log folder; the desktop never
# sees it on the console. Fall back to /dev/null so the script also works when
# it is started by hand outside NextUI.
LOG_FILE="$LOGS_PATH/$PAK_NAME.txt"
# A redirection error on a special builtin is fatal in ash/dash, so probe in a
# subshell; /dev/null keeps a hand-run launch.sh usable outside NextUI.
if ! ( : >"$LOG_FILE" ) 2>/dev/null; then
    LOG_FILE=/dev/null
fi
exec >>"$LOG_FILE" 2>&1

echo "=== $PAK_NAME $(date) ==="
echo "pak: $PAK_DIR"
echo "device: ${DEVICE:-unknown} platform: ${PLATFORM:-unknown}"

cd "$PAK_DIR" || exit 1

# The pak ships bin/wiliwili, lib/*.so* and resources/ next to this script.
export PATH="$PAK_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$PAK_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# wiliwili resolves its config directory as $XDG_CONFIG_HOME/wiliwili, so this
# yields $SHARED_USERDATA_PATH/wiliwili/wiliwili_config.json.
export XDG_CONFIG_HOME="$SHARED_USERDATA_PATH"
export HOME="$SHARED_USERDATA_PATH/wiliwili"

# The device has no PulseAudio; the vendor SDL2 provides the "mali" video
# driver and an ALSA audio backend. The video driver is left on auto-selection
# so a renamed vendor driver cannot abort startup, but audio is pinned because
# SDL otherwise probes backends the image does not have.
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
if [ -n "${WILIWILI_SDL_VIDEODRIVER:-}" ]; then
    export SDL_VIDEODRIVER="$WILIWILI_SDL_VIDEODRIVER"
fi

# No system CA store: NextUI ships a Mozilla-derived bundle. Prefer it and fall
# back to the copy inside the pak if a future image moves it.
CA_BUNDLE="$SDCARD_PATH/.system/res/ca-bundle.crt"
if [ ! -r "$CA_BUNDLE" ]; then
    CA_BUNDLE="$PAK_DIR/res/ca-bundle.crt"
fi
export CURL_CA_BUNDLE="$CA_BUNDLE"
export SSL_CERT_FILE="$CA_BUNDLE"
echo "ca bundle: $CA_BUNDLE"

# WiFi power save: with it on, wlan0 latency averages ~23ms, off it drops to
# ~5-9ms (measured on this device by the Moonlight pak), which decides how fast
# pages and video segments load. Turn it off for the run and put the previous
# state back afterwards. Every step is best effort: a missing iw/wlan0 or a
# failed call just leaves the radio alone.
WLAN_IFACE=wlan0
POWER_SAVE_CHANGED=0
POWER_SAVE_PREVIOUS=

wifi_power_save_restore() {
    [ "$POWER_SAVE_CHANGED" = "1" ] || return 0
    POWER_SAVE_CHANGED=0
    if iw dev "$WLAN_IFACE" set power_save "$POWER_SAVE_PREVIOUS" 2>/dev/null; then
        echo "wifi power_save restored to $POWER_SAVE_PREVIOUS on $WLAN_IFACE"
    else
        echo "wifi power_save restore to $POWER_SAVE_PREVIOUS failed on $WLAN_IFACE"
    fi
    return 0
}

# The restore must run however the app ends, including SIGTERM from the
# desktop. Recording the signal and finishing normally keeps the exit status
# meaningful and the restore unconditional.
LAUNCH_SIGNAL=
trap 'LAUNCH_SIGNAL=HUP' HUP
trap 'LAUNCH_SIGNAL=INT' INT
trap 'LAUNCH_SIGNAL=TERM' TERM

if command -v iw >/dev/null 2>&1 && iw dev "$WLAN_IFACE" info >/dev/null 2>&1; then
    POWER_SAVE_CURRENT=$(iw dev "$WLAN_IFACE" get power_save 2>/dev/null)
    POWER_SAVE_PREVIOUS=${POWER_SAVE_CURRENT##* }
    case "$POWER_SAVE_PREVIOUS" in
        on)
            if iw dev "$WLAN_IFACE" set power_save off 2>/dev/null; then
                POWER_SAVE_CHANGED=1
                echo "wifi power_save off on $WLAN_IFACE (was on)"
            else
                echo "wifi power_save off failed on $WLAN_IFACE"
            fi
            ;;
        off)
            echo "wifi power_save already off on $WLAN_IFACE"
            ;;
        *)
            echo "wifi power_save state unknown on $WLAN_IFACE: ${POWER_SAVE_CURRENT:-no output}"
            ;;
    esac
else
    echo "wifi power_save: iw or $WLAN_IFACE not available, left alone"
fi

echo "starting: $PAK_DIR/bin/wiliwili $*"
"$PAK_DIR/bin/wiliwili" "$@"
status=$?

wifi_power_save_restore
if [ -n "$LAUNCH_SIGNAL" ]; then
    echo "interrupted by SIG$LAUNCH_SIGNAL"
    case "$LAUNCH_SIGNAL" in
        HUP) status=129 ;;
        INT) status=130 ;;
        TERM) status=143 ;;
    esac
fi

echo "exit status: $status"
exit $status
