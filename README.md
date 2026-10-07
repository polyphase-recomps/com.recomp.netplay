# com.recomp.netplay

Networked multiplayer for the recomp game runtimes, starting with N64. Several machines play one
game together: over a LAN, found automatically, or by the host's IP:port. Windows, Linux and macOS
machines play in the same session.

## How it works

**Lockstep input sync.** This is how N64 and PS1 netplay is normally done. The games are
deterministic: their clocks come from the frame counter, and the same code computes the same
results on every machine. So the machines only exchange controller input.
- Frame N runs once every player's input for frame N is known.
- Each player's input is scheduled *input delay* frames ahead, which hides the network latency.
  **Auto** (the default) picks it when the game starts, from the round trips measured in the
  lobby: their average plus twice their jitter, or their recent peak if higher, plus 8 ms. In
  practice that is 2 frames on a wired LAN and 3 on typical Wi-Fi (more on a spiky link).
- A late packet makes the game wait a moment. Nothing is guessed, so nothing is ever rolled back.
  The player waits for it within the tick (up to 10 ms) rather than giving the whole frame up,
  so a slightly late input costs nothing visible.
- **The network thread:** packets are received and answered as they arrive, on a thread of their
  own, not once per game frame. A host passes a client's input on to the others at once. Polling
  per frame used to add up to a frame at each machine, which over Wi-Fi meant stalls.

**Topology.** One host and up to 3 clients, each client connected to the host only (a star).
- The host collects everyone's input and sends each completed frame (all four controller ports)
  to every client. So every machine runs exactly the same inputs.
- A player who drops is decided on once, by the host: their port reads as idle from then on, and
  everyone else plays on.

**Starting.**
1. The host sends its save data to every client. Games keep unlocks and options there, which
   change how they play.
2. Every machine reboots the game from frame 0 with that save. It is kept in
   `Saves/netplay.sra`, so a player's own save is never touched.
3. When the session ends, each game reboots with its own save.

**Desync check.** Every 60 frames each machine hashes what its game drew. If any differs, the
session stops with a message. That means a different build or ROM, not a network problem. N64
games hash the frame's display lists (`n64_frame_signature`: commands, vertices, matrices), so
players can pick different render resolutions; older game libraries hash the picture.

**The wire.**
- UDP, little-endian, so any CPU can play with any other.
- Every packet repeats all unacknowledged input (up to 32 frames). A lost packet only costs a
  resend.
- Every packet also measures the round trip.
- The core is plain C99 with the OS's sockets (`Source/netplay.c`), with no engine dependency.

## Using it in the editor

**Developer > Netplay...** opens the Netplay window. Start the game (Play) first, so the session
knows which game this machine plays.

- **Host a session:**
  - choose a UDP port (default 27464; hosts on it show up in LAN searches);
  - wait for players;
  - press **Start** once everyone is ready.
- **Join:**
  - type the host's address (`192.168.1.20`, or `host:port` for a non-default port), or tick
    **Search the LAN** and pick a session;
  - wait for the host to start.
- **Input delay:** the host's setting counts. Leave it on **Auto**; set a number (the - button
  below 1 goes back to Auto) only to force one.
- **Over the internet:** the host forwards its UDP port on its router. IPv4 only for now.

**The in-game netplay menu: Tools > Recomp > UI > Generate Network Scene...**

This makes the scene `Assets/Scenes/SC_Netplay.oct`. It is built with com.recomp.mod.base's UI
builder and widgets, so it takes the game's Menu Style and the gamepad navigates it. It has:
- the status line and the players with their ping;
- **Host a session**, with a port field;
- an **Address** field and **Join**;
- the input delay, with - and + buttons;
- **Search the LAN**, and the sessions found (press one to join it);
- **Start**, **Leave** and **Close**.

To use it:
1. Add the scene to the game's scene (or launcher).
2. Open it in the game with the **Open with** button (default L3, since Select opens the mod
   settings) or from the HOME menu.
3. Typing an address needs a keyboard; with a gamepad, use **Search the LAN**.

Generating again only adds what is missing, so your changes to the scene stay. The nodes behind
it are `NetplayButton` (a RecompButton with an `Action`: host, join, search, joinfound:N, start,
leave, delay-, delay+, close) and `NetplayMenu`, which fills the texts in by node name. Both work
in packaged games.

**In the Mods UI.** The same window's **Add Netplay to the Mods UI** button (also
**Tools > Recomp > UI > Add Netplay to Mods UI...**) adds a **Netplay** tab and page to the
game's Mod Settings scene. They are made the way com.recomp.mod.base makes its own
(`Tab_Netplay` with `@page:Netplay`), so its page switching, gamepad navigation and Menu Style
work on them. The page holds `ListScroll > List`: the list scrolls in its own ScrollContainer
(a ScrollContainer already around the list is kept), and the menu scrolls it so the selected
button stays in view, so a controller reaches every row on a 480-line screen. Regenerating the
Mods UI keeps them; run Add again afterwards so the tab strip stays wide enough.

**SignalBus.** The engine's global bus, `GetSignalBus()` in C++ and `SignalBus` in Lua:

| Signal | Direction | What it does |
|---|---|---|
| `Netplay.Open` | in | Opens the netplay menu. In the Mods UI it opens the Mods UI on its Netplay tab. Optional argument: the name of the UI's root node (`"Netplay"`, `"ModSettings"`) when a scene has several; otherwise the first one. |
| `Netplay.Opened` | out | The netplay menu was shown. |
| `Netplay.Closed` | out | It went away: closed, B, another Mods UI tab, or the session started (the menu closes itself then). |

Both out signals carry the UI's name and the session state (`idle`, `lobby`, `ready`,
`running`, `desync`, ...), so whoever opened the menu decides what comes next: reopen its own
menu, or let the game run when the state is `running`. For example:

```lua
SignalBus.Subscribe("Netplay.Closed", self, function(listener, ui, state)
    if state ~= "running" then self:ShowMainMenu() end
end)
SignalBus.Emit("Netplay.Open")
```

`Netplay.OpenMenu([ui])`, `Netplay.CloseMenu()` and `Netplay.IsMenuOpen()` do the same from Lua.

**Packaged builds without a netplay menu** take the session from environment variables. They are
read once, when the game starts:

| Variable | What it does |
|---|---|
| `NETPLAY=host` | Host on the default port (`host:27500` for another). |
| `NETPLAY=join:<address>` | Join a host (`join:192.168.1.20`, or `join:192.168.1.20:27500`). |
| `NETPLAY_PLAYERS=2` | Host: starts the game once this many players are in and ready (default 2). |
| `NETPLAY_NAME`, `NETPLAY_DELAY` | This machine's player name, and the input delay (host: frames, or `auto`, the default). |

The `Netplay` Lua table does the same, for a game's own menus. `Hosts()` and `Players()` return
arrays of tables.

| Function | What it does |
|---|---|
| `Netplay.Host([port])` | Opens a session as the host. |
| `Netplay.Join(address)` | Joins a host. |
| `Netplay.Search(on)` | Starts or stops looking for LAN sessions. |
| `Netplay.Hosts()` | The LAN sessions found: `{address, game, host, players, maxPlayers, inGame, compatible}`. |
| `Netplay.Start()` | Host: starts the game once everyone is ready. |
| `Netplay.Leave()` | Leaves the session. |
| `Netplay.Players()` | The players: `{player, name, ready, connected, ping, isLocal}`. |
| `Netplay.State()` | `"idle"`, `"lobby"`, `"joining"`, `"syncing"`, `"ready"`, `"running"`, `"desync"`, `"disconnected"` or `"error"`. |
| `Netplay.Status()` | A status line to show to players. |
| `Netplay.IsHost()` | Whether this machine hosts. |
| `Netplay.IsPlaying()` | Whether the game runs on the session's inputs. |
| `Netplay.LocalPlayer()` | This machine's player number (1-4). |
| `Netplay.Frame()` | The next frame to run. |
| `Netplay.SetName(name)` | This machine's player name. |
| `Netplay.SetDelay(frames)` / `Netplay.Delay()` | The input delay (0: auto; `Delay()` is 0 while auto, before the start). |
| `Netplay.DefaultPort()` | The default UDP port (27464). |

## Adding it to a game

**N64 games built on com.recomp.n64's shared player** (`Game/N64GamePlayer.h`, the template's
games): add the dependency to the game package's `package.json`. That's all.

```json
"dependencies": { "com.recomp.netplay": "https://github.com/polyphase-recomps/com.recomp.netplay" }
```

The shared player turns netplay on when it can include `NetplayN64.h`
(`Game/N64GamePlayerImpl.h`, `N64_GAME_NETPLAY`). It then registers the game, reads the local
controller, and takes its frames from the session while one plays.

**A game with its own player node** (e.g. `Ssb64Player.cpp` until it moves to the shared player):
add the dependency, `#include "NetplayN64.h"`, and three calls. See the header comment in
`Source/NetplayN64.h`.
1. **After booting:** `NetplayN64::Configure(id, title, flavor, bootedFile, saveFile)`.
2. **In `SendInput`:** keep port 1's pad. That is this machine's controller.
3. **In `Tick`, before the player's own frame loop:**

   ```cpp
   const int ran = NetplayN64::RunFrames(deltaTime, localPad, reboot, afterFrame, saveDir, mFrameAccumulator);
   if (ran >= 0) { if (ran > 0) UpdateDisplayTexture(); return; }
   ```

   Here `reboot(save)` shuts the game down and boots it again with that save file, and
   `afterFrame` submits each frame's audio.

**Other runtimes** (PS1, GameCube) use the C API in `netplay.h` directly:
1. `netplay_frame()` before each game frame, which gives every port's input or says to wait;
2. `netplay_frame_done(hash)` after it.

`NetplayPad` has 32 button bits and 4 axes.

## Playing across platforms

Every machine needs:
- **the same game build:** same runtime version, same game library flavor (recomp builds are the
  surest);
- **the same ROM.** A session only forms between machines with the same game id and version;
  the version is a hash of the game's file when the player provides it.

The desync check catches anything else.

Recompiled games compute bit-identical results on x86-64 and ARM64. com.recomp.n64 builds the
generated game code with `-ffp-contract=off`, because ARM64 compilers would otherwise fuse
multiply-adds and round differently. On x86-64 that flag changes nothing: the generated assembly
is identical with and without it, checked with clang and GCC.

## Tests (no engine needed)

```
cmake -S Tests -B build/tests && cmake --build build/tests
```

- **`netplay_selftest [loss%]`:** a host and two clients in one process over 127.0.0.1, with
  packets dropped on purpose. It checks that:
  - the save data arrives intact;
  - every machine runs every frame with the inputs each player sent;
  - a diverging machine is caught as a desync;
  - a vanished client is dropped while the others play on, still identical.
- **`netplay_lan [seconds]`:** lists the sessions on the LAN.
- **With an N64 game library** (`-DNETPLAY_N64_LIB=<lib> -DNETPLAY_N64_INCLUDE=<com.recomp.n64>/Native/include`):
  - **`netplay_n64`:** a headless runner. Two to four of them, on one machine or several, play one
    game, each fuzzing its own controller port. Their `--dump` frames and final hashes must match.
  - **`netplay_player`:** the game-player side (`NetplaySession` + `NetplayN64.h`) driven by a
    60 Hz tick. It boots alone, then hosts or joins, reboots on start, and runs with the desync
    check on. Timing options: `--realtime` ticks at 60 Hz with the real frame time, as the engine,
    and prints the speed the session played at; `--lag ms[,jitter]` holds this machine's packets
    back (a slow or Wi-Fi link); `--delay N` (0: auto); `--nobg` / `--nowait` turn the network
    thread / the in-tick wait off (the behavior before them).
- **`netplay_selftest [loss%] [port] [bg]`:** `bg` 1 runs every machine on its network thread.

```
netplay_n64 --rom ssb64.z64 --host 27464 --players 2 --frames 3000 --dump d1 --fuzz 1
netplay_n64 --rom ssb64.z64 --join 192.168.1.20 --frames 3000 --dump d2 --fuzz 2
```

### Verified (2026-10-06, Super Smash Bros. recompiled)

- **Self-test:** passes on Windows (clang) and Linux (GCC) at 0, 20, 30 and 40% packet loss.
- **Windows ↔ Windows over localhost:** 3000 frames, all 30 checkpoint frames identical, the same
  final hash.
- **Linux host (GCC), Windows client (clang) over the LAN:** 3000 frames, all 30 checkpoint
  frames byte-identical, the same final hash.
- **3 players** (Linux host, two Windows clients): identical on all machines, and identical
  again on a second run.
- **Player path** (`netplay_player`), Linux ↔ Windows: one reboot each, the desync check every
  60 frames, the same final picture.
- **LAN discovery:** a Windows search finds the Linux host on 27464.

- **Slowdown over a slow link** (`netplay_player --realtime`, two machines on one PC, SSB):

  | Link (each way) | Before (delay 2, no thread, no wait) | Now, delay 2 | Now, auto |
  |---|---|---|---|
  | 5 ms + 0..10 ms jitter (Wi-Fi) | 57.8 fps (96%) | 60 fps | 60 fps, delay 3, no waits |
  | 10 ms + 0..25 ms jitter (bad Wi-Fi) | 40.4 fps (67%) | 58.1 fps (97%) | 60 fps, delay 5, no waits |
  | none (LAN) | 60 fps | 60 fps | 60 fps, delay 2 |

- **Self-test with the network thread** (`bg` 1) and without: passes on Windows (MSVC) and Linux
  (GCC) at 0, 20 and 40% loss.

macOS uses the same POSIX code as Linux but hasn't been run yet.

## Not yet

- **Rollback:** it needs save states of the whole game (RDRAM, the runtime's OS state, audio, and
  the game threads' fiber stacks), taken every frame and restored in the same process, and the
  game re-run several frames per tick. Possible for recompiled games on PC; not for the decomp
  console builds.
- **Spectators and joining a game in progress.**
- **IPv6.**
- **Consoles (Wii, 3DS) as peers:** the engine's network layer has sockets for them, but the
  console builds of the games aren't recompiled the same way yet.
- **Mid-session save writes go to `Saves/netplay.sra` only.** By design.

## Files

| File | What |
|---|---|
| `Source/netplay.h`, `netplay.c` | The core: protocol, lobby, save sync, lockstep, desync check (C99, sockets only). |
| `Source/NetplaySession.*` | The process-wide session the addon owns (host / join / start, game registration). |
| `Source/NetplayN64.h` | Netplay for an N64 game player (header-only). |
| `Source/NetplayMenu.*` | The in-game menu's nodes: `NetplayButton`, `NetplayMenu` (needs com.recomp.mod.base). |
| `Source/NetplaySceneGen.*` | Tools > Recomp > UI > Generate Network Scene (editor). |
| `Source/ComRecompNetplay.cpp` | The addon: per-tick polling, the `Netplay` Lua table, the editor windows. |
| `Tests/` | The self-test, the headless N64 runners, the LAN lister. |
