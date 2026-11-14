#!/bin/bash
# The prompt bundle/install-aax-signer.sh sets up on Samuel's Mac (a launchd job, every 2 minutes).
# When a release's "AAX signing" run reaches "Sign on the Mac", it asks once: plug the iLok into this Mac,
# then Sign (runs `sign-aax`) or Later (run `sign-aax` yourself when ready). It never signs on its own,
# because the iLok may be in the PC. Log: ~/Library/Logs/SammyJ-AAX-Signer.log
set -uo pipefail

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
repo="samueljustice/plugins"
stage="Sign on the Mac"
state="$HOME/Library/Application Support/SammyJ/aax-signer"
asked="$state/asked-runs"
mkdir -p "$state"
touch "$asked"

notify() {
  osascript - "$1" "$2" >/dev/null 2>&1 << 'EOF' || true
on run argv
  display notification (item 2 of argv) with title "AAX signing" subtitle (item 1 of argv)
end run
EOF
}

# One prompt at a time.
lock="$state/prompt.lock"
if ! mkdir "$lock" 2>/dev/null; then
  [[ -n "$(find "$lock" -maxdepth 0 -mmin +60 2>/dev/null)" ]] || exit 0   # a prompt is already open
  rm -rf "$lock"; mkdir "$lock" || exit 0
fi
trap 'rm -rf "$lock"' EXIT

gh auth status >/dev/null 2>&1 || exit 0   # offline or logged out: nothing to ask about

# Runs waiting for the Mac that haven't been asked about yet.
runs=$(gh run list -R "$repo" --workflow aax-signing.yml --status in_progress --limit 50 \
         --json databaseId,displayTitle --jq '.[] | "\(.databaseId)|\(.displayTitle)"' 2>/dev/null) || exit 0
new_runs=()
releases=()
while IFS='|' read -r run title; do
  [[ -z "$run" ]] && continue
  grep -qx "$run" "$asked" && continue
  jobs=$(gh run view "$run" -R "$repo" --json jobs --jq '.jobs[] | "\(.name)|\(.status)"' 2>/dev/null) || continue
  grep -qx "$stage|in_progress" <<< "$jobs" || continue
  new_runs+=("$run")
  releases+=("${title##* }")
done <<< "$runs"
[[ ${#new_runs[@]} -eq 0 ]] && exit 0

printf '%s\n' "${new_runs[@]}" >> "$asked"
list=$(printf '%s\n' "${releases[@]}")
echo "== $(date '+%Y-%m-%d %H:%M:%S') waiting for the Mac: $(paste -sd ' ' - <<< "$list")"

answer=$(osascript - "$list" 2>/dev/null << 'EOF'
on run argv
  set releaseList to item 1 of argv
  try
    set reply to display dialog "Ready to sign the AAX for:" & return & return & releaseList & return & return & ¬
      "Plug the iLok into this Mac, then click Sign." with title "AAX signing" ¬
      buttons {"Later", "Sign"} default button "Sign" cancel button "Later" giving up after 1200
    if gave up of reply then return "later"
    return button returned of reply
  on error
    return "later"
  end try
end run
EOF
)

if [[ "$answer" != "Sign" ]]; then
  echo "   later (run sign-aax when the iLok is in this Mac)"
  exit 0
fi

notify "Signing" "$(paste -sd ',' - <<< "$list" | sed 's/,/, /g')"
output=$("$HOME/.local/bin/sign-aax" 2>&1)
status=$?
echo "$output"
if [[ $status -ne 0 ]]; then
  reason=$(grep -m1 -E 'wraptool Error|LICENSE ERROR|iLok|failed|not found' <<< "$output" | cut -c1-180)
  notify "Signing failed" "${reason:-See ~/Library/Logs/SammyJ-AAX-Signer.log}. Fix it, then run sign-aax."
  exit 1
fi
done_list=$(grep '^Signed:' <<< "$output" | sed 's/^Signed: *//' | paste -sd ',' - | sed 's/,/, /g')
notify "Signed on the Mac" "${done_list:-Nothing needed signing}. Move the iLok to the PC next."
exit 0
