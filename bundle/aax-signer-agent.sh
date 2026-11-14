#!/bin/bash
# The background job bundle/install-aax-signer.sh installs on Samuel's Mac (launchd, every 10 minutes).
# Each run fetches the current bundle/sign-aax.sh, resolve-releases.sh and plugins.txt from main and runs
# sign-aax.sh, which signs the macOS AAX of any new release and starts its notarisation. Shows a macOS
# notification when it signs something or when signing fails (e.g. the iLok isn't available).
# Log: ~/Library/Logs/SammyJ-AAX-Signer.log
set -uo pipefail

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
repo="samueljustice/plugins"
state="$HOME/Library/Application Support/SammyJ/aax-signer"
mkdir -p "$state"

notify() {
  osascript -e "display notification \"$2\" with title \"Sammy J AAX signing\" subtitle \"$1\"" >/dev/null 2>&1 || true
}

# One run at a time (a slow notarisation upload can outlast the 10-minute interval).
lock="$state/run.lock"
if ! mkdir "$lock" 2>/dev/null; then
  if [[ -n "$(find "$lock" -maxdepth 0 -mmin +60 2>/dev/null)" ]]; then rm -rf "$lock"; mkdir "$lock"; else exit 0; fi
fi
work="$(mktemp -d)"
trap 'rm -rf "$work" "$lock"' EXIT

echo "== $(date '+%Y-%m-%d %H:%M:%S')"
gh auth status >/dev/null 2>&1 || { echo "gh isn't logged in"; notify "Can't check releases" "GitHub CLI isn't logged in (run gh auth login)."; exit 1; }

# Today's scripts from main, so the job never needs reinstalling when they change.
mkdir -p "$work/bundle"
for f in sign-aax.sh resolve-releases.sh plugins.txt; do
  gh api "repos/$repo/contents/bundle/$f?ref=main" -H "Accept: application/vnd.github.raw" > "$work/bundle/$f" \
    || { echo "couldn't fetch bundle/$f"; exit 0; }   # offline: try again next time, quietly
done
chmod +x "$work/bundle/"*.sh

cd "$state"
# Only releases published after the job was installed; older ones are signed by hand when wanted.
since=$(cat "$state/since" 2>/dev/null || true)
output=$("$work/bundle/sign-aax.sh" ${since:+--since "$since"} 2>&1)
status=$?
echo "$output"

if [[ $status -ne 0 ]]; then
  reason=$(grep -m1 -E 'wraptool Error|LICENSE ERROR|failed|Missing|not found|not in the keychain' <<< "$output" | cut -c1-180)
  notify "Signing failed" "${reason:-See ~/Library/Logs/SammyJ-AAX-Signer.log}"
  exit 1
fi
signed=$(grep '^Signed:' <<< "$output" | sed 's/^Signed: *//' | paste -sd ',' - | sed 's/,/, /g')
[[ -n "$signed" ]] && notify "Signed and sent for notarisation" "$signed"
exit 0
