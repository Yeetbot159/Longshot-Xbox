# Longshot Xbox preview

This prerelease contains two game-free packages signed by the included `LongshotXbox.cer`:

- `LongshotXboxProbe.msix` collects hardware, UWP, memory, storage, audio, input, and D3D12 capability data. Run it
  on the target Series S or Series X and share `probe.txt` and `probe.json` from its LocalState folder.
- `Longshot.msix` is the launcher and Xbox host prototype. This workflow does not yet build or package a Vulkan
  renderer, so this launcher package cannot render or play the game. It also does not contain the in-process,
  memory-bounded console builder; build the game module on a PC for now.

The packages contain no game files or data derived from a game. The included certificate is public and is required
only to trust/install these sideloaded packages. Set the app type to **Game** in Dev Home when collecting useful
memory measurements.
