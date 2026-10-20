# Explode Arena - native Android port

A **native** Android app (Java + C via the NDK, **no WebView**) that runs the original
*Explode Arena* (Series 60 / Symbian, 2005) - the real game engine, the real menus, levels,
sprites, sound effects and tracker music.

It is not a re-implementation: the game's own ARM code (from `ExplodeArena.app`) runs inside a small
ARM interpreter, and this project supplies the handful of Symbian OS services that code needs
(heap, files, key events, audio stream, bitmap memory, timer). See `docs/PORTING_NOTES.md`.

## Build the APK with GitHub Actions

1. Create a new **private** GitHub repository (it contains the original game's code and data - keep it private).
2. Upload **everything in this zip to the repository root** (no extra wrapper folder).
   GitHub only runs the workflow from `.github/workflows/build-apk.yml`. Because `.github` starts with a dot, phones and some
   file managers hide it and may skip it when uploading, so the zip also contains a **visible copy in `workflows/build-apk.yml`**.
   If `.github/workflows/build-apk.yml` is missing in your repo afterwards: on GitHub choose *Add file -> Create new file*,
   type `.github/workflows/build-apk.yml` as the file name, paste the contents of `workflows/build-apk.yml`, and commit.
3. Open the **Actions** tab -> **Build Android APK** -> **Run workflow** (it also runs on every push).
4. When it finishes, open the run and download the artifact **ExplodeArena-apk**.
   It unzips to a single flat file: `ExplodeArena.apk` (no wrapper folder).
5. Install the APK (allow "install unknown apps"). Tag a commit `v1.0` to also get the APK attached to a GitHub Release.

## Controls

| On screen | Original phone key |
|---|---|
| D-pad | joystick / arrow keys (move, menu navigation) |
| **BOMB** | joystick press **+ key 5** - selects in menus and drops a bomb in the game |
| **DET** | key 8 - the detonator power-up |
| LEFT / RIGHT | left / right soft key (e.g. BACK, PROCEED) |
| gear icon | **Settings**: screen orientation - Portrait, Landscape or Auto-rotate (remembered) |

The game's default keyboard layout ("A") uses keys 4/5/6 for the bomb and 7/8/9 for the detonator, and the joystick
press only selects in menus - that is why BOMB sends both. You can change layouts in the game under Options -> Keyboard.

Hardware keyboard or gamepad: arrows / D-pad, Enter / Space / A = bomb, Y / R1 = detonator, Menu / X = left soft key,
Back / Esc / B = right soft key, digits 0-9, Backspace = C. Settings are saved to `game.cfg` in the app's private storage.

In landscape the game picture is centred and as tall as possible, with the d-pad on the left and DET / BOMB on the right.

## Updating

Builds are signed with the fixed debug key in `app/debug.keystore`, so a new APK installs over the previous one and keeps
your settings. (If you installed an earlier build that was signed with a different key, uninstall it once first.)

## Known limitations

* **Bluetooth multiplayer is not available.** The MULTIPLAYER lobby opens but finds no devices.
* The game runs exactly as it did on an unregistered phone (a dummy device ID is used; no real identifiers are read).
* The game picture keeps the original 176x208 aspect ratio, in portrait or landscape. Audio has roughly a tenth of a second of latency.

## Repository layout

```
.github/workflows/build-apk.yml   CI: builds app-debug.apk and uploads ExplodeArena.apk (the file GitHub runs)
workflows/build-apk.yml           visible copy of the same workflow (for phones that hide .github)
app/src/main/java/...             Activity, surface view + touch controls, engine thread, audio output
app/src/main/cpp/                 ea_host.c (ARM interpreter + Symbian services), jni_bridge.c, CMakeLists.txt
app/src/main/assets/ea/           code.bin (game code), pakhead.bin, 00..97.bin (game data, decoded)
tools/desktop/                    desktop test harness (boots the engine, scripts keys, dumps frames/audio)
tools/unpack/                     scripts that extracted the data from the original data.pak
docs/                             porting notes and the reverse-engineering report
```

## Desktop test run (optional)

```
gcc -O2 -I app/src/main/cpp -o dev app/src/main/cpp/ea_host.c tools/desktop/dev_main.c -lpthread
printf 'boot\nrun 440\ndump menu.ppm\n' > script.txt
./dev app/src/main/assets/ea script.txt
```

Key-timing unit test (plain Java):

```
javac -d /tmp/k app/src/main/java/com/explodearena/port/KeyFeeder.java tools/test/KeyFeederTest.java
java -cp /tmp/k com.explodearena.port.KeyFeederTest
```
