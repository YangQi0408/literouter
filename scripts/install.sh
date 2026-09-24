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
Install a literouter release binary on Linux.

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

if [ "$UNINSTALL" -eq 1 ]; then
    need_root "--uninstall"
    systemctl disable --now literouter 2>/dev/null || true
    rm -f "$UNIT_PATH" "$TARGET"
    systemctl daemon-reload 2>/dev/null || true
    note "binary and unit removed; configuration and state were kept"
    exit 0
fi

TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

SOURCE_BINARY="$FROM"
if [ -z "$SOURCE_BINARY" ]; then
    command -v curl >/dev/null 2>&1 || die "curl is required to download a release"
    case "$(uname -m)" in
        x86_64|amd64) PLATFORM="linux-x86_64" ;;
        aarch64|arm64) die "no linux-arm64 release is published; build from source with mcpp" ;;
        *) die "unsupported architecture: $(uname -m)" ;;
    esac
    if [ "$VERSION" = "latest" ]; then
        URL="https://github.com/${REPO}/releases/latest/download/literouter-${PLATFORM}"
    else
        VERSION="${VERSION#v}"
        URL="https://github.com/${REPO}/releases/download/v${VERSION}/literouter-v${VERSION}-${PLATFORM}"
    fi
    SOURCE_BINARY="$TMP_DIR/literouter"
    echo "Downloading $URL"
    curl -fL --retry 3 --connect-timeout 15 -o "$SOURCE_BINARY" "$URL" || die "download failed"
    if curl -fsL --retry 3 -o "$SOURCE_BINARY.sha256" "$URL.sha256" 2>/dev/null; then
        expected="$(awk '{print $1}' "$SOURCE_BINARY.sha256")"
        actual="$(sha256sum "$SOURCE_BINARY" | awk '{print $1}')"
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
command -v systemctl >/dev/null 2>&1 || die "systemctl is not available"
if [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ]; then
    printf 'Install and enable the literouter systemd service? [y/N] '
    read -r answer
    case "$answer" in
        y|Y|yes|YES) ;;
        *) exit 0 ;;
    esac
fi

UNIT_SOURCE="$(cd "$(dirname "$0")/.." && pwd)/deploy/literouter.service"
[ -f "$UNIT_SOURCE" ] || die "cannot find deploy/literouter.service"
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
