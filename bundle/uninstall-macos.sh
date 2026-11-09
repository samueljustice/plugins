#!/bin/bash
# Removes Sammy J's Plugins from a Mac. Run by "Uninstall Sammy J's Plugins.app" (bundle/uninstaller.applescript),
# which ships next to the installer in the macOS disk image, with plugins.txt and userdata.txt beside it.
#
#   uninstall-macos.sh list   <plugins.txt>                        names and formats, one per line
#   uninstall-macos.sh system <plugins.txt>                        as root: the apps and plug-ins, and the
#                                                                  installer receipts
#   uninstall-macos.sh user   <plugins.txt> <userdata.txt> [--data] as the user: copies in the user's own
#                                                                  Library and, with --data, settings and presets
#
# Testing: SJ_ROOT puts every system path under a scratch folder, and HOME the user paths; with SJ_ROOT set,
# receipts are not touched and Audio Units are not rescanned.
set -uo pipefail

mode="${1:-}"
plugins="${2:-}"
root="${SJ_ROOT:-}"

[[ -f "$plugins" ]] || { echo "usage: $0 list|system|user <plugins.txt> [<userdata.txt> [--data]]" >&2; exit 2; }

# Each plug-in: id|prefix|name|bundle|app (blank lines and # comments skipped).
each_plugin() {
  grep -v '^#' "$plugins" | grep -v '^[[:space:]]*$'
}

# Deletes one path, refusing anything that isn't a plain absolute path with a name in it.
remove() {
  local p="$1"
  case "$p" in
    /*/*) ;;
    *) echo "refusing to remove '$p'" >&2; return 1 ;;
  esac
  if [[ -e "$p" || -L "$p" ]]; then
    rm -rf -- "$p" && echo "Removed $p"
  fi
}

case "$mode" in
  list)
    each_plugin | while IFS='|' read -r id _prefix name _bundle app; do
      if [[ -n "$app" ]]; then echo "$name (app, VST3, AU)"; else echo "$name (VST3, AU)"; fi
    done
    ;;

  system)
    each_plugin | while IFS='|' read -r id _prefix _name bundle app; do
      [[ -n "$bundle" ]] || continue
      remove "$root/Library/Audio/Plug-Ins/VST3/$bundle.vst3"
      remove "$root/Library/Audio/Plug-Ins/Components/$bundle.component"
      [[ -n "$app" ]] && remove "$root/Applications/$app.app"
      if [[ -z "$root" ]]; then
        # This installer's receipts, and the ones the plug-in's own stand-alone installer left.
        pkgutil --pkgs 2>/dev/null \
          | grep -E "^(com\.samueljustice\.sammyj|com\.sweetjusticesound)\.$id\." \
          | while read -r r; do pkgutil --forget "$r" >/dev/null 2>&1 && echo "Forgot receipt $r"; done
      fi
    done
    if [[ -z "$root" ]]; then
      # Make hosts rescan Audio Units so the removed ones disappear without a restart.
      killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
    fi
    ;;

  user)
    userdata="${3:-}"
    data="${4:-}"
    [[ -n "${HOME:-}" && -d "$HOME" ]] || { echo "no home folder" >&2; exit 2; }
    each_plugin | while IFS='|' read -r id _prefix _name bundle _app; do
      [[ -n "$bundle" ]] || continue
      remove "$HOME/Library/Audio/Plug-Ins/VST3/$bundle.vst3"
      remove "$HOME/Library/Audio/Plug-Ins/Components/$bundle.component"
    done
    if [[ "$data" == "--data" && -f "$userdata" ]]; then
      grep -v '^#' "$userdata" | grep -v '^[[:space:]]*$' | while IFS='|' read -r _id path parent; do
        [[ -n "$path" ]] || continue
        remove "$HOME/$path"
        if [[ -n "$parent" && -d "$HOME/$parent" ]]; then
          # Shared vendor folders go only once nothing else is in them (.DS_Store aside).
          rm -f "$HOME/$parent/.DS_Store"
          rmdir "$HOME/$parent" 2>/dev/null && echo "Removed $HOME/$parent"
        fi
      done
    fi
    true
    ;;

  *)
    echo "unknown mode '$mode'" >&2
    exit 2
    ;;
esac
exit 0
