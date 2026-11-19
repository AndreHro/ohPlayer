#!/bin/bash
set -e

PLATFORM="aarch64-scarthgap-linux"
DEBUG_MODE="Release"
DEPS_DIR="./dependencies/${PLATFORM}"

HOST="https://linn-artifacts-public.s3-eu-west-1.amazonaws.com"
DROP="artifacts"

mkdir -p "$DEPS_DIR"

declare -A DEPS
DEPS[ohNet]="1.42.6286"
DEPS[ohWafHelpers]="0.2.176"
DEPS[ohMediaPlayer]="1.173.1270"
DEPS[ohNetGenerated]="1.1.260"

declare -A PLATFORM_MAP
PLATFORM_MAP[ohNet]="aarch64-scarthgap-linux-${DEBUG_MODE}"
PLATFORM_MAP[ohWafHelpers]=""
PLATFORM_MAP[ohMediaPlayer]="aarch64-scarthgap-linux-${DEBUG_MODE}"
PLATFORM_MAP[ohNetGenerated]="AnyPlatform-${DEBUG_MODE}"  # headers-only package (Generated/Cp* and Dv*); the aarch64 lib comes from the ohContainerBuilder-built package

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

# ----------------------------------------------------------------------------
# Generated ohNet headers (Cp* proxies and Dv* providers) are not shipped in
# the S3 packages for this platform. Fetch them from the openhome/ohNetGenerated
# GitHub repository at the tag matching the version above and install them
# flat into ${DEPS_DIR}/Generated/, which is on the compiler include path
# (-I$(DEPS_DIR) resolves #include <Generated/...>).
# Only headers are needed: the corresponding .cpp files are already compiled
# into the ohNetGenerated library.
OHNG_GITHUB_TAG="ohNetGenerated_${DEPS[ohNetGenerated]}"
OHNG_TARBALL="https://github.com/openhome/ohNetGenerated/archive/refs/tags/${OHNG_GITHUB_TAG}.tar.gz"
OHNG_TMP="$(mktemp -d)"

echo "Downloading ohNetGenerated sources (${OHNG_GITHUB_TAG}) for Generated headers..."
echo "Target URL: $OHNG_TARBALL"

curl -sSL "$OHNG_TARBALL" | tar -xz -C "$OHNG_TMP" 2>/dev/null
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "ERROR: Failed to download ohNetGenerated sources from GitHub!"
else
    OHNG_SRC=$(find "$OHNG_TMP" -maxdepth 1 -mindepth 1 -type d | head -1)
    mkdir -p "${DEPS_DIR}/Generated"
    cp "${OHNG_SRC}"/OpenHome/Net/ControlPoint/Proxies/*.h "${DEPS_DIR}/Generated/" 2>/dev/null
    cp "${OHNG_SRC}"/OpenHome/Net/Device/Providers/*.h "${DEPS_DIR}/Generated/" 2>/dev/null
    cp "${OHNG_SRC}"/OpenHome/Net/Device/Providers/*.cpp "${DEPS_DIR}/Generated/" 2>/dev/null
    HEADER_COUNT=$(ls "${DEPS_DIR}/Generated"/*.h 2>/dev/null | wc -l)
    echo "Installed ${HEADER_COUNT} Generated headers and provider sources into ${DEPS_DIR}/Generated/"
fi
rm -rf "$OHNG_TMP"

KFR_NAME="kfr"
KFR_VERSION="7.2.0"
ARCH=$(uname -m)
TARGET_PATH="${DEPS_DIR}/${KFR_NAME}"

if [ "$ARCH" = "aarch64" ]; then
    KFR_ZIP="KFR-${KFR_VERSION}-Linux-ARM64.zip"
else
    KFR_ZIP="KFR-${KFR_VERSION}-Linux-ARM.zip"
fi
URL="https://github.com/kfrlib/kfr/releases/download/${KFR_VERSION}/${KFR_ZIP}"

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
