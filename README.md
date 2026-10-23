# No Man's Sky on Mac: fix for never seeing other players

If you play No Man's Sky on a Mac through Steam and **never see anyone** (the Anomaly is always empty,
you never run into other travelers), even with multiplayer enabled and even though it works on your
other devices, this might be your fix.

> **This is not an official Hello Games fix.** It's a community workaround for a bug in the Mac version,
> tested on No Man's Sky **4.70** (Steam, Apple Silicon). Use it at your own risk, and stop using it once
> the game is patched.

## What's actually wrong

It isn't your network, router, firewall, VPN or settings. The game reaches its servers fine and even joins
sessions. Then it leaves them on its own, within a second, every few seconds, forever.

The cause is a small bug in the Mac build. When the game sets up multiplayer, it forgets to initialize the
field that holds its **voice chat** connection, so the field contains leftover memory. The game reads that
leftover value as "voice chat is already set up" and skips setting it up. Then, every time it joins a
session, connecting voice chat fails, and the game treats that as fatal and leaves.

Whether the leftover memory happens to be clean is luck. That's why multiplayer might have worked for you
once in a blue moon, and why turning Voice Chat off in the settings doesn't help: the setup runs anyway.

Curious how this was tracked down? See [FINDINGS.md](FINDINGS.md) for the full step-by-step.

## The fix

A tiny library is loaded into the game at launch. It clears that one field at the right moment, so the
game sets up voice chat normally and stays in sessions. No game files are changed: stop using the launcher
and the game is exactly as before.

### 1. Get the files

Download this repository (green **Code** button → **Download ZIP**, then unzip it), or:

```bash
git clone https://github.com/dantnad/project-astro.git
cd project-astro
```

### 2. Install Apple's command-line tools (once)

The library is built from source on your own Mac. In Terminal:

```bash
xcode-select --install
```

If they're already installed, it just tells you so.

### 3. Build the fix (once, and again after game updates)

In Terminal, from the project folder:

```bash
./nms-party-shim/build.sh
```

### 4. Launch the game with it

Open Steam and sign in. Then, instead of pressing Play, run:

```bash
./nms-party-shim/launch.sh
```

Cloud saves, friends and achievements still work, because Steam is still running the show. Steam's own
Launch Options can't be used for this on macOS (you'd get "OS Error 260"), which is why there's a launcher.

## Checking that it worked

When you quit the game, the launcher prints a short log (also saved at `~/Library/Logs/nms-party-shim.log`).
Look for:

```
fix: chat-control handle before = 0x11800000004  <-- uninitialized garbage
PartyDeviceCreateChatControl(...) -> 0 OK
PartyNetworkConnectChatControl(...) -> 0 OK
```

- **`fix: chat-control handle before = …`**: it found and cleared the bad value. 🎉
  If the value shown is `0x0`, the memory was clean this time and the game would have been fine anyway.
- **`fix: skipped, unexpected call site`**: the game has been updated and the code moved. The fix
  deliberately does nothing in that case instead of guessing. Try playing normally from Steam first; the
  bug may be fixed. If it isn't, the fix needs updating for the new version.

Want to see the bug with your own eyes? Run `NMS_SHIM_NOFIX=1 ./nms-party-shim/launch.sh`. The log will show
`PartyNetworkConnectChatControl(... chat=<garbage>) -> 6 failed to find specified resource` every few
seconds, and you'll see nobody.

## Removing it

Delete `~/.local/lib/nms_party_shim.dylib` (and, if you like, `~/Library/Logs/nms-party-shim.log`), then go
back to pressing Play in Steam. That's all.

## Things that are *not* the cause

Save yourself the time: port forwarding and NAT type, the macOS firewall, VPNs and web filters, IPv6, DNS,
Bluetooth headphones, microphone permissions, the in-game Voice Chat setting, and your Mac's hardware.

---

Not affiliated with Hello Games or Microsoft. No game files are included or modified.
Code is MIT-licensed, see [LICENSE](LICENSE).
