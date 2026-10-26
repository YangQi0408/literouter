#!/usr/bin/env bash
# Wrap the literouter binary in a macOS .app bundle.
#
# Why a bundle at all, when the binary already runs: a bare executable started
# from Finder or LaunchServices gets no bundle identity. That costs three
# things a user notices — no Dock icon of its own, a Terminal window that opens
# and stays, and a Gatekeeper verdict that cannot be granted per-app. A bundle
# is the only form macOS lets you double-click, and the only form `open -a` and
# LaunchServices can address by name.
#
# Signing is optional and never claims notarization. The complete bundle is
# ad-hoc signed without an identity, or Developer-ID signed when --sign is given.
#
# Usage:
#   make_macos_app.sh --binary PATH --version VERSION --output DIR
#                     [--sign "Developer ID Application: ..."]
set -euo pipefail

BINARY=""
VERSION=""
OUTPUT=""
SIGN_IDENTITY=""
NO_SIGN=0

usage() {
    cat <<'EOF'
Build a macOS .app bundle around the literouter binary.

  --binary PATH     the built literouter executable (required)
  --version VERSION version string for CFBundleShortVersionString (required)
  --output DIR      directory to write literouter.app into (required)
  --sign IDENTITY   codesign identity; omit for ad-hoc signing on macOS
  --no-sign         skip signing (packaging fixtures or explicit manual signing)
  -h, --help        show this message
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --binary)  BINARY="${2:?--binary needs a path}"; shift 2 ;;
        --version) VERSION="${2:?--version needs a value}"; shift 2 ;;
        --output)  OUTPUT="${2:?--output needs a directory}"; shift 2 ;;
        --sign)    SIGN_IDENTITY="${2:?--sign needs an identity}"; shift 2 ;;
        --no-sign) NO_SIGN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$BINARY" ] || { echo "error: --binary is required" >&2; exit 2; }
[ -n "$VERSION" ] || { echo "error: --version is required" >&2; exit 2; }
[ -n "$OUTPUT" ] || { echo "error: --output is required" >&2; exit 2; }
[ -f "$BINARY" ] || { echo "error: $BINARY does not exist" >&2; exit 1; }
NUMERIC_VERSION="${VERSION#v}"
[[ "$NUMERIC_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'error: invalid version' >&2; exit 2; }
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

APP="$OUTPUT/literouter.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"

rm -rf "$APP"
mkdir -p "$MACOS" "$RESOURCES"

install -m 0755 "$BINARY" "$MACOS/literouter"
mkdir -p "$MACOS/assets"
install -m 0644 "$REPO_ROOT/tray/assets/literouter-tray.png" "$MACOS/assets/literouter-tray.png"
for doc in LICENSE NOTICE THIRD_PARTY_NOTICES.md; do
    install -m 0644 "$REPO_ROOT/$doc" "$RESOURCES/$doc"
done
python3 "$SCRIPT_DIR/stage_licenses.py" --output "$RESOURCES/licenses"
# Keep the bundle's OS claim no lower than LLVM's actual baseline. A caller can
# raise the deployment target, but never declare 11.0 for a 14.0 Mach-O image.
MINIMUM="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
python3 - "$MINIMUM" <<'PY'
import re, sys
value = sys.argv[1]
if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,2}', value) or tuple(map(int, value.split('.'))) < (14, 0):
    raise SystemExit('macOS deployment target must be 14.0 or newer')
PY

# CFBundleIdentifier is what LaunchServices keys the bundle on, so it has to be
# stable across versions or the system treats every release as a new app and
# forgets the user's Gatekeeper grant. The reverse-DNS form is the project's own
# repository host, which is the convention and costs nothing.
cat > "$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>literouter</string>
    <key>CFBundleDisplayName</key>       <string>literouter</string>
    <key>CFBundleIdentifier</key>        <string>io.github.yangqi0408.literouter</string>
    <key>CFBundleVersion</key>           <string>${NUMERIC_VERSION}</string>
    <key>CFBundleShortVersionString</key><string>${NUMERIC_VERSION}</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleExecutable</key>        <string>literouter</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key>    <string>${MINIMUM}</string>
    <key>LSUIElement</key>              <true/>
    <key>LSApplicationCategoryType</key> <string>public.app-category.developer-tools</string>
    <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
PLIST

# PkgInfo is legacy but some Finder paths still consult it, and it is two
# fields long — cheaper to write than to explain its absence.
printf 'APPL????' > "$CONTENTS/PkgInfo"

if [ "$NO_SIGN" -eq 1 ]; then
    echo "signing explicitly skipped"
elif [ -n "$SIGN_IDENTITY" ]; then
    echo "signing with: $SIGN_IDENTITY"
    # --options runtime is the hardened runtime, required for notarization.
    # --timestamp asks Apple's server for a trusted timestamp, which is what
    # keeps the signature valid after the certificate expires.
    codesign --force --deep --options runtime --timestamp \
        --sign "$SIGN_IDENTITY" "$APP"
    codesign --verify --strict --verbose=2 "$APP"
else
    if [ "$(uname -s)" = "Darwin" ] && command -v codesign >/dev/null 2>&1; then
        codesign --force --deep --sign - "$APP"
    fi
    echo "no Developer ID given; bundle is not notarized"
fi

echo "built $APP"
