#!/usr/bin/env bash
#
# Install a literouter release binary on Linux.
#
#   scripts/install.sh                       # latest release, system-wide
#   scripts/install.sh --prefix ~/.local     # into a prefix, no sudo
#   scripts/install.sh --version v0.1.0
#   scripts/install.sh --from ./literouter   # an already-downloaded binary
#   scripts/install.sh --service             # also install the systemd unit
#
# Deliberately a shell script rather than a package: the release is one static-
# ish binary and a service file, and a tarball plus this script is smaller than
# the packaging machinery for three distributions.
set -euo pipefail

REPO="mcpp-community/literouter"
PREFIX="/usr/local"
VERSION="latest"
FROM=""
INSTALL_SERVICE=0
UNINSTALL=0
ASSUME_YES=0

usage() {
    # The leading comment block is the help text, so it cannot drift from the
    # script: printed by the same file that implements the options.
    awk 'NR > 2 && /^#/ { sub(/^# ?/, ""); print; next } NR > 2 { exit }' "$0"
    cat <<'EOF'

Options:
  --prefix DIR     Install into DIR (default /usr/local)
  --version TAG    Release tag to install (default: the latest release)
  --from PATH      Install this local binary instead of downloading one
  --service        Install and enable the systemd unit (Linux, needs root)
  --uninstall      Remove the binary, the unit and the service account
  -y, --yes        Do not ask before installing the systemd unit
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

# A prefix outside the user's home needs root for the copy; saying so up front is
# kinder than a permission error three steps in.
need_root() {
    [ "$(id -u)" -eq 0 ] || die "$1 needs root; re-run with sudo, or use --prefix ~/.local"
}

# Whether this user could create and write the prefix. Checking only the prefix
# itself is wrong for the common case of a path that does not exist yet —
# `--prefix /tmp/lr` is perfectly writable and is exactly how the install is
# tested — so the nearest existing ancestor decides.
prefix_is_writable() {
    local probe="${PREFIX}"
    if [ -d "${probe}" ]; then
        [ -w "${probe}" ]
        return
    fi
    while [ ! -d "${probe}" ] && [ "${probe}" != "/" ]; do
        probe="$(dirname "${probe}")"
    done
    [ -w "${probe}" ]
}

BIN_DIR="${PREFIX}/bin"
TARGET="${BIN_DIR}/literouter"
UNIT_PATH="/etc/systemd/system/literouter.service"
STATE_DIR="/var/lib/literouter"
CONFIG_DIR="/etc/literouter"

if [ "${UNINSTALL}" -eq 1 ]; then
    need_root "--uninstall"
    echo "Removing literouter"
    systemctl disable --now literouter 2>/dev/null || true
    rm -f "${UNIT_PATH}" "${TARGET}"
    systemctl daemon-reload 2>/dev/null || true
    note "binary and unit removed"
    note "kept ${CONFIG_DIR} (your keys) and ${STATE_DIR} (telemetry)"
    note "remove them by hand if you are sure: rm -rf ${CONFIG_DIR} ${STATE_DIR}"
    exit 0
fi

# ── fetch the binary ─────────────────────────────────────────────────────────
TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "${TMP_DIR}"; }
trap cleanup EXIT

SOURCE_BINARY=""
if [ -n "${FROM}" ]; then
    [ -f "${FROM}" ] || die "--from ${FROM} is not a file"
    SOURCE_BINARY="${FROM}"
else
    command -v curl >/dev/null 2>&1 || die "curl is required to download a release"
    case "$(uname -m)" in
        x86_64|amd64) PLATFORM="linux-x86_64" ;;
        aarch64|arm64)
            # Named explicitly rather than silently falling through to x86_64,
            # which would install a binary that cannot exec.
            die "no linux-arm64 release is published; build from source with mcpp build -p cli --release" ;;
        *) die "unsupported architecture: $(uname -m)" ;;
    esac
    if [ "${VERSION}" = "latest" ]; then
        URL="https://github.com/${REPO}/releases/latest/download/literouter-${PLATFORM}"
    else
        URL="https://github.com/${REPO}/releases/download/${VERSION}/literouter-${VERSION}-${PLATFORM}"
    fi
    echo "Downloading ${URL}"
    curl -fL --retry 3 --connect-timeout 15 -o "${TMP_DIR}/literouter" "${URL}" \
        || die "download failed; check the version tag and your network"
    # The release publishes a .sha256 next to each asset. Verifying it is what
    # turns "the download finished" into "the bytes are the ones that were
    # published", which matters for something that will hold API keys.
    if curl -fsL --retry 3 -o "${TMP_DIR}/literouter.sha256" "${URL}.sha256" 2>/dev/null; then
        EXPECTED="$(awk '{print $1}' "${TMP_DIR}/literouter.sha256")"
        ACTUAL="$(sha256sum "${TMP_DIR}/literouter" | awk '{print $1}')"
        if [ -n "${EXPECTED}" ] && [ "${EXPECTED}" != "${ACTUAL}" ]; then
            die "checksum mismatch: expected ${EXPECTED}, got ${ACTUAL}"
        fi
        note "checksum verified"
    else
        echo "warning: no .sha256 published for this asset; skipping verification" >&2
    fi
    SOURCE_BINARY="${TMP_DIR}/literouter"
fi

# ── install ──────────────────────────────────────────────────────────────────
if ! prefix_is_writable; then
    need_root "installing into ${PREFIX}"
fi
# The binary's interpreter is patched at release time, but a binary built from
# source on this machine still points at the build toolchain's loader. Checked on
# the SOURCE before installing, so a binary that cannot exec never lands in
# ${BIN_DIR}, and checked with an explanation rather than letting the loader's
# own message — or a bare `set -e` abort — be the only thing the operator sees.
if ! "${SOURCE_BINARY}" --version >/dev/null 2>&1; then
    die "${SOURCE_BINARY} does not run; on a from-source build run scripts/patch_elf_interp.py first"
fi
mkdir -p "${BIN_DIR}"
install -m 0755 "${SOURCE_BINARY}" "${TARGET}"
echo "Installed ${TARGET}"
"${TARGET}" --version

if [ "${INSTALL_SERVICE}" -eq 0 ]; then
    echo
    note "start it with: LITEROUTER_CONFIG=${CONFIG_DIR}/config.json ${TARGET} serve --port 8787"
    note "or install the service: sudo $0 --prefix ${PREFIX} --service -y"
    exit 0
fi

# ── systemd ──────────────────────────────────────────────────────────────────
need_root "--service"
command -v systemctl >/dev/null 2>&1 || die "systemctl is not available; run the binary directly instead"

if [ "${ASSUME_YES}" -eq 0 ] && [ -t 0 ]; then
    printf 'Install the systemd unit, create the literouter account and enable the service? [y/N] '
    read -r answer
    case "${answer}" in
        y|Y|yes|YES) ;;
        *) echo "Skipped the service; the binary is installed."; exit 0 ;;
    esac
fi

UNIT_SOURCE="$(dirname "$(readlink -f "$0")")/../deploy/literouter.service"
[ -f "${UNIT_SOURCE}" ] || die "cannot find deploy/literouter.service next to this script"

if ! id literouter >/dev/null 2>&1; then
    useradd --system --no-create-home --home-dir "${STATE_DIR}" --shell /usr/sbin/nologin literouter
    note "created the literouter service account"
fi
install -d -o literouter -g literouter -m 0750 "${STATE_DIR}"
install -d -o root -g literouter -m 0750 "${CONFIG_DIR}"

if [ ! -f "${CONFIG_DIR}/config.json" ]; then
    # Seeded as the service account so the file it will read is one it owns;
    # the directory itself stays root-owned so a compromised service cannot
    # rewrite its own routing table.
    LITEROUTER_CONFIG="${CONFIG_DIR}/config.json" "${TARGET}" config init --force >/dev/null
    chown literouter:literouter "${CONFIG_DIR}/config.json"
    chmod 0640 "${CONFIG_DIR}/config.json"
    note "wrote a seed config to ${CONFIG_DIR}/config.json — edit it before relying on it"
fi

install -m 0644 "${UNIT_SOURCE}" "${UNIT_PATH}"
systemctl daemon-reload
systemctl enable --now literouter
sleep 1
systemctl --no-pager --lines=0 status literouter || true
echo
note "logs: journalctl -u literouter -f"
note "config: ${CONFIG_DIR}/config.json"
