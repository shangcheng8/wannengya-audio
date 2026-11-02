#!/bin/sh
set -eu

# Remove every installed component. Source and build output in the project
# directory are left alone. Run as root: sudo sh scripts/uninstall.sh

echo "Stopping agent"
/usr/bin/pkill -f 'wannengya Agent.app/Contents/MacOS/wannengya Agent' 2>/dev/null || true

echo "Removing HAL driver"
for name in Wannengya WannengyaIn12 WannengyaIn34 WannengyaIn56 WannengyaIn78 \
            WannengyaOut12 WannengyaOut34 WannengyaOut56 WannengyaOut78; do
    rm -rf "/Library/Audio/Plug-Ins/HAL/$name.driver"
done

echo "Removing plug-ins and agent"
rm -rf "/Library/Audio/Plug-Ins/VST3/wannengya Capture.vst3"
rm -rf "/Library/Audio/Plug-Ins/VST3/wannengya Return.vst3"
rm -rf "/Applications/wannengya Agent.app"

for home in /Users/*; do
    [ -d "$home" ] || continue
    rm -rf "$home/Applications/wannengya Agent.app"
    rm -rf "$home/Library/Audio/Plug-Ins/VST3/wannengya Capture.vst3"
    rm -rf "$home/Library/Audio/Plug-Ins/VST3/wannengya Return.vst3"
done

echo "Removing per-user shared memory"
rm -f /tmp/macloopback_*.ring /tmp/wannengya_loop_*.ring 2>/dev/null || true

CONSOLE_UID="$(/usr/bin/id -u "$(/usr/bin/stat -f %Su /dev/console)" 2>/dev/null || true)"
if [ -n "$CONSOLE_UID" ]; then
    for name in ctl tap ch channel ret; do
        /bin/launchctl asuser "$CONSOLE_UID" /bin/sh -c "rm -f /dev/null" 2>/dev/null || true
    done
    # shm objects live in /var/run; drop this user's namespace entries.
    ls /var/run/wannengya.* 2>/dev/null | while read -r f; do rm -f "$f"; done
fi

echo "Restarting core audio"
/bin/launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null \
    || killall coreaudiod 2>/dev/null || true

/usr/sbin/pkgutil --forget com.wannengya.installer >/dev/null 2>&1 || true
echo "Done."
