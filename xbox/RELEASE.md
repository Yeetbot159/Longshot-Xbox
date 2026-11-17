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

## Experimental status and limitations

The available Series S probe report is from package 0.2.5.0 and is incomplete before the D3D12 results. An earlier
0.2.2.0 report measured shader model 6.4, D3D12 mesh shader tier 0, and successful creation of a bounded non-uniform
SRV-array pipeline. Its older `ID3D12Device3::OpenExistingHeapFromAddress` test returned `E_INVALIDARG`; the current
probe tests a full-size `ID3D12Device13::OpenExistingHeapFromAddress1` import and XAudio source playback, but neither
result has been collected from the console.

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
unverified on console. The available Series S probe report is from package 0.2.5.0 and is incomplete before the D3D12
results; the newer Device13 import test has not yet been run there. On-console Python/clang building and a Series X
probe also remain unverified.

The packages contain no game files or data derived from a game. The certificate is public and contains no private
signing key. After installing, set the app type to **Game** in Dev Home for useful memory measurements.
