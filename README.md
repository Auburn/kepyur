# kepyur

Android analog FPV receiver for spectating. A TP-Link Archer T4U v3 (RTL8812BU) plugged into the phone
streams 40 MS/s raw IQ through [wifi-sdr](https://github.com/snokvist/wifi-sdr) (fork:
`gilankpam/wifi-sdr`, branch `kepyur-android`) and [devourer](https://github.com/OpenIPC/devourer); kepyur
decodes the channel you pick (or scans the 5.8 GHz bands for one) in colour (NTSC and PAL; grey when the colour
burst is too weak).

The picture processing (comb filter, clean OSD edges, smooth lines, sparkle repair), two-antenna diversity,
tuning beside the channel and the sturdier USB streaming come from [Play RX](https://github.com/scerstt/PlayRX),
which built them on top of kepyur for Windows.

The TP-Link Archer T3U (`2357:012d`) and T3U Plus (`2357:0138`) carry the same chip and should work the same
way, but are untested so far: reports welcome.

The latest APK from `main` is the rolling **main** pre-release (tag `main-latest`), rebuilt by CI
(`.github/workflows/android.yml`) on every push to `main`. It is a debug build signed with CI's key: if a
locally built kepyur is installed, uninstall it first (`adb uninstall com.kepyur.app`).

## The app

The picture fills the screen. The menu lies over a black screen, with the picture moved into a small preview
at the foot of its left-hand column; the menu is open when the app starts. Tapping the preview closes the menu
and leaves nothing on screen but the picture. A tap on the picture brings up a **MENU** button (bottom centre)
for three seconds; Back does the same. Where there is no picture (no adapter, a scan, a channel with nothing on
it, a picture that has stopped), full size or in the preview, colour bars show the chosen channel (**R8**), or
**NO SIGNAL** before one has been chosen.

- **Channel** — the band (Raceband, Fatshark, Boscam A / B / E, Lowband) and its eight channels. **Scan** is
  optional: it sweeps every band, marks the channels with video on them and tunes the strongest (or the one
  you were on, if it is still there). The channel is remembered for the next start.
- **View** — aspect ratio (4:3, or 16:9 for a widescreen camera), scaling (**Fit** the whole picture,
  **Stretch** it to the screen, or **Crop** it to fill the screen), size (50–150 %), and **Goggles**: the
  picture twice, side by side, for goggles that hold a phone, with an eye spacing slider to line the two
  pictures up with the lenses.
- **Picture** — comb filter (sharper, no echo after edges), clean OSD edges (no rainbow fringe around text),
  smooth lines (no stair steps, no shimmer), sparkle repair (for a weak signal), colour comb (fewer rainbows
  on fine detail). The first three are on by default. Smooth lines and sparkle repair work on every camera;
  the other three only change an NTSC picture. None adds delay.
- **Signal** — sample rate (40 MS/s, or 20 for a USB link that cannot carry 40; the app also falls back by
  itself when 40 keeps restarting or losing blocks past the first 10 s of watching, and stops doing so for the
  session once 20 has locked up on that phone, where 20 is the worse of the two), diversity (both antennas combined: fit two; it adds
  about half again to the work of one of the two processing threads), tune beside the channel (keeps the
  picture off the chip's DC point; at 40 MS/s only: at 20 the stream is too narrow and the radio stays on
  the channel), gain (automatic, or by hand for a very close transmitter), and a line of numbers (USB speed,
  processing load, drops, restarts).

The status at the top left says what the adapter is doing. Nothing there needs attention unless it says so:
a radio that does not start, or stops delivering samples, is closed and started again by itself, on the
channel it was on. On some phones' USB 3 ports (a Pixel 8 Pro's, for one) the chip's USB data path locks up
every few seconds to minutes (wifi-sdr's "C2H wedge", FINDINGS §4), and only opening the adapter again brings
it back: that is done at once, a gap of about three seconds without the menu coming up (the last picture
stays up, dimmed, until the new one comes), and when it keeps happening the status says that another phone or
adapter may hold up better. The shorter stream restarts that some phones make every few seconds leave a hole
of about 55 ms; the frame torn by one is not shown (the one before it stays up), at most twice a second. While watching it also says when the phone is not keeping up, and what to switch off: when
a block of samples is lost in the app's own queues, when a processing thread has no time to spare, or when
the adapter's stream gives up on three blocks within about five seconds (one now and then is normal and is
only counted, under Signal). When the picture goes away for such a reason while the menu is closed (the adapter
unplugged, the radio being started again), the menu opens by itself to say so, and closes again when the
picture is back unless it was touched meanwhile. After 30 s in the background the adapter is let go; it is
opened and tuned again when the app returns.

Dragging the Size or Eye spacing slider makes the menu step aside and shows the picture sharp, so the
change can be judged as it is made.

## Web version

**https://gilankpam.github.io/kepyur/** — the same receiver in Chrome or Edge on a computer (Windows, macOS,
Linux): plug the adapter in, click Connect, fly with the monitor as the video receiver. It is the app's own decoder
compiled to WebAssembly, talking to the adapter over WebUSB, and it watches at 20 MS/s (what WebUSB can carry).
Rebuilt by CI (`.github/workflows/pages.yml`) on every push to `main`. Firefox and Safari have no WebUSB.

- **Windows:** swap the adapter's driver for WinUSB once with [Zadig](https://zadig.akeo.ie/) (Options → List All
  Devices → the adapter, e.g. "802.11ac NIC" with USB ID 2357 0115 → WinUSB → Replace Driver). The page has the same steps under
  "First time on Windows?". Untested so far: reports welcome (open the page with `?debug` and use Copy report).
- **Linux:** keep `rtw88_8822bu` off the card and give your user access (a udev rule for its USB ID).
- **macOS:** nothing to set up.

On screen: the OSD pill (channel, frequency, SNR, decode → screen latency) can be dragged, ✕ / **O** hides it;
⚙ (top right) has Channels and Scan; full screen bottom right or **F**.

## Layout

- `native/dsp/` — video decoder (FM discriminator, sync, fields, the picture switches), spectrum + pilot
  finding. No hardware.
- `native/radio/` — the adapter (wifi-sdr `Receiver`) + a DSP thread: scan, watch, stream test; antenna
  combining (`combine.cpp`).
- `native/render/` — the picture on screen (OpenGL ES 3): layout, side-by-side view, the menu's preview,
  the NO SIGNAL card.
- `native/jni/` — the app's native side; a render thread draws each frame on the video Surface as it is decoded.
- `native/tools/` — desktop: `kp_replay` (recordings), `kp_live` (adapter on this PC), `kp_sim` (kp_live
  against a recording played back as a tunable adapter), `kp_gen` (a synthetic transmitter: recordings to
  test with), `kp_render` (the app's renderer off screen, to a picture file).
- `android/` — the app (Kotlin, plain Views): `MainActivity` (the screen), `MenuView` + `Widgets` (the menu),
  `UsbLink` (the adapter's life: permission, USB 3 switch, open, watch over, reopen), `Settings`.
- `patches/` — Play RX's changes to wifi-sdr and devourer, applied to the submodules by the build
  (`native/cmake/patches.cmake`; by hand: `scripts/apply-patches.sh`).
- `web/` — the web version (`kepyur_web.cpp` + `site/`). `scripts/` — build and phone helpers.

## Build

Everything comes from `shell.nix`:

```sh
git clone https://github.com/gilankpam/kepyur && cd kepyur
git submodule update --init third_party/libusb third_party/wifi-sdr
git -C third_party/wifi-sdr submodule update --init external/devourer
nix-shell
scripts/build-desktop.sh      # build/desktop/kp_replay, kp_live, kp_sim, kp_gen (+ kp_render with EGL / GLES 3)
scripts/build-android.sh      # android/app/build/outputs/apk/debug/app-debug.apk
scripts/build-web.sh          # build/web/site; serve it with: node web/serve.mjs (http://localhost:8080)
```

The app's own code runs without a phone, in JVM tests (Robolectric) against a stand-in for the native side:
the activity and the adapter's state machine through a failed start, a radio that dies or stalls, the USB 3
switch, unplugging, the background; every menu control down to the call it makes; and the menu itself drawn
to PNG files, with a transparent background where the picture shows through.

```sh
cd android && gradle testDebugUnitTest                                                               # everything
cd android && gradle testDebugUnitTest --tests com.kepyur.app.MenuScreenshotTest -Pshots=/tmp/menu   # /tmp/menu/*.png
```

## On the phone

The adapter takes the USB-C port, so adb runs over Wi-Fi (Developer options → Wireless debugging):

```sh
scripts/phone.sh pair 192.168.1.23:37000 123456   # once ("Pair device with pairing code")
scripts/phone.sh connect 192.168.1.23:41000       # the port on the Wireless debugging screen
scripts/phone.sh install
scripts/phone.sh usbtest    # 60 s at 40 MS/s + 60 s at 20 MS/s, then scan/watch; logs -> logs/
scripts/phone.sh run        # normal start
scripts/phone.sh logs NAME  # pull kepyur.log + logcat
```

Without an adapter, `adb shell am start -n com.kepyur.app/.MainActivity --ez testcard true` puts a moving test
picture where the video would be (`false` takes it away): enough to try the view settings, or to line up
goggles before there is anything to watch.

To see how long a phone's USB link holds up, `--ez soak true` streams at 40 MS/s after every open instead of
watching; `kepyur.log` then has each stream fault as it happens (with what wifi-sdr itself reports) and the
adapter's USB 3 power management at every open. `--es env WSDR_URB_DEPTH=512` passes a switch to wifi-sdr
(several: comma-separated). The app's own: `KP_CONCEAL=0` shows the frames torn by a stream restart, and
`KP_FAST_CPUS=0` lets Android put the two processing threads on any core (by default they are kept off the
phone's slowest cores, where they fall behind).

## Desktop bench

```sh
build/desktop/kp_gen rec/r4 --mhz 5769 --label R4 --cnr 12   # a synthetic transmitter: rec/r4.cs16 + .json
build/desktop/kp_replay rec/r4.cs16 --luma-comb --adaptive --smooth --repair   # the picture switches (--comb: colour comb)
KP_SIM_FILE=rec/r4.cs16 build/desktop/kp_sim --scan --offset-tune --diversity   # the radio's switches, no hardware
KP_SIM_FILE=rec/r4.cs16 KP_SIM_DROP_EVERY=700 build/desktop/kp_sim --watch 5769   # a link that loses every 700th block: falls back to 20 MS/s (after the 10 s grace)
KP_SIM_FILE=rec/r4.cs16 build/desktop/kp_sim --watch 5769 --msps 20               # watch at 20 MS/s (kp_live takes --msps too)
build/desktop/kp_render out.ppm --view 2400x1080 --aspect 16:9 --fit crop --stereo   # what a view setting draws
build/desktop/kp_render out.ppm --nofeed --label R8 --inset 1 --box 68,722,493,289 --radius 37   # no picture on R8, in the menu's preview
build/desktop/kp_replay ../vtx-spike/rec/demo.cs16            # lock numbers
build/desktop/kp_replay ../vtx-spike/rec/r4.cs16 --cnr 8      # with noise added (CNR in an 18 MHz channel)
build/desktop/kp_replay ../vtx-spike/rec/r4.cs16 --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -i -
KP_SIM_FILE=../vtx-spike/rec/r4.cs16 build/desktop/kp_sim --scan   # scan -> watch without hardware
build/desktop/kp_live --scan --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -framerate 60 -fflags nobuffer -i -
```

Measured on the spike's recordings (R4 NTSC VTX on the bench): 96.5 % of lines locked, no lock losses
over 10 s; the picture holds to about 5 dB CNR (it gets snowy, it doesn't drop).

License: GPL-2.0 (wifi-sdr is GPL-2.0-only).
