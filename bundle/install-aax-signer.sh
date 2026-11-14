#!/bin/bash
# Sets up AAX signing on Samuel's Mac (the one the iLok plugs into):
#   - the `sign-aax` command (~/.local/bin/sign-aax): runs the latest bundle/sign-aax.sh from main, which signs
#     the macOS AAX of every release waiting at "Sign on the Mac" (or the plug-ins you name);
#   - "Sign AAX" (~/Applications/Sign AAX.app, keep it in the Dock): opens a Terminal window running sign-aax;
#   - a prompt (launchd, every 2 minutes) that asks once per release when signing is waiting: plug the iLok
#     in, then Sign or Later. It never signs without you.
#
#   bundle/install-aax-signer.sh               install, or update after these scripts change
#   bundle/install-aax-signer.sh --uninstall
#
# Needs what bundle/sign-aax.sh needs (gh logged in, PACE's tools, ~/.config/sammyj/pace.env, your Developer
# ID certificate). Log: ~/Library/Logs/SammyJ-AAX-Signer.log
set -euo pipefail

label="com.samueljustice.sammyj.aax-signer"
here="$(cd "$(dirname "$0")" && pwd)"
dir="$HOME/Library/Application Support/SammyJ/aax-signer"
bin="$HOME/.local/bin"
plist="$HOME/Library/LaunchAgents/$label.plist"
app="$HOME/Applications/Sign AAX.app"
log="$HOME/Library/Logs/SammyJ-AAX-Signer.log"
domain="gui/$(id -u)"

launchctl bootout "$domain/$label" >/dev/null 2>&1 || true

if [[ "${1:-}" == "--uninstall" ]]; then
  rm -f "$plist" "$dir/aax-signer-agent.sh" "$bin/sign-aax"
  rm -rf "$app"
  echo "Removed sign-aax, the Sign AAX app and the prompt (~/.config/sammyj and the log are left alone)."
  exit 0
fi

mkdir -p "$dir" "$bin" "$(dirname "$plist")" "$(dirname "$log")"
cp "$here/aax-signer-agent.sh" "$dir/aax-signer-agent.sh"
chmod 755 "$dir/aax-signer-agent.sh"

cat > "$bin/sign-aax" << 'EOF'
#!/bin/bash
# sign-aax: signs the macOS AAX of releases waiting for this Mac. Runs the current bundle/sign-aax.sh from
# samueljustice/plugins' main branch (see its header for options). Installed by bundle/install-aax-signer.sh.
export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
repo="samueljustice/plugins"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bundle"
for f in sign-aax.sh resolve-releases.sh plugins.txt; do
  gh api "repos/$repo/contents/bundle/$f?ref=main" -H "Accept: application/vnd.github.raw" > "$work/bundle/$f" \
    || { echo "Couldn't fetch bundle/$f from GitHub (offline, or gh not logged in?)"; exit 1; }
done
chmod +x "$work/bundle/"*.sh
cd "$HOME/Library/Application Support/SammyJ/aax-signer"
"$work/bundle/sign-aax.sh" "$@" 2>&1 | tee -a "$HOME/Library/Logs/SammyJ-AAX-Signer.log"
exit "${PIPESTATUS[0]}"
EOF
chmod 755 "$bin/sign-aax"

cat > "$plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$label</string>
    <key>ProgramArguments</key>
    <array><string>/bin/bash</string><string>$dir/aax-signer-agent.sh</string></array>
    <key>StartInterval</key><integer>120</integer>
    <key>ProcessType</key><string>Interactive</string>
    <key>StandardOutPath</key><string>$log</string>
    <key>StandardErrorPath</key><string>$log</string>
</dict>
</plist>
EOF
launchctl bootstrap "$domain" "$plist"

# Sign AAX: the click-to-sign shortcut. A Terminal window shows sign-aax's progress and result.
mkdir -p "$(dirname "$app")"
rm -rf "$app"
osacompile -o "$app" - << APPLESCRIPT
tell application "Terminal"
  activate
  do script "clear; echo 'Signing the AAX waiting for this Mac. Make sure the iLok is plugged in.'; echo; '$bin/sign-aax'; echo; echo 'Done: you can close this window.'"
end tell
APPLESCRIPT
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.samueljustice.sammyj.sign-aax" "$app/Contents/Info.plist" 2>/dev/null \
  || /usr/libexec/PlistBuddy -c "Add :CFBundleIdentifier string com.samueljustice.sammyj.sign-aax" "$app/Contents/Info.plist"

echo "Installed sign-aax ($bin/sign-aax), the Sign AAX app ($app: drag it to the Dock) and the prompt."
case ":$PATH:" in
  *":$bin:"*) ;;
  *) echo "Add $bin to your PATH to type sign-aax anywhere, e.g.: echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.zshrc" ;;
esac
echo "Log: $log    Remove with: $0 --uninstall"
