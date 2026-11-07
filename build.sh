#!/usr/bin/env bash
# Build SPC Echo as a macOS .clap bundle using only clang (no CMake needed).
# Produces build/"SPC Echo.clap" and, on request, installs it for the current user.
#
#   ./build.sh            # build universal (arm64 + x86_64) bundle
#   ./build.sh install    # build, then copy to ~/Library/Audio/Plug-Ins/CLAP

set -euo pipefail
cd "$(dirname "$0")"

CLAP_TAG="1.2.10"
NAME="SPC Echo"
BUNDLE_ID="com.danielhuebert.spc-echo"
VERSION="1.0.0"

# 1. Fetch the CLAP headers if they are not vendored yet.
if [[ ! -f deps/clap/include/clap/clap.h ]]; then
  echo ">> fetching CLAP ${CLAP_TAG} headers"
  git clone --depth 1 --branch "${CLAP_TAG}" \
    https://github.com/free-audio/clap.git deps/clap
fi

# 2. Lay out the bundle:  SPC Echo.clap/Contents/{MacOS,Info.plist,PkgInfo}
BUNDLE="build/${NAME}.clap"
rm -rf "${BUNDLE}"
mkdir -p "${BUNDLE}/Contents/MacOS"

echo ">> compiling (arm64 + x86_64)"
clang++ -std=c++17 -O2 -Wall -Wextra \
  -Ideps/clap/include \
  -dynamiclib -arch arm64 -arch x86_64 \
  src/spc_delay.cpp \
  -o "${BUNDLE}/Contents/MacOS/${NAME}"

cat > "${BUNDLE}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>English</string>
  <key>CFBundleExecutable</key><string>${NAME}</string>
  <key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
  <key>CFBundleName</key><string>${NAME}</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleSignature</key><string>????</string>
</dict>
</plist>
PLIST

printf 'BNDL????' > "${BUNDLE}/Contents/PkgInfo"

# Ad-hoc code signature so recent macOS will load the bundle locally.
codesign --force --sign - "${BUNDLE}" 2>/dev/null || \
  echo ">> (codesign skipped — not required for local testing)"

echo ">> built ${BUNDLE}"
lipo -archs "${BUNDLE}/Contents/MacOS/${NAME}"

# 3. Optional install for the current user.
if [[ "${1:-}" == "install" ]]; then
  DEST="${HOME}/Library/Audio/Plug-Ins/CLAP"
  mkdir -p "${DEST}"
  rm -rf "${DEST}/${NAME}.clap"
  cp -R "${BUNDLE}" "${DEST}/"
  echo ">> installed to ${DEST}/${NAME}.clap"
fi
