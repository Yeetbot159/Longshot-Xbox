<p align="center">
  <img src="assets/branding/xbox-hookshot.png" alt="Longshot Xbox hookshot logo" width="220">
</p>

# Longshot Xbox

**Longshot Xbox** is an experimental Xbox Series X|S Developer Mode port maintained in this repository by **Yeetbot159**. It adapts the original [Longshot project](https://git.magicbean.dev/MagicBeanDev/Longshot), a static recompilation of a Nintendo Switch 2 game, for Xbox's UWP app model.

> [!WARNING]
> **Xbox gameplay is not verified yet.** The launcher and console builder are prototypes. The renderer now includes guest-address translation and a descriptor-backed mesh fallback for systems without Vulkan buffer device addresses; these paths remain experimental, and PC simulation is not proof of Series S gameplay.

This repository contains the Xbox-specific app, renderer packaging, builder, loader, probe, and integration changes. It does not contain the game, game keys, or game-derived files.
The supplied Xbox hookshot artwork is used for launcher and package branding, the website, and the compiled game's start-up logo.

## Project status

| Part | Status |
| --- | --- |
| Launcher app | Builds as a sideloadable UWP package; finds player files on removable storage and selects Series S or Series X build settings. |
| On-console builder | Prototype packages CPython, its dependencies, and llvm-mingw. It has not yet completed a game build on an Xbox. |
| Game module and loader | UWP host shims and a PE module loader are included. The module must be built from the player's own files. |
| Vulkan renderer | `longshot_vk.dll` and Mesa Dozen are packaged. A PC simulation exercised the no-BDA/no-mesh fallback and rendered through Kokiri Forest; Series S renderer startup and gameplay remain unverified. |
| Capability probe | Measures UWP, memory, storage, input, audio, and D3D12 features. The latest probe source checks Dozen's `ID3D12Device13::OpenExistingHeapFromAddress1` path and GPU access through imported memory. |
| Series S and X hardware | Series S probe data is available in the task history; no Series X probe result has been collected. Whole-console hardware specifications do not describe the resources available to a UWP app. |

### Renderer limitation

The Series S UWP device reports shader model 6.4. Mesa Dozen exposes Vulkan buffer device addresses only with shader model 6.6 or later, while the current guest shaders use physical storage-buffer pointers. The renderer has bounded guest-address translation and descriptor-backed task/mesh fallback paths when BDA is unavailable, but they depend on host-memory import. A Series S probe (0.2.9.0) returned `0x80070057` for the tested host-memory imports, so those paths are not established on this console. A PC run with `RX_VK_NO_BDA=1` and `RX_VK_NO_MESH=1` reached Kokiri Forest, but unsupported draws remain and no Series S run has confirmed the same behavior.

The reported mesh-shader tier is for the current UWP/Dozen path. Microsoft documents native mesh-shader pipelines on Series X|S devkits through GDK; this UWP measurement does not establish the console silicon's full graphics capabilities.

The Xbox profiles cap VMA-managed renderer allocations at up to 2 GB on Series S and 4 GB on Series X, dynamically limited to half of the expected app-memory limit when the platform reports it. Imported-pool mirrors are capped separately at 256 MB and 512 MB. These are renderer allocator limits, not whole-process limits: game/host allocations and non-VMA imports are not included. The Series S profile is based on a measured 5 GB app limit; the Series X 8 GB profile is not yet verified by a probe.

## In-game menu

This upstream Longshot menu reference supplies the choices and defaults shown in the website's graphics guide. The menu code remains authoritative for names, descriptions, order, and restart behavior; the site checks this table against it during build.

| Tab | Option | Choices | Default |
|---|---|---|---|
| Graphics | Quality preset | Custom, Minimum (4GB), Low (6GB), Balanced (8GB), High (12GB), Ultra (16GB+), Auto (this PC) | Auto (this PC) on a first start; Custom in an options file from before it |
| | Aspect ratio | The shapes your screen's resolutions have, narrowest first (5:4, 4:3, 16:10, 16:9, 21:9, 32:9, ...), your monitor's own marked "(monitor's)" (after a restart) | Your monitor's |
| | Output resolution | The Aspect ratio's sizes: your screen's resolutions and the common ones (1440p, 4K, 5K at 16:9; 1920x1200, 2560x1600 at 16:10; 3440x1440 at 21:9; ...), and 2048x1152 (the game's own reduced size, 80% of its 2560x1440, marked "(reduced)") at 16:9; your screen's own size is marked "(native)" (after a restart) | 2560x1440, the game's own picture, or your screen's size where it is smaller, at its own shape; Auto (this PC) leaves it as it is |
| | Custom output size | A size of your own, typed as `<width>x<height>` (after a restart) | None |
| | Profile | Docked, Handheld (after a restart) | Docked; a quality preset sets it |
| | Upscaler | Game's own, NVIDIA DLSS (only in builds with NVIDIA's DLSS SDK, on NVIDIA RTX GPUs), AMD FSR 3.1 (every build, on GPUs it runs on), AMD FSR 4 (only in builds with FSR 4, on GPUs AMD's files cover), Off (no upscaling). NVIDIA DLAA and AMD FSR 4 Native AA are DLSS's and FSR 4's Native, chosen on the Render resolution row | Game's own; Auto (this PC) chooses DLSS or FSR 4 where they run |
| | DLSS model | Transformer (K), Transformer 2 (M), Transformer 2 (L), NVIDIA default (shown where DLSS runs; used by DLSS and DLAA) | Transformer (K) |
| | Render resolution | Auto (the game's own dynamic resolution), 100%, 75%, 67%, 50% (each with its size). With DLSS or FSR 4: Auto, Native, 92%, 83%, 75%, Quality (67%), Balanced (58%), Performance (50%), Ultra Performance (33%). With FSR 3.1: Auto, 100%, then the same as DLSS from 92%. With Off: Auto, 100%, 75%, 67%, 50%, 33% | Auto; a quality preset sets it |
| | Sharpness | 0 to 100%, in steps of 10 | 0%; a quality preset sets it |
| | Frame generation | Off, Auto, 2x to 8x | Off |
| | Frame generation by | Auto (NVIDIA DLSS Frame Generation where it runs: RTX 40 and newer with a temporal upscaler; otherwise the built-in one), Built-in FG. Changes at once | Auto |
| | Frame pacing | Off, On | Off; a quality preset turns it on |
| | Game frame rate cap | 60, 90, 120, 144, 165, 240, Uncapped (changes at once) | 60 |
| | NVIDIA Reflex | Off, On, On + Boost (shown only on NVIDIA GPUs with `VK_NV_low_latency2`) | Off |
| | AMD Anti-Lag 2 | Off, On (shown only on AMD GPUs with `VK_AMD_anti_lag`) | Off |
| | Fullscreen | Off, On (the same as <kbd>F11</kbd>) | Off |
| | V-Sync | Auto, On, Off | Auto |
| | Window scaling filter | Off, Catmull-Rom, Lanczos | Off; a quality preset sets it |
| | Ultrawide menus | Full width; Centred, blurred sides; Centred, black bars | Centred, blurred sides |
| | HDR | Off, Expanded, Native (the game's) | Off |
| | HDR paper white | 80 to 400 nits, in steps of 10 (while HDR is on) | 200 nits |
| | HDR peak brightness | 400 to 4000 nits, in steps of 100 (while HDR is on) | 1000 nits |
| | HDR contrast | 50% to 200% around mid grey, in steps of 5 (while HDR is on) | 100% |
| | HDR tone mapper | Vanilla (the game's curve), PsychoV35 (RenoDX's; with HDR set to Native. Compatible DLSS/DLAA, FSR 4/native AA and shared-chain FSR 3.1 upscaling run automatically) | Vanilla |
| | Anisotropic filtering | Game's, 2x, 4x, 8x, 16x | 16x |
| | GPU device | Auto, or each graphics card by name (after a restart) | Auto |
| | Threaded rendering | Auto, On, Off (after a restart). Auto is On with 12 or more processor threads for the game | Auto |
| Audio | Volume | 0 to 100%, in steps of 5 | 100% |
| | Mute in background | Off, On | Off |
| | Audio output | Auto, Stereo, 5.1, Mono (layout changes after a restart; Mono/Stereo live on stereo output) | Auto |
| Gameplay | FPS counter | Off, FPS, Frame time, Graph, Breakdown, Dev | Off |
| | FPS counter position | Top right, Top left, Bottom right, Bottom left | Top right |
| | Menu size | Small, Medium, Large | Medium |
| | Game speed | Per frame, Real time | Real time |
| | Field of view | 50% to 150%, in steps of 5; applies immediately during gameplay | 100% (original) |
| | Text speed | Normal, 2x, 4x, Instant | Normal |
| | Text size | Original, 110%, 125%, 150%, 175% (dialogue, Navi and signs) | Original |
| | Skip title screen | Off, On (after a restart) | Off |
| | Microphone | Off, Default, or each microphone by name | Default |
| | Game language | The game's 16 languages, each by its own name (after a restart) | `language.txt`'s, else English (US) |
| | Menu language | Auto (the game's language), or one of the supported languages | Auto |
| | Discord status | Off, On ("Playing Longshot" and the time played in your Discord status, while Discord runs on this PC) | On |
| | Skip opening | Off, On | Off |
| | Bug report | Saves a report into a new folder in `bug-reports` | |
| Controls | Controller in background | Off, On | Off |
| | Mouse camera | As the stick, Native, Off | As the stick |
| | Disable camera centering | Off, On | Off |
| | Mouse sensitivity | 0.10 to 5.00, in steps of 0.10 | 1.00 |
| | Invert mouse vertical | Off, On | Off |
| | Invert camera X | Off, On | Off |
| | Invert camera Y | Off, On | Off |
| | Camera speed | 50% to 400%, in steps of 25 | 100% |
| | Stick dead zone | Default (24%), 5%, 10%, 15%, 20%, 30% | Default (24%) |
| | Rumble strength | Off, 25% to 150%, in steps of 25 | 100% |
| | Layout preset | Nintendo (Def), Xbox / PC, Swap Face; Custom once you bind a button yourself (on the Controls page) | Nintendo (Def) |
| | Button prompts | Auto, Switch, Xbox, PlayStation 4, PlayStation 5, Steam Deck, Keyboard (on the Controls page; changes at once where the game allows it, else after a restart) | Auto |
| | Motion | Opens the Motion page: Motion controls, sensitivity X and Y, invert X and Y, and deadzone | |
| | Walk mode | Hold, Toggle | Hold |
| | Walk speed | 15% to 90%, in steps of 5 | 45% |
| | Controls | Opens the Controls page | |
| | Menu shortcut | Insert or F1; F1 only; Insert, F1 or Esc | Insert, F1 or Esc |
| | Compact menu key | Right Shift, Right Ctrl | Right Shift |
| Debug | Player: Damage taken | Half, Normal, Double, Quadruple | Normal |

- **Threaded rendering:** Auto enables the render and recorder threads only when the game process can use at least 12 logical processors and the selected mode is safe. On enables them; Off disables them. The setting takes effect after a restart.

The Controls page binds the `walk` control (Caps Lock by default). Hold it to walk, or choose Toggle in Walk mode.

## Releases

Download the game-free app, probe, and public signing certificate from [GitHub Releases](https://github.com/Yeetbot159/Longshot-Xbox/releases). Releases are experimental prereleases. The `xbox-v*` tag workflow builds and publishes:

- `Longshot.msix` — launcher, Xbox host, renderer, Mesa Dozen runtime, and builder tools.
- `LongshotXboxProbe.msix` — hardware and capability probe.
- `LongshotXbox.cer` — public certificate required to install the sideloaded packages.

Install only packages from a release you trust. The certificate is public and is used to verify these packages; it does not contain a private signing key.

## Install and collect a probe

1. Enable Developer Mode on the Xbox and use Dev Home to install the certificate and `.msix` packages. Trust the certificate in the device's trusted people store before installing the signed packages.
2. Set the probe's app type to **Game** in Dev Home when collecting memory measurements.
3. Run `LongshotXboxProbe` from Dev Home. It writes `probe.txt` and `probe.json` to its LocalState folder as it runs.
4. Share the probe output when reporting a renderer or builder issue. The files describe device capabilities and contain no game dump.

The launcher currently expects the player's game files on removable storage. It does not download or include those files.

## Build from source

### Xbox packages

Use Windows with Visual Studio C++ build tools, the Windows SDK, PowerShell, Python, and the dependencies listed in the workflow. The local package scripts are:

```powershell
powershell -ExecutionPolicy Bypass -File xbox\probe\build.ps1 -Version 0.2.3.0
powershell -ExecutionPolicy Bypass -File xbox\app\build.ps1 -Version 0.2.3.0
```

Local MSIX signing uses a self-signed certificate and may require elevated Windows certificate permissions. The GitHub Actions workflow builds the renderer and packages, signs them on its Windows runner, and publishes a prerelease when an `xbox-vMAJOR.MINOR.PATCH` tag is pushed.

The CI workflow is the authoritative full package build. It compiles `longshot_vk.dll` for UWP, includes Mesa Dozen and the console builder tools, runs the launcher and host checks, and builds both MSIX packages.

### Building a game module

The console builder is experimental and still requires an Xbox run to prove that Python, clang, linking, storage access, and memory use work together. To build the module on a PC instead, use the original Longshot build pipeline with an llvm-mingw toolchain:

```powershell
$env:LONGSHOT_XBOX = '1'
$env:LONGSHOT_MINGW = '<path to llvm-mingw>'
python -I nx2\make_game.py --romfs '<your game files>\romfs' --exefs '<your game files>\exefs' --out '<output folder>' --work '<work folder>' --no-prepare-shaders
```

Build and use only from files you are entitled to use. Do not commit, upload, or distribute game files, keys, generated game modules, lifted code, or other game-derived output.

## How it is put together

- `xbox/app` — UWP launcher, console model and memory reporting, options, removable-drive discovery, audio, and package manifest.
- `xbox/probe` — UWP D3D12 and system capability probe.
- `xbox/loader` — PE module loader used by the app.
- `host/uwp` — UWP platform interface and desktop API shims for the game module.
- `gpu/vk` — Longshot's Vulkan renderer, built as `longshot_vk.dll` for Xbox.
- `.github/workflows/xbox.yml` — builds game-free Xbox packages and publishes tagged prereleases.
- `xbox/README.md` — detailed implementation notes, capability findings, and current technical limitations.

## License

Longshot is free software under the [GNU General Public License, version 3 or later](https://git.magicbean.dev/MagicBeanDev/Longshot/src/branch/main/LICENSE) (GPL-3.0-or-later). This Xbox port adapts Longshot, which is a modified version of Hookshot, released by NotRexed under the same license. The changes made to Longshot since that fork belong to the Longshot project; the Xbox-specific changes are maintained in this repository by Yeetbot159 and contributors. See [`LICENSE`](LICENSE), and preserve the notices already present in source files.

```yaml
Hookshot: Copyright (C) 2026 NotRexed and the Hookshot contributors
Longshot: Copyright (C) 2026 the Longshot contributors
```

Longshot includes code from Eden and suyu, also under the GPL. Third-party components under compatible licenses keep their own notices in their folders, including Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus fonts, zbic, and the a64-kit tools. Mesa Dozen and the Xbox package's other components retain their own licensing terms as well.

Longshot is an unofficial fan project, not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo.

## Credits

This repository builds on the original [Longshot project](https://git.magicbean.dev/MagicBeanDev/Longshot) and its contributors. The Xbox launcher, UWP host integration, capability probe, package workflow, and console port work here are maintained by Yeetbot159 and contributors.

- **NotRexed**: [Hookshot](https://github.com/NotRexed/Hookshot), the project Longshot forked from, plus BlueShell (Mario Kart World), Skybound (Tears of the Kingdom), and the a64-kit tools.
- [**Eden**](https://git.eden-emu.dev/eden-emu/eden) and **Eglantine**: shader recompiler and Switch 2 shader work.
- **suyu**: graphics decoders and the AArch64 translator.
- [**seedds/nxnative**](https://github.com/seedds/nxnative): reference for task and mesh shaders.
- [**EvilBoyce / evilmfkr**](https://github.com/evilmfkr/Hookshot) (EvilOOT): GPU culling, ambient light probes, pond water, game fog, and other graphics fixes carried by Longshot.
- [**zortos293/Lichii**](https://github.com/zortos293/Lichii): reference for Ampere shader lowering and its compare-exchange retry fix.
- [**Mesa**](https://www.mesa3d.org/): Dozen, the Vulkan-on-Direct3D 12 driver packaged for Xbox, and graphics compiler work used by the original project.
- [**mstorsjo/llvm-mingw**](https://github.com/mstorsjo/llvm-mingw): the Windows compiler toolchain used to build game modules.
- **kinnay/zbic, Opus, and SDL**: decompression, audio, and controller support in the original Longshot codebase.
- **NVIDIA DLSS and AMD FidelityFX**: optional technologies supported by the original Longshot project. Their SDKs, runtime files, and license terms belong to NVIDIA and AMD; they are not bundled in these Xbox packages.
- Everyone who contributed fixes and testing to Longshot, including SkippySteve, monsterlag, klimovich008, hckrman101, madebyjordan, N3oTraX, shayderrr, gab9281, Wpaif, nicholasgower, ZurielAtr, Joey-Magnus, Zorkats, d3zessete, chrispository, vzeol, leonidasmark, Segmentree, ShyVortex, hiKareeem, and i900gaming.

## Contributing

Bug reports and code contributions for the Xbox port belong in [this repository's Issues and Pull Requests](https://github.com/Yeetbot159/Longshot-Xbox). Include the Xbox model, Dev Mode setup, release version, and relevant probe or launcher logs. Never attach game files or generated game data.

For changes to the original Longshot project, use its [upstream repository](https://git.magicbean.dev/MagicBeanDev/Longshot) and follow its [contribution guide](https://git.magicbean.dev/MagicBeanDev/Longshot/src/branch/main/CONTRIBUTING.md).
