# WUP Stream Client

Mirror your Windows desktop (and its sound) to a Wii U GamePad running **WUP Stream**, and use the GamePad as a keyboard/mouse, an Xbox 360 controller, or a desktop remote, with an on-screen keyboard.

Built on [libwupstream](https://github.com/ravensclawx/wup-stream) (`host/`). License: GPL-3.0-or-later.

## Requirements

- Windows 7 or later (64-bit build)
- A Wii U running the **WUP Stream** homebrew app
- PC and Wii U on the same Wi-Fi/LAN
- *Optional:* [ViGEmBus](https://github.com/nefarius/ViGEmBus) driver, needed only for **Controller** mode. The app offers to download and install it for you the first time you use that mode (needs internet and an admin approval).

## Quick start

1. On the Wii U, open **WUP Stream** from the Homebrew Menu.
2. On the PC, run `wupclient.exe`.
3. Leave the IP box empty to auto-detect, or type the GamePad's IP.
4. Pick a control mode, then click **Start streaming**. Press **HOME + Y** on the GamePad to open the settings menu.
5. The status line shows the FPS and GamePad battery once connected. If the GamePad drops, the client reconnects on its own.

Your IP, sound, pointer, mode, microphone and stream-quality choices are remembered in `wupclient.ini` next to the exe.

## Window options

| Option | What it does |
|---|---|
| GamePad IP | Leave empty to auto-detect. Locked while streaming. |
| Stream PC sound | Sends the default output device's audio to the GamePad (WASAPI loopback). Locked while streaming. |
| Draw mouse pointer | Draws a pointer on the streamed picture. Untick it if you see two. |
| GamePad controls | Selects the control mode (below). Can be changed while streaming. |
| GamePad settings | Open on the GamePad with HOME + Y; changes are saved automatically. |

## HOME is the fn key

Hold **HOME** and press another button. While HOME is held, nothing else reaches the PC or the game (buttons and sticks are suspended), and hints appear on the GamePad screen.

| Combo | Action |
|---|---|
| HOME + **+** | Next mode |
| HOME + **−** | Previous mode |
| HOME + D-pad **Up** | Keyboard + mouse |
| HOME + D-pad **Right** | Controller |
| HOME + D-pad **Down** | Desktop |
| HOME + D-pad **Left** | Off |
| HOME + **X** | Show / hide the on-screen keyboard |
| HOME + **Y** | Open / close the GamePad settings menu |

Mode order for +/−: Keyboard + mouse, Controller, Desktop, Off, then it wraps.

In the settings menu, use D-pad Up/Down to select, **A** to change a setting, and **B** to close. Available settings: GamePad microphone, stream quality, mouse pointer, control mode, and close. Stream quality offers Balanced (4.5 Mbps) and Low (3.5 Mbps); a quality change takes effect after the next reconnect.

Tapping HOME **on its own** (press and release with no combo) does the mode's normal HOME action, listed below.

### Knowing which mode you are in

The mode name appears on the GamePad screen while you hold HOME and for about 1.5 seconds after any switch, with a short buzz as confirmation. It is hidden the rest of the time. The PC window's dropdown always shows the current mode too.

## Control modes

### Keyboard + mouse (games)

| GamePad | PC |
|---|---|
| Left stick | W A S D |
| Right stick | Mouse look (curved response: small pushes are precise) |
| ZR | Left click (fire) |
| ZL | Right click (aim) |
| A | Space |
| B | Ctrl (crouch) |
| X | E |
| Y | R |
| L | Q |
| R | F |
| L3 | Shift (sprint) |
| R3 | V (melee) |
| D-pad Up / Right / Down / Left | 1 / 2 / 3 / 4 |
| + | Esc (pause in most games) |
| − | Tab |
| HOME (tap) | Enter |
| Touch screen | Mouse (for menus) |

Keys are sent as scancodes, which works with most games.

### Controller (virtual Xbox 360)

Appears as an Xbox 360 pad to games. Needs **ViGEmBus**; if it is missing, the status line says so.

- Face buttons are matched **by position**: the GamePad's bottom button (B) is Xbox A, right (A) is Xbox B, left (Y) is Xbox X, top (X) is Xbox Y.
- ZL / ZR are the triggers, L / R the bumpers.
- Sticks, D-pad, L3 / R3 pass through.
- **+** is Start, **−** is Back.
- **HOME (tap) sends the Esc key**, so you can pause or back out of games that ignore Start (for example Geometry Dash). The Xbox Guide button is not sent.
- Game rumble is sent to the GamePad.
- The touch screen still works as a mouse.

### Desktop

| GamePad | PC |
|---|---|
| Touch screen | Mouse (drag to drag) |
| Left stick | Move mouse |
| Right stick | Scroll |
| ZR / ZL / L3 | Left / right / middle click |
| D-pad | Arrow keys |
| A | Enter |
| B | Esc |
| X | Space |
| Y | Tab |
| − | Backspace |
| + | Windows key |
| L / R | Browser back / forward |
| HOME (tap) | Play / pause media |
| R3 | Mute |

### Off

GamePad controls are not sent to the PC. Video and audio still stream.

## On-screen keyboard

Press **HOME + X** to show a QWERTY keyboard over the bottom of the GamePad screen, and again to hide it. Tap keys on the touch screen.

- Number row, Tab, Caps, Enter, Backspace, Esc, arrow keys and Space are included.
- A key is held while your finger is on it, so holding gives normal key repeat.
- **Shift, Ctrl, Alt and Win latch:** tap once and they stay down (shown orange) for the next key, then release.
- Touches on the keyboard do not move or click the mouse; touches above it still do.
- One finger at a time. Works in Keyboard + mouse, Controller and Desktop modes, not in Off.

## Mouse speed

Mouse look (right stick in Keyboard + mouse) and the Desktop cursor (left stick) have adjustable speeds in `wupclient.ini`:

```ini
[wup]
look=12     ; mouse look speed, 1-100
cursor=9    ; desktop cursor speed, 1-100
```

Lower is slower. Restart the app after editing. The file is created the first time you change a setting or start streaming.

## Updates

The client checks the latest GitHub Release in the background at startup. If a newer version is published, it asks before downloading and installing it. The downloaded executable is checked against the SHA-256 digest published by GitHub Releases before the current executable is replaced. The updater then restarts the client.

When publishing a release, bump WUPCLIENT_VERSION in wupclient.c to the release's numeric version (without the leading V). Releases need a Windows executable asset with a SHA-256 digest in GitHub's release metadata.

## GamePad microphone

The GamePad settings menu includes a **GamePad microphone** toggle. To use it in Windows apps, install **VB-CABLE** from VB-Audio. WUPClient sends the GamePad microphone to the VB-CABLE playback endpoint named **CABLE Input**; in Discord, games, or another app, select **CABLE Output** as the recording/microphone device. The microphone is off by default. This is a virtual audio routing path; it does not replace a physical microphone driver.

## How it works

- **Video:** DXGI Desktop Duplication on the primary monitor, so full-screen games come through. It falls back to GDI ("basic capture" shows in the status line) on old Windows, HDR, rotated, or remote-desktop setups. The picture is scaled to the GamePad's 854x480 and keeps its aspect ratio.
- **Audio:** WASAPI loopback of the default output, converted to 48 kHz stereo 16-bit. Silence is sent while nothing is playing.
- **GamePad microphone:** the stream library returns 48 kHz mono audio; WUPClient routes it to VB-CABLE when enabled.
- **Input:** Keyboard and mouse use `SendInput`. Controller mode uses ViGEmBus.
- Capture follows resolution changes; if a game changes the display mode, the client reopens capture automatically.

## Stability and troubleshooting

The default stream cap is now 4.5 Mbps, with a 3.5 Mbps Low option in the GamePad settings. If the GamePad struggles or the Wi-Fi is busy, switch to Low. Audio worker threads are joined before the stream connection is released, and closing the PC client waits for the stream worker to finish.

## Troubleshooting

- **"Looking for a GamePad..." never ends:** make sure WUP Stream is open on the Wii U, both devices are on the same network, and try typing the GamePad's IP. Check that the Windows firewall allows the app.
- **Two mouse pointers:** untick *Draw mouse pointer*.
- **Controller mode does nothing (for example in BeamNG):** the status line should say "virtual controller active". If it says the driver is missing, the app offers to download and install ViGEmBus when you pick Controller mode; click Yes and approve the Windows prompt. Then start the game *after* the stream is running in Controller mode and choose the Xbox 360 controller in the game's controls settings.
- **Can't pause a game:** in Controller or Keyboard + mouse mode, try + or tap HOME (both can send Esc, depending on the mode). Or use the on-screen keyboard's Esc key.
- **Don't know which mode you're in:** hold HOME and look at the GamePad screen.
- **Mouse too fast or slow:** adjust `look` and `cursor` in `wupclient.ini`.
- **No sound:** tick *Stream PC sound* before starting. It follows the default output device at the time you start.
- **Low FPS:** a weak Wi-Fi link is the usual cause. A wired PC helps.

## Building

Source is `wupclient.c`. It needs `wupstream.h`, `libwupstream.a` (libwupstream plus the GamePad-patched x264), and the ViGEm client library. The repository GitHub Actions workflow builds a Windows x64 EXE and publishes it as a downloadable workflow artifact.

**MSYS2 UCRT64 (as the source header says):** build libwupstream with `host/build.sh` from the wup-stream repo, then link `wupclient.c` against `libwupstream.a`, the ViGEmClient sources, and:

`-mwindows -ld3d11 -ldxgi -lole32 -lsetupapi -lws2_32 -lwinmm -lgdi32 -luuid -lurlmon -lshell32 -lpropsys -lpthread -lm -lbcrypt`

**Linux cross-compile (MinGW-w64):**

1. Build x264 at commit `b35605a` with `host/x264-drh-mode.patch` applied, using `--host=x86_64-w64-mingw32 --cross-prefix=x86_64-w64-mingw32- --enable-static --disable-cli --bit-depth=8`.
2. Compile `wupstream.c` against that x264.
3. Compile `ViGEmClient/src/ViGEmClient.cpp` with `g++` (add case-fix header shims for `Windows.h` and `SetupAPI.h`).
4. Link everything with `-static` and the libraries above.

## Notes

- Not affiliated with Nintendo. Wii U and GamePad are trademarks of Nintendo.
