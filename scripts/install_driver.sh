#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

BUNDLE="$PWD/build/drivers/Wannengya.driver"
TARGET="/Library/Audio/Plug-Ins/HAL/Wannengya.driver"

if [ ! -d "$BUNDLE" ]; then
    echo "No driver bundle. Run scripts/build_driver.sh first." >&2
    exit 1
fi

echo "Installing wannengya virtual audio devices."
echo "System audio will be interrupted for a moment. Save your DAW session first."

sudo mkdir -p /Library/Audio/Plug-Ins/HAL

# The previous generation installed one bundle per direction. Remove them so
# the four duplex devices are the only wannengya devices left.
for old in WannengyaIn12 WannengyaIn34 WannengyaIn56 WannengyaIn78 \
           WannengyaOut12 WannengyaOut34 WannengyaOut56 WannengyaOut78; do
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/$old.driver"
done

sudo rm -rf "$TARGET"
sudo /usr/bin/ditto "$BUNDLE" "$TARGET"

# coreaudiod only loads HAL plug-ins that it can trust: the bundle must be
# owned by root. ditto preserves the build tree's ownership (your account), so
# fix it here - this is the step that makes the devices show up.
sudo /usr/sbin/chown -R root:wheel "$TARGET"
sudo /bin/chmod -R go-w "$TARGET"
sudo /usr/bin/xattr -cr "$TARGET" 2>/dev/null || true
sudo /usr/bin/codesign --verify --deep --strict "$TARGET"

sudo /bin/launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null \
    || sudo /usr/bin/killall coreaudiod 2>/dev/null || true

sleep 2

echo "Installed. Audio MIDI Setup should list four devices, each 2ch in and 2ch out:"
echo "  wannengya 1/2"
echo "  wannengya 3/4"
echo "  wannengya 5/6"
echo "  wannengya 7/8"
echo
echo "If they are still missing, run: sh scripts/doctor.sh"
