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
# sees it on the console.
LOG_FILE="$LOGS_PATH/$PAK_NAME.txt"
rm -f "$LOG_FILE"
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

echo "starting: $PAK_DIR/bin/wiliwili $*"
"$PAK_DIR/bin/wiliwili" "$@"
status=$?

echo "exit status: $status"
exit $status
