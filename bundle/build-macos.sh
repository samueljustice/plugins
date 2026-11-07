#!/usr/bin/env bash
# Builds the combined Sammy J macOS installer (unsigned) from each plug-in's newest release.
#   bundle/build-macos.sh <releases file from resolve-releases.sh> <bundle version> <work dir>
# Leaves <work dir>/SammyJ-Plugins-macOS-Installer.pkg. The workflow signs and notarises it.
#
# The bundles inside come straight from the plug-ins' own releases, which are already signed, notarised
# and stapled, so nothing here re-signs them. Every component package removes the old copy first
# (system-wide and in the user's own Library) and forgets the old per-plug-in installer's receipt, so an
# upgrade leaves no stale files or duplicate plug-ins behind.
set -euo pipefail

releases="$1"
version="$2"
work="$3"
here="$(cd "$(dirname "$0")" && pwd)"

rm -rf "$work"
mkdir -p "$work"/{dl,roots,scripts,pkgs,resources}

choices=""
outline=""
refs=""
welcome_rows=""

# Makes <work>/pkgs/<pkgid>.pkg installing <root>'s single bundle to <location>.
#   make_pkg <pkgid> <root> <location> <bundle file name> <old receipt prefix> [au]
make_pkg() {
  local pkgid="$1" root="$2" location="$3" bundle="$4" oldreceipt="$5" au="${6:-}"
  local scripts="$work/scripts/$pkgid"
  mkdir -p "$scripts"

  cat > "$scripts/preinstall" << EOF
#!/bin/bash
# Remove the old copy first so files a newer version no longer has don't linger, then any copy in the
# logged-in user's own Library, which hosts would otherwise list twice.
rm -rf "$location/$bundle"
user=\$(stat -f%Su /dev/console 2>/dev/null || true)
if [ -n "\$user" ] && [ "\$user" != "root" ]; then
  home=\$(dscl . -read "/Users/\$user" NFSHomeDirectory 2>/dev/null | awk '{print \$2}')
  if [ -n "\$home" ]; then
    rm -rf "\$home$location/$bundle"
  fi
fi
# The old stand-alone installer's receipt for this file; this package owns it now.
for r in \$(pkgutil --pkgs 2>/dev/null | grep -F "$oldreceipt" || true); do
  pkgutil --forget "\$r" >/dev/null 2>&1 || true
done
exit 0
EOF
  chmod +x "$scripts/preinstall"

  if [[ -n "$au" ]]; then
    cat > "$scripts/postinstall" << 'EOF'
#!/bin/bash
# Make hosts rescan Audio Units so the new version shows up without a restart.
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
exit 0
EOF
    chmod +x "$scripts/postinstall"
  fi

  # Not relocatable: otherwise Installer "updates" a copy it finds elsewhere (a build folder, say)
  # instead of installing to the plug-in folder. Not version-checked: always lay down the version this
  # installer carries, even over a newer build someone installed by hand.
  pkgbuild --analyze --root "$root" "$work/pkgs/$pkgid.plist" >/dev/null
  local n
  n=$(/usr/libexec/PlistBuddy -c "Print" "$work/pkgs/$pkgid.plist" | grep -c "RootRelativeBundlePath" || true)
  for ((i = 0; i < n; i++)); do
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$work/pkgs/$pkgid.plist" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$work/pkgs/$pkgid.plist"
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsVersionChecked" "$work/pkgs/$pkgid.plist" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsVersionChecked bool false" "$work/pkgs/$pkgid.plist"
  done

  pkgbuild --identifier "$pkgid" --version "$version" --root "$root" \
    --component-plist "$work/pkgs/$pkgid.plist" --scripts "$scripts" \
    --install-location "$location" "$work/pkgs/$pkgid.pkg" >/dev/null
}

# Checks a bundle is signed and notarised before it goes in.
check_bundle() {
  codesign --verify --deep --strict "$1"
  xcrun stapler validate "$1" >/dev/null || echo "warning: $1 has no stapled notarisation ticket"
}

while IFS='|' read -r id prefix name bundle app; do
  [[ -z "$id" || "$id" == \#* ]] && continue
  line=$(grep "^$id|" "$releases")
  tag=$(cut -d'|' -f2 <<< "$line")
  ver=$(cut -d'|' -f3 <<< "$line")
  echo "== $name $ver ($tag)"

  dl="$work/dl/$id"
  patterns=(-p '*macOS-VST3.zip' -p '*macOS-AU.zip')
  [[ -n "$app" ]] && patterns+=(-p '*macOS-App.dmg')
  gh release download "$tag" "${patterns[@]}" -D "$dl"

  r="$work/roots/$id"
  mkdir -p "$r/vst3" "$r/au" "$r/app"
  ditto -x -k "$(ls "$dl"/*macOS-VST3.zip)" "$r/vst3"
  ditto -x -k "$(ls "$dl"/*macOS-AU.zip)" "$r/au"
  [[ -d "$r/vst3/$bundle.vst3" ]] || { echo "$bundle.vst3 not in $tag's VST3 zip"; exit 1; }
  [[ -d "$r/au/$bundle.component" ]] || { echo "$bundle.component not in $tag's AU zip"; exit 1; }
  check_bundle "$r/vst3/$bundle.vst3"
  check_bundle "$r/au/$bundle.component"

  make_pkg "com.samueljustice.sammyj.$id.vst3" "$r/vst3" /Library/Audio/Plug-Ins/VST3 "$bundle.vst3" "com.sweetjusticesound.$id.vst3"
  make_pkg "com.samueljustice.sammyj.$id.au" "$r/au" /Library/Audio/Plug-Ins/Components "$bundle.component" "com.sweetjusticesound.$id.au" au
  kinds="vst3 au"

  if [[ -n "$app" ]]; then
    mnt="$work/mnt-$id"
    mkdir -p "$mnt"
    hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$(ls "$dl"/*macOS-App.dmg)" >/dev/null
    ditto "$mnt/$app.app" "$r/app/$app.app"
    hdiutil detach "$mnt" >/dev/null
    check_bundle "$r/app/$app.app"
    make_pkg "com.samueljustice.sammyj.$id.app" "$r/app" /Applications "$app.app" "com.sweetjusticesound.$id.app"
    kinds="app vst3 au"
  fi

  outline+="        <line choice=\"$id\">"$'\n'
  choices+="    <choice id=\"$id\" title=\"$name $ver\"/>"$'\n'
  for k in $kinds; do
    case $k in
      app) title="$app app (Applications)" ;;
      vst3) title="VST3 plug-in" ;;
      au) title="Audio Unit plug-in" ;;
    esac
    pkgid="com.samueljustice.sammyj.$id.$k"
    outline+="            <line choice=\"$id.$k\"/>"$'\n'
    choices+="    <choice id=\"$id.$k\" title=\"$title\"><pkg-ref id=\"$pkgid\"/></choice>"$'\n'
    refs+="    <pkg-ref id=\"$pkgid\" version=\"$version\">$pkgid.pkg</pkg-ref>"$'\n'
  done
  outline+="        </line>"$'\n'
  formats="VST3, AU"
  [[ -n "$app" ]] && formats="app, VST3, AU"
  welcome_rows+="<li><b>$name</b> $ver <span>($formats)</span></li>"$'\n'
done < "$here/plugins.txt"

cat > "$work/resources/welcome.html" << EOF
<!doctype html>
<html><head><meta charset="utf-8"><style>
body { font: 13px -apple-system, sans-serif; }
li { margin: 3px 0; } span { color: #888; }
</style></head><body>
<h2>Sammy J's Plugins</h2>
<p>Toys for sound designers, by Samuel Justice. This installs the newest version of each:</p>
<ul>
$welcome_rows</ul>
<p>Older copies are removed first, so you won't see duplicates in your host.
Choose <b>Customize</b> to pick which plug-ins and formats to install.</p>
<p>Universal (Apple silicon and Intel), macOS 11 or later.</p>
</body></html>
EOF

cat > "$work/distribution.xml" << EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2.0">
    <title>Sammy J's Plugins</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <domains enable_localSystem="true"/>
    <options customize="allow" require-scripts="false" hostArchitectures="x86_64,arm64"/>
    <allowed-os-versions><os-version min="11.0"/></allowed-os-versions>
    <choices-outline>
$outline    </choices-outline>
$choices$refs</installer-gui-script>
EOF

productbuild --distribution "$work/distribution.xml" --resources "$work/resources" \
  --package-path "$work/pkgs" --version "$version" "$work/SammyJ-Plugins-macOS-Installer.pkg"
ls -la "$work/SammyJ-Plugins-macOS-Installer.pkg"
