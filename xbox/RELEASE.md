# Longshot Xbox preview

This prerelease contains two game-free packages signed by the included `LongshotXbox.cer`:

- `LongshotXboxProbe.msix` collects hardware, UWP, memory, storage, audio, input, and D3D12 capability data. Run it
  on the target Series S or Series X and share `probe.txt` and `probe.json` from its LocalState folder.
- `Longshot.msix` contains the launcher and Xbox host prototype, the Xbox-targeted `longshot_vk.dll`, Mesa Dozen
  (`vulkan_dzn.dll`) and DXIL runtime, plus Python and llvm-mingw builder tools. The launcher tries to build the
  game module on the console with one compiler job and a 3 GB compiler memory budget.

The Series S probe run confirmed that generated code can execute, a 512 GB address range can be reserved, and the
app can start a child copy of itself. It did not test starting Python or clang, and the renderer has not yet been
validated on the console. Treat renderer launch and console building as experimental until those are tested on the
target Xbox. The console's UWP D3D12 device reports shader model 6.4, while Mesa Dozen gates Vulkan buffer device
addresses at 6.6; Longshot's current shaders require those addresses, so the renderer reports that capability gap
rather than start the game. An address-table shader path is still needed. The D3D12 probe previously compiled a
shader-model 6.6 shader against an adapter limited to 6.4; this package's probe source now builds its compute pipeline
at 6.4 and checks bounded non-uniform SRV descriptor arrays for an address-table fallback. Its UWP device reports no
mesh shader tier. Microsoft's public GDK sample documents mesh-shader pipelines on Series X|S devkits, so that probe
result describes this UWP/Dozen path and not the console hardware's full GDK capability.

The packages contain no game files or data derived from a game. The included certificate is public and is required
only to trust/install these sideloaded packages. Set the app type to **Game** in Dev Home when collecting useful
memory measurements.
