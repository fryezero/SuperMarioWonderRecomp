#!/bin/bash
# Copy Wonder's data from private/wonder to the iOS app's Documents, or pull the app's log back.
# usage: tools/wonder_push_ios.sh [--no-data] [--config <file>]
#        tools/wonder_push_ios.sh --pull-log
#   (default)        analysis/{main,sdk}.bin, fonts/, pipeline_capture.bin (if there), spirv/ and extract/romfs/
#   --no-data        leave out spirv/ and extract/romfs/ (4 GB)
#   --config <file>  also copy <file> as Documents/config.txt (NX_* lines, read at start)
#   --pull-log       only copy Documents/nx.log to private/wonder/ios_nx.log
# devicectl skips files that have not changed since they were copied, so pushing again only sends what changed (the
# first push of the romfs takes a while). The app must be installed first (tools/build_ios.sh --install): the data
# goes to its container, and deleting the app deletes the data.
# Settings (environment or private/ios.env, see tools/ios_env.sh): NX_IOS_DEVICE (UDID), and NX_IOS_BUNDLE_ID or the
# NX_IOS_TEAM it defaults from.
set -euo pipefail

DATA=private/wonder

usage() {
    echo "usage: $0 [--no-data] [--config <file>] | --pull-log" >&2
    exit 2
}

# Parsed before changing to the repository root, so a relative --config path means what the caller meant.
data=1 config="" pull_log=0
while [ $# -gt 0 ]; do
    case "$1" in
        --no-data) data=0 ;;
        --config) [ $# -ge 2 ] || usage; config=$(realpath "$2"); shift ;;
        --pull-log) pull_log=1 ;;
        *) usage ;;
    esac
    shift
done
cd "$(dirname "$0")/.."
. tools/ios_env.sh
ios_need NX_IOS_DEVICE "the phone's UDID (xcrun devicectl list devices)"
ios_need NX_IOS_BUNDLE_ID "the app's bundle id (or NX_IOS_TEAM, which it defaults from)"
DEVICE=$NX_IOS_DEVICE
BUNDLE_ID=$NX_IOS_BUNDLE_ID

# copy to|from <more devicectl options>: the app's data container on the device.
copy() {
    local direction=$1
    shift
    xcrun devicectl device copy "$direction" --device "$DEVICE" --domain-type appDataContainer \
        --domain-identifier "$BUNDLE_ID" --quiet "$@"
}

# push <file or directory> <path under Documents>. A directory's contents are merged into the destination.
push() {
    if [ ! -e "$1" ]; then
        echo "missing $1" >&2
        exit 1
    fi
    # devicectl copies a symlink as a link and fails on one given as the source, or on one inside the tree whose
    # target has '..' in it. extract/romfs is a link to ../romfs, so copy from the real path, and refuse a tree that
    # still holds links rather than send it half.
    local src
    src=$(realpath "$1")
    if [ -d "$src" ] && [ -n "$(find "$src" -type l -print -quit)" ]; then
        echo "$1 holds symlinks, which devicectl cannot copy" >&2
        exit 1
    fi
    echo "Documents/$2 <- $1 ($(du -sh "$src" | cut -f1))"
    copy to --source "$src" --destination "Documents/$2"
}

if [ "$pull_log" = 1 ]; then
    copy from --source Documents/nx.log --destination "$PWD/$DATA/ios_nx.log"
    echo "$DATA/ios_nx.log"
    exit 0
fi

push "$DATA/analysis/main.bin" analysis/main.bin
push "$DATA/analysis/sdk.bin" analysis/sdk.bin
push "$DATA/fonts" fonts
# The pipelines captured on the Mac (runtime/src/nvn/nvn_pipeline_warm.cpp): the app rebuilds them in the background
# at start, instead of compiling each one mid-game the first time it is drawn. This replaces the phone's own capture.
if [ -f "$DATA/pipeline_capture.bin" ]; then
    push "$DATA/pipeline_capture.bin" pipeline_capture.bin
fi
if [ -n "$config" ]; then
    push "$config" config.txt
fi
if [ "$data" = 1 ]; then
    push "$DATA/spirv" spirv
    push "$DATA/extract/romfs" extract/romfs
fi
echo done
