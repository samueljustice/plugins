#!/usr/bin/env bash
# Builds De-Honk's macOS installer package from bundles that are already signed, notarised and stapled.
# The package itself is left unsigned: the workflows sign and notarise it.
#
#   dehonk/make-installer-macos.sh <staging folder> <version> <output .pkg>
#
# The staging folder holds any of vst3/SammyJs De-Honk.vst3, au/SammyJs De-Honk.component, aax/SammyJs De-Honk.aaxplugin and
# app/SammyJs De-Honk.app. The installer has a choice for each one that is there (all ticked; Customize to pick).
# Used by .github/workflows/build-release_dehonk.yml (no AAX yet) and dehonk-test-installers.yml (with the
# PACE-signed AAX). Each part removes the old copy first and is installed where hosts look for it, even if
# a copy with the same bundle ID is somewhere else on the disk. The first test build's bundles were called
# "De-Honk.<type>"; those are removed too, so a host never finds two copies with the same plug-in code.
set -euo pipefail

staging="$(cd "${1:?staging folder}" && pwd)"
version="${2:?version}"
out="${3:?output .pkg}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

outline=""
choices=""
refs=""

for kind in vst3 au aax app; do
  case "$kind" in
    vst3) bundle="SammyJs De-Honk.vst3";      location="/Library/Audio/Plug-Ins/VST3";                      title="VST3 plug-in" ;;
    au)   bundle="SammyJs De-Honk.component"; location="/Library/Audio/Plug-Ins/Components";                title="Audio Unit plug-in" ;;
    aax)  bundle="SammyJs De-Honk.aaxplugin"; location="/Library/Application Support/Avid/Audio/Plug-Ins";  title="AAX plug-in (Pro Tools)" ;;
    app)  bundle="SammyJs De-Honk.app";       location="/Applications";                                     title="De-Honk app" ;;
  esac
  root="$staging/$kind"
  [[ -d "$root/$bundle" ]] || continue
  pkgid="com.samueljustice.dehonk.$kind"
  echo "== $title: $bundle"

  scripts="$work/scripts-$kind"
  mkdir -p "$scripts"
  cat > "$scripts/preinstall" << EOF
#!/bin/bash
# Remove the old copy first, so files a newer version no longer has don't linger, and the copy the
# first test build installed under its old name.
rm -rf "$location/$bundle"
rm -rf "$location/${bundle#SammyJs }"
exit 0
EOF
  if [[ "$kind" == "au" ]]; then
    cat > "$scripts/postinstall" << 'EOF'
#!/bin/bash
# Make hosts rescan Audio Units so the new version shows up without a restart.
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
exit 0
EOF
  fi
  chmod +x "$scripts"/*

  # Not relocatable: otherwise Installer "updates" a copy with the same bundle ID it finds elsewhere (a
  # build folder, say) instead of installing here. Not version-checked: always lay down this version.
  # One entry per top-level bundle (a PACE-signed AAX's __Pace_Eden.bundle is listed inside its parent).
  plist="$work/$kind.plist"
  pkgbuild --analyze --root "$root" "$plist" >/dev/null
  i=0
  while /usr/libexec/PlistBuddy -c "Print :$i:RootRelativeBundlePath" "$plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$plist" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$plist"
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsVersionChecked" "$plist" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsVersionChecked bool false" "$plist"
    i=$((i + 1))
  done

  pkgbuild --identifier "$pkgid" --version "$version" --root "$root" \
    --component-plist "$plist" --scripts "$scripts" \
    --install-location "$location" "$work/$kind.pkg"

  outline+="        <line choice=\"$kind\"/>"$'\n'
  choices+="    <choice id=\"$kind\" title=\"$title\"><pkg-ref id=\"$pkgid\"/></choice>"$'\n'
  refs+="    <pkg-ref id=\"$pkgid\" version=\"$version\">$kind.pkg</pkg-ref>"$'\n'
done

[[ -n "$refs" ]] || { echo "Nothing to package in $staging"; exit 1; }

cat > "$work/distribution.xml" << EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2.0">
    <title>De-Honk $version</title>
    <organization>com.samueljustice</organization>
    <domains enable_localSystem="true"/>
    <options customize="always" require-scripts="false" hostArchitectures="x86_64,arm64"/>
    <choices-outline>
$outline    </choices-outline>
$choices$refs</installer-gui-script>
EOF
cat "$work/distribution.xml"

productbuild --distribution "$work/distribution.xml" --package-path "$work" --version "$version" "$out"
echo "Built $out"
