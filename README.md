# Coin Rush

Coin Rush is a small real-time multiplayer game in C using GLFW, fixed-function
OpenGL and Cyclone DDS. The server owns the simulation. Clients use room codes
to create or join isolated matches.

## Build in WSL

From the project directory, build the game with:

```sh
make
```

On Debian/Ubuntu/WSL, the Makefile checks for the compiler, Cyclone DDS, GLFW,
OpenGL, OpenSSL and SDL2_mixer development packages. If any are missing, it
asks `sudo` to install them with `apt`. It then generates the DDS message code,
builds the server and client, creates local credentials on the first build, and copies
the executables to the project root as `./server` and `./client` (the originals
remain in `build/`).

For a clean rebuild:

```sh
make clean && make
```

### Low-level optimizations

The server uses explicit SSE2 integer comparisons to check up to four player
rectangles against each falling coin or bomb at once on x86/x86-64. It keeps a
scalar tail and a scalar-only fallback on other CPU architectures. The fixed
coin update and gold-count loops are manually unrolled four items at a time.
These optimizations preserve player-ID collision priority and coin respawn
order. The game has small fixed limits (five players and eight coins), so the
SIMD path demonstrates vector processing but should be benchmarked before
claiming a measurable frame-rate or CPU-time improvement.

### Windows WSL launchers

Double-click `server.bat` on the host PC and `client.bat` on each player PC.
The first launch asks for a WSL distro with Cyclone DDS 0.10.5 and remembers
that choice for that Windows account. Later launches only need the relevant
batch file. The server window stays open while the host is running. The client
launcher builds the client, securely imports that PC's invite on first launch,
and connects automatically to quick-play room `100000`. All clients use that
reserved room; the server creates it when the first client arrives. The normal
room-code UI remains available when starting `./client` directly.

The first build verifies Cyclone DDS 0.10.5 and the linked runtime library, and
rebuilds when the Linux distro or DDS release changes. Each PC still needs its
own local WSL build and working WSLg graphics. For LAN discovery, follow the
mirrored networking and firewall setup below.

To launch a local demo with a server and first client in one terminal:

```sh
make run
```

## Start a match

After `make`, start `./server` in the first WSL terminal. In a second terminal,
run `./client` and choose **Create Room**. In each additional terminal, run
`./client`; the first client to create a room hosts it, and the others choose
**Join a Room** and enter its six-digit code. For a one-command local demo,
`make run` starts the server and first client together.

With no credential argument, each `./client` process takes the first locally
available signed identity in launch order. An OS file lock prevents two local
clients from selecting the same identity, and closing a client releases it.

#### Secure one-time player setup

Each remote player needs a distinct certificate signed by the host's local
authority. On the host, after the first `server.bat` setup, double-click
`create-invite.bat` and choose `client2` through `client6`. The script creates
an AES-encrypted invite and asks for a passphrase. Alternatively, from WSL:

```sh
make invite PLAYER=client2
```

Put the encrypted `.gpg` file in the friend's project folder under `invites/`
(or send it separately and have them place it there). Give the passphrase over
a separate channel. Make a different invite for every player. The friend
double-clicks `client.bat`; the first launch verifies/imports the invite and
asks for its passphrase, then automatically joins quick-play. Later launches
only need `client.bat`.

The invite contains only that player's encrypted credential. It is imported
into the player's private WSL home data directory with restricted file
permissions, outside the shared project folder. Never send the plaintext
`credentials-private/` directory, the server identity, or
`credentials-private/authority/ca.key`.
The encrypted invite can be included in a project zip; its passphrase must be
shared separately. Each invite is valid only for its matching host authority.

### Connect between two WSL computers on a LAN

WSL 2 normally uses NAT, which can prevent Cyclone DDS multicast discovery
from reaching another computer even when both Windows PCs can ping each other.
On **both** PCs, use Windows 11 22H2 or later and create or edit
`%UserProfile%\.wslconfig`. Add this under an existing `[wsl2]` section, or
create that section if the file is new:

```ini
[wsl2]
networkingMode=mirrored
```

Then run `wsl --shutdown` in PowerShell on each PC and reopen Ubuntu. Start the
host with `server.bat` and each player with `client.bat`. The program sets Cyclone DDS to use
automatic participant indices (0–15), allowing a local server and clients to
coexist and making its discovery port range predictable. The normal
certificate authentication, signed permissions and encrypted DDS traffic
remain enabled.

If the connection still times out, allow the game's UDP discovery ports in
the Hyper-V/WSL firewall on both PCs. In an administrator PowerShell window,
replace `192.168.1.0/24` with your trusted LAN subnet and run:

```powershell
New-NetFirewallHyperVRule -Name "CoinRushDDS" -DisplayName "Coin Rush DDS" `
  -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' `
  -Protocol UDP -LocalPorts 17900-17941 -RemoteAddresses 192.168.1.0/24 `
  -Action Allow
```

This rule only permits inbound UDP on the game's configured discovery range
from the specified LAN subnet. The friend should also confirm that their
terminal prints `Using imported player identity client2` before starting
`./client`.

The server supports up to eight rooms with up to five players in each room.
At least two players start a round. Close a client window to leave its room.
Clients must be able to discover the same Cyclone DDS server on the network.

The automatic identity choice is local convenience only; DDS certificate
authentication, signed topic permissions, session tokens and sequence checks
continue to protect network play.

## Controls

- Use the left/right arrow keys or A/D to move.
- Press F11 to enter or leave fullscreen; Escape still opens the in-game exit menu.
- Resize the client window freely; the arena keeps its 4:3 layout and adds
  letterboxing rather than stretching or cropping the game.
- In the room-code screen, type six digits, press Enter to join, Backspace to
  edit, or Escape to return to the main menu.
- Use **Exit Game** in the main menu, or press Escape during a match, to open
  the exit confirmation. Confirming closes the client and removes that player
  from the room; cancel returns to the previous screen.
- At the Level 1 intermission, click **Continue to Level 2** or press Enter;
  the server advances the whole room together.
- Level 1 and Level 2 play their respective bundled MP3 tracks. In the game
  HUD's upper-right corner, click **SND/OFF** to mute or unmute and use **−/+**
  to adjust the music volume.

## Graphics and game screens

- The main menu offers room creation and room-code joining.
- Two menu cats use randomized idle, walk and jump animations from
  `assets/menu/tofu_idle.png`, `cato_walk.png` and `cato_jump.png`. Each client
  gets separate starting positions and animation timing. They bounce inside
  bounded side lanes so they stay on screen and clear of the menu buttons.
- The lobby waits for the second player and displays the room code to share.
- The winner screen shows the winner, top score and results countdown.
- Players are rendered with the cat rotation sprites in `assets/cats/idle`;
  each player has a separate tint and faces left or right as they move.
- The HUD, falling coins, scores, timer and player positions use the latest
  authoritative room snapshot from the server.
- Each round has two 30-second levels. Level 1 is the original coin-only mode.
  It ends at a shared **Level 1 Complete** screen; any player can continue the
  room to Level 2. Level 2 increases coin speed and introduces falling bombs
  and three heart lives per player. Bomb hits cost one life, and eliminated
  players stop collecting coins. The winner screen appears when Level 2 ends.
  Hearts, bomb animation and coin pickup plus effects use the sprites in
  `assets/effects/`.
- Players can pass through one another. Coins are clipped to the playfield below
  the HUD, including after the window is resized.
- On WSLg, the client uses Mesa software OpenGL with two worker threads and
  synchronized frame swaps to avoid the Intel Arc D3D12 black-frame issue.

Secure channels use separate certificate identities; each identity may hold
1 active player slot across all rooms and attempt at most 5 joins per second.
Give each remote player a distinct credential directory.

After updating from shared DDS topics, generate a new credential set in a new
folder with `bash security.sh credentials-private`; old signed permissions cannot
access the private channels. Start the server with
`./build/server credentials-private/server` and each remote client with its
corresponding credentials directory. Keep the authority private.
