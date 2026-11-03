#!/bin/bash
# Build the iOS app (an Xcode project in private/wonder/build-ios), then optionally install and start it on the phone.
# usage: tools/build_ios.sh [--install] [--launch [--console]]
#   --install   install the app on the device
#   --launch    start it, ending a running instance first; --console stays attached to its output until it exits
# Settings (environment or private/ios.env, see tools/ios_env.sh): NX_IOS_TEAM (signing team), NX_IOS_DEVICE (UDID,
# for --install and --launch), NX_IOS_BUNDLE_ID, NX_IOS_JOBS (parallel compiles, default 6: each compiler takes GBs on
# the generated code at -O2, so more than that swaps a 16 GB Mac).
# The game data goes to the app separately: tools/wonder_push_ios.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

. tools/ios_env.sh
BUILD=private/wonder/build-ios

install=0 launch=0 console=0
for arg in "$@"; do
    case "$arg" in
        --install) install=1 ;;
        --launch) launch=1 ;;
        --console) console=1 ;;
        *) echo "usage: $0 [--install] [--launch [--console]]" >&2; exit 2 ;;
    esac
done
if [ "$console" = 1 ] && [ "$launch" = 0 ]; then
    echo "--console needs --launch" >&2
    exit 2
fi
ios_need NX_IOS_TEAM "the Apple development team that signs the app"
if [ "$install" = 1 ] || [ "$launch" = 1 ]; then
    ios_need NX_IOS_DEVICE "the phone's UDID (xcrun devicectl list devices)"
fi
TEAM=$NX_IOS_TEAM
BUNDLE_ID=$NX_IOS_BUNDLE_ID
DEVICE=${NX_IOS_DEVICE:-}
JOBS=${NX_IOS_JOBS:-6}

# Configuring every time is cheap (the project file is only rewritten when it changed) and picks up new sources.
cmake -S . -B "$BUILD" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DNX_GAME=wonder -DNX_IOS_TEAM="$TEAM" -DNX_IOS_BUNDLE_ID="$BUNDLE_ID"
# -quiet: only warnings and errors, not a command line per file of the generated code.
xcodebuild -project "$BUILD/SwitchGamesRecomp.xcodeproj" -target wonder -configuration Release -sdk iphoneos \
    -allowProvisioningUpdates -jobs "$JOBS" -quiet build

APP="$PWD/$BUILD/Release-iphoneos/wonder.app"
if [ ! -d "$APP" ]; then
    echo "the build reported success but there is no $APP" >&2
    exit 1
fi
echo "app: $APP"

if [ "$install" = 1 ]; then
    xcrun devicectl device install app --device "$DEVICE" "$APP"
fi
if [ "$launch" = 1 ]; then
    if [ "$console" = 1 ]; then
        xcrun devicectl device process launch --device "$DEVICE" --terminate-existing --console "$BUNDLE_ID"
    else
        xcrun devicectl device process launch --device "$DEVICE" --terminate-existing "$BUNDLE_ID"
    fi
fi
