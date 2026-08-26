# ernxOS Desktop v3

This version adds:

- clickable FILES, TERMINAL, and GAMES desktop launchers
- a real FILES viewer showing files from the guest filesystem
- an ERNX GAMES launcher that lists `.ernx` scripts found in the filesystem
- clicking an ERNX game leaves the desktop, runs the script, and returns to the desktop when it finishes
- clicking TERMINAL enters the normal shell
- keyboard shortcuts in the desktop: `T` = terminal, `F` = files, `G` = games, `ESC` = leave graphics mode
- VirtualBox is configured to start the VM in fullscreen mode by `run.sh`

The guest graphics mode remains VGA Mode 13h (320x200). VirtualBox scales the guest image when fullscreen is enabled.

Build:

```bash
chmod +x build.sh run.sh
./build.sh
./run.sh
```

The bundled `.ernx` games are in `files/` and include `dice.ernx`, `guess.ernx`, and `math.ernx`.
