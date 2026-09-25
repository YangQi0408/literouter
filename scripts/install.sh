#!/usr/bin/env bash
set -euo pipefail

REPO="YangQi0408/literouter"
PREFIX="/usr/local"
VERSION="latest"
FROM=""
INSTALL_SERVICE=0
UNINSTALL=0
ASSUME_YES=0

usage() {
    cat <<'EOF'
Install a literouter release binary on Linux x86_64 or macOS arm64.

Options:
  --prefix DIR     Install into DIR (default /usr/local)
  --version TAG    Install a release tag (default: latest)
  --from PATH      Install this local binary instead of downloading one
  --service        Install and enable the systemd unit
  --uninstall      Remove the binary and systemd unit
  -y, --yes        Do not ask before installing the service
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
        -y|--yes) ASSUME_YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
need_root() {
    [ "$(id -u)" -eq 0 ] || die "$1 needs root; use sudo or choose --prefix ~/.local"
}

BIN_DIR="${PREFIX}/bin"
TARGET="${BIN_DIR}/literouter"
UNIT_PATH="/etc/systemd/system/literouter.service"
STATE_DIR="/var/lib/literouter"
CONFIG_DIR="/etc/literouter"
OS="$(uname -s)"
VERSION_REF="main"
if [ "$VERSION" != "latest" ]; then
    VERSION_REF="v${VERSION#v}"
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

if [ "$UNINSTALL" -eq 1 ]; then
    need_root "--uninstall"
    if command -v systemctl >/dev/null 2>&1; then
        systemctl disable --now literouter 2>/dev/null || true
        systemctl daemon-reload 2>/dev/null || true
    fi
    rm -f "$UNIT_PATH" "$TARGET"
    note "binary and unit removed; configuration and state were kept"
    exit 0
fi

TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

SOURCE_BINARY="$FROM"
if [ -z "$SOURCE_BINARY" ]; then
    command -v curl >/dev/null 2>&1 || die "curl is required to download a release"
    case "${OS}:$(uname -m)" in
        Linux:x86_64|Linux:amd64) PLATFORM="linux-x86_64" ;;
        Linux:aarch64|Linux:arm64) die "no linux-arm64 release is published; build from source with mcpp" ;;
        Darwin:arm64|Darwin:aarch64) PLATFORM="macos-arm64" ;;
        Darwin:x86_64|Darwin:amd64) die "no macOS x86_64 release is published; use Rosetta on Apple Silicon or build from source" ;;
        *) die "unsupported platform: ${OS} $(uname -m)" ;;
    esac
    if [ "$VERSION" = "latest" ]; then
        URL="https://github.com/${REPO}/releases/latest/download/literouter-${PLATFORM}"
    else
        VERSION="${VERSION#v}"
        URL="https://github.com/${REPO}/releases/download/${VERSION_REF}/literouter-${VERSION_REF}-${PLATFORM}"
    fi
    SOURCE_BINARY="$TMP_DIR/literouter"
    echo "Downloading $URL"
    curl -fL --retry 3 --connect-timeout 15 -o "$SOURCE_BINARY" "$URL" || die "download failed"
    if curl -fsL --retry 3 -o "$SOURCE_BINARY.sha256" "$URL.sha256" 2>/dev/null; then
        expected="$(awk '{print $1}' "$SOURCE_BINARY.sha256")"
        actual="$(sha256_of "$SOURCE_BINARY")"
        [ -n "$expected" ] && [ "$expected" = "$actual" ] || die "checksum mismatch"
        note "checksum verified"
    else
        echo "warning: no checksum asset was published" >&2
    fi
else
    [ -f "$SOURCE_BINARY" ] || die "--from $SOURCE_BINARY is not a file"
fi

[ -x "$SOURCE_BINARY" ] || chmod +x "$SOURCE_BINARY"
"$SOURCE_BINARY" --version >/dev/null 2>&1 || die "$SOURCE_BINARY does not run"

parent="$PREFIX"
while [ ! -d "$parent" ] && [ "$parent" != "/" ]; do
    parent="$(dirname "$parent")"
done
if [ ! -w "$PREFIX" ] 2>/dev/null && [ ! -w "$parent" ] 2>/dev/null; then
    need_root "installing into $PREFIX"
fi
mkdir -p "$BIN_DIR"
install -m 0755 "$SOURCE_BINARY" "$TARGET"
echo "Installed $TARGET"

if [ "$INSTALL_SERVICE" -eq 0 ]; then
    note "start it with: $TARGET serve"
    exit 0
fi

need_root "--service"
[ "$OS" = "Linux" ] || die "--service is only supported on Linux with systemd"
command -v systemctl >/dev/null 2>&1 || die "systemctl is not available"
if [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ]; then
    printf 'Install and enable the literouter systemd service? [y/N] '
    read -r answer
    case "$answer" in
        y|Y|yes|YES) ;;
        *) exit 0 ;;
    esac
fi

UNIT_SOURCE=""
SCRIPT_SOURCE="${BASH_SOURCE[0]:-}"
if [ -n "$SCRIPT_SOURCE" ] && [ -f "$SCRIPT_SOURCE" ]; then
    UNIT_SOURCE="$(cd "$(dirname "$SCRIPT_SOURCE")/.." && pwd)/deploy/literouter.service"
fi
if [ ! -f "$UNIT_SOURCE" ]; then
    UNIT_SOURCE="$TMP_DIR/literouter.service"
    UNIT_URL="https://raw.githubusercontent.com/${REPO}/${VERSION_REF}/deploy/literouter.service"
    curl -fsSL --retry 3 --connect-timeout 15 -o "$UNIT_SOURCE" "$UNIT_URL" || \
        die "cannot download deploy/literouter.service from $UNIT_URL"
fi
if ! id literouter >/dev/null 2>&1; then
    useradd --system --no-create-home --home-dir "$STATE_DIR" --shell /usr/sbin/nologin literouter
fi
install -d -o literouter -g literouter -m 0750 "$STATE_DIR"
install -d -o root -g literouter -m 0750 "$CONFIG_DIR"
if [ ! -f "$CONFIG_DIR/config.json" ]; then
    LITEROUTER_CONFIG="$CONFIG_DIR/config.json" "$TARGET" config init --force >/dev/null
    chown literouter:literouter "$CONFIG_DIR/config.json"
    chmod 0640 "$CONFIG_DIR/config.json"
fi
install -m 0644 "$UNIT_SOURCE" "$UNIT_PATH"
systemctl daemon-reload
systemctl enable --now literouter
