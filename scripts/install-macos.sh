#!/usr/bin/env bash
set -euo pipefail

REPO="YangQi0408/literouter"
PREFIX="${HOME}/.local"
VERSION="latest"
FROM=""
INSTALL_SERVICE=0
UNINSTALL=0

usage() {
    cat <<'EOF'
Install literouter on macOS (Apple Silicon) and optionally start it at login.

Options:
  --prefix DIR     Install into DIR (default: ~/.local)
  --version TAG    Install a release tag (default: latest)
  --from PATH      Install this local binary instead of downloading one
  --service        Register and start a macOS LaunchAgent
  --uninstall      Stop the LaunchAgent and remove the binary
  -h, --help       Show this message
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --version) VERSION="${2:?--version needs a tag}"; shift 2 ;;
        --from) FROM="${2:?--from needs a path}"; shift 2 ;;
        --service) INSTALL_SERVICE=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }

case "$(uname -s):$(uname -m)" in
    Darwin:arm64|Darwin:aarch64) ;;
    *) die "this installer requires macOS on Apple Silicon" ;;
esac

TARGET="${PREFIX}/bin/literouter"
LABEL="com.literouter.gateway"
PLIST_DIR="${HOME}/Library/LaunchAgents"
PLIST_PATH="${PLIST_DIR}/${LABEL}.plist"
USER_ID="$(id -u)"

if [ "$UNINSTALL" -eq 1 ]; then
    launchctl bootout "gui/${USER_ID}/${LABEL}" 2>/dev/null || true
    rm -f "$PLIST_PATH" "$TARGET"
    note "LaunchAgent and binary removed; configuration and state were kept"
    exit 0
fi

TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

SOURCE_BINARY="$FROM"
if [ -z "$SOURCE_BINARY" ]; then
    command -v curl >/dev/null 2>&1 || die "curl is required to download a release"
    VERSION_REF="main"
    if [ "$VERSION" != "latest" ]; then VERSION_REF="v${VERSION#v}"; fi
    if [ "$VERSION" = "latest" ]; then
        URL="https://github.com/${REPO}/releases/latest/download/literouter-macos-arm64"
    else
        URL="https://github.com/${REPO}/releases/download/${VERSION_REF}/literouter-${VERSION_REF}-macos-arm64"
    fi
    SOURCE_BINARY="${TMP_DIR}/literouter"
    echo "Downloading $URL"
    curl -fL --retry 3 --connect-timeout 15 -o "$SOURCE_BINARY" "$URL" || die "download failed"
    if curl -fsL --retry 3 -o "$SOURCE_BINARY.sha256" "$URL.sha256" 2>/dev/null; then
        expected="$(awk '{print $1}' "$SOURCE_BINARY.sha256")"
        actual="$(shasum -a 256 "$SOURCE_BINARY" | awk '{print $1}')"
        [ -n "$expected" ] && [ "$expected" = "$actual" ] || die "checksum mismatch"
        note "checksum verified"
    else
        echo "warning: no checksum asset was published" >&2
    fi
fi

[ -f "$SOURCE_BINARY" ] || die "--from $SOURCE_BINARY is not a file"
[ -x "$SOURCE_BINARY" ] || chmod +x "$SOURCE_BINARY"
"$SOURCE_BINARY" --version >/dev/null 2>&1 || die "$SOURCE_BINARY does not run"
mkdir -p "${PREFIX}/bin"
install -m 0755 "$SOURCE_BINARY" "$TARGET"
echo "Installed $TARGET"

if [ "$INSTALL_SERVICE" -eq 0 ]; then
    note "start it with: $TARGET serve"
    exit 0
fi

escape_xml() {
    printf '%s' "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g'
}

mkdir -p "$PLIST_DIR" "${HOME}/Library/Logs"
target_xml="$(escape_xml "$TARGET")"
home_xml="$(escape_xml "$HOME")"
log_dir="$(escape_xml "${HOME}/Library/Logs")"
cat > "$PLIST_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>${LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>${target_xml}</string>
        <string>serve</string>
        <string>--port</string>
        <string>8787</string>
    </array>
    <key>WorkingDirectory</key><string>${home_xml}</string>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>StandardOutPath</key><string>${log_dir}/literouter.stdout.log</string>
    <key>StandardErrorPath</key><string>${log_dir}/literouter.stderr.log</string>
</dict>
</plist>
EOF

plutil -lint "$PLIST_PATH" >/dev/null || die "generated LaunchAgent is not valid plist"
launchctl bootout "gui/${USER_ID}/${LABEL}" 2>/dev/null || true
launchctl bootstrap "gui/${USER_ID}" "$PLIST_PATH"
launchctl enable "gui/${USER_ID}/${LABEL}"
launchctl kickstart -k "gui/${USER_ID}/${LABEL}"
note "LaunchAgent installed and started; it will run at login"
note "check it with: launchctl print gui/${USER_ID}/${LABEL}"
