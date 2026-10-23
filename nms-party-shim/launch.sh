#!/bin/zsh
# Launches No Man's Sky with the shim (logging + multiplayer fix) loaded.
# Steam on macOS can't pass environment variables through launch options, so we open the app ourselves
# with the variables set. Steam must already be running and signed in; SteamAppId keeps the Steam API
# from relaunching the game through Steam (which would drop DYLD_INSERT_LIBRARIES).
# NMS_SHIM_NOFIX=1 ./launch.sh  runs with logging only (for comparison).
set -e
app="$HOME/Library/Application Support/Steam/steamapps/common/No Man's Sky/No Man's Sky.app"
shim="$HOME/.local/lib/nms_party_shim.dylib"
log="$HOME/Library/Logs/nms-party-shim.log"
appid=275850

[[ -f "$shim" ]] || { echo "Shim not built. Run ${0:A:h}/build.sh first."; exit 1; }
[[ -d "$app" ]] || { echo "Game not found: $app"; exit 1; }
pgrep -x steam_osx >/dev/null || { echo "Start Steam (and sign in) first."; exit 1; }

env_args=(--env DYLD_INSERT_LIBRARIES="$shim" --env SteamAppId=$appid --env SteamGameId=$appid)
[[ -n "$NMS_SHIM_NOFIX" ]] && env_args+=(--env NMS_SHIM_NOFIX=1)

echo "Launching NMS with shim. Log: $log"
open -W -n -a "$app" "${env_args[@]}"
echo "Game exited. Shim log for this run:"
awk '/===== launch pid/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$log" | grep -v -E "StateChange type=(17|21) "
