# Longshot Xbox preview

This prerelease contains two game-free packages signed with the included public certificate:

- `Longshot.msix` includes the launcher, Xbox host, UWP Vulkan renderer, Mesa Dozen (`vulkan_dzn.dll`) and its DXIL
  runtime, plus the Python and llvm-mingw tools used by the experimental on-console builder.
- `LongshotXboxProbe.msix` measures UWP, memory, storage, audio, input, and D3D12 capabilities. It also checks
  shader-model 6.4 descriptor-array support and host-memory import behavior.
- `LongshotXbox.cer` is the public certificate required to install the sideloaded packages.

## Install on a Windows PC

These packages use a self-signed test certificate. Before installing an MSIX on Windows, download `LongshotXbox.cer`
from this release, open an elevated PowerShell window in the download folder, and run:

```powershell
Import-Certificate -FilePath .\LongshotXbox.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
```

This adds the publisher certificate to the computer's Trusted People store; only do this for a certificate from the
release you intend to install. Then install `Longshot.msix`. Remove the certificate from **Local Computer > Trusted
People** in `certlm.msc` after testing if you no longer want to trust packages signed by it.

## What changed

- On this PC, under a 5 GiB job, eight CPUs, and a 33.33% host CPU cap, the long opening movie (9,254 pictures) plays when guest maps stay at 1,100 MB during a movie, dav1d is in its low-memory mode, and Dozen uses coherent copies for guest pools that cannot be imported. After that movie the in-engine Deku Tree timeline was not requested. Kokiri Forest, Hyrule Field, and a game save were not reached. This was not run on a console.
- The probe tests a CPU-mapped D3D12 write-back buffer using a shader that reads CPU-written values and writes results
  for the CPU to check after a fence. A Series S 0.2.23.0 report passed this small test; it is not a guest-memory fallback.
- The probe and its shader were built locally under a 5 GiB job memory limit and eight-CPU affinity, with a 33.33%
  host-wide CPU cap. The package's signature, version, contents, and executable were checked locally. These limits
  do not emulate the Xbox GPU or its UWP memory accounting.
- The Xbox translator's bounded batches now live in Longshot's wrapper rather than the vendored a64-kit source. The
  packaged native translator is checked with one million synthetic instructions against the desktop path.
- Invalid compiler memory budgets that overflow during conversion to MB now fall back to the free-memory policy.
- Windows control-bridge connections use the Windows named-pipe APIs, and stale Windows test fixtures were updated.
- The renderer and launcher were built locally with a 5 GiB job memory limit, affinity to eight logical CPUs and a
  33.33% host-wide CPU cap. The Windows game-free suite passed 561 checks with 38 skips; the unpacked local MSIX passed
  five bundled-builder tests. These PC limits do not emulate Xbox GPU power, clocks or UWP memory accounting.
- Packaged NumPy DLLs beside their extension modules; the console builder's `numpy` import no longer calls the
  unsupported `os.add_dll_directory` on the read-only app package.
- Skipped the unavailable PowerShell process query on Xbox and now report the limitation directly; interrupted compiler
  processes still need manual cleanup.
- Reduced Xbox instruction-translation batch size and copied only populated native output bytes to lower peak memory
  during the lift after the latest console build stopped with `MemoryError`.
- Expanded the capability probe to compare D3D12 host-heap import across `VirtualAlloc`, `VirtualAllocFromApp`, and
  `VirtualAlloc2FromApp`, including the allocation region metadata needed to diagnose the remaining renderer blocker.
- Xbox builds default to one lift worker and apply the compiler memory estimate before starting a second compiler.
  An oversized first compiler can still run; neither setting is a hard memory cap, and console validation is pending.
- The launcher directs the game and renderer to the built game folder for caches and logs while keeping the selected
  ROMFS as the asset source. The host-side correction requires rebuilding the game module; Xbox play is unverified.
- The package requests UTF-8 for narrow file paths and warns if the system does not enable it; console behavior is unverified.
- The console builder package now includes the GPU headers needed for compilation and prebuilds its translator and ZBIC
  helper DLLs rather than writing them into the read-only installation. Windows package checks exercise these files;
  loading the helpers and completing a build on Xbox remain unverified.
- Xbox builds now stop before installation if their game module or image is missing, preserving any existing build.
- The console builder starts its Python scripts by absolute path and saves full output in the app's
  `LocalState\build-console.log`; recent activity wraps on screen. The reported `unpack.py` failure's full
  cause was not visible, so this still needs verification on Xbox.
- Guest-memory imports now select a coherent memory type compatible with both the specific host pointer and Vulkan
  buffer, check the buffer's required size, and report which import step failed.
- The console builder now uses temporary directories on the selected work drive with inherited permissions to address
  an Xbox access-denied error. This workaround still needs verification on the console.
- The probe keeps its XAudio mastering voice alive while testing source playback, correcting the earlier audio check.
- USB discovery now recognizes the supplied `<drive>\Longshot\game\romfs` and
  `<drive>\Longshot\game\ExeFS\main` layout without scanning the build-work directory.
- XAudio initialization and reset handling now report failures and wait for queued buffers to drain before freeing a
  stream. The probe also creates a stereo source voice, plays a brief tone, and records its completion callback.
- When Vulkan BDA is unavailable, the renderer can translate global-memory loads through a bounded table of up to
  eight imported pools exposed to the current recording. Each pool is limited to 64 MiB of guest address space;
  opt-in global stores are limited to 1 MiB per pool and remain disabled by default.
- Mesh/task draws also have a descriptor-backed compute fallback when BDA is unavailable, including indirect task
  input. It requires host-memory import, shader int64, and sufficient descriptor limits; this implementation has not
  yet been validated on Xbox.
- ROMFS and ExeFS are discovered, selected, and persisted independently. The selected ROMFS and ExeFS folders can
  reside on different removable drives. Build work stays under `<ROMFS drive>\Longshot\work`, and the game module is
  installed under `<ROMFS drive>\Longshot\game`.
- The launcher has a redesigned two-column setup screen with independent folder selectors, a contextual quick guide,
  and a concise activity panel.
- The normal builder now routes shader-census and prompt-asset subprocess output to stdout. `--prepare` retains its
  stderr protocol, avoiding the reported PowerShell native-command error during the normal build.
- The probe records whether the one-byte Device13 heap-import query succeeds, then still tests a full aligned 16 MB
  allocation. A rejected one-byte query no longer prevents the meaningful full-size test.
- Series S caps renderer shader/pipeline and asynchronous shader-load workers at one; Series X caps each at two to
  reduce transient pressure on shared CPU/GPU memory. This is a concurrency safeguard, not a guarantee that the
  complete renderer fits the app's memory limit.
- The launcher now caps VMA-managed renderer allocations at half of the expected app-memory limit, up to 2 GB on
  Series S or 4 GB on Series X; it falls back to 2 GB when the expected limit is unavailable. This is not a total
  app-memory cap, and imported-pool mirrors have separate limits.
- The supplied Xbox hookshot artwork now brands the launcher, package assets, website, and compiled game's
  start-up screen. The previous Longshot image is preserved as an unused original asset.
- The website settings guide now builds from the restored menu reference, the site consistently uses the Xbox hookshot
  logo, and public progress titles neutralize remaining case variants of game-specific names.

## Experimental status and limitations

This preview does not include the latest upstream changes; their integration has unresolved renderer and builder
conflicts and has not been validated.

The Series S probe report from package 0.2.22.0 measured shader model 6.4, D3D12 mesh shader tier 0, and successful
creation of a bounded non-uniform SRV-array pipeline. Both `ID3D12Device3::OpenExistingHeapFromAddress` and
`ID3D12Device13::OpenExistingHeapFromAddress1` returned `E_INVALIDARG` for the tested ordinary and guest-window
allocations, including `VirtualAllocFromApp` and `VirtualAlloc2FromApp`, despite the existing-heap capability flag.
The corrected probe also submitted a stereo XAudio source buffer and recorded completion; game audio remains unverified.

When Vulkan buffer device addresses (BDA) are unavailable, the renderer enters a limited bound-buffer compatibility
mode. A bounded guest-address table is implemented: it searches up to eight imported pools with statically indexed
storage-buffer descriptors, and only translates accesses within the configured ranges. Pointers outside those ranges
remain unsupported. The mesh-to-compute fallback has an experimental descriptor-backed no-BDA path, gated on
host-memory import, shader int64, and descriptor capacity. Neither it nor imported-pool access has been validated on
console; rendering and full gameplay remain unverified on Series S or Series X. The UWP/Dozen mesh-tier result
describes this app path, not the console silicon's separate GDK capabilities. The Series S probe also measured a
5,120 MB app memory limit and a 4,864 MB CPU commit ceiling. Current upstream Dozen gates BDA and descriptor indexing
at D3D12 shader model 6.6; the reported 6.4 path supports fixed-size dynamically indexed storage-buffer arrays but not
nonuniform indexing or runtime descriptor arrays. Dozen storage-buffer array binding and imported-pool access remain
unverified on console. The failed host-import attempts leave the current guest-pool and mesh fallback paths unverified
on the tested Series S; a successful D3D12 SRV-array pipeline alone does not establish Vulkan or gameplay support. On-console
Python/clang building and a Series X probe also remain unverified.

A PC run of the opening movie under those limits is not console play. The Deku Tree scene, Kokiri Forest, Hyrule
Field, and a game save were not reached.

The packages contain no game files or data derived from a game. The certificate is public and contains no private
signing key. After installing, set the app type to **Game** in Dev Home for useful memory measurements.
