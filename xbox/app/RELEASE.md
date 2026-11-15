**An experimental test build.** It contains the Xbox Vulkan renderer module and Mesa Dozen's Vulkan-on-Direct3D 12
driver, but rendering has not yet been verified on a Series S or Series X. The game module and all game data are
built from the player's own files and are not included.

The current Series S reports D3D12 shader model 6.4. Mesa Dozen only exposes Vulkan buffer device addresses at
shader model 6.6, and Longshot's shaders still require them, so this build reports the missing capability and stops
before loading the game's graphics pipelines. The mesh-to-compute fallback also needs buffer device addresses.

What it does now, on an Xbox Series X|S in Developer Mode:

- shows the console's model and the memory it gives the app;
- **Mode**: Auto, Series S or Series X (1080p, Minimum preset, 768 MB of textures; or 1440p, Low preset, 1.5 GB),
  written into the game's options at once;
- finds the folders on your USB drive holding your own `romfs` and `exefs`;
- **Build**: uses the bundled CPython and llvm-mingw tools to try building on the console, one compile job at a time
  with a 3 GB compiler memory budget. The Series S probe confirmed generated code and a child copy of the probe run,
  but did not test Python or clang, so this path remains experimental;
- **Play**: starts a game module built for the Xbox from your own files (`<drive>\Longshot\work\build\longshot_game.dll`),
  with sound. Rendering and full gameplay remain unverified.

The package holds no game and nothing made from one.

Install: Device Portal (`https://<console ip>:11443`) > Add > `Longshot.msix`. Then in Dev Home select **Longshot**,
press View, **View details**, set **App type** to **Game**. `LongshotXbox.cer` is the certificate it is signed with.
