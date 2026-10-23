# How the bug was found

A step-by-step account of how we went from "I never see anyone on my Mac" to a single uninitialized field
in the game's code. Dead ends included, because they're half the story and they'll save you time if
you're chasing something similar.

**Setup:** No Man's Sky 4.70 (Steam, arm64), an M4 Max MacBook Pro on macOS 27. Multiplayer worked on the
same account from other devices, never from the Mac.

## Contents

1. [Rule out the usual suspects](#1-rule-out-the-usual-suspects)
2. [Decrypt the game's traffic](#2-decrypt-the-games-traffic)
3. [Read the game's own telemetry](#3-read-the-games-own-telemetry)
4. [Dead end: the "small lobby" flag](#4-dead-end-the-small-lobby-flag)
5. [Dead end: the voice chat toggle, and one lucky launch](#5-dead-end-the-voice-chat-toggle-and-one-lucky-launch)
6. [Find the voice-chat setup in the binary](#6-find-the-voice-chat-setup-in-the-binary)
7. [Put a logger inside the game](#7-put-a-logger-inside-the-game)
8. [The launch method matters](#8-the-launch-method-matters)
9. [Catch the garbage value](#9-catch-the-garbage-value)
10. [Fix it and confirm](#10-fix-it-and-confirm)
11. [Tips and gotchas](#tips-and-gotchas)

## 1. Rule out the usual suspects

The Mac had a few things other devices didn't, so those went first:

- **A debugging proxy** (Proxyman) set as the system proxy while it was open.
- **NordVPN Threat Protection**, which on macOS runs as an always-on transparent proxy for TCP *and* UDP,
  even with the VPN disconnected. That's a classic way to break peer-to-peer game traffic.
- The **macOS firewall** and per-app network permissions.

Multiplayer failed with the proxy closed, and again with Threat Protection off. The firewall and network
permissions allowed the game. DNS resolved all of the game's servers correctly, and HTTPS to them worked
over both IPv4 and IPv6. **None of it was the cause.**

## 2. Decrypt the game's traffic

With the proxy closed, the game held ~40 HTTPS connections to Azure Front Door at once, thousands over a
few minutes. Those turned out to be **PlayFab**, Microsoft's game backend (the binary contains
`%s.playfabapi.com`; the title is `a247c.playfabapi.com`). Connections that succeed but keep repeating
usually mean the server is answering with errors, or the client doesn't like the answers.

Proxyman can decrypt HTTPS for a single app (SSL Proxying scoped to the No Man's Sky app), and the game
accepted its certificate. Its CLI exports captures:

```bash
proxyman-cli export-log -m domains -d a247c.playfabapi.com -f har -o capture.har
```

> Captures contain your Steam session ticket and PlayFab tokens. Don't share or commit them.

PlayFab responses are base64-encoded JSON. Decoded, a full multiplayer attempt looked like this:

```
Client/LoginWithSteam              -> 200
Match/CreateMatchmakingTicket      -> 200   (queue "Nexus")
Match/GetMatchmakingTicket         -> 200   Status: Matched   (~1.5 s)
Lobby/JoinLobby                    -> 200
Lobby/LeaveLobby                   -> 200   (~1 s later)
Match/CreateMatchmakingTicket      -> 200   ... and again, ~30 times
Match/CreateMatchmakingTicket      -> 429   APIClientRequestRateLimitExceeded
```

**Matchmaking worked.** The game found a session, joined it, and left one second later. A network timeout
takes far longer than that, so the game was choosing to leave.

## 3. Read the game's own telemetry

The capture also had `Event/WriteTelemetryEvents`: the game reporting on itself to PlayFab. For every lobby:

```
client_api_error_occurred    Api=PartyNetworkConnectChatControl  ErrorDetail=6
client_network_destroyed     Reason "Requested"
client_connected_to_network  Result 14 "LeaveNetworkCalled"  (13–51 ms after connecting)
```

And from the quality-of-service probes: **UDP to all 17 PlayFab relay regions succeeded, zero failures.**

So the network was healthy, and the game itself called `LeaveNetwork` right after a failed
`PartyNetworkConnectChatControl`. "Chat control" is **voice chat** in PlayFab Party, Microsoft's networking
and voice library that the game uses for sessions.

## 4. Dead end: the "small lobby" flag

The lobby data showed the Mac announcing `smallLobby: "true"` and asking matchmaking for
`needsSmallLobby: 1`, while most other players said `false`. A data file in the game, `gpu.cfg`, sorts Mac
models into performance tiers, and this Mac's model wasn't listed. The theory: the Mac was misclassified as
low-end and rejected the big lobbies it was put into.

Disassembling the game (more in the next step) killed that theory:

- `smallLobby` is the **hardcoded string `"true"`** in every Mac join path. There's no hardware check.
- `needsSmallLobby` is `ticketType != 1`, and both callers pass `0`, so it's 1 for every ticket.
- Nothing in the client ever **reads** `smallLobby` back.

It's harmless metadata, and `gpu.cfg` only affects graphics presets.

## 5. Dead end: the voice chat toggle, and one lucky launch

Voice chat was on in settings, so: turn it off. On the first try, flipping the toggle in-game, it
**connected and other players showed up**. The next launches, with the setting already off, failed again.
So did launches that toggled it on and off before loading a save, and launches with the audio output on
the built-in speakers instead of AirPods.

Comparing the decrypted traffic of the one success against every failure showed one call that only the
success made, **one second after login**, before any lobby:

```
MultiplayerServer/GetCognitiveServicesToken   {"CognitiveServicesType": "TextToSpeech"}
```

That's the token PlayFab Party fetches when the game sets a text-to-speech voice on a newly created voice
chat control. The toggle had nothing to do with it. **In failed launches, the game never created its
voice-chat control at startup.**

## 6. Find the voice-chat setup in the binary

The Mac binary is universal and still has C++ symbol names, which makes it far more readable:

```bash
lipo -thin arm64 "No Man's Sky.app/Contents/MacOS/No Man's Sky" -output nms_arm64
nm -C nms_arm64 > syms.txt
```

At 220 MB the binary is too big for radare2's full analysis to finish in reasonable time, so the work
was targeted instead:

- **String references:** ARM64 code loads addresses in two instructions (`ADRP` for the 4 KB page, then
  `ADD`/`LDR` for the offset). A short numpy script decoded every `ADRP` in `__text` and matched the
  follow-up instruction against a string's address. Some strings are reached through pointer tables in
  `__DATA_CONST`, so those tables were searched too (chained-fixup pointers store the offset in the low bits).
- **Call sites:** same idea for `BL`/`B` instructions targeting a function or import stub.
- **Reading code:** `r2 -2 -q -c 'pd 40 @ 0xADDR' nms_arm64` at each hit.

Callers of the PlayFab Party imports led to `cGcMatchmakingPlayfab::OnPlayfabTokenRetrieved()`, which runs
after login:

```
PartyCreateLocalUser(..., &this->localUser)        ; this+0x2c0
UpdateTitleUserName / UpdateUserData
PartyLocalUserGetEntityId
FindMyLobbies
if (this->chatControl != NULL) return;             ; ldr x8,[x19,#0x2c8] ; cbnz x8, skip
PartyGetLocalDevice
PartyDeviceCreateChatControl(..., &this->chatControl)
PartyChatControlSetTextToSpeechProfile(...)        ; triggers the TTS token request
```

Every later lobby join calls `PartyNetworkConnectChatControl(network, this->chatControl)`.

## 7. Put a logger inside the game

To see exactly which Party call failed and why, a small library was injected to intercept the Party
functions, log their return codes and Microsoft's error messages, and pass everything through unchanged.
The game allows this: its code signature has `com.apple.security.cs.allow-dyld-environment-variables` and
`disable-library-validation`, and PlayFab Party is a separate framework, so dyld interposing
(`__DATA,__interpose`) works cleanly with `DYLD_INSERT_LIBRARIES`.

Getting the variable into the game was the next problem. Steam on macOS **doesn't support**
`VAR=value %command%` in Launch Options: it treats the whole thing as the executable and fails with
"OS Error 260". Starting the binary straight from a terminal works, given a `steam_appid.txt` or
`SteamAppId` so the Steam API doesn't relaunch the game through Steam and lose the variable.

## 8. The launch method matters

Launched from the terminal with the logger, multiplayer worked **3 out of 3 times**:

```
PartyCreateLocalUser          -> 0 OK
PartyGetLocalDevice           -> 0 OK
PartyDeviceCreateChatControl  -> 0 OK
PartyNetworkConnectChatControl -> 0 OK
```

Something about how the game was launched changed the outcome. macOS's process registry showed the
difference: Steam launches resolved as `app<uk.co.hellogames.nms>`, a proper app, while terminal launches
resolved as `anon<No Man's Sky>`. That changes what happens during startup, which, as it turned out,
changes what's left over in memory.

To reproduce the failure *with* the logger, the game was launched as a proper app with the variables set:

```bash
open -n -a "No Man's Sky.app" --env DYLD_INSERT_LIBRARIES=… --env SteamAppId=275850
```

## 9. Catch the garbage value

That launch failed, and the log showed exactly where:

```
PartyCreateLocalUser -> 0 OK
(nothing: PartyGetLocalDevice is never called)
```

Between those two calls the code is a straight line. The only ways out are a failing
`PartyLocalUserGetEntityId`, which is unlikely, or `this->chatControl != NULL`. And no chat control had
ever been created. So the field held garbage.

The constructor confirms it. `cGcMatchmaking::CreateMatchmaking()` allocates the object with
`operator new(0xa08)` from the engine's allocator, which doesn't zero memory. Then
`cGcMatchmakingPlayfab::cGcMatchmakingPlayfab()` clears its neighbors and skips the handle:

```
stp q1, q1, [x0, #0x2a4]   ; zero 0x2a4–0x2c3
str wzr,    [x0, #0x2c4]   ; zero 0x2c4–0x2c7   (localUser is now 0)
strb wzr,   [x0, #0x2d0]   ; zero 0x2d0
                            ; 0x2c8–0x2cf (chatControl): never written
```

Whatever the allocator hands back decides the outcome:

- **Clean memory:** chat control gets created, and multiplayer works.
- **Leftover data:** creation is skipped, every join passes garbage to `PartyNetworkConnectChatControl`,
  Party answers error 6 ("failed to find specified resource"), and the game leaves the session.

That explains the 1-in-13 success rate from Steam, the 3-for-3 run from the terminal (different startup,
different leftovers), and why other platforms, with their own builds and allocators, are fine.

## 10. Fix it and confirm

The game passes `&this->localUser` (`this+0x2c0`) to `PartyCreateLocalUser`, and the broken handle is the
very next 8 bytes. So the injected library clears that slot right after `PartyCreateLocalUser` succeeds.
Two guards keep it from ever touching anything else:

1. It only acts when the call comes from that exact spot in `OnPlayfabTokenRetrieved()`, checked via
   `dladdr` on the return address (offset `0x9c7c4` in the 4.70 binary). After a game update it logs
   "skipped" and does nothing.
2. It only acts if no real chat control has been created yet in this run.

In the launch mode that had just failed:

```
fix: chat-control handle before = 0x11800000004  <-- uninitialized garbage
PartyGetLocalDevice           -> 0 OK
PartyDeviceCreateChatControl  -> 0 OK
```

Then an A/B test in-game, same launcher, same session:

| | Log | In game |
|---|---|---|
| Fix on | `PartyNetworkConnectChatControl(...) -> 0 OK` | Other players visible |
| Fix off (`NMS_SHIM_NOFIX=1`) | `PartyNetworkConnectChatControl(chat=0x11800000004) -> 6 failed to find specified resource`, then `LeaveNetworkCalled` every ~5 s | Nobody |

The proper fix in the game is one line: initialize that member to `nullptr` in the constructor. Treating a
failed voice-chat connection as non-fatal would make it more robust too, since voice chat is optional.

## Tips and gotchas

- **zsh's builtin `log` shadows macOS's `/usr/bin/log`.** `log show …` in zsh silently prints nothing.
  Always use `/usr/bin/log show`. This hid real data for a while.
- **The game logs to the unified log**, not to `FullLog.txt`:
  `/usr/bin/log show --predicate 'process BEGINSWITH[c] "No Man"'`.
- **HAR timestamps are UTC**, while the system log uses local time.
- **Process names:** Steam sometimes launches the game via an ALL-CAPS path. Match it case-insensitively,
  e.g. `pgrep -if "No Man.s Sky.app/Contents/MacOS"`.
- **The `~/Applications/No Man's Sky.app` shortcut** just runs `open steam://run/275850`, so it's the same
  as pressing Play.
- **Universal binaries:** extract the arm64 slice with `lipo` before computing file offsets, or every
  address will be wrong.
