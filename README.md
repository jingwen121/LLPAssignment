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
The first launch asks for a WSL distro with Cyclone DDS 0.10.4 or 0.10.5 and remembers
that choice for that Windows account. Later launches only need the relevant
batch file. The server window stays open while the host is running. The client
launcher builds the client, securely imports that PC's invite on first launch,
and opens the Create Room / Join Room menu. One player creates a room and shares
its six-digit code; others join with that code. Every connected player presses
Ready before the first match starts, with at least two players required.

The first build verifies Cyclone DDS 0.10.4 or 0.10.5 and the linked runtime library, and
rebuilds when the Linux distro or DDS release changes. Each PC still needs its
own local WSL build and working WSLg graphics. For LAN discovery, follow the
mirrored networking and firewall setup below.

To launch a local demo with a server and first client in one terminal:

```sh
make run
```

## Start a match

Enter a display name in the menu before creating or joining a room. Names can
contain up to 16 letters, digits, spaces, hyphens or underscores. Names appear
above players and in the leaderboard; certificate identities remain private
connection identities and cannot be replaced by choosing someone else's name.

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
asks for its passphrase, then opens Create Room / Join Room. Later launches
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
At least two players and unanimous ready votes are required to start a round. Close a client window to leave its room.
Clients must be able to discover the same Cyclone DDS server on the network.

The automatic identity choice is local convenience only; DDS certificate
authentication, signed topic permissions, session tokens and sequence checks
continue to protect network play.

## Controls

- In name and room-code fields, hold Backspace to delete repeatedly, or use
  Ctrl+A to select all and type to replace it (Backspace/Delete clears the selection).

- Use the left/right arrow keys or A/D to move.
- Press F11 to enter or leave fullscreen; Escape still opens the in-game exit menu.
- Resize the client window freely; the arena keeps its 4:3 layout and adds
  letterboxing rather than stretching or cropping the game.
- In the room-code screen, type six digits, press Enter to join, Backspace to
  edit, or Escape to return to the main menu.
- Use **Exit Game** in the main menu, or press Escape during a match, to open
  the exit confirmation. Confirming closes the client and removes that player
  from the room; cancel returns to the previous screen.
- At each level intermission, click **Continue to the next level** or press Enter;
  your green checkmark and ready count confirm your vote. Every connected player
  must vote before the room advances.
- Each level and the bonus round play their matching bundled MP3 track. Coin
  pickups and bomb hits play retro sound effects. In the game
  HUD's upper-right corner, click the **speaker icon** to mute or unmute and use **−/+**
  to adjust music and sound-effect volume. You can also click the volume bars.
  Music is mixed below the effects. Only the focused client window plays audio,
  so multiple local players do not stack their music.

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
- Each match has three 30-second levels, with shared Continue screens between them.
  Click Continue or press Enter to vote ready. A green checkmark confirms your
  vote and the ready count shows progress (for example, 1/3). Every connected
  player must confirm; duplicate clicks count once. Departed players are removed
  from the count, and votes reset for each new level or match.
  Level 1 introduces coins. Level 2 adds faster coins and bombs. Bombs deduct
  3 points (minimum zero), with a 900 ms hit cooldown. Nobody is eliminated.
  Level 3 adds red floor warnings, tempting gold coins and bomb bursts after
  1.5 seconds. Warnings become more frequent in the final 10 seconds.
- Scores carry across all three levels. A first-place tie starts a 15-second
  coin-only bonus round for tied leaders; other players spectate. Bonus points
  are separate from match totals. Further ties replay among the tied bonus leaders.
- The final screen ranks every connected player and marks the winner. It waits until every connected player confirms the next round. Each level and the bonus round use their matching MP3 in `assets/music/`.
- Players can pass through one another. Coins are clipped to the playfield below
  the HUD, including after the window is resized.
- On WSLg, the client uses Mesa software OpenGL with two worker threads and
  synchronized frame swaps to avoid the Intel Arc D3D12 black-frame issue.

Secure channels use separate certificate identities; each identity may hold
1 active player slot across all rooms and attempt at most 5 joins per second.
Give each remote player a distinct credential directory.

After updating from shared DDS topics, generate a new credential set in a new
folder with `bash scripts/security.sh credentials-private`; old signed permissions cannot
access the private channels. Start the server with
`./build/server credentials-private/server` and each remote client with its
corresponding credentials directory. Keep the authority private.

Shared launcher and credential helpers live in `scripts/`. The root launchers
`server.bat`, `client.bat`, and `create-invite.bat` call them automatically.

## Project layout

- `src/`: client, server, game rules and networking implementations.
- `include/`: shared game and networking headers.
- `tests/`: gameplay tests, network validation and the QA runner.
- `vendor/`: the third-party pixel font header.
- `protocol/`: DDS message definitions; generated C files stay in `build/`.
- `scripts/`: WSL launch helpers and credential setup utilities.
- `assets/`: graphics and music.

Run `make`, `make test`, `make qa`, and `make test-sanitize` from the project root.
The Windows launchers remain at the top level.
