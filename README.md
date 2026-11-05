# Coin Rush

Catch coins, dodge bombs and compete for the highest score across three levels.
Coin Rush is a real-time multiplayer game written in C with GLFW, OpenGL and
Cyclone DDS. Each room supports **2-5 players** and one server can run up to
**8 rooms**.

![Coin Rush main menu](docs/screenshots/menu.png)

[Quick start](#start-here) · [Windows setup](#play-on-windows) ·
[Rules](#how-a-match-works) · [Controls](#controls) ·
[Linux build](#build-and-run-in-wsllinux) ·
[Send to a friend](#set-up-another-computer) ·
[LAN setup](#lan-discovery-on-windows) · [FAQ](#faq-and-troubleshooting)

## Start here

- **Hosting:** extract the project, run `server.bat`, then run `client.bat` in a
  second window. Keep the server running while anyone is playing.
- **Playing on a friend's server:** extract the per-player zip the host made for
  you, run `client.bat`, import your invite when prompted, then join the host's
  six-digit room code. You do not need to start another server.
- **Sending the game:** run `package-friend.bat` on the host and give each friend
  their own `packages/CoinRush-clientN.zip`. See the detailed steps below.

Commands marked **PowerShell** run in Windows; commands marked **WSL/Linux** run
inside the selected Linux distribution. Paths shown below are examples: replace
them with the folder where you extracted the project. Always extract the zip
before launching; keep `assets/`, `scripts/`, and the other project folders together.

## Current features

- Main menu with display-name entry, Create Room, Join a Room, and exit confirmation.
- Animated cats in different player colors, plus randomly positioned idle,
  walking, and jumping menu cats that stay inside the scene.
- Shared rooms with server-assigned player slots, a visible room code, lobby
  ready votes, level-break Continue screens, and a final winner/leaderboard screen.
- Lobby **Back** button below the header on the left; **Esc** also leaves the
  lobby and returns to the menu. Leaving releases your slot through the secure
  connection; you can create or join another room without restarting the client.
- Live timer, scores and player information; coins stay below the HUD.
  Players can walk through each other and stay inside the arena.
- Three levels, bomb/life effects, coin collection indicators, and bonus tie-breaks.
- Resizing/fullscreen, per-level music, pickup/hit sounds, mute and volume controls.
- Authenticated and encrypted DDS networking, individual signed player invites,
  server-side input validation, and SSE2/scalar collision implementations.

## Play on Windows

You need WSL 2 with working WSLg graphics and a Linux distribution containing
**Cyclone DDS 0.10.4 or 0.10.5**. Each computer builds its own Linux executable.
The launchers check dependencies and can install missing development packages
on Debian/Ubuntu using `apt`.

### First-time WSL setup

In **PowerShell**, list installed distributions:

```powershell
wsl --list --verbose
```

`VERSION 2` in this list means WSL 2, not the Cyclone DDS release. Select a
distribution by its exact registered name. For example, check an installed
`Ubuntu-26.04` with:

```powershell
wsl -d Ubuntu-26.04 -- bash -lc "pkg-config --modversion CycloneDDS"
```

If WSL or the desired distribution is missing, use `wsl --list --online` to see
available choices, then `wsl --install -d NAME` with a name from that list. Finish
the distribution's initial Linux username/password setup. The game launchers
bootstrap build tools, but do not install WSL or choose an OS release for you.
See [Microsoft's WSL command reference](https://learn.microsoft.com/en-us/windows/wsl/basic-commands).

The launch scripts accept **0.10.4 and 0.10.5**. Using **the same release on all
players and the server** makes troubleshooting easier; the setup that resolved
the earlier handshake failure used **0.10.5 on both computers**. An Ubuntu name
alone does not guarantee a particular DDS release: check it inside that distro.
The scripts reject 0.8.2; they do not upgrade an existing DDS installation to a
chosen release or guarantee compatibility with every future version.

### Start a game

1. On the host computer, double-click **`server.bat`** and keep its window open.
2. Open **`client.bat`** on the host and each player's computer.
3. When prompted for a WSL distribution, enter its installed name, such as `Ubuntu`.
   The launcher remembers the choice.
4. Enter your display name. One player chooses **Create Room** and shares the
   six-digit room code; everyone else chooses **Join a Room**.
5. Every player presses **Ready** or **Enter**. The match starts once at least
   two players are present and everyone is ready.

Display names accept up to 16 letters, digits, spaces, hyphens or underscores.

For local testing, open `client.bat` more than once. Clients automatically choose
different available local credentials. **Only the focused client window plays
audio**, so several local windows do not stack their music.

Players on another computer need a one-time invite from the host; see
[Set up another computer](#set-up-another-computer).

To choose a distro explicitly, run these from the project folder in **PowerShell**:

| Task | Command (replace the example distro name) |
| --- | --- |
| Host the server | `.\server.bat Ubuntu-26.04` |
| Open a client | `.\client.bat Ubuntu-26.04` |
| Create one encrypted invite on the host | `.\create-invite.bat client2 Ubuntu-26.04` |
| Create a complete per-player zip on the host | `.\package-friend.bat client2 Ubuntu-26.04` |

## How a match works

| Stage | Duration | What changes |
| --- | --- | --- |
| Level 1 | 30 seconds | Catch normal coins for **1 point** and gold coins for **3 points**. |
| Level 2 | 30 seconds | Coins fall faster and bombs appear. Players start each level with **3 lives**; each bomb hit removes **1 life** without deducting points. |
| Level 3 | 30 seconds | Red floor warnings precede bomb bursts, with gold coins tempting players into danger zones. Warnings become more frequent near the end; **3 bomb hits** remove **1 life** without deducting points. The current heart shows increasing damage after the first two hits; a lost heart breaks and fades. |
| Tie-breaker | 15 seconds | Tied leaders compete in a coin-only round while other players spectate. Another tie repeats the bonus round among its tied leaders. |

- Scores carry across the three levels. At zero lives, players stop moving and
  collecting coins; their saved scores remain eligible to win. A level ends when
  its timer expires or everyone is out. Level 2 advances to the Level 3 ready
  screen; after Level 3, tied leaders enter a bonus round or the results appear.
  Scores are retained and lives reset to three when the next level begins.
- At each level break, everyone must confirm before the next level begins.
  A green checkmark confirms your vote and the ready count shows the room's progress.
- The final leaderboard ranks players by their total score. Bonus points resolve
  first-place ties separately from that total.
- There is no automatic rematch. Everyone must confirm **Continue to Next Round**;
  a new match resets all scores to zero.
- Duplicate ready clicks count once. Players who leave are removed from the ready count.

## Controls

| Action | Control |
| --- | --- |
| Move left/right | **A / D** or **Left / Right arrows** |
| Confirm readiness | **Enter** or the on-screen Ready/Continue button |
| Join a room | Enter its six-digit code, then press **Enter** or click **Join Room** |
| Edit a name or room code | Hold **Backspace** to erase; **Ctrl+A** selects all; typing replaces the selection; **Backspace/Delete** clears it |
| Toggle fullscreen | **F11** |
| Leave the lobby for the main menu | **Esc** or **Back** below the header, at the top-left of the playfield |
| Return from Join a Room | **Esc** or **Back** |
| Open the exit confirmation | **Escape** during a match |
| Cancel the exit confirmation | **Esc** or **Cancel** |
| Mute/unmute | Click the **speaker icon** |
| Adjust audio volume | Click **- / +** or the volume bars |

Each level and the bonus round have their own music track. Coin pickups and
bomb hits play sound effects. Volume controls use a separate tick, whose level
follows the volume setting. Music is mixed below the effects.

Windows can be resized freely; the arena keeps its 4:3 layout with letterboxing.

## Build and run in WSL/Linux

Run commands from the project root. The build needs a C11 compiler, Cyclone DDS,
GLFW, OpenGL, OpenSSL and SDL2_mixer development packages. The Makefile checks
these dependencies and installs missing packages through `apt` on Debian/Ubuntu,
using `sudo` when needed. Secure
invite transfer also uses GnuPG and `tar`.

For a fresh Debian/Ubuntu installation where `make` or `pkg-config` is missing,
bootstrap them in **WSL/Linux** first:

```sh
sudo apt update
sudo apt install -y build-essential pkgconf
cd /mnt/c/LLP/LLPAssignment
```

Then run:

```sh
make
```

This generates the DDS message code, builds the server and client and creates
local credentials if none exist. Build outputs live in `build/`; runnable copies
are also installed as `./server` and `./client` for the launchers.

Guests using a supplied invite should use `client.bat`, which builds just the
client and imports the host's identity. Running full `make` also generates an
independent local authority when credentials are absent; those new identities
are not trusted by the friend's server.

Start the server, then open clients in separate terminals:

```sh
./server
```

```sh
./client
```

Or start a server and the first client together with `make run`, then use
`make run-player2` in another terminal. The Create/Join menu and ready votes
still apply.

Use `make client` to rebuild/install only the client, or `make server` for only
the server. These targets expect credentials to have already been prepared.
For a full rebuild, close the game processes, run `make clean`, then `make`.
`make clean` removes `build/`, generated root message files, root executables,
objects, core dumps, and temporary executable-install files. It preserves
credentials, imported invites, encrypted invite archives, and `packages/`.
There is no `make clear` target.

## Set up another computer

Each remote player needs a distinct credential signed by the host's authority.
Use the same current source release on all computers. This is a LAN setup;
a room code does not configure firewalls, cross NAT, or locate an Internet server.

### Host: make and send a player's package

1. Run `server.bat` once to build the project and create the host's credentials.
2. Run `package-friend.bat`. Choose an unused identity, for example `client2`.
   `client1` is reserved for the host and is rejected by the invite/package helpers.
3. Enter the same private passphrase twice. **It must have at least 12 characters.**
   The current batch prompt suggests `coinrushgame`, but that is only 11 characters
   and the invite helper rejects it. Choose a longer passphrase; the batch file
   does not set a passphrase automatically or store it in the zip.
4. Wait for `Package created in packages\CoinRush-client2.zip`.
5. Send that zip to the intended player and share its passphrase separately.
   Use `client3`, `client4`, etc. for other players. Keep a record of assignments.
6. Keep `server.bat` open, launch your own `client.bat`, choose **Play - Create
   Room**, and share the new six-digit room code.

For a project stored at `C:\LLP\LLPAssignment`, the example output is
`C:\LLP\LLPAssignment\packages\CoinRush-client2.zip`. A project extracted
elsewhere creates `packages/` beneath that project folder. The encrypted invite
alone is `invites/coinrush-client2.tar.gz.gpg`; it is not the complete game package.

You can also select both identity and distro from **PowerShell**:

```powershell
cd C:\LLP\LLPAssignment
.\package-friend.bat client2 Ubuntu-26.04
Get-ChildItem .\packages\*.zip
```

Substitute your installed distro name. Each room still has only five slots;
the existence of `client6` does not create a sixth slot. There are six signed
client identities in this starter credential set, even though the server has
space for eight rooms. Additional rooms do not provision additional identities.

The package helper copies source, assets, tests, documentation, launch helpers,
and **one encrypted player invite**. It excludes `.git`, generated builds,
executables, and the host/imported credential directories. Do not zip the whole
host folder by hand: it may contain authority and server private keys.

### Friend: extract, launch, and join

1. Extract the zip fully, then open its inner `CoinRush-clientN` folder.
2. Set up WSL 2/WSLg and a distro with a supported DDS release as described above.
3. Double-click `client.bat`. Choose that distro if asked; allow dependency
   installation and enter your own Linux `sudo` password if prompted.
4. On the first launch, enter the **invite passphrase** from the host. This is
   separate from your Linux password. The helper verifies and imports the invite.
5. Enter a display name, select **Join a Room**, and enter the host's current code.
6. Click **Ready** or press **Enter**. Everyone in the room must do this.
7. On later launches, run `client.bat` again; the imported identity is remembered
   in that WSL user account/distribution. Switching distro requires importing
   the invite in the new distro too.

The host and guests also need LAN discovery configured; see the next section.
For a local two-window game on the host, start the server, then open two clients.
Keep any identity assigned to a remote friend free on the host.

### Invite-only alternative

The manual per-player flow is:

1. Start `server.bat` on the host to complete its initial setup.
2. On the host, run **`create-invite.bat`** and select an unused identity from
   `client2` through `client6`. Choose a passphrase when prompted.
   Use a passphrase of at least 12 characters. The WSL equivalent is
   `make invite PLAYER=client2`.
3. Put that player's encrypted `.gpg` invite in the guest's project folder under
   `invites/`. Share its passphrase separately.
4. The guest runs **`client.bat`** and enters the passphrase. The launcher verifies
   and imports the credential, then opens the Create/Join menu. Later launches
   only need `client.bat`.

The import stores credentials in private WSL user storage. Share only the
player's encrypted invite, **never** the host's `credentials-private/` folder,
server identity or authority private key. Credential identities authenticate
connections; display names do not grant access to another player's session.

Imported credentials normally live in
`~/.local/share/coinrush/credentials-invites/clientN/`, with a `default-player`
file beside the identity directories. If `XDG_DATA_HOME` is set, its directory
replaces `~/.local/share`. Legacy project-local `credentials-invites/` is also
recognized. These stores belong to a particular Linux user and distro.

### LAN discovery on Windows

Configure this on the host and every guest. Mirrored networking requires Windows
11 22H2 or later and a current WSL installation.

**1. Enable mirrored networking.** Merge this into `%UserProfile%\.wslconfig`:

```ini
[wsl2]
networkingMode=mirrored
```

Close the game, run `wsl --shutdown` in PowerShell and reopen WSL. This also stops
other WSL applications. `wslinfo --networking-mode` should report `mirrored`.

**2. Identify the game LAN.** Open **PowerShell as administrator**, from any
folder:

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Select-Object InterfaceAlias, IPAddress, PrefixLength

$lanSubnet = (Read-Host 'Trusted game LAN subnet in CIDR notation').Trim()
```

Use the network address and prefix for the physical Wi-Fi or Ethernet interface,
not a Docker, VPN or WSL NAT interface. Use the actual prefix length rather than
assuming `/24`. Computers on the same subnet use the same CIDR.

**3. Create both rules.** Run once per PC. If these names already exist, use
step 4 instead.

```powershell
New-NetFirewallRule -Name 'CoinRush-LAN-UDP' -DisplayName 'Coin Rush LAN' `
  -Direction Inbound -Action Allow -Profile Any `
  -Protocol UDP -LocalPort 17900-17941 -RemoteAddress $lanSubnet

New-NetFirewallHyperVRule -Name 'CoinRush-WSL-UDP' -DisplayName 'Coin Rush WSL' `
  -Direction Inbound -Action Allow `
  -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' `
  -Protocol UDP -LocalPorts 17900-17941 -RemoteAddresses $lanSubnet
```

The two rules cover Windows Firewall and WSL's Hyper-V firewall. The UDP range
covers DDS domain **42** with participant indices **0-15**. Both address filters
are restricted to the supplied LAN.

**4. Update existing rules after changing networks.** Set `$lanSubnet` from step
2 in the same administrator PowerShell session, then run:

```powershell
Get-NetFirewallRule -Name 'CoinRush-LAN-UDP' |
  Get-NetFirewallAddressFilter |
  Set-NetFirewallAddressFilter -RemoteAddress $lanSubnet

Set-NetFirewallHyperVRule -Name 'CoinRush-WSL-UDP' `
  -RemoteAddresses $lanSubnet -Enabled True
```

**5. Verify the effective settings.**

```powershell
Get-NetFirewallRule -Name 'CoinRush-LAN-UDP' |
  Get-NetFirewallAddressFilter | Select-Object RemoteAddress
Get-NetFirewallRule -Name 'CoinRush-LAN-UDP' |
  Get-NetFirewallPortFilter | Select-Object Protocol, LocalPort
Get-NetFirewallHyperVRule -PolicyStore ActiveStore -Name 'CoinRush-WSL-UDP' |
  Format-List Enabled, Action, Protocol, LocalPorts, RemoteAddresses, EnforcementStatus
```

Start the server and clients, create a room and join with its code. Guests still
need distinct credentials issued by that host. Changing the network rules does
not require resending the packages.

References: [WSL networking](https://learn.microsoft.com/en-us/windows/wsl/networking)
and [Hyper-V firewall](https://learn.microsoft.com/en-us/windows/security/operating-system-security/network-security/windows-firewall/hyper-v-firewall).


## Testing

Build first, then run the checks from the project root:

```sh
make test            # Gameplay, client input/audio and network validation
make qa              # Full integration, security, room isolation and a real match
make test-sanitize   # AddressSanitizer and UndefinedBehaviorSanitizer checks
```

QA evidence is written to `build/qa/`. Client audio tests use a dummy audio device
so automated tests do not depend on speakers or an active game window.
`make profile` provides server-polling measurements for the local machine.

The implementation uses SSE2 collision comparisons on supported x86 targets,
with a scalar fallback and four-way loop unrolling in coin updates. Run
`make profile` on the target machine for performance measurements.

## Implementation

The server owns movement, coin pickups, bomb penalties, timers, ready votes and
results. Clients send input; the server updates positions and scores.
DDS connections use signed player identities, encrypted private channels,
session tokens, sequence checks and room isolation. The server validates
incoming names and input independently of the UI.

## Project layout

```text
assets/       Graphics, music and sound effects
include/      Shared game and networking headers
protocol/     DDS message definitions
scripts/      WSL launchers and credential helpers
src/          Client, server, game rules and networking
tests/        Gameplay, client, network and QA tests
vendor/       Third-party pixel font
docs/         README screenshots
build/        Generated code, executables, test data and logs (Git-ignored)
```

`Makefile`, `server.bat`, `client.bat`, `create-invite.bat`, and
`package-friend.bat` remain at the host project's root.
The shared Windows launcher is **`scripts/run-game.bat`**; it calls
**`scripts/run-game-wsl.sh`** inside the selected WSL distribution.

The third-party font is in `vendor/stb_easy_font.h`. Bundled effect asset notes
and license files are retained in `assets/effects/source/`.

## FAQ and troubleshooting

These entries cover the launch, build, sharing, network, and rendering problems
encountered during development. Apply the fix for the current error first;
different errors can have different causes.

[Launchers/WSL](#launchers-and-wsl) ·
[Build errors](#building-and-dependencies) ·
[Packages/certificates](#packaging-and-certificates) ·
[Network/handshake](#connecting-across-computers) ·
[Graphics/audio/controls](#graphics-input-audio-and-game-flow) ·
[Diagnostic checklist](#if-a-launch-problem-remains)

### Launchers and WSL

**1. What does “Enter a distro with Cyclone DDS…” mean?**

A distro is an installed Linux environment inside WSL, such as `Ubuntu`,
`Ubuntu-24.04`, or `Ubuntu-26.04`. They have separate packages and user homes.
In PowerShell, run `wsl --list --verbose`, then check DDS in the distro you plan
to use. Enter its exact name without quotes at the batch prompt. The remembered
choice is `%LOCALAPPDATA%\CoinRush\wsl-distro.txt`. You only need one suitable
distro to play; you do not need to launch all the installed distributions.

**2. “There is no distribution with the supplied name” / `WSL_E_DISTRO_NOT_FOUND`.**

`wsl --list --online` lists installable distros; `wsl --list --verbose` lists
the ones actually installed. Install/initialize a missing distro first. If the
name is already installed, use the latest launcher and pass its name explicitly:

```powershell
cd C:\LLP\LLPAssignment
wsl --list --verbose
wsl -d Ubuntu-26.04 -- bash -lc "pkg-config --modversion CycloneDDS"
.\server.bat Ubuntu-26.04
# In another PowerShell window:
.\client.bat Ubuntu-26.04
```

If the direct `wsl -d ...` command succeeds but the batch launch fails, check
that you are running the updated batch files from this extracted folder. The
earlier launcher had a distro-argument problem; reinstalling the already working
distro is unnecessary. You can bypass the Windows wrapper from the project root:

```powershell
wsl --distribution Ubuntu-26.04 --exec bash scripts/run-game-wsl.sh server
# Use 'client' instead of 'server' for the client.
```

If the saved choice is wrong, explicitly pass the right distro as above; the
launcher saves that choice. Do not include the `*` from the WSL listing.

**3. The batch window closes immediately or says “Server process has stopped.”**

Run it from an existing PowerShell window to retain the error output:

```powershell
cd C:\LLP\LLPAssignment
.\server.bat Ubuntu-26.04
```

The server should stay running with `Coin Rush room server ready ...`; it does
not open a game window. Start `client.bat` separately for graphics. A stopped
server means an exit or startup failure: read the first error above that message,
then use the matching FAQ entry. The current server wrapper pauses on exit and
the client wrapper pauses on failure. Keep the host's server window open while
players use its rooms.

**4. Can I double-click `client` or `server` in Windows Explorer?**

Those are Linux executables. Use `client.bat`/`server.bat` in Windows, or
`./client`/`./server` inside WSL from the project root. Extracting the zip does
not create a native Windows `.exe`. The Windows batch launchers require WSL;
other operating systems need their own compatible build/dependencies.

### Building and dependencies

**5. `make`, `pkg-config`, or `idlc` is not found; DDS/GLFW/OpenSSL headers are missing.**

Install tools in the **same WSL distro that runs the game**. Installing them in
another Ubuntu distro does not fix this one. For a manual Debian/Ubuntu setup:

```sh
sudo apt update
sudo apt install -y build-essential pkgconf cyclonedds-dev cyclonedds-tools \
  libglfw3-dev libgl1-mesa-dev libssl-dev libsdl2-mixer-dev gnupg
pkg-config --modversion CycloneDDS
make
```

Check the reported DDS version before attempting multiplayer. `apt` installs
the release available in that distro's repositories; it is not a command to
force 0.10.5. The launchers bootstrap `make`/`pkg-config` and the Makefile installs
remaining supported dependencies. A manual `make` cannot install itself.

| Error | Required package or action |
| --- | --- |
| `idlc: command not found` | `cyclonedds-tools` |
| `pkg-config: command not found` | `pkgconf` (or the distro's `pkg-config` package) |
| `dds/dds.h: No such file` | `cyclonedds-dev` |
| `GLFW/glfw3.h: No such file` | `libglfw3-dev` |
| `GL/gl.h: No such file` | `libgl1-mesa-dev` |
| `openssl/pem.h: No such file` | `libssl-dev` |
| SDL or mixer header/library missing | `libsdl2-mixer-dev` and the current Makefile |
| `gpg: command not found` | `gnupg` |

Linux password prompts do not show characters while typing. Use that Linux
user's password; the invite passphrase is a different prompt.

**6. `JoinResponse`/`Input`/`GameState` has no member `client_id`, `session_id`, or `seq`.**

The generated DDS messages and source are out of sync, or an old header in the
project root is shadowing the generated header. Close running game processes,
then run this in WSL from the project root:

```sh
make clean
make
```

The current schema is `protocol/messages.idl`; `make` generates
`build/messages.c` and `build/messages.h`. Use the current source, IDL, Makefile,
and scripts together on all machines. Do not copy generated files from an old
zip or use the old root-level `gcc client.c messages.c ...` commands. Those
commands predate the `src/`, `include/`, and `protocol/` layout.

**7. `JoinResponse__alloc()` gives “expected '}' before ';'”, or `dds_free` has too many arguments.**

These came from an older client and Cyclone DDS 0.8.2's generated C API. Update
to the current project and rebuild using a supported DDS release. For maintaining
that older source, allocation macros containing a trailing semicolon must be
assigned in a statement after array initialization, rather than embedded in
`{ ... }`. Plain `dds_free` takes one argument; generated type cleanup functions
such as `JoinResponse_free(sample, DDS_FREE_ALL)` accept the cleanup flag. Do not
edit generated headers to conceal the version/API mismatch.

**8. `SDL_INIT_AUDIO` is undeclared or `SDL_Init` is implicitly declared.**

That older client was missing the SDL declaration. The current `src/client.c`
includes `<SDL2/SDL.h>` and `<SDL2/SDL_mixer.h>` and the Makefile supplies the
SDL compiler/linker flags. Update these files together, install
`libsdl2-mixer-dev`, and rebuild. Installing mixer alone cannot fix a missing
`#include` in an old source file.

**9. There are `stb_easy_font` warnings. Did compilation fail?**

The previously seen missing-braces and unused-static-function warnings originate
in the bundled font header and do not, by themselves, prevent the executable
being built. Look for `error:` or a nonzero Make exit. An IDL “No default
extensibility provided” warning also differs from a failed code-generation step.
Do not assume every warning explains a black window or a connection failure.

**10. `./server: No such file or directory`, or the executable still behaves like an old version.**

Run from the project root containing `Makefile` and `assets/`. Run `make` for a
host, or the batch launcher for your role. Successful builds install root copies
as `./server` and `./client`; `build/server` and `build/client` are also generated.
Older builds only produced the `build/` copies. Restart every running server and
client after rebuilding: rebuilding a file does not replace a running process.

**11. Is it `make clear` or `make clean`? Does cleaning erase invites or packages?**

Use `make clean`, then `make` to rebuild a host installation. It removes generated
build files/executables and preserves keys, imported identities, invites and
packages. It does not uninstall Linux packages or update old zip files. Guests
can use `make clean`, then `client.bat` to rebuild/import using the guest path.

**12. “Could not resolve libddsc.so.0” / “Could not resolve Cyclone DDS” / unexpected library path.**

Use the latest `scripts/run-game-wsl.sh`; the old check assumed a specific
`.so.0` name. The current check accepts versioned `libddsc.so*` names and verifies
the resolved path against `pkg-config`. In the selected WSL distro, inspect:

```sh
pkg-config --modversion CycloneDDS
pkg-config --variable=libdir CycloneDDS
ldd ./server | grep -E 'libdds|not found'
ldd ./client | grep -E 'libdds|not found'
printenv LD_LIBRARY_PATH
```

On a guest without `server`, only inspect `client`. Missing libraries require a
working DDS installation. An unexpected path can mean a custom DDS installation
or `LD_LIBRARY_PATH` overrides the distro libraries. Align the compiler headers,
runtime library and DDS security plugins, then clean/rebuild inside that distro.
Do not rename libraries or fake symlinks to pass the check.

### Packaging and certificates

**13. Where is the package? What exactly do I send my friend?**

Send `packages/CoinRush-clientN.zip` created on the host, plus its passphrase
separately. For the usual host path and `client2`, that is
`C:\LLP\LLPAssignment\packages\CoinRush-client2.zip`. In PowerShell:

```powershell
Get-ChildItem .\packages\*.zip
```

If `packages/` is absent, creation has not succeeded. `create-invite.bat` makes
an encrypted invite, while `package-friend.bat` creates the complete game zip.
An `invites/` file is not the final zip. Wait for the explicit success message.

**14. Why is `client1` rejected? Can everyone use `client2`?**

The package helpers accept `client2` through `client6`; `client1` is reserved
for the host. Give each player a different identity issued by this host. Copying
one invite to multiple devices shares its private key and can cause session
conflicts. Player numbers shown in a room are assigned by the server on joining;
the certificate name does not force a particular in-room player number.

**15. Why does the suggested `coinrushgame` passphrase fail?**

The existing batch prompt suggests an 11-character value, but
`scripts/make-invite.sh` requires **12 or more characters**, entered identically
twice. Choose a longer private passphrase at the prompt. This is a prompt/helper
mismatch in the current scripts, not a DDS or network error. No passphrase is
automatically applied by the packaging batch file.

**16. “An invite already exists” / how do I recreate a missing or outdated zip?**

Do not allocate the same identity to a second person. If the intended friend's
encrypted invite already exists and is still valid for this host, recreate the
zip from that existing invite in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-friend.ps1 -Player client2
```

This replaces `packages/CoinRush-client2.zip` using the current source/assets and
the existing invite; its existing passphrase is unchanged. If a replacement
invite is needed, archive the old `invites/coinrush-client2.tar.gz.gpg` outside
`invites/` first, then rerun the package helper for that same intended player.
Re-exporting a credential does not revoke earlier copies of that credential.
If an identity is in use locally, close that client before exporting it.

**17. `join-invite.sh` prints Usage, or no invite is found.**

Current helper scripts are in `scripts/`. From the extracted project root,
confirm the actual filename, then import it in WSL:

```sh
ls invites/
bash scripts/join-invite.sh invites/coinrush-client2.tar.gz.gpg
```

Replace the filename with the one you received, including its `.tar.gz.gpg`
extension. A nonexistent path prints Usage. Normally `client.bat` imports the
single invite under `invites/` automatically. Keep only this player's invite
there; if several are present, move the unrelated ones elsewhere.

**18. The imported identity is missing, incomplete, already in use, or belongs to an old host.**

Check you selected the same WSL distro and Linux user used for the import.
Imported credentials normally reside in
`~/.local/share/coinrush/credentials-invites/`, not alongside the Windows zip.
Close any other client using that identity. To replace an invite for the same
identity, stop the client, privately back up that identity directory outside
the credential store, and import the correct new invite. Also check for a
legacy project-local `credentials-invites/` store. The importer refuses to
overwrite an existing identity directory.

The `default-player` setting selects the imported identity on later launches.
If joining a different host, obtain that host's invite and import it; independently
generated local credentials will not authenticate to that server. `make clean`
deliberately preserves the credential store, so cleaning alone cannot replace
a wrong invite.

**19. “Secure DDS connection failed” or certificate/permissions verification fails.**

Use a fresh invite issued by the running host and verify both computers' clocks.
The client needs `identity.pem`, `identity.key`, `ca.pem`, `permissions.p7s`, and
`governance.p7s` together. Check the first DDS security error and that the matching
security plugins are installed. Certificates are time limited: the generated
certificate lifetime is 365 days and the permission grant expires after about
364 days. A room code cannot repair expired or unrelated credentials.

You can compare the CA SHA-256 fingerprints without revealing private keys.
On the host, in WSL:

```sh
openssl x509 -in credentials-private/server/ca.pem -noout -fingerprint -sha256
```

On a guest using the default credential storage and `client2`:

```sh
openssl x509 -in ~/.local/share/coinrush/credentials-invites/client2/ca.pem -noout -fingerprint -sha256
```

The fingerprints must match. Adjust the guest path for `XDG_DATA_HOME`, its
assigned identity, or legacy storage. If the host intentionally replaces its
authority, all guests need newly issued invites. Never send `authority/ca.key`,
the server private key, or the complete `credentials-private/` directory.

### Connecting across computers

**20. We can ping each other but cannot join the room.**

Ping success does not confirm the game's UDP discovery, encryption, or admission.
Check these in order:

1. Keep one host server running. The host's client must create a room first;
   share its current six-digit code. Guests choose **Join a Room**.
2. Use the same current project source/protocol and preferably the same supported
   DDS release. Check the version in the distro actually launching each process.
3. Give each guest a different invite from this host. Check the CA fingerprints
   if unsure. The server log should show another player joining that room.
4. Configure mirrored WSL networking and both scoped UDP firewall rules
   described in [LAN discovery](#lan-discovery-on-windows) on every computer.
   Recheck the address filters after changing networks.
5. Check guest Wi-Fi/client isolation, VPN adapters/routes, multicast restrictions,
   and institutional network policy. Use an approved network allowing peer traffic.

Do not start a separate guest server to solve discovery. That creates another
server/room environment. The room code is a room identifier, not an IP address,
certificate, password, or mechanism for bypassing network restrictions.

**21. `ddsi_udp_conn_write ... failed with retcode -1` to `239.255.0.1` or a LAN IP.**

This reports a DDS UDP send failure, so investigate routing/interface/firewall
configuration before changing room codes. In WSL, inspect:

```sh
ip -brief address
ip route
printenv CYCLONEDDS_URI
```

The multicast address and the machine's current LAN addresses must be reachable
through the selected network. Check for a stale peer IP or interface in any
custom DDS configuration, especially after switching Wi-Fi or VPNs. If you
previously exported a custom `CYCLONEDDS_URI` only for experimentation, remove
that override in the launching shell with `unset CYCLONEDDS_URI` and retry using
the game's default configuration. Keep an intentional administrator-provided
configuration when the network requires it. Recheck mirrored mode and outbound
as well as inbound policy; opening an inbound port cannot fix a missing route.

**22. “Begin handshake reply failed: Deserialize PID 0x59 ... internal_size 4 != external_size 8”.**

The earlier case involved DDS **0.8.2** on one computer and **0.10.4** on the
other. That is a version/security interoperability clue, not proof of a bad room
code. The connection worked after both computers used **0.10.5** and rebuilt.
The current launcher allows 0.10.4/0.10.5, but choosing one common release is the
first troubleshooting step. In each selected WSL distro:

```sh
pkg-config --modversion CycloneDDS
ldd ./client | grep libdds
# Host only:
ldd ./server | grep libdds
```

`pkg-config` reports the build metadata; `ldd` shows the actual core library path.
The `.so` suffix alone is not the package release. Ensure the authentication,
access-control and crypto plugins come from the same DDS installation, then
close all old clients/servers and rebuild through the launchers. Old processes,
mixed custom/system libraries, stale credentials, and unrelated DDS participants
in domain 42 can still complicate a handshake even when the printed versions match.
Keep authentication enabled while investigating the specific security error.

**23. My friend updated packages, but DDS is still 0.10.4 or 0.8.2.**

An update uses that distro's package repositories; it does not guarantee 0.10.5.
Check `wsl --list --verbose` in PowerShell and query DDS explicitly inside each
candidate distro. Select a suitable installed distro with the batch argument,
or install/prepare one that provides the release shared by the host. Rebuild
there and re-import the invite for that Linux user/distro. Do not assume the
default `Ubuntu` distro changed because another distro was installed.

**24. “Joining room” never finishes / “No server reply” / “Room unavailable, full, or server not running”.**

The UI times out after roughly 12 seconds without admission. Confirm the host
server remains running, the room code is current, and there is room for another
player. Restarted servers need new rooms/codes. A room can hold at most five
players; an occupied identity can also conflict even if another slot is free.
If the server only logs the host joining, apply the network and handshake checks
above. If the server logs your friend's admission but the UI still waits, inspect
the return network path, matching protocol/build, identity, and client-side errors.
“Using imported player identity client2” only confirms local selection, not that
the server accepted the connection.

### Graphics, input, audio, and game flow

**25. The game window is completely black; where are the coins and players?**

Update the client/source/assets together and rebuild. Run from the extracted
project root, and check the terminal for GLFW/OpenGL or missing-asset errors.
The current client uses a compatibility OpenGL context and automatically enables
Mesa software rendering under WSLg to work around the earlier D3D12 black-window
problem. The message `WSLg detected; using Mesa software OpenGL (2 worker threads)`
is expected. To try that renderer explicitly from WSL after building:

```sh
LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2 ./client
```

If GLFW cannot initialize or create a window, repair/update WSLg first; in
PowerShell use `wsl --update`, then restart WSL after saving other WSL work.
Check `DISPLAY` and `WAYLAND_DISPLAY` inside WSL rather than blindly overwriting
them with an old external X-server configuration.

The arena/lobby/menu should render before a round starts. **Coins only fall
during active gameplay**, after at least two players join and everyone is Ready.
Missing falling coins during the lobby is expected; a completely black UI is not.

**26. The graphics lag, stutter, buffer, or flicker.**

Use the latest client and restart old windows. The WSLg fallback limits Mesa
to two worker threads and uses a longer swap interval; the client also uses
double buffering and renders separately from the network worker. Close extra
test clients and compare one active game window before blaming network traffic.
Software rendering trades GPU use for compatibility and can still be slower
on some devices. If input/state freezes while menus animate smoothly, investigate
network loss/discovery separately. Check WSLg/graphics updates and collect the
renderer/startup output if the current build still flickers.

**27. Cats are upside down, assets are missing, or the UI still looks old.**

Use the current renderer and bundled `assets/` from the same release, rebuild,
then close/reopen all clients. The source assets have already been converted to
the expected `.rgba` layout; original files in the developer's Downloads folder
are not needed on a friend's computer. Copy the complete extracted package, not
only `client` or isolated PNG/spritesheet files. Recreate packages after updates;
existing zip archives are not automatically refreshed by `make`.

**28. Why does it stay in the lobby or on “Continue”? How do I leave?**

At least two players and **all active players' Ready votes** are needed to start.
Click Ready/Continue or press Enter in each client. The green check/count confirms
a vote. Level breaks and the result screen also need everyone's confirmation;
they do not advance on a timer. In the lobby, click **Back** below the header
on the left or press **Esc** to return to the main menu. Esc during active play
opens the exit confirmation instead. Movement affects the focused client window.

**29. Why are bombs/hearts absent in Level 1? Why did a cat stop moving?**

Level 1 is coin-only. Level 2 introduces faster coins, bombs and three lives;
Level 3 adds the warning/burst mechanic and three bomb hits per lost life.
An eliminated cat cannot move/collect until the next level resets lives, while
its accumulated score remains. During bonus tie-breaks, only tied leaders play;
the other players spectate. See the rules table above for the full progression.

**30. There is no sound, or several local clients used to play music at once.**

Focus the client window, unmute its speaker icon, raise its volume bars, and
check Windows/WSLg audio output. Only the focused client plays audio. Music
plays during active levels/bonus rounds, so a silent menu/lobby is expected.
Keep bundled `assets/music/` and `assets/sounds/` and install SDL2_mixer. The
terminal reports audio initialization/loading failures; a missing audio device
does not explain a DDS handshake failure.

**31. How is text drawn, and are SIMD/loop unrolling implemented?**

`vendor/stb_easy_font.h` turns text into quad vertices; the client draws those
quads with OpenGL and measures/positions text for centering. It does not require
a system font installation. `src/game.c` uses SSE2 to compare four player
collision rectangles on supported x86 builds, a scalar fallback elsewhere, and
four-way coin-update loop unrolling. These are source-level optimizations;
performance gains depend on the workload/device and should be measured using
`make profile` rather than assumed.

### If a launch problem remains

Capture the **first error**, the host and guest terminal output around the join,
and these diagnostics. In **PowerShell**:

```powershell
wsl --list --verbose
```

In the **WSL distro used by the failing launcher**, from the extracted project root:

```sh
pwd
cat /etc/os-release
pkg-config --modversion CycloneDDS
pkg-config --variable=libdir CycloneDDS
ldd ./client | grep -E 'libdds|not found'
ip -brief address
ip route
date -u
```

On the host, also capture `ldd ./server | grep -E 'libdds|not found'`. Say which
batch file/distro you used, whether this is local or across devices, which
identity was assigned, and whether the host logged that player's admission.
Share errors and CA fingerprints when needed, not private keys or passphrases.
