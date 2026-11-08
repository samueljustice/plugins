#!/usr/bin/env bash
# Notarises a signed file and staples the ticket to it (an app is zipped to submit, then stapled itself).
#   bundle/notarise.sh <.pkg | .dmg | .app>     needs APPLE_ID, APPLE_APP_PASSWORD, APPLE_TEAM_ID
set -euo pipefail
f="$1"
auth=(--apple-id "$APPLE_ID" --password "$APPLE_APP_PASSWORD" --team-id "$APPLE_TEAM_ID")

submit="$f"
if [[ "$f" == *.app ]]; then
  submit="$(mktemp -d)/$(basename "$f" .app).zip"
  ditto -c -k --keepParent "$f" "$submit"
fi

echo "Notarising $f"
ID=$(xcrun notarytool submit "$submit" "${auth[@]}" --output-format json | jq -r '.id')
xcrun notarytool wait "$ID" "${auth[@]}"
STATUS=$(xcrun notarytool info "$ID" "${auth[@]}" --output-format json | jq -r '.status')
if [[ "$STATUS" != "Accepted" ]]; then
  xcrun notarytool log "$ID" "${auth[@]}"
  exit 1
fi

# Fetching the ticket from Apple sometimes drops the connection (stapler error 68), at times for minutes,
# so retry with a growing pause (30 s, 60 s, ..., about 10 minutes in all).
delay=30
for attempt in 1 2 3 4 5 6 7; do
  if xcrun stapler staple "$f"; then exit 0; fi
  echo "Stapling $f failed (attempt $attempt of 7)"
  [[ "$attempt" -lt 7 ]] && { sleep "$delay"; delay=$((delay + 30)); }
done
exit 1
