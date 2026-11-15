# Longshot on Xbox Series X|S (Developer Mode)

An experimental port of Longshot to Xbox consoles in Developer Mode, as a UWP app, the way the Xbox Emulation Hub's
ports are installed. **It does not play the game yet.** What is here now is the first step: a probe that measures what
the console gives a UWP game, so the port can be built to fit.

## The rule that shapes everything

The Xbox package never contains the game or anything made from it (CONTRIBUTING.md, "The rule with no exceptions").
An emulator's package ships without games; Longshot's game executable *is* made from the game, so it cannot ship at all.
The plan is the emulators' arrangement taken one step further:

- **The package** (`.msix`, downloadable, GPL) holds Longshot's host, its renderer and the Direct3D 12 layer: no game.
- **The game module** is built by each player on their own PC with the Longshot Builder from their own copy of the game,
  and copied with their `romfs` to a USB drive. The app loads it from there.

Whether the console lets an app run code from a USB drive is one of the probe's questions ("code from outside the
package" below).

## For the console owner: run the probe

You need an Xbox Series X|S in Developer Mode with Device Portal on (Dev Home, Remote Access), and a PC or phone on
the same network.

1. Download `LongshotXboxProbe.msix` (from this repository's releases, or built as below).
2. Open Device Portal in a browser (the address Dev Home shows, `https://<console ip>:11443`), **Add**, choose the
   `.msix`, and install. There are no dependency packages: the app carries its own runtime.
3. In Dev Home, select **Longshot Xbox Probe**, press the View (two squares) button, **View details**, and set
   **App type** to **Game**. As an App it gets about 1 GB of memory; as a Game about 5 GB, which is what the port needs
   measured.
4. Start it. It writes its findings on the screen as it goes (D-pad or left stick scrolls, A follows the end again).
   It takes about a minute; the last section fills memory on purpose until the console refuses, and may end the app.
   That is expected; everything before it is saved.
5. In Device Portal, **File explorer**, `LocalAppData` > `LongshotXbox.Probe_...` > `LocalState`, download
   `probe.txt` (and `probe.json`) and send them back. They hold no personal data: the console's
   system version, CPU and GPU details, memory figures and which Windows functions exist.

## What the probe measures

| Section | Question for the port |
| --- | --- |
| system | CPU (AVX2), cores and affinity the app gets, system version |
| imports | Which of the 304 Windows functions a built Longshot imports exist in the app container: what the UWP layer must replace |
| storage | LocalState, the USB drive (`E:\`) by Win32 and by WinRT, free space |
| audio, input | XAudio2 output, Windows.Gaming.Input pads |
| threads | 64 MB thread stacks, affinity, priority, fibers, high-resolution timers |
| memory | The 512 GB reservation at the fixed address the game's code is built for, commit-on-touch by an exception handler, write traps, write watch, the app's memory limit |
| code from outside the package | Whether memory can be made executable (`codeGeneration`), 640 MB of it, and whether a DLL loads from LocalState |
| Direct3D 12 | Feature level, shader model, mesh shaders, wave ops, 64-bit atomics, enhanced barriers, bindless; importing CPU memory into the GPU with no copy (`OpenExistingHeapFromAddress`), a compute shader writing it; shaders compiled on the console by the packaged DXC; how much GPU memory can be allocated |
| CPU commit limit | How much memory the app can commit and touch before the console refuses |

## Building (on Windows)

Visual Studio 2022 or later with the x64 C++ tools (the Build Tools are enough) and the Windows 10/11 SDK
(10.0.26100: makeappx, signtool, C++/WinRT, `dxcompiler.dll` and `dxil.dll` come from it), and Python 3.

    powershell -ExecutionPolicy Bypass -File xbox\probe\build.ps1

It writes `xbox\out\LongshotXboxProbe.msix`, signed with a self-signed certificate made once in your user store
(`CN=LongshotXbox`, exported as `xbox\out\LongshotXbox.cer`). `xbox\out\` is never committed.

`xbox\probe\imports.inc` is the list of Windows functions a built Longshot imports (names only). Regenerate it from
a built game with `python -I xbox\probe\gen_imports.py <game>\bin\longshot.exe <game>\bin\longshot_vk.dll`.

## Measuring memory on a PC

`xbox\tools\memfit.ps1` runs a built game without a window on a copy of its folder and records committed memory plus
GPU memory every two seconds, the sum the console counts against its limit. Results are in "Status" below.

## Status

| Step | State |
| --- | --- |
| Probe app | Built; waiting for a run on a Series S |
| Memory on PC | See below |
| UWP host layer (`host/uwp/`) | Not started: waits for the probe's import and memory answers |
| Vulkan to Direct3D 12 layer (`gpu/vkd3d12/`) | Not started: waits for the probe's Direct3D 12 answers |
| Game module on USB | Not started: waits for the probe's code-loading answer |
