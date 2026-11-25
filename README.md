# CSD3116 Low Level Programming: Drifto

A networked drift-racing game built for the CSD3116 group assignment at SIT / DigiPen, and
the project where I first had to write a client and server stack instead of calling one.

## What is in here

| Component | Role |
| --- | --- |
| `client_folder/`, `game/`, `gfx/`, `audio/` | Drifto game client (C++, OpenGL, GLFW) |
| `client.c`, `server.c`, `secure_participant.c`, `auth.c` | legacy C client/server used for the protocol and security exercises |
| `api_server`, `game_server` | account/auth API and gameplay server |
| `net/` | shared networking library: IDL contract, config, implementation, its own tests |
| `messages.idl` | the message contract, compiled with `idlc` |
| `governance.xml`, `permissions.xml` | DDS Security configuration (authentication, access control) |
| `gen_certs.sh` | certificates for the DDS Security handshake and TLS |
| `perf/` | measurement harness |
| `docs/` | assignment documentation |

All networking runs over Eclipse Cyclone DDS rather than raw sockets, with DDS Security
for authenticated participants. Player passwords are hashed with OpenSSL.

## Attribution

Track generation and vehicle physics are adapted from
[JohnnyHowe/drifto-old](https://github.com/JohnnyHowe/drifto-old) (C#, 2021). Everything
else here, including the servers, the networking library, the auth and security
configuration and the clients, is the team's own work. The borrowed code is used for
coursework only.

Built by a student team for CSD3116.

---

## Setup

**Windows**
- Clone the repository in GitHub Desktop
- The servers and the legacy C client run in Linux (WSL); the Drifto game client
  (`drifto.exe`) also builds natively on Windows -- see [Windows](#windows-native-client)

**Linux**
To clone the repository: 

HTTPS:
```bash
git clone https://github.com/tanukifurhire/csd3116_project.git
```

SSH:
```bash
git clone git@github.com:tanukifurhire/csd3116_project.git
```
*(This requires you to have already generated an SSH key in WSL and added it to your GitHub profile.)*

### Prerequisites

Run all of these inside WSL2 (Ubuntu), not in Windows.

1. **Open WSL2.**

2. **Install the OpenGL development files.**
   ```bash
   sudo apt install libgl1-mesa-dev
   ```

3. **Graphics Library Framework (GLFW)** [1] shall be used for windowing and keyboard input.

4. **Install the GLFW headers and libraries.**
   ```bash
   sudo apt install libglfw3-dev
   ```

5. **Cyclone DDS** [2] shall be used for all networking — install with:
   ```bash
   sudo apt update
   sudo apt install -y cyclonedds-dev cyclonedds-tools
   ```
   `cyclonedds-dev` provides the headers and the `CycloneDDS.pc` file that the
   makefile looks up via `pkg-config`; `cyclonedds-tools` provides `idlc`, the
   IDL compiler that turns `messages.idl` into `messages.c`/`messages.h`.

6. **Install the OpenSSL development headers.** The server hashes player
   passwords with OpenSSL, and the DDS Security plugins need it too.
   ```bash
   sudo apt install libssl-dev
   ```

To confirm everything is in place before building:
```bash
idlc --version
pkg-config --exists CycloneDDS && echo "CycloneDDS OK"
pkg-config --exists glfw3 && echo "GLFW OK"
```

### References

- [1] GLFW — https://www.glfw.org/
- [2] Eclipse Cyclone DDS — https://cyclonedds.io/

## Run

**Compile the project**
```bash
make certs assets   # certs + converted models/HUD; both ship in the submission zip, so this is a no-op there
make all            # legacy server/client + drifto, api_server, game_server
```
This compiles the project with all the necessary tools, libraries and links with OpenGL/GLFW/DDS.
Run every program from the repository root (it finds `certs/`, `assets/` and `net/config/` there).

**Certificates (`certs/`) -- keep the provided ones for online play.** The
submission includes our team's DDS Security certificates in `certs/`. Our online
servers (`--api 121.6.130.7`) authenticate every client against the CA in that
folder, so the provided certs are what let you log in there; a client with
newly generated certs fails the DDS handshake and reports the API server as
unreachable. `make certs` leaves an existing `certs/` alone and `make clean` no
longer deletes it. Only run `make clean-certs certs` if you want a fresh set
for your own local servers -- you then can't join ours. The CA's private key is
not included; the server cert/key are, so local servers built from this folder
work with the same certs.

**Assets (`assets/`) -- included.** The submission also ships the converted
models, textures, skyboxes, HUD sprites and audio in `assets/`, so no download
or conversion is needed (`make assets` sees they are current and does nothing).
They come from third-party sources credited in `assets/CREDITS.txt`, which are
not ours to redistribute further. In a git checkout `assets/` is not committed:
`make assets` downloads and converts them.

### Run the game (Drifto)

```bash
./api_server &                      # login + rooms, TCP 7600
./game_server --tcp-port 8443 &     # game router, UDP 7700-7701 + TCP 8443 fallback
./drifto --api 127.0.0.1            # menu -> ONLINE PLAY; start 2+ clients to race
./drifto --api 121.6.130.7          # or play on our online servers (needs the provided certs/)
```
**Logging in online:** there is no server password. The password is your own: the
first login with a new name creates that account with the password you type, and
later logins with that name must use the same password ("wrong password" means the
name is already taken -- pick another). Each client needs a different name; two
clients on one PC can use the same `certs/` (or `--cert 2` for the second).
Other modes, no servers needed:
```bash
./drifto --offline --bots 3         # race against bots
./drifto --lan                      # LAN: rooms are discovered on the local network
./drifto --car rx7                  # pick the RX-7 (default AE86); also A/D in the menus
```
Controls: auto-throttle, A/D or Left/Right steer, Space nitro (charges while drifting; a burn needs 5% to start), Esc back. Windows: racing wheels and pedals too (see "Racing wheel and pedals").
In the menus, room list and lobby A/D or Left/Right picks your car (AE86 / RX-7, same physics; remembered).
Lobby: H host / C create a room, Space ready, Enter start (host). Max 4 players per room.
All players and servers need the same `certs/` (or all use `--insecure`). See `docs/MULTIPLAYER.md`
for deployment on a public server, ports and the LAN details.

**Tests and profiling**
```bash
make check_game                     # 51 simulation tests (no GL, no DDS)
make game_tests_asan && ./build/asan/game_tests
make net_tests && ./net_tests       # 13 networking scenarios, starts its own servers
make bench_hot && perf stat ./bench_hot bot 100   # hot-path benchmark (perf/)
make bot_bench && ./bot_bench 12 90 solo,pack,mix # bots: wrecks, takedowns, m/min, nitro use
```

### Run the course baseline (legacy server/client)

For Client:
```bash
./client       # or ./client N for client cert N (1-4)
```
You'll be asked for a player name and password. A new name registers that password; an existing name must match the one it registered with.

For Server: 
```bash
./server
```
The server stores each account's salted password hash in `players.auth` (created next to the binary, `chmod 600`, never committed) so registered players are remembered across restarts.

**Or run a full local demo in one shot**
```bash
make run N=3   # starts the server, then N clients (1-4)
```
## Windows native client

`drifto.exe` (the game client only -- `api_server`/`game_server` stay on Linux)
builds with MSVC + vcpkg from `CMakeLists.txt` / `vcpkg.json`. The Linux
makefiles are unchanged. Windows players can use Online mode (the staging
servers, or Linux servers anywhere) and LAN mode. LAN works better from native
Windows than from WSL2, because WSL2's NAT networking blocks LAN multicast.

### Prerequisites (Windows)

- **Visual Studio 2022** (Community is fine) with the *Desktop development with
  C++* workload. It includes MSVC, CMake and **vcpkg**. *Build Tools for Visual Studio 2022*
  with the same workload also works. VS 2026 also works, but the scripts pick 2022.
- **Git for Windows** on `PATH`. vcpkg uses it to fetch its port registry.
- Internet access on the first build. vcpkg downloads and builds `glfw3`, `openssl`
  and `cyclonedds` (0.10.5 with DDS Security, wire-compatible with the Linux
  servers' 0.10.4). This takes about 15–30 minutes once. Later builds reuse the binary cache in
  `%LOCALAPPDATA%\vcpkg\archives`.
- Linux side (for the runtime files): `make assets` and `make certs` in WSL.
  `assets/` and `certs/` are not in git (the submission zip includes both);
  the sync script copies them over.

### Build

From WSL (the WSL repo stays the source of truth):
```bash
tools/windows/sync_and_build.sh          # rsync to C:\Users\<you>\source\repos\drifto + build Release x64
tools/windows/sync_and_build.sh --clean  # clean rebuild
tools/windows/sync_and_build.sh --sync   # copy only
```
Or on Windows, in a checkout that has `assets\` and `certs\`:
```bat
tools\windows\build.bat
```
Output: `build\windows\Release\drifto.exe`. Shaders, `net\config`, `certs`,
`assets` and the DLLs (GLFW, CycloneDDS + its DDS Security plugins, OpenSSL)
are copied next to it, so it runs from any folder or by double-click. Don't
build straight from `\\wsl$\...`: MSVC and vcpkg are slow and unreliable on
that share.

### Run

```bat
cd %USERPROFILE%\source\repos\drifto\build\windows\Release
drifto.exe --help
drifto.exe --api 121.6.130.7              :: online (staging)
drifto.exe --lan                          :: LAN, rooms found by multicast
drifto.exe --lan --lan-peer 192.168.1.20  :: LAN when multicast is blocked
drifto.exe --headless --offline --fast --races 1 :: no window: quick self-test
```

- **Windows Defender Firewall**: the first time `drifto.exe` uses the network,
  Windows asks whether to allow it. Tick **Private networks** (and Public only
  if you trust it), then click *Allow*. If you dismissed the prompt, LAN games
  won't be found. Fix it in *Windows Security > Firewall & network protection >
  Allow an app through firewall* (add `drifto.exe`), or in an admin PowerShell:
  `New-NetFirewallRule -DisplayName Drifto -Direction Inbound -Program "<path>\drifto.exe" -Action Allow`.
  Online play only makes outgoing connections, so it works even without the rule.
- **GPU**: native Windows uses your GPU vendor's OpenGL driver directly. None
  of the WSL settings (`GALLIUM_DRIVER=d3d12`, `MESA_*`) are needed or used.
- **Several network adapters** (VPNs such as Tailscale, Radmin, OpenVPN, or
  Hyper-V/WSL adapters): if LAN rooms don't show up, pin the real adapter with
  `set DRIFTO_DDS_INTERFACE=Wi-Fi` (an adapter name from `ipconfig`, or its IPv4
  address, comma-separated for more than one) before starting `drifto.exe`.
- **Windows and WSL2 on the same PC can't play LAN together** in WSL's default
  NAT mode. The Hyper-V firewall drops traffic from Windows into the WSL VM
  (UDP and TCP; checked on this machine), so even `--lan-peer` doesn't help.
  Use `networkingMode=mirrored` (see `docs/MULTIPLAYER.md`, "LAN mode"), or play
  Windows against Windows. Online play from Windows to servers inside WSL works
  through `--api 127.0.0.1`, because WSL forwards TCP on localhost; the game
  link then falls back to TCP.
- `lan_probe.exe host|join NAME [--peer IP] [--insecure]` (built next to
  `drifto.exe`) is a headless LAN check without a window.

### Racing wheel and pedals (DirectInput)

On Windows, `drifto.exe` drives with a DirectInput wheel (Logitech G29/G920/G27,
Thrustmaster, Fanatec and others that show up as a *driving* device). The
keyboard keeps working alongside it. A wheel plugged in while the game is
running is picked up within a few seconds.

- **Steer**: the wheel. The car reaches full lock at 1/2.5 of the wheel's
  rotation (180 deg on a 900 deg wheel; set `wheel_steer_gain`).
- **Throttle pedal**: auto-throttle stays on until you first press the pedal.
  From then on the pedal sets the throttle. Pedals calibrate themselves:
  the end of the axis where a released pedal rests counts as 0.
- **Brake pedal**: read and sent with the inputs, but the arcade physics has
  no brake, so it does nothing yet.
- **Nitro**: shift paddles (buttons 4/5 on Logitech).
- **Menus**: D-pad = arrow keys, Cross/A = Enter, Circle/B = Esc.

`drifto.exe --wheel-debug` prints the raw axes and the buttons you are holding
once a second. Use it to find the numbers for a wheel that is mapped differently,
then add lines to `%APPDATA%\drifto\settings.txt` (they are kept when the game
rewrites the file):

| key | default | meaning |
| --- | --- | --- |
| `wheel` | `on` | `off` = ignore wheels (same as `--no-wheel`) |
| `wheel_device` | *(first wheel)* | use the controller whose name contains this text, even if it is not a driving device |
| `wheel_steer_axis` | `x` | `x y z rx ry rz slider0 slider1` |
| `wheel_throttle_axis` / `wheel_brake_axis` | `y` / `rz` | `none` disables that pedal |
| `wheel_combined_pedals` | `off` | one axis for both pedals (throttle on `wheel_throttle_axis`, towards -1) |
| `wheel_pedals` | `on` | `off` = auto-throttle always, pedals ignored |
| `wheel_steer_gain` | `2.5` | 1 = full wheel lock for full steering |
| `wheel_steer_deadzone` / `wheel_pedal_deadzone` | `0.01` / `0.05` | fraction of the axis |
| `wheel_invert_steer` | `off` | |
| `wheel_nitro_buttons` | `4,5` | comma-separated, 0-based |
| `wheel_accept_button` / `wheel_back_button` | `0` / `2` | `-1` = none |

Force feedback is not used. A wheel without a centring spring feels loose,
so turn on the spring in the wheel's own software (G HUB, Thrustmaster
Control Panel) if you want one.
