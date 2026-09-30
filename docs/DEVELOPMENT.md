# Development

## Layout

```
src/                 PhoneLink.asi (C++20)
  main.cpp             DllMain: register script + keyboard handler, clean shutdown/abandon on exit
  Script.cpp           script fiber: command queue, call state machine, call panel, F7 contacts menu
  ApiServer.cpp        HTTP + WebSocket API (IXWebSocket); sends from a dedicated thread
  NativePhone.cpp      game phone integration (contacts injection + call screen), after iFruitAddon2
  Audio.cpp            miniaudio: clips, jitter-buffered stream, microphone capture
  Tts.cpp              Windows SAPI text-to-speech (used by /call with text)
  Config.cpp, Log.cpp, Watchdog.cpp, Base64.cpp
sdk/                 ScriptHookV headers (main.h, nativeCaller.h, types.h) + ScriptHookV.def
studio/              PhoneLink Studio (Python 3.11: FastAPI + aiortc)
  app.py               control panel (8770) + public call page server (8771)
  calls.py             WebRTC call sessions in both directions, presence, texts
  plugin.py            client for the plugin API
  static/control.html  control panel UI;  public/call.html  friends' call page
tests/
  harness/             stub ScriptHookV.dll + harness.exe that run the real plugin outside GTA
  studio/              end-to-end tests of Studio + plugin
tools/               gen_natives.py, package.ps1, stackdump.cpp (hang diagnostics)
examples/            phonelink_client.py (plugin API client)
```

## Building the plugin

Requirements: Visual Studio 2022 with the C++ workload (MSVC x64), CMake ≥ 3.21 with Ninja (both are bundled with VS), Python 3, and git.

```powershell
# from a "x64 Native Tools Command Prompt" / Developer PowerShell
cmake --preset release
cmake --build --preset release          # -> build/PhoneLink.asi and build/harness/*
```

The configure step downloads and verifies its pinned dependencies, so nothing third-party is committed:
- `miniaudio.h` and `json.hpp`, downloaded by URL and checked with SHA-256
- **IXWebSocket**, fetched with FetchContent at tag `v11.4.5`
- `natives.json` from [alloc8or/gta5-nativedb-data](https://github.com/alloc8or/gta5-nativedb-data), at a pinned commit, from which `tools/gen_natives.py` generates `natives.h`
- `ScriptHookV.lib`, generated from `sdk/ScriptHookV.def` with `lib.exe`. The `.def` lists the exports of `ScriptHookV.dll`, taken from `dumpbin /exports`.

To update the native DB, change the commit and SHA-256 in `CMakeLists.txt`.

**Auto-deploy to your game** (local only): create `CMakeUserPresets.json`. It's git-ignored.

```json
{
  "version": 6,
  "configurePresets": [{ "name": "local", "inherits": "release",
    "cacheVariables": { "PHONELINK_DEPLOY": "ON", "GTA_DIR": "C:/Games/GTAV" } }],
  "buildPresets": [{ "name": "local", "configurePreset": "local" }]
}
```

Then run `cmake --preset local && cmake --build --preset local`. The copy fails while GTA is running, because the game has the `.asi` open.

The `debug` preset (`build-dbg/`) builds `RelWithDebInfo` with PDBs, for crash analysis.

## Running Studio from source

```powershell
cd studio
.\Start.bat            # creates .venv with uv on first run, then starts app.py
```

`PHONELINK_STUDIO_SETTINGS=<path>` makes Studio use another settings file, and write its log next to that file. The tests use this to run a second instance.

## Tests

All tests run without GTA. Python needs `websockets`; the Studio test uses Studio's own `.venv`.

| Test | Covers |
|---|---|
| `python tests/harness/e2e.py build/harness` | Plugin API: auth, texts, text-to-speech/clip/streamed calls, busy/declined/missed, a client dying mid-call |
| `python tests/harness/exit_test.py build/harness` | The game quitting while idle or mid-call: the plugin must not crash or hang (regression test for a crash on exit) |
| `studio\.venv\Scripts\python tests/studio/e2e_twoway.py` | Studio + plugin: texts, friend→player and player→friend (accept/decline/cancel) over real WebRTC |

The harness tests use ports 18765/18766 and the Studio test uses 18770/18771, so they don't interfere with a running game or Studio. Harness commands on stdin:
- `y` / `n`: answer or hang-up key
- `k <hex vk>`: any key
- `phone up` / `phone contacts` / `phone down` / `phone select <slot>` / `phone cancel`: drive the simulated game phone. The stub prints every call made to the phone's UI as `[scaleform] …`.
- `exit`: a game-like `ExitProcess`
- `q`: quit

With `HARNESS_TRACE=1`, the harness prints a symbolized stack for any C++ exception or crash.

## CI and releases

`.github/workflows/build.yml` runs on every push and pull request. On a Windows runner it:
1. builds the plugin and the harness
2. runs the exit smoke test
3. packages `dist/PhoneLink-<version>.zip` with `tools/package.ps1`
4. uploads the `.asi` and the zip as artifacts

**Releasing:** bump `project(PhoneLink VERSION x.y.z)` in `CMakeLists.txt`, commit, then `git tag vx.y.z && git push --tags`. The workflow publishes a GitHub Release with the zip and the `.asi`.

## Design notes

- **Threads:**
  - Natives are only called from the ScriptHookV script fiber.
  - Network threads push commands onto a queue (`g_commands`) that the fiber drains every frame.
  - Outgoing WebSocket messages go through one sender thread, so a slow or dead client can't stall a game frame.
  - IXWebSocket is never called while the plugin's connection lock is held, because a failed send calls back into the close handler.
- **Shutdown:** on process exit, the other threads are already dead. `DllMain` then *abandons* its threads and servers (detach or leak) instead of joining them, because joining would crash (`std::terminate`) or hang.
- **Audio:** call audio uses its own WASAPI stream (miniaudio), not the game's audio engine, which can only play sounds packed into the game files. The live stream has a 100 ms jitter buffer.
- **WebRTC (Studio):** aiortc only gathers ICE candidates on the adapter that routes to the internet. With every adapter included (VPN, Hyper-V, WSL), STUN timeouts made each call take 5 s to connect.

## Game phone integration

`NativePhone.cpp` follows [iFruitAddon2](https://github.com/Bob74/iFruitAddon2)'s approach, using natives only and no memory offsets:

- **Detecting the phone:**
  - The phone is up when the `cellphone_flashhand` script runs.
  - The Contacts app is open when `appcontacts` runs.
- **Which phone UI:**
  - `cellphone_badger` for Franklin
  - `cellphone_facade` for Trevor
  - `cellphone_ifruit` for everyone else
- **Adding contacts:** `SET_DATA_SLOT` on view 2, from slot 50 upwards, re-sent every frame while Contacts is open.
- **Selecting a contact:** `INPUT_CELLPHONE_SELECT` (176), then `GET_CURRENT_SELECTION`. A slot that belongs to us stops `appcontacts` and shows view 4 (the call screen) with `CELL_211` (dialing), then `CELL_219` (connected).
- **Hanging up:** `INPUT_CELLPHONE_CANCEL` (177). Closing the phone runs `SHUTDOWN_MOVIE`, then `DESTROY_MOBILE_PHONE`, then restarts `cellphone_flashhand` and `cellphone_controller`.
- **Incoming calls** still use PhoneLink's own ring and panel.

## Diagnosing a game that hangs on exit

`tools/stackdump.cpp` prints the call stacks of a running process:

```powershell
cmake --build --preset release --target stackdump
build\stackdump.exe <pid of GTA5.exe>
```

Frames show module names, and function names where symbols are available. This is how a shutdown hang caused by another ASI mod was identified.
