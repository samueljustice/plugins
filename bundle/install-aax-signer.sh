#!/bin/bash
# Installs (or with --uninstall removes) the background job that signs each new release's macOS AAX on this
# Mac: a launchd agent running bundle/aax-signer-agent.sh every 10 minutes while you're logged in.
#   bundle/install-aax-signer.sh              install, or update after aax-signer-agent.sh changes
#   bundle/install-aax-signer.sh --uninstall
#   bundle/install-aax-signer.sh --run-now    install and run once straight away
# Needs everything bundle/sign-aax.sh needs (gh logged in, PACE's tools, ~/.config/sammyj/pace.env, your
# Developer ID certificate, the iLok available). Log: ~/Library/Logs/SammyJ-AAX-Signer.log
set -euo pipefail

label="com.samueljustice.sammyj.aax-signer"
here="$(cd "$(dirname "$0")" && pwd)"
dir="$HOME/Library/Application Support/SammyJ/aax-signer"
plist="$HOME/Library/LaunchAgents/$label.plist"
log="$HOME/Library/Logs/SammyJ-AAX-Signer.log"
domain="gui/$(id -u)"

launchctl bootout "$domain/$label" >/dev/null 2>&1 || true

if [[ "${1:-}" == "--uninstall" ]]; then
  rm -f "$plist" "$dir/aax-signer-agent.sh"
  echo "Removed the AAX signing job (settings in ~/.config/sammyj and the log are left alone)."
  exit 0
fi

mkdir -p "$dir" "$(dirname "$plist")" "$(dirname "$log")"
cp "$here/aax-signer-agent.sh" "$dir/aax-signer-agent.sh"
# Releases published before the first install are left alone (sign them with bundle/sign-aax.sh if wanted).
[[ -f "$dir/since" ]] || date -u +%Y-%m-%dT%H:%M:%SZ > "$dir/since"
chmod 755 "$dir/aax-signer-agent.sh"

cat > "$plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$label</string>
    <key>ProgramArguments</key>
    <array><string>/bin/bash</string><string>$dir/aax-signer-agent.sh</string></array>
    <key>StartInterval</key><integer>600</integer>
    <key>RunAtLoad</key><$([[ "${1:-}" == "--run-now" ]] && echo true || echo false)/>
    <key>ProcessType</key><string>Background</string>
    <key>LowPriorityIO</key><true/>
    <key>StandardOutPath</key><string>$log</string>
    <key>StandardErrorPath</key><string>$log</string>
</dict>
</plist>
EOF

launchctl bootstrap "$domain" "$plist"
echo "Installed: releases published after $(cat "$dir/since") get their macOS AAX signed (checked every 10 minutes while you're logged in)."
echo "Log: $log    Remove with: $0 --uninstall"
