# Device test: WiliWili on TrimUI Brick Pro (tg5040)

Physical acceptance pass for the NextUI pak. The build itself runs in CI; this
checklist is what a human with the device (and ADB) has to confirm. Nothing here
needs `setsid`.

Prepare:

- Install `dist/WiliWili.pakz` (NextUI app installer) or unzip it to
  `/Tools/tg5040/WiliWili.pak/` with `launch.sh` executable.
- Keep a shell: `adb shell`, then
  `tail -f /mnt/SDCARD/.userdata/tg5040/logs/WiliWili.txt`.
- Have a Bilibili account ready for the QR login.

## 1. Launch and UI

1. Start **WiliWili** from Tools.
2. Expect: no black screen, the home page at 1024x768 with the full width used
   (not a centered 16:9 letterbox), readable CJK text (headlines, titles and the
   bottom hint bar), and the controller icon set matching the device.
3. The log must show `sdl: GL Renderer: PowerVR Rogue GE8300` and
   `OpenGL ES 3.x`, plus `MPV Version`/`FFMPEG Version` lines.
4. `grep "ca bundle" "$LOG"` reports the system bundle, not the fallback.

## 2. Login

5. Open the login page, scan the QR code with the Bilibili app.
6. After confirming on the phone the app must switch to the logged-in home page
   and show the avatar.
7. Relaunch: the session must persist (config is written on exit).

## 3. Navigation and input

8. D-pad and left stick move the focus; A activates; B goes back.
9. Open a video detail page, move through tabs with L/R.
10. If the physical A/B labels feel inverted, enable A-B swap in settings and
    confirm the change takes effect.
11. There is no on-screen keyboard: the search box and comment input are
    expected to be unusable. Record what happens (dialog opens but cannot be
    typed into) so it can be documented as a known limit.

## 4. Playback (software decode)

12. Play a 480P video (`32`): must be smooth with audio in sync for at least
    2 minutes; check the on-screen FPS/stat overlay if enabled.
13. Play a 720P video (`64`): expected to be usable, roughly 30 fps. Note the
    actual behaviour and whether the player drops frames.
14. Confirm the quality menu does not offer 1080P (compile-time cap).
15. Seek forward/back, pause/resume, change volume with the shoulder buttons.
16. Audio must come out of the speakers; plug in headphones and confirm the
    switch.
17. Danmaku must render and move (it is nanovg, not libass). Disable/enable it
    in the player menu.

## 5. Lifecycle

18. Press HOME while a video plays: NextUI's system panel must appear over the
    picture, `Resume` must return to the app, `Exit to desktop` must close it
    and return to the desktop within ~2 s.
19. After exit: `ps` shows no leftover `wiliwili` and the desktop is back.
20. Kill the app from ADB (`kill <pid>`): the desktop must come back.
21. Reboot: launch again, confirm the config (login, quality, volume) persisted.

## 6. Failure triage

If it fails, capture:

- `WiliWili.txt` from the log path above (full file)
- the exact step that failed
- whether the UI appeared at all
- `adb shell ls -l /Tools/tg5040/WiliWili.pak/lib` (SD-card path)

Fast isolation:

- **UI black / crash before window**: check `ldd`-style load errors with
  `LD_DEBUG=libs /mnt/SDCARD/Tools/tg5040/WiliWili.pak/launch.sh` and whether
  `LD_LIBRARY_PATH` picked `lib/`.
- **UI renders, video black**: rebuild with `MPV_NO_FB=1`
  (workflow dispatch input `mpv_no_fb`) and retest; this isolates the FBO path.
- **No audio**: try `SDL_AUDIODRIVER=alsa` (already the default) and check the
  `ao` line in the mpv log (`--msg-level=ao=v` via `WILIWILI_MPV_DEBUG` would
  need a build flag; instead inspect `WiliWili.txt` for mpv errors).
- **TLS failures**: confirm `/mnt/SDCARD/.system/res/ca-bundle.crt` exists; the
  pak falls back to `res/ca-bundle.crt`.

Report back with the log, the step number, and the pakz build number from CI.
