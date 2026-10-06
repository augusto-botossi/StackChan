#!/bin/sh
# Builds melotts_finish_helper_v2 and installs it as a systemd service so it starts on every boot.
# Run ON the Module LLM, as root, after pushing the two files to /opt:
#   adb push module-llm/melotts_finish_helper_v2.c        /opt/
#   adb push module-llm/melotts-finish-helper.service     /opt/
#   adb push module-llm/install_finish_helper_service.sh  /opt/
#   adb shell sh /opt/install_finish_helper_service.sh
set -e

[ "$(id -u)" = 0 ] || { echo "Run as root."; exit 1; }
command -v systemctl >/dev/null 2>&1 || { echo "No systemctl on this system: this script needs systemd."; exit 1; }
command -v gcc >/dev/null 2>&1 || { echo "gcc not found."; exit 1; }
[ -f /opt/melotts_finish_helper_v2.c ] || { echo "/opt/melotts_finish_helper_v2.c is missing."; exit 1; }
[ -f /opt/melotts-finish-helper.service ] || { echo "/opt/melotts-finish-helper.service is missing."; exit 1; }

# Two copies would both inject a finish message, so stop any hand-started one first (-f: the name
# is longer than 15 characters, plain pkill matches nothing).
pkill -f /opt/melotts_finish_helper_v2 2>/dev/null || true

gcc /opt/melotts_finish_helper_v2.c -lzmq -o /opt/melotts_finish_helper_v2
cp /opt/melotts-finish-helper.service /etc/systemd/system/melotts-finish-helper.service
systemctl daemon-reload
systemctl enable --now melotts-finish-helper.service

echo
echo "Installed. Status:"
systemctl --no-pager --lines=5 status melotts-finish-helper.service || true
echo
echo "Follow its log with:  journalctl -u melotts-finish-helper -f"
