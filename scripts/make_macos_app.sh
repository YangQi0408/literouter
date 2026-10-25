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
# This script does not sign anything. Signing needs a Developer ID certificate
# and `codesign --deep` on a machine that has it; doing that here would produce
# a bundle that claims to be signed when the signature means nothing. The
# workflow passes --sign when it has the identity, and the bundle is otherwise
# left ad-hoc signed by the linker, which is what an unsigned binary already is.
#
# Usage:
#   make_macos_app.sh --binary PATH --version VERSION --output DIR
#                     [--sign "Developer ID Application: ..."]
set -euo pipefail

BINARY=""
VERSION=""
OUTPUT=""
SIGN_IDENTITY=""

usage() {
    cat <<'EOF'
Build a macOS .app bundle around the literouter binary.

  --binary PATH     the built literouter executable (required)
  --version VERSION version string for CFBundleShortVersionString (required)
  --output DIR      directory to write literouter.app into (required)
  --sign IDENTITY   codesign identity; omit for an unsigned bundle
  -h, --help        show this message
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --binary)  BINARY="${2:?--binary needs a path}"; shift 2 ;;
        --version) VERSION="${2:?--version needs a value}"; shift 2 ;;
        --output)  OUTPUT="${2:?--output needs a directory}"; shift 2 ;;
        --sign)    SIGN_IDENTITY="${2:?--sign needs an identity}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$BINARY" ] || { echo "error: --binary is required" >&2; exit 2; }
[ -n "$VERSION" ] || { echo "error: --version is required" >&2; exit 2; }
[ -n "$OUTPUT" ] || { echo "error: --output is required" >&2; exit 2; }
[ -f "$BINARY" ] || { echo "error: $BINARY does not exist" >&2; exit 1; }

APP="$OUTPUT/literouter.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"

rm -rf "$APP"
mkdir -p "$MACOS" "$RESOURCES"

install -m 0755 "$BINARY" "$MACOS/literouter"

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
    <key>CFBundleVersion</key>           <string>${VERSION}</string>
    <key>CFBundleShortVersionString</key><string>${VERSION}</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleExecutable</key>        <string>literouter</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <!-- The program is a foreground gateway that prints a startup block and
         then serves until interrupted, so it is NOT an LSUIElement agent: it
         owns a Terminal window when started that way, which is what the CLI
         already documents. Naming the category is what gives the Dock icon a
         sane grouping. -->
    <key>LSApplicationCategoryType</key> <string>public.app-category.developer-tools</string>
    <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
PLIST

# PkgInfo is legacy but some Finder paths still consult it, and it is two
# fields long — cheaper to write than to explain its absence.
printf 'APPL????' > "$CONTENTS/PkgInfo"

if [ -n "$SIGN_IDENTITY" ]; then
    echo "signing with: $SIGN_IDENTITY"
    # --options runtime is the hardened runtime, required for notarization.
    # --timestamp asks Apple's server for a trusted timestamp, which is what
    # keeps the signature valid after the certificate expires.
    codesign --force --deep --options runtime --timestamp \
        --sign "$SIGN_IDENTITY" "$APP"
    codesign --verify --strict --verbose=2 "$APP"
else
    echo "no signing identity given; leaving the bundle ad-hoc signed"
fi

echo "built $APP"
