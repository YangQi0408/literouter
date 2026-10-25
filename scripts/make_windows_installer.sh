#!/usr/bin/env bash
# Build the Windows installer from an already-built literouter.exe.
#
# This runs on the Windows runner under bash (the release workflow's default
# shell), so it is a shell script rather than PowerShell: Inno Setup's compiler
# is a plain .exe and the quoting is simpler here than through PowerShell's
# argument marshalling.
#
# Inno Setup is not installed on GitHub's windows runners, so this fetches it
# with Chocolatey. That is the same trust boundary the workflow already extends
# to mcpp and Node, and the alternative — committing a 5 MB installer compiler
# to the repository — is worse.
#
# Usage:
#   make_windows_installer.sh --binary PATH --version VERSION
set -euo pipefail

BINARY=""
APP_BINARY=""
VERSION=""

usage() {
    cat <<'EOF'
Build literouter-<version>-windows-x86_64-setup.exe.

  --binary PATH      the built literouter.exe (required)
  --app PATH         the built literouter-tray.exe (optional; the desktop app)
  --version VERSION  version string for the installer (required)
  -h, --help         show this message
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --binary)  BINARY="${2:?--binary needs a path}"; shift 2 ;;
        --app)     APP_BINARY="${2:?--app needs a path}"; shift 2 ;;
        --version) VERSION="${2:?--version needs a value}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$BINARY" ] || { echo "error: --binary is required" >&2; exit 2; }
[ -n "$VERSION" ] || { echo "error: --version is required" >&2; exit 2; }
[ -f "$BINARY" ] || { echo "error: $BINARY does not exist" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Locate ISCC. Chocolatey puts it under Program Files (x86) by default; the
# explicit fallback covers a per-user install.
ISCC=""
for candidate in \
    "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
    "/c/Program Files/Inno Setup 6/ISCC.exe" \
    "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe"
do
    if [ -f "$candidate" ]; then ISCC="$candidate"; break; fi
done

if [ -z "$ISCC" ]; then
    echo "Inno Setup not found; installing with Chocolatey"
    choco install --no-progress -y innosetup
    for candidate in \
        "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
        "/c/Program Files/Inno Setup 6/ISCC.exe"
    do
        if [ -f "$candidate" ]; then ISCC="$candidate"; break; fi
    done
fi

[ -n "$ISCC" ] || { echo "error: ISCC.exe still not found after install" >&2; exit 1; }
echo "using $ISCC"

# The .iss resolves its [Files] sources relative to itself, so the binary and
# the documentation have to be reachable from there. A staging copy keeps the
# repository root free of build outputs and makes the relative paths obvious.
STAGE="$REPO_ROOT/scripts/.installer-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BINARY" "$STAGE/literouter.exe"
if [ -n "$APP_BINARY" ]; then
    [ -f "$APP_BINARY" ] || { echo "error: $APP_BINARY does not exist" >&2; exit 1; }
    cp "$APP_BINARY" "$STAGE/literouter-tray.exe"
fi
for doc in README.md README_en.md LICENSE NOTICE THIRD_PARTY_NOTICES.md; do
    [ -f "$REPO_ROOT/$doc" ] && cp "$REPO_ROOT/$doc" "$STAGE/"
done

# A copy of the .iss next to the staged files, so both its `Source:` paths and
# its `OutputDir` resolve without the caller having to reason about where ISCC
# was invoked from.
sed -e "s|^  #define SourceBinary .*|  #define SourceBinary \"literouter.exe\"|" \
    "$SCRIPT_DIR/make_windows_installer.iss" > "$STAGE/installer.iss"
# The [Files] entries point at "..\README.md" because the .iss normally lives in
# scripts/. Staged, the documents are siblings, so drop the parent hop.
sed -i 's|"\.\.\\|"|g' "$STAGE/installer.iss"
# OutputDir is relative to the .iss, which is now in the stage directory.
sed -i 's|^OutputDir=\.\.\\dist-release|OutputDir=dist-release|' "$STAGE/installer.iss"

mkdir -p "$STAGE/dist-release"
iscc_args=(//Q //DAppVersion="$VERSION")
if [ -n "$APP_BINARY" ]; then
    iscc_args+=(//DSourceApp="literouter-tray.exe")
fi
"$ISCC" "${iscc_args[@]}" "$STAGE/installer.iss"

OUTPUT="$STAGE/dist-release/literouter-$VERSION-windows-x86_64-setup.exe"
[ -f "$OUTPUT" ] || { echo "error: ISCC produced no installer at $OUTPUT" >&2; exit 1; }

mkdir -p "$REPO_ROOT/dist-release"
cp "$OUTPUT" "$REPO_ROOT/dist-release/"
echo "built $REPO_ROOT/dist-release/$(basename "$OUTPUT")"
rm -rf "$STAGE"
