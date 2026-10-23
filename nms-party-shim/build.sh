#!/bin/zsh
# Builds the shim against the game's own PlayFabPartyMac and installs it to ~/.local/lib.
set -e
here=${0:A:h}
fw="$HOME/Library/Application Support/Steam/steamapps/common/No Man's Sky/No Man's Sky.app/Contents/Frameworks"
out="$HOME/.local/lib/nms_party_shim.dylib"
mkdir -p "${out:h}"
clang -arch arm64 -dynamiclib -O2 -o "$out" "$here/party_shim.c" \
  -F "$fw" -framework PlayFabPartyMac -Wl,-rpath,"@executable_path/../Frameworks"
codesign -s - -f "$out"
echo "built $out"
echo "Run the game with it:  $here/launch.sh"
