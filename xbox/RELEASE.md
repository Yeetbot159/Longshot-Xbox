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

## Experimental status and limitations

The Series S probe package 0.2.2.0 reported shader model 6.4, D3D12 mesh shader tier 0, and successful creation of a
bounded non-uniform SRV-array pipeline. Its older `ID3D12Device3::OpenExistingHeapFromAddress` test returned
`E_INVALIDARG`; that package did not include the current full-size `ID3D12Device13::OpenExistingHeapFromAddress1`
test. The current probe's full-size import result has not been collected.

When Vulkan buffer device addresses (BDA) are unavailable, the renderer enters a limited bound-buffer compatibility
mode. Arbitrary guest-pointer translation and the mesh compute fallback still require implementation; rendering and
full gameplay have not been verified on Series S or Series X. The UWP/Dozen mesh-tier result describes this app path,
not the console silicon's separate GDK capabilities. The Series S probe also measured a 5,120 MB app memory limit
and a 4,864 MB CPU commit ceiling. On-console Python/clang building and a Series X probe remain unverified.

The packages contain no game files or data derived from a game. The certificate is public and contains no private
signing key. After installing, set the app type to **Game** in Dev Home for useful memory measurements.
