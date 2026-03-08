#!/bin/bash
# dev-launch.sh — Launch Sounder Desktop for development with proper macOS permissions
#
# On macOS Sequoia (15+), TCC checks the "responsible process" in the attribution
# chain. When launched from an IDE terminal (e.g., VS Code, Claude Code), the
# terminal's process is the "responsible" process, and if it lacks the
# com.apple.security.device.audio-input entitlement, microphone access is silently
# denied without showing a permission dialog.
#
# Using `open` launches the .app bundle via Launch Services, making Electron.app
# the responsible process, which has the correct entitlements.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ELECTRON_APP="$SCRIPT_DIR/node_modules/electron/dist/Electron.app"

if [ ! -d "$ELECTRON_APP" ]; then
    echo "Error: Electron.app not found at $ELECTRON_APP"
    echo "Run 'npm install' first."
    exit 1
fi

# Reset TCC to ensure a fresh prompt (optional, uncomment if needed)
# tccutil reset Microphone com.hauksbee.sounder

echo "[dev-launch] Opening Sounder via Launch Services..."
echo "[dev-launch] This ensures macOS shows the microphone permission dialog."
echo "[dev-launch] Logs: /tmp/sounder_stdout.log, /tmp/sounder_stderr.log"

# Use open to launch via Launch Services — this makes Electron.app the
# responsible process so TCC checks its entitlements (not the terminal's).
# --stdout and --stderr redirect output to log files since open detaches.
open "$ELECTRON_APP" --args "$SCRIPT_DIR" --enable-logging \
    2>/tmp/sounder_stderr.log

echo "[dev-launch] App launched. Check System Settings > Privacy > Microphone if prompted."
