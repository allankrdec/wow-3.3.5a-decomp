# wowdecomp

A C++ decompilation of the World of Warcraft 3.3.5a client (`Wow.exe`, build 12340, June 2010).

Join the discord for other clients and to show off some cool stuff you build.

https://discord.gg/DsZMGUD8aT

Every function is reconstructed from the original binary and keeps the address it came from in an
`// OFFSET:` tag, so each one can be compared against the original. The source tree follows the layout
of the original client (`Engine/Source/<Module>`, `WoW/Source/<Module>`, original file names).

x64 is the main target. Win32 still builds and is used to check struct layouts against the 32-bit
original.

## Progress

The percentages below measure how much of the original binary has a counterpart in this source tree. They do
**not** say how much of the game works, and the missing part is not missing gameplay: the client builds into a
complete executable and the game is 100% playable. Bugs could still be hiding, though; a ported function can behave differently
from the original.

Measured against the 12340 binary on 2026-10-15. A function counts as ported when its address is
tagged in the source.

| | Ported | Total | Share of the original |
|---|---:|---:|---:|
| Functions | 24,515 | 27,157 | 90.3% |
| Code bytes | 5,270,186 | 5,656,573 | 93.2% |

The total leaves out 658 functions that IDA identifies as compiler library code and 132 import thunks.
Most of the untagged remainder is code the build gets elsewhere: compiler-generated code (static
initializers, template instances) that our compiler emits on its own, C runtime helpers, and third-party
libraries that are linked from their upstream sources instead of being decompiled (FreeType, zlib, PCRE,
parts of FMOD Ex).

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143 and a Windows SDK)
- CMake 3.10 or newer (the one that ships with Visual Studio works)
- For Win32 builds only: [NASM](https://www.nasm.us/), on `PATH` or passed as `-DNASM_EXECUTABLE=<path>`, and
  `DivxDecoder.dll` from your 3.3.5a client copied to `ThirdParty/DivxDecoder/` (the build copies it next to the
  executables; the x64 build uses its own decoder instead)
- A World of Warcraft **3.3.5a (12340)** client. No game data is included in this repository.

## Building

From a Developer PowerShell or any shell with `cmake` on `PATH`:

```powershell
git clone https://github.com/SatyPardus/wowdecomp.git
cd wowdecomp

# x64 (main target)
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --target WowClient --parallel

# Win32 (optional, needs NASM)
cmake -S . -B build/win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build/win32 --config Release --target WowClient --parallel
```

In-source builds are refused, so keep the build directory separate (`build/` above).

The client is written to `bin/Win/`:

| Target | x64 | Win32 |
|---|---|---|
| `WowClient` | `WowClient64.exe` | `WowClient32.exe` |
| `Wow` | `Wow64.exe` | `Wow32.exe` |

`WowClient` is the client with a crash logger: it opens a console window and writes
`WowClient_crash.log` when the client crashes. `Wow` is the same client without it. You can also turn the
logger off in `WowClient` with `-DWOWCLIENT_CRASH_LOG=OFF`.

### Options

| CMake option | Default | Effect |
|---|---|---|
| `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` | `OFF` | Skip the MPQ file and `.toc.sig` signature checks, so modified game data and interface files load |
| `WOWCLIENT_CRASH_LOG` | `ON` | Build `WowClient` with the crash logger |
| `WOWCLIENT_OUTPUT_OVERRIDE` | empty | Name the `WowClient` executable something other than `WowClient64` / `WowClient32` |

## Running

1. Copy the `Data` folder of your 3.3.5a client into `bin/Win/`, so you have `bin/Win/Data/`.
2. Point `Data/<locale>/realmlist.wtf` at the server you want to connect to.
3. Start `bin/Win/WowClient64.exe` (or `Wow64.exe`).

## macOS (Apple Silicon)

The same tree builds a native arm64 macOS client. It renders through the OpenGL backend (a legacy 2.1
context, the one `-opengl` uses on Windows), plays sound through FMOD's CoreAudio output and decodes movies
with xvidcore. Intel Macs are not supported.

### Requirements

- macOS 11 or newer on Apple Silicon
- Xcode or the Command Line Tools (`xcode-select --install`)
- CMake and Ninja (`brew install cmake ninja`)
- A World of Warcraft **3.3.5a (12340)** client (the Windows data works as is)

### Building

```sh
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target Wow
```

The client is written to `bin/Mac/World of Warcraft.app`, signed ad-hoc. Add
`-DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON` to load modified data, such as translation patches.

### Running

1. Put the `Data` folder of your 3.3.5a client next to the app, as `bin/Mac/Data/`. A symbolic link works:
   `ln -s "/path/to/World of Warcraft 3.3.5a/Data" bin/Mac/Data`.
2. Point `Data/<locale>/realmlist.wtf` at your server.
3. Open `World of Warcraft.app`, or run `scripts/jogar-mac.command`, which builds the app first if needed.

The game reads and writes `WTF/`, `Interface/AddOns/`, `Cache/`, `Screenshots/`, `Logs/` and `Errors/` in the
folder that contains the app, like the Windows client does next to the executable. Settings that Windows keeps
in the registry go to `~/Library/Application Support/wowdecomp/Registry.ini`.

The client reports itself as Windows (`'Win'`) to the server, since private servers expect the Windows
build. Build with `-DCMAKE_CXX_FLAGS=-DWOWDECOMP_REPORT_OS=0x4F5358` to report `'OSX'` like the original Mac
client.

### Environment variables

| Variable | Effect |
|---|---|
| `WOWDECOMP_MAC_CMD_AS_CTRL=1` | Cmd works as Ctrl in key bindings (by default Cmd only drives the app menu: Cmd+Q, Cmd+H, Cmd+M) |
| `WOWDECOMP_MAC_MOUSE_ACCEL=1` | Mouse-look follows the accelerated pointer motion instead of the raw device motion |
| `WOWDECOMP_MAC_FPSLOG=1` | Print the frame rate to stderr once a second |
| `WOWDECOMP_MAC_BACKGROUND=1` | Keep the window behind other windows without taking focus (for automated runs) |

### Not supported on macOS

- Voice chat recording
- Battle.net account login (the Grunt login of private servers works)
- The Direct3D backend: a request for Direct3D opens the OpenGL device

## Layout

| Path | Contents |
|---|---|
| `Engine/Source/` | Engine modules: Storm, Gx (Direct3D 9 / OpenGL), Model2, Frame/FrameXML UI, SoundEngine, Lua, ... |
| `WoW/Source/` | Game modules: objects, world rendering, UI, networking, database, glue screens |
| `WoW/Common/` | Code shared by the game modules |
| `ThirdParty/` | Libraries linked from their upstream sources |
| `Tools/WowClient/` | The `WowClient` executable target |
| `Tools/XvidDecoder/` | x64 movie decoder that replaces the original 32-bit `DivxDecoder.dll` |

## Special thanks

- [binana](https://github.com/thunderbrewhq/binana)
- [thunderbrew](https://github.com/thunderbrewhq/thunderbrew)
- [whoa](https://github.com/whoahq/whoa)
- TOMRUS, for the initial IDB
- Everyone who has reported bugs so far
- The WoW modding community
