**An early test build. It does not play the game yet: there is no picture.** The Direct3D 12 renderer is the part
still missing.

What it does now, on an Xbox Series X|S in Developer Mode:

- shows the console's model and the memory it gives the app;
- **Mode**: Auto, Series S or Series X (1080p, Minimum preset, 768 MB of textures; or 1440p, Low preset, 1.5 GB),
  written into the game's options at once;
- finds the folders on your USB drive holding your own `romfs` and `exefs`;
- **Build**: measures this console's CPU and estimates how long building the game here would take;
- **Play**: starts a game module built for the Xbox from your own files (`<drive>\Longshot\work\build\longshot_game.dll`),
  with sound; without the renderer the screen stays on the launcher.

The package holds no game and nothing made from one.

Install: Device Portal (`https://<console ip>:11443`) > Add > `Longshot.msix`. Then in Dev Home select **Longshot**,
press View, **View details**, set **App type** to **Game**. `LongshotXbox.cer` is the certificate it is signed with.
