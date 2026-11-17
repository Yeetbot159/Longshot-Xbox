**Longshot Xbox Probe**: not the game. A small app that measures what an Xbox Series X|S in Developer Mode gives a
UWP game, so that Longshot's Xbox port can be built to fit. It contains nothing of any game.

### Install (Xbox Series X|S in Developer Mode)

1. Download `LongshotXboxProbe.msix` below.
2. Open Device Portal from a PC or phone on the same network (the address Dev Home shows under Remote Access,
   `https://<console ip>:11443`), choose **Add**, pick the `.msix` and install. No dependency packages are needed.
3. In Dev Home, highlight **Longshot Xbox Probe**, press the View button, **View details**, set **App type** to **Game**.
4. Plug in a USB drive with at least 1 GB free (the probe writes a 512 MB file to `E:\LongshotProbe` to time the drive,
   then deletes it), start the probe and let it run for about a minute. The audio check plays a brief tone to verify
   source-voice playback. The last test fills memory until the console refuses, and may close the app: that is expected,
   and everything before it is saved.

### Send back the results

Device Portal, **File explorer**, `LocalAppData`, `LongshotXbox.Probe_...`, `LocalState`: download `probe.txt` and
`probe.json`. They hold the console's system version, CPU and GPU details, memory figures and which Windows functions
the app can use; nothing personal.

`LongshotXbox.cer` is the self-signed certificate the package is signed with, in case your setup asks for it.
