# Play OpenStrikers on Windows

This guide targets **64-bit Windows on an Intel or AMD PC**, the tested
platform. You need your own **Super Mario Strikers GameCube disc image**,
uncompressed as `.iso` or `.gcm`. The launcher does not accept compressed RVZ
files. No disc image or extracted game data ships with this project.

If someone gave you a Windows build, start at [Open the launcher](#open-the-launcher).
Otherwise, follow the source build steps below. You only need the build tools
when compiling; a complete packaged build runs without MSYS2 or Node.js installed.

## Build from source

### 1. Install the tools

In **PowerShell**, install MSYS2 and Node.js LTS:

```powershell
winget install --id MSYS2.MSYS2 -e
winget install --id OpenJS.NodeJS.LTS -e
```

You can also use the [MSYS2 installer](https://www.msys2.org/) and
[Node.js LTS installer](https://nodejs.org/en/download). Node.js provides npm,
which builds the launcher.

Open **MSYS2 UCRT64** from the Start menu. All `bash` blocks in this build
section run there. This project uses GCC's `-fpermissive`; use the UCRT64
environment rather than a Visual Studio developer shell.

Update MSYS2:

```bash
pacman -Syu
```

If the update closes the terminal, reopen **MSYS2 UCRT64** and run the same
command again to finish. See [MSYS2's update instructions](https://www.msys2.org/docs/updating/).
Then install the compiler and build tools:

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  git
```

Check that Node.js is visible in this shell:

```bash
node --version
npm.cmd --version
```

If either command is missing, reopen UCRT64 after installing Node.js. For a
default Node.js installation you can also add its directory in this shell:

```bash
export PATH="/c/Program Files/nodejs:$PATH"
```

### 2. Get the source and apply the patches

Still in **MSYS2 UCRT64**, clone the repository and its nested dependencies:

```bash
git clone --recurse-submodules https://github.com/rhit-windsors/openstrikers.git
cd openstrikers
bash ./apply_patches.sh
```

For an existing clone, run `git submodule update --init --recursive` if the
dependencies are missing. Apply the patch stack once on a fresh checkout.
An already-patched checkout normally has modified submodules: the script will
refuse to run over those edits. See the [patch workflow](BUILDING.md#applying-the-patch-stack)
before updating or reapplying patches.

### 3. Build the game with sound and the launcher

In the same **MSYS2 UCRT64** terminal, from the repository root:

```bash
export PATH="/ucrt64/bin:$PATH"
export PROCESSOR_ARCHITECTURE=AMD64
export TMPDIR="$(mktemp -d -t openstrikers-build.XXXXXX)"
export TMP="$(cygpath -m "$TMPDIR")"
export TEMP="$TMP"

cmake -S . -B build-audio -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENSTRIKERS_AUDIO=ON \
  -DOPENSTRIKERS_LAUNCHER=ON
cmake --build build-audio
```

The writable temporary directory avoids compiler failures caused by an
unwritable inherited `TEMP`. `PROCESSOR_ARCHITECTURE` selects the prebuilt
Dawn/nod dependencies, and the policy option supports dependencies with older
CMake minimums. The first build downloads dependencies, including Electron,
so keep the internet connection available.

Wait for the build to finish successfully. You should have:

```text
build-audio/
  openstrikers.exe
  initial_pipeline_cache.db
  ...runtime DLLs...
  launcher/
    OpenStrikersLauncher-win32-x64/
      OpenStrikersLauncher.exe
      ...Electron files and resources...
```

For later changes in an already-configured checkout, rebuild with
`cmake --build build-audio`. See [Building and patching](BUILDING.md) for
other options and how to share a build.

## Open the launcher

1. If you received a ZIP, extract the entire archive first. Keep the game
   executable, runtime DLLs, shader seed and complete launcher directory together.
2. Open `build-audio\launcher\OpenStrikersLauncher-win32-x64\OpenStrikersLauncher.exe`
   from Explorer. In a shared build, the top-level folder may have a different name.
3. In **Settings**, browse for your **Disc image**. Under **openstrikers**,
   select `build-audio\openstrikers.exe` if it was not found automatically.
4. Choose your display settings. Start at **1920 x 1080**, anti-aliasing **Off**,
   and **VSync** on; lower the resolution if play is slow. The launcher initially
   selects 4K, so check this even on your first launch.
5. Open **Play**, optionally enter names for controller ports 1–4, then click
   **Play**. The game opens its own menus. Choose an exhibition match, captain,
   sidekick, side and stadium to start a match, or use the game's cup mode.

The launcher minimizes while the game runs and returns when it exits.
**F11** or **Alt+Enter** toggles fullscreen during play.

**Original 4:3** adds side bars on a wide display. **Widescreen 16:9** uses the
game's wider camera; the HUD and menus stretch horizontally as in the original
game's widescreen mode. **Stretch to fill** fills the window. Forcing 4:3 or
16:9 also changes the game's Aspect option while playing, including if you
save your options.

## Controls and local multiplayer

SDL-compatible gamepads work without configuration. Connect them before
starting the game. Xbox-style face buttons follow their labels: A is A, B is
B, X is X, Y is Y. The right shoulder button maps to GameCube Z; the triggers
map to L/R. Use the in-game **Strikers 101** lessons to learn gameplay actions.

The keyboard shares the first controller port:

| GameCube input | Keyboard |
|---|---|
| Main stick / move | Arrow keys |
| A / confirm | **X** |
| B / back | **Z** |
| X | **C** |
| Y | **S** |
| Z | **D** |
| L | **Q** |
| R | **W** |
| Start / pause | **Enter** |
| C-stick up / left / down / right | **I / J / K / L** |
| D-pad up / left / down / right | **T / F / G / H** |

For local multiplayer, connect up to four controllers and assign sides in the
game's menus. Keyboard input and the first gamepad control the same port;
they do not provide two separate players.

## Play with friends online

Everyone needs a complete Windows build and their own disc image selected in
Settings. Use the **same game executable and disc region/revision** on every
computer; the lobby checks their build and disc identifiers before starting.

1. Everyone opens **Online** and enters a name.
2. One person clicks **Open lobby**.
3. Friends select the lobby under **On your network**, or enter the host's
   address and click **Join**. On the same LAN, use the host's local IPv4
   address (shown by `ipconfig` in PowerShell).
4. Pick the fielders each person will control. Each computer can use up to
   four local controllers; there are eight fielder positions across both
   teams. Empty positions and goalkeepers are controlled by the computer.
5. Each side's captain chooses its team and sidekick. The host sets stadium,
   match length, computer skill and rules, then clicks **Start match**.
6. At the match summary, press GameCube **A** (**X** on the keyboard) to return
   to the lobby. The host starts the next match there.

For internet play, forward **TCP 52321** (lobby) and **UDP 52320** (match) to
the host PC, then join the host's public address. Alternatively, put everyone
on the same VPN network and join the host's VPN address. Local lobby discovery
uses **UDP 52322**; if discovery does not work, try joining by address.
Allow both the launcher and game through Windows Firewall on the network
you are using. There is no hosted matchmaking or relay service.

If the match repeatedly pauses waiting for input, the host can increase the
input delay before starting another match. Lower rendering resolution on a
slow PC as well: every computer needs to keep up with the match.

The lobby also offers **Co-op cup** for up to four players on one team against
the computer. The host chooses a cup and team; standings appear in the lobby
between matches. Progress is saved after each match and can be resumed by the
host later. These cups use the launcher's own save and do not unlock trophies
or content in the game's memory-card save.

## Start without the launcher

From the repository root in **PowerShell**, substitute your actual disc path:

```powershell
.\build-audio\openstrikers.exe "D:\Games\Strikers.iso" --fullscreen --resolution 1920x1080 --vsync
```

Use `--help` for supported options. In UCRT64, pass a Windows path such as
`D:/Games/Strikers.iso`, rather than `/d/Games/Strikers.iso`, to the native
executable. [Terminal netplay and synchronization details](NETPLAY.md) are
documented separately.

## Troubleshooting

| Problem | What to check |
|---|---|
| Launcher was not built | Check `node --version` and `npm.cmd --version` in UCRT64, then rerun the configure and build commands. CMake skips the launcher if npm is missing. |
| Launcher cannot find the game | In Settings, select `openstrikers.exe` in the build folder. Keep the launcher directory near it when moving a build. |
| Disc image rejected | Choose an uncompressed Super Mario Strikers `.iso` or `.gcm` and check that the file is readable. A compressed RVZ or ZIP cannot be selected directly. |
| Game does nothing, or reports a missing DLL | Extract the whole build. For a source build, rerun the build to copy runtime DLLs alongside `openstrikers.exe`; keep all Electron files beside the launcher too. |
| No sound | Check that Settings points to the audio-enabled executable, not a silent `build/` executable. Reconfigure with `-DOPENSTRIKERS_AUDIO=ON`; also check in-game volume and Windows' volume mixer. |
| Temporary black patches on first play | Let shader compilation finish during boot and the menus. Keep `initial_pipeline_cache.db` next to the game executable. |
| Slow gameplay | Lower the render resolution and turn off MSAA. Online, check every participant's performance. |
| Lobby cannot connect | Check the host address, both firewall rules and both forwarded ports. Try a direct address if LAN discovery fails. |
| Different build or out-of-sync error | Use the same game executable and matching disc region/revision on all PCs. Save the run logs if the problem persists. |
| Repeating corrupted-save prompt | Back up the memory-card save before replacing it; very old development saves may be invalid. See the locations below. |

Launcher settings, co-op cup progress and logs live at:

```text
%LOCALAPPDATA%\OpenStrikers\launcher.json
%LOCALAPPDATA%\OpenStrikers\cup.json
%LOCALAPPDATA%\OpenStrikers\logs\
```

Use **Settings > Open logs folder** to find game logs. The game's memory-card
saves and shader caches live separately in `%APPDATA%\openstrikers\`.
The US save is `USA\Card A\01-G4QE-GameSoccer.gci` within that directory.
Back it up before moving an invalid old save out of the way; the next boot
can create a fresh save.

For a reproducible problem, include the build/commit, disc region/revision,
what you did, whether it was local or online, and the game log. See
[current issues](../ISSUES.md) and [developer diagnostics](DEVELOPMENT.md).
