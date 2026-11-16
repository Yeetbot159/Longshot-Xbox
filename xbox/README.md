# Longshot on Xbox Series X|S (Developer Mode)

An experimental port of Longshot to Xbox consoles in Developer Mode, as a UWP app, the way the Xbox Emulation Hub's
ports are installed. **It does not play the game yet: there is no picture.** What is here now:

- **Longshot** (`xbox/app`), the app a player installs: it finds their game files on a USB drive, has a **Series S**
  and a **Series X** mode, estimates the build on this console, and starts a built game module with its sound,
  keyboard, clipboard and links. The package now builds the Vulkan module and carries Mesa Dozen as the Vulkan-on-D3D12
  driver; this path has not yet been validated on the Series S.
- The **Xbox host layer** (`host/uwp`) and the **Xbox build** of the game (`LONGSHOT_XBOX=1`): the game as a module
  that names no desktop DLL.
- The **loader** (`xbox/loader`) the app runs the module with, and the **probe** (`xbox/probe`) that measures what the
  console gives a UWP game.

## The rule that shapes everything

The Xbox package never contains the game or anything made from it (CONTRIBUTING.md, "The rule with no exceptions").
An emulator's package ships without games; Longshot's game executable *is* made from the game, so it cannot ship at all.
The plan is the emulators' arrangement taken one step further:

- **The package** (`.msix`, downloadable, GPL) holds Longshot's host, its renderer, the Direct3D 12 layer and the
  builder's tools: no game.
- **The game module** is built from the player's own copy of the game, on the console itself (below) or on their PC
  with the Longshot Builder, and kept on their USB drive. The app loads it from there with its own loader.

## The app and its two modes

`Longshot.msix` (`xbox/app`, built by `xbox\app\build.ps1` and by CI, released on `v*` tags) opens on one screen, run
with the controller (D-pad, A):

| Row | What it does |
| --- | --- |
| Console | The model, from `GetGamingDeviceModelInformation`, and the memory the console gives the app |
| Mode | **Auto** (the console's own), **Series S** or **Series X**; written into `longshot_options.ini` in the app's LocalState at once |
| ROMFS folder | Cycle through detected `romfs\` folders on removable storage; press A to rescan |
| ExeFS folder | Independently cycle through detected `exefs\` folders containing `main` |
| Build | Compiles from the selected folders on-console; estimates the build when its tools are unavailable |
| Play | Loads `<ROMFS drive>\Longshot\game\longshot_game.dll` and starts the game with the selected ROMFS |

The modes use the consoles' published hardware specifications as starting points, not as promises about what a UWP
app can use. Series S has an 8-core Zen 2 CPU, a 20-CU RDNA 2 GPU rated at 4 TFLOPS, and 10 GB GDDR6 (8 GB at
224 GB/s and 2 GB at 56 GB/s). Series X has an 8-core Zen 2 CPU, a 52-CU RDNA 2 GPU rated at 12 TFLOPS, and 16 GB
GDDR6 (10 GB at 560 GB/s and 6 GB at 336 GB/s). These are whole-console figures; the OS and app model affect what
Longshot can allocate. ([Series S specs](https://news.xbox.com/en-us/2020/09/09/xbox-series-s/), [Series X specs](https://news.xbox.com/en-us/2020/03/16/xbox-series-x-tech/))

Microsoft documents `expandedResources` as granting access to Game Mode CPU resources and at least a 4 GB memory
partition on Xbox. That is not a build-process working-set guarantee: `AppMemoryUsageLimit` can begin lower and grow,
and graphics allocations count against the same practical budget. We measure the limit at runtime and must profile the
compiler and linker on each console before setting job counts or claiming the build fits. ([Xbox UWP resources](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation), [expandedResources](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations), [expected app limit](https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager.expectedappmemoryusagelimit))

The launcher package now carries CPython, its build dependencies, and llvm-mingw and attempts to run the existing
`nx2/make_game.py` pipeline on the console. ROMFS and ExeFS can be chosen independently; the build work and resulting
module stay on the selected ROMFS drive under `Longshot\work` and `Longshot\game`. The Series S probe measured a
5,120 MB app memory limit, a 4,864 MB commit ceiling, executable generated code, and successful creation of a child
copy of the probe. The builder uses one compile job with a 3 GB compiler budget. The probe did not
try to start Python or clang, and this builder has not yet been run on the console; package-process restrictions,
memory use during linking, and writable USB discovery are still open. It is an experimental implementation, not a
verified console build.

The Series S game profile also limits renderer shader/pipeline workers and asynchronous shader-load workers to one
each; Series X allows two each. This reduces transient compiler concurrency against the consoles' shared CPU/GPU
memory limits, at the cost of slower shader preparation and first use. These are conservative settings, not a measured
guarantee that the complete renderer stays within the app's memory ceiling.

The package declares `removableStorage` for the player's USB drive. It does not declare `broadFileSystemAccess`, which
Microsoft documents as unsupported on Xbox. ([Xbox UWP limitations](https://learn.microsoft.com/en-us/uwp/extension-sdks/uwp-limitations-on-xbox))

| | Series S | Series X |
| --- | --- | --- |
| Output | 1920x1080 | 2560x1440 (the game's own) |
| Quality preset | Minimum (`quality_preset=1`) | Low (`quality_preset=2`) |
| Frame rate | 60 (`frame_rate_cap=0`) | 60 |
| Texture budget | 768 MB (`RX_VK_VRAM_MB`) | 1.5 GB |
| Image pool at start | off (`RX_VK_IMAGE_PREALLOC=0`) | off |
| Files compiled at once by the builder | 1 | 2 |

`xbox\app\test\run.ps1` checks the portable parts on a PC (21 checks): the model ids, the modes' choice, the options
file kept intact but for the mode's lines, the estimate, and the drive search.

## The game on the console: the app, the module and the platform table

A UWP app may not call the desktop's window, sound and shell functions. Of the 304 Windows functions a built
Longshot imports, 227 are in the app container's import library and 77 are not (`python -I xbox\tools\uwp_gap.py`):
59 window, cursor, clipboard and raw-input calls of user32, winmm's `waveOut` sound, `ShellExecute`, `MessageBox`,
a crash dump, two processor-group calls. Nearly all are in `host/display.c` and `host/nn_audio.c`.

- **`host/uwp/uwp_platform.h`**: the table the app gives the module. The window's size and events, the keyboard,
  the clipboard, links, messages, sound (one 16-bit PCM stream), the folders, the console's model and memory limit.
- **`host/uwp/display_uwp.c`**: the window, in place of `host/display.c` (as `host/posix/display_sdl.c` is on Linux).
  It starts the renderer with the window bridge (`MkwVkWindow`, now for UWP builds too: `MKWVK_API_VERSION` 24) and
  passes on size, focus, typed text and the app's closing. No mouse: the controller plays.
- **`host/uwp/win32_uwp.c`**: the 77 functions, defined in the module so its link names no desktop DLL. `waveOut`
  plays through the app's XAudio2, keeping `waveOut`'s rules (`WHDR_DONE` polled, as `nn_audio.c` uses it); message
  boxes are drawn over the game by the app; links open in the console's browser; the desktop's windows don't exist.
- **`host/uwp/uwp_entry.c`**: the module's exports, `longshot_uwp_set_platform` and `longshot_uwp_main` (the game's
  `main`, compiled under another name).
- **`xbox/app/platform.cpp`**: the app's side: CoreWindow events into the table, XAudio2, the WinRT clipboard and
  launcher.

`host\uwp\test\run.ps1` checks `win32_uwp.c` against a stand-in app (34 checks) and links it against the UWP import
library alone, so a desktop function that isn't replaced fails the link.

### Building the module

`LONGSHOT_XBOX=1` makes `nx2/build.py` build `longshot_game.dll` for the app instead of `longshot.exe`, with llvm-mingw:
the consoles' Zen 2 (`RX_MARCH=znver2` unless given), emulated thread-locals, `LONGSHOT_UWP`, `host/uwp` in place of
`host/display.c`, and a link against `libwindowsapp.a` alone (plus the C and C++ runtimes, whose DLLs the app
carries). No renderer is built: the app package has it. The launcher passes the selected ROMFS and ExeFS paths to
`make_game.py`; output is installed at `<ROMFS drive>\Longshot\game` and intermediate files stay in
`<ROMFS drive>\Longshot\work`.

    set LONGSHOT_XBOX=1
    set LONGSHOT_MINGW=<llvm-mingw folder>
    python -I nx2\make_game.py --romfs <dump>\romfs --exefs <dump>\exefs --out <folder> --work <drive>\Longshot\work --no-prepare-shaders

## Graphics: Vulkan on Direct3D 12

The renderer (`gpu/vk`) calls 165 Vulkan functions, 72 of them commands, and leans on buffer device addresses
(`PhysicalStorageBuffer` in its shaders), push descriptors, dynamic rendering, extended dynamic state 3, mesh
shaders, conditional rendering and timeline semaphores. A UWP app on the console has Direct3D 12 and no Vulkan.

Two ways to give it Vulkan there:

- **Mesa's Dozen** (`src/microsoft/vulkan`, MIT): a Vulkan driver on Direct3D 12, with Mesa's own SPIR-V to DXIL
  compiler (no DXC needed at run time). It already does most of the 165 functions. The Series S's shader model 6.4
  is below Dozen's shader model 6.6 requirement for buffer device addresses; its D3D12 mesh shader tier is 0. The
  renderer already has static-pipeline fallbacks for the extended dynamic state 3 features it uses. The package
  carries Mesa's desktop Windows binaries and the app supplies UWP shims for its cache path, version checks and
  composition presentation; that loader path still needs a Series S run.
- **A layer of our own** (`gpu/vkd3d12`): the 165 functions written for this renderer alone. Smaller, but all of it
  new.

The CI workflow now builds `longshot_vk.dll` as a standalone UWP renderer and packages it with the Mesa Dozen runtime.
The Series S probe reports feature level 11.0, shader model 6.4, wave operations, and 64-bit shader operations, but its
UWP D3D12 device reports mesh shader tier 0. Microsoft's public GDK sample documents a native mesh-shader pipeline on
Series X|S devkits; this probe therefore describes the current UWP/Dozen API path, not the console silicon or GDK path
([Microsoft's Simple Mesh Shader sample](https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/simplemeshshader/)).
Dozen exposes Vulkan buffer device addresses only when the D3D12 shader model is at least 6.6
([upstream Dozen capability checks](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/dbbd251b6f00db68c1e9f0b7e8504b3b4278eaac/src/microsoft/vulkan/dzn_device.c));
this console reports 6.4, so Dozen will not expose the address feature these guest shaders require. When BDA is missing,
the renderer now enters a limited bound-buffer compatibility mode instead of stopping at startup. Arbitrary guest
pointers remain untranslated and the mesh-to-compute fallback still requires BDA, so this mode does not establish
complete rendering. The Series S probe package 0.2.2 successfully created a shader-model 6.4 pipeline with a bounded,
non-uniform SRV descriptor array, a promising prerequisite for a UWP-safe address-table path. It does not establish
that Dozen's Vulkan storage-buffer descriptor indexing or guest-memory import works; the updated Device13 import test
still needs to be run on the console.

The previous probe's D3D12 compute-pipeline failure requested shader model 6.6 on an adapter limited to 6.4; the
probe source now uses 6.4 for that check. The earlier `ID3D12Device3::OpenExistingHeapFromAddress` test returned
`E_INVALIDARG`. The current probe separately records the one-byte result and still attempts
`ID3D12Device13::OpenExistingHeapFromAddress1` with the full aligned 16 MB allocation, on ordinary and reserved-window
memory, before checking GPU reads and writes. The attached Series S report came from probe package 0.2.2.0 and did not
include that newer test; zero-copy import into Dozen remains unverified until the updated probe is run. The renderer now also
logs Vulkan's bounded storage-buffer array features and descriptor limits when BDA is missing; this checks one
prerequisite for a future address-table lowering path, which is not implemented yet. The renderer source compiles and
the package includes Mesa Dozen, but no Series S pipeline or frame has been verified.

How far Dozen is from a UWP app, by its imports (`uwp_gap.py --pe` on mesa-dist-win 26.2.6's `vulkan_dzn.dll`,
`spirv_to_dxil.dll` and `dxil.dll`): 151 of its 162 Windows functions are in the app container's library. The 11
that are not are its desktop presentation (GDI's `CreateDIBSection`, `StretchBlt` and five more, `GetDC`,
`GetClientRect`), `SHGetFolderPathA` for its cache folder and `GetFileVersionInfoSizeW`. The presentation is the part
to replace anyway (a swap chain on the CoreWindow, `CreateSwapChainForCoreWindow`); the other two are a folder and a
version check.

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
| Probe app | Series S report received. Follow-up probes test the shader-model 6.4 compute pipeline and bounded non-uniform SRV arrays |
| Launcher and builder tools (`xbox/app`) | Launcher package includes Python, dependencies, llvm-mingw and a one-job, 3 GB build limit. Console execution is unverified |
| Memory on PC | Measured (below): over the console's budget as it stands |
| UWP host layer (`host/uwp/`) | Written; its PC test passes (34 checks, linked against the UWP import library alone) |
| The game as a module (`LONGSHOT_XBOX=1`) | See "Building the module" |
| Vulkan on Direct3D 12 | UWP renderer compiles and is packaged with Dozen's UWP import shims. Series S reports shader model 6.4, below Dozen's BDA threshold; the renderer currently stops with a clear message |
| Game-module loader (`xbox/loader`) | Written; its test passes on a PC (14 checks). On the console: waits for the probe's `codeGeneration` answer |
| Builder on the console | Packaged CPython/llvm-mingw subprocess path is implemented with a measured memory budget; Python/clang launch, USB output, and full build still need an Xbox run |

The CPU benchmark on the PC this was written on (24 threads): one thread 259 ms, 24 threads at once 587 ms.

### Memory on a PC (memfit.ps1, Minimum preset, 1280x720, title and attract mode, no window)

| Settings | Exit | Committed | Working set | GPU dedicated | GPU shared |
| --- | --- | --- | --- | --- | --- |
| Defaults | ran | 12 439 MB | 6 593 MB | 5 537 MB | 1 517 MB |
| Texture budget 1 GB (`RX_VK_VRAM_MB=1024`) | ran | 9 043 MB | 5 283 MB | 3 512 MB | 521 MB |
| Game memory 6 GB (`HOS_MEMORY_MB=6144`), textures 1 GB | crashed at start (access violation) | | | | |
| Game memory 8 GB (`HOS_MEMORY_MB=8192`), textures 1 GB | crashed at start (access violation) | | | | |

A Game on the console gets about 5 GB for CPU and GPU together. With a 1 GB texture budget the game still touches
about 5.3 GB of CPU memory and 3.5 GB of GPU memory, close to 9 GB, so it needs to come down by about half. The game
crashes at start when its memory is set below the default, so the cuts have to come from what the host and renderer
allocate rather than from the game's own heap. The probe's commit test gives the console's real limit.
