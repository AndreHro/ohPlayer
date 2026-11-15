#!/bin/bash
set -e

PLATFORM="aarch64-scarthgap-linux"
DEBUG_MODE="Release"
DEPS_DIR="./dependencies/${PLATFORM}"

HOST="https://linn-artifacts-public.s3-eu-west-1.amazonaws.com"
DROP="artifacts"

mkdir -p "$DEPS_DIR"

declare -A DEPS
DEPS[ohNet]="1.42.6276"
DEPS[ohWafHelpers]="0.2.176"
DEPS[ohMediaPlayer]="1.172.1253"
DEPS[ohNetGenerated]="1.1.260"

declare -A PLATFORM_MAP
PLATFORM_MAP[ohNet]="aarch64-scarthgap-linux-${DEBUG_MODE}"
PLATFORM_MAP[ohWafHelpers]=""
PLATFORM_MAP[ohMediaPlayer]="aarch64-scarthgap-linux-${DEBUG_MODE}"
PLATFORM_MAP[ohNetGenerated]="AnyPlatform"

for NAME in "${!DEPS[@]}"; do
    VERSION=${DEPS[$NAME]}
    echo "Downloading ${NAME} v${VERSION}..."
    MAP_VAL=${PLATFORM_MAP[$NAME]}
    
    if [ -z "$MAP_VAL" ]; then
        URL="${HOST}/${DROP}/${NAME}/${NAME}-${VERSION}.tar.gz"
    else
        URL="${HOST}/${DROP}/${NAME}/${NAME}-${VERSION}-${MAP_VAL}.tar.gz"
    fi
    
    echo "Target URL: $URL"
    TARGET_PATH="${DEPS_DIR}/${NAME}"
    mkdir -p "$TARGET_PATH"
    
    curl -fsSL "$URL" | tar -xz -C "$TARGET_PATH" --strip-components=1
    echo "Successfully downloaded and unpacked ${NAME} into position."
    echo "--------------------------------------------------------"
done

KFR_NAME="kfr"
KFR_VERSION="7.2.0"
ARCH=$(uname -m)
TARGET_PATH="${DEPS_DIR}/${KFR_NAME}"

if [ "$ARCH" = "aarch64" ]; then
    KFR_ZIP="KFR-${KFR_VERSION}-Linux-ARM64.zip"
else
    KFR_ZIP="KFR-${KFR_VERSION}-Linux-ARM.zip"
fi

URL="https://github.com{KFR_VERSION}/${KFR_ZIP}"

if [ ! -d "${TARGET_PATH}/include/kfr" ]; then
    echo "Downloading prebuilt ${KFR_ZIP} from ${URL}..."
    TMP_ZIP=$(mktemp /tmp/kfr.XXXXXX.zip)
    TMP_DIR=$(mktemp -d /tmp/kfr.XXXXXX)
    curl -fsSL "$URL" -o "$TMP_ZIP"
    unzip -q "$TMP_ZIP" -d "$TMP_DIR"

    rm -rf "$TARGET_PATH"
    mkdir -p "$TARGET_PATH"
    if [ -d "$TMP_DIR/include/kfr" ]; then
        mv "$TMP_DIR"/* "$TARGET_PATH/"
    else
        mv "$TMP_DIR"/*/* "$TARGET_PATH/"
    fi
    rm -rf "$TMP_ZIP" "$TMP_DIR"
    echo "Successfully unpacked ${KFR_NAME} v${KFR_VERSION} into ${TARGET_PATH}."
fi
