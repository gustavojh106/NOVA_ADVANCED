#!/usr/bin/env bash
# macOS counterpart of build-nova.ps1 — builds NOVA with xcodebuild.
#
#   ./scripts/build-nova.sh                                  # Debug Standalone (dev/test default)
#   ./scripts/build-nova.sh -c Release -t NOVA_VST3          # Release VST3
#   ./scripts/build-nova.sh -t All                           # every plugin format
#   ./scripts/build-nova.sh -a universal                     # universal binary (arm64 + x86_64)
#
# Debug builds default to the host architecture so iteration stays fast;
# Release builds default to a universal binary for distribution.

set -euo pipefail

CONFIGURATION="Debug"
TARGET="NOVA_StandalonePlugin"
ARCH=""

usage() {
    cat <<'USAGE'
Usage: build-nova.sh [-c Debug|Release] [-t TARGET] [-a native|universal]

Targets: NOVA_StandalonePlugin (default), NOVA_VST3, NOVA_AU, NOVA_SharedCode, All
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--configuration) CONFIGURATION="$2"; shift 2 ;;
        -t|--target)        TARGET="$2";        shift 2 ;;
        -a|--arch)          ARCH="$2";          shift 2 ;;
        -h|--help)          usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

case "$CONFIGURATION" in
    Debug|Release) ;;
    *) echo "Configuration must be Debug or Release (got '$CONFIGURATION')" >&2; exit 1 ;;
esac

case "$TARGET" in
    NOVA_StandalonePlugin) XCODE_TARGET="NOVA - Standalone Plugin" ;;
    NOVA_VST3)             XCODE_TARGET="NOVA - VST3" ;;
    NOVA_AU)               XCODE_TARGET="NOVA - AU" ;;
    NOVA_SharedCode)       XCODE_TARGET="NOVA - Shared Code" ;;
    All)                   XCODE_TARGET="NOVA - All" ;;
    *) echo "Unknown target: $TARGET" >&2; usage; exit 1 ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$REPO_ROOT/Builds/MacOSX/NOVA.xcodeproj"

if [[ ! -d "$PROJECT" ]]; then
    echo "Xcode project not found at $PROJECT" >&2
    echo "Generate it first with: ./scripts/resave-nova.sh" >&2
    exit 1
fi

if [[ -z "$ARCH" ]]; then
    [[ "$CONFIGURATION" == "Release" ]] && ARCH="universal" || ARCH="native"
fi

case "$ARCH" in
    native)    ARCH_FLAGS=(ARCHS="$(uname -m)" ONLY_ACTIVE_ARCH=YES) ;;
    universal) ARCH_FLAGS=(ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO) ;;
    *) echo "Arch must be native or universal (got '$ARCH')" >&2; exit 1 ;;
esac

echo "Project:       $PROJECT"
echo "Target:        $XCODE_TARGET | Configuration: $CONFIGURATION | Arch: $ARCH"

# Ad-hoc signing keeps local builds runnable without a developer certificate.
xcodebuild \
    -project "$PROJECT" \
    -target "$XCODE_TARGET" \
    -configuration "$CONFIGURATION" \
    "${ARCH_FLAGS[@]}" \
    CODE_SIGN_IDENTITY="-" \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGNING_ALLOWED=NO \
    build

echo
echo "Build products in Builds/MacOSX/build/$CONFIGURATION:"
ls -1 "$REPO_ROOT/Builds/MacOSX/build/$CONFIGURATION" 2>/dev/null || true
