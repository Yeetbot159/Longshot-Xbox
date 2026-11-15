# Longshot on Xbox Series X|S (Developer Mode)

An experimental port of Longshot to Xbox consoles in Developer Mode, as a UWP app, the way the Xbox Emulation Hub's
ports are installed. **It does not play the game yet.** What is here now: a probe that measures what the console gives
a UWP game, so the port can be built to fit, and the loader the app will run the game module with.

## The rule that shapes everything

The Xbox package never contains the game or anything made from it (CONTRIBUTING.md, "The rule with no exceptions").
An emulator's package ships without games; Longshot's game executable *is* made from the game, so it cannot ship at all.
The plan is the emulators' arrangement taken one step further:

- **The package** (`.msix`, downloadable, GPL) holds Longshot's host, its renderer, the Direct3D 12 layer and the
  builder's tools: no game.
- **The game module** is built from the player's own copy of the game, on the console itself (below) or on their PC
  with the Longshot Builder, and kept on their USB drive. The app loads it from there with its own loader.

## Building the game on the console

The goal: a player plugs in a USB drive holding their own dump, starts **Longshot** in Dev Home, picks the dump, and the
app builds the game there and then plays it. Nothing leaves the console.

What the Longshot Builder does on a PC, and what each step needs on the console:

| Step on a PC | Size on a PC | On the console |
| --- | --- | --- |
| Read the dump, unpack the code and the `romfs` (`nx2/nup.py`, `nso.py`): Python with lz4 and zstandard | minutes | CPython embedded in the app, lz4 and zstandard built as part of it |
| Lift the ARM64 code to C (`nx2/lift.py`, `a64-kit/lift`): Python with capstone and numpy | ~1.4 GB of C | The same, capstone and numpy built for the app |
| Compile 473 objects with clang, -O1 | 25-43 min on 16-24 threads, ~10 GB at peak, ~60 GB of disk | clang as a library in the app (no program may be started: the probe says whether that holds), one or two files at a time (0.8-1.7 GB each) to fit the ~5 GB a Game gets; the work folder on the USB drive. Hours, not minutes: the probe's CPU benchmark gives the estimate |
| Link `longshot_game.dll` with lld | ~1-2 GB | lld as a library, the same flags plus `-femulated-tls` |
| Windows loads the DLL | | `xbox/loader`: the app maps the DLL into its own memory and makes it executable (`codeGeneration`) |

Constraints already settled:

- **Thread-locals.** A module the app loads by hand gets no slot in Windows' per-thread TLS array, so the game module
  is compiled with `-femulated-tls` and links the toolchain's C++ runtime as DLLs (libc++abi keeps real thread-locals);
  those DLLs are toolchain files and ship in the package, where Windows loads them. The loader refuses a module with
  thread-local data and says so.
- **Exceptions.** The loader registers the module's exception table (`RtlAddFunctionTable`), without which an
  exception or a `longjmp` through game code ends the app.
- **The rule.** The build reads the player's files from their drive and writes only to it and to the app's own folder.
  Nothing of the game is in the package, in CI, or uploaded.

### The loader (`xbox/loader`)

`pe_loader.c` loads a 64-bit DLL from a file or from memory: headers and sections, base relocations, imports (the
host's functions through a resolver the host passes, then loaded DLLs, the package's and the system's), section
protections, the exception table, TLS callbacks and `DllMain`, exports with forwarders, and unloading. It looks every
system function up at run time, so the same file builds into a desktop program and the UWP host.

Its test builds a module with llvm-mingw the way the game module will be built and drives it from a program built with
MSVC, as the UWP host is: loaded away from its image base, two copies at once, from a file and from memory, an import
resolved to the host, a static constructor, a C++ exception thrown and caught in the module, emulated thread-locals on
two threads, and a module with thread-local data refused:

    powershell -ExecutionPolicy Bypass -File xbox\loader\test\run.ps1 [-Mingw <llvm-mingw folder>]

## For the console owner: run the probe

You need an Xbox Series X|S in Developer Mode with Device Portal on (Dev Home, Remote Access), and a PC or phone on
the same network.

1. Download `LongshotXboxProbe.msix` (from this repository's releases, or built as below).
2. Open Device Portal in a browser (the address Dev Home shows, `https://<console ip>:11443`), **Add**, choose the
   `.msix`, and install. There are no dependency packages: the app carries its own runtime.
3. In Dev Home, select **Longshot Xbox Probe**, press the View (two squares) button, **View details**, and set
   **App type** to **Game**. As an App it gets about 1 GB of memory; as a Game about 5 GB, which is what the port needs
   measured.
4. Plug in a USB drive with at least 1 GB free (the probe times it with a 512 MB file in `E:\LongshotProbe`, then
   deletes it), and start the probe. It writes its findings on the screen as it goes (D-pad or left stick scrolls, A follows the end again).
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
| code from outside the package | Whether memory can be made executable (`codeGeneration`), 640 MB of it, whether a DLL loads from LocalState, `RtlAddFunctionTable`, file mapping from apps |
| building on the console | A CPU benchmark set against a PC's (`bench_pc.exe`, built beside the probe), whether the app may start a program, the USB drive's write and read speed |
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
| Game-module loader (`xbox/loader`) | Written; its test passes on a PC (14 checks). On the console: waits for the probe's `codeGeneration` answer |
| Builder on the console (embedded CPython, clang and lld as libraries) | Designed (above); waits for the probe's answers on starting programs, CPU speed and the USB drive |

The CPU benchmark on the PC this was written on (24 threads): one thread 259 ms, 24 threads at once 587 ms.
