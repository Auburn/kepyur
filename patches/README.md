# Patches to the submodules

Changes to `third_party/wifi-sdr` and its `external/devourer` that are not (yet) upstream. They come from
[Play RX](https://github.com/scerstt/PlayRX) (GPL-2.0, like kepyur), a Windows receiver built on kepyur, and
were regenerated against the submodule commits this repository pins. The wifi-sdr patch also carries two
additions of kepyur's own, on top of Play RX's content, which is otherwise as Play RX has it.

| Patch | Applies in | What it carries |
|---|---|---|
| `wifi-sdr-0001-playrx-stream.patch` | `third_party/wifi-sdr` | `RxSettings::centre` (put the hardware centre beside the pilot, off the chip's DC point; `shift_hz` stays `freq - centre`); late or short banks in the first few of a run dropped instead of restarting the stream (behind a USB hub the first bank ships late every time); mid-run, an occasional late or overrun bank dropped instead of a restart (a burst still ends the run); `request_usb3(..., force)`; `WSDR_URB_DEPTH` and `WSDR_FW_TABLE_OFLD` overrides, the firmware table offload on by default only on Windows. **kepyur's additions** (comments marked `kepyur:`): the banks a run drops are reported while it runs, in `RxStatus::mid_drops` and `RxStatus::start_late_skips` (through `StreamCfg::on_drop`), and `BankMeta::seq` steps over them. Without it nothing above the stream could tell that banks were missing: a run that keeps dropping one now and then never ends, so never returns its `StreamStats`. And the C2H wedge (wifi-sdr's FINDINGS §4: on some USB 3 hosts the chip's RX DMA locks up, and only opening the adapter again recovers it) is recognised when a packet does not come and the 0x294 request is still pending: the run ends at once with `StreamStats::wedged`, the Receiver stops with `RxStatus::wedged` instead of failing five more starts, and the missed-packet report also gives the RX DMA registers. A restart after a fault pauses 10 ms between starts instead of 100, and a start drains for 10 ms instead of 30: the hole a restart leaves in the stream goes from ~200 ms to ~55 ms (measured on a Pixel 8 Pro, which restarts every few seconds at 40 MS/s), with no more failed starts. And whether the link is SuperSpeed comes from the descriptors when the kernel cannot say (Session::open: libusb asks with USBDEVFS_GET_SPEED, which older kernels lack, an LG G7 on Android 10 for one): taken as USB 2, a SuperSpeed link got 4096-byte EP0 writes, which the chip stalls (LIBUSB_ERROR_PIPE at every start); a bulk endpoint's 1024-byte packets now say USB 3 |
| `devourer-0001-playrx-usb.patch` | `third_party/wifi-sdr/external/devourer` | control reads retried with a back-off; a circuit breaker that fails every control transfer at once after 8 failures in a row (or one NO_DEVICE), instead of minutes of retries on a vanished adapter; on Windows only: synchronous EP0 writes and retried writes |

The `#ifdef _WIN32` parts are inert on Android, Linux and the web build.

## Applying

The build applies them: `native/CMakeLists.txt` and `web/CMakeLists.txt` include `native/cmake/patches.cmake`,
which applies each patch unless it is already in, and stops the configure if one neither applies nor is in (the
submodule pin moved without the patch). To build wifi-sdr some other way, run `scripts/apply-patches.sh` first.
`.gitmodules` has `ignore = dirty` for wifi-sdr, so the patched working trees do not show in `git status`.

Both work on the sources as plain directories too (no submodule metadata: an archive, or kepyur copied into
another repository), and on a copy of a checkout made without its top `.git` directory, whose submodules still
have a `.git` file that now leads nowhere. Both check after the fact that a line each patch adds is really in
the tree. A patch
that adds or removes that line needs the marker in `patches.cmake` and `apply-patches.sh` changed with it.

## Regenerating one

After changing the patched code, or after moving a submodule pin and fixing whatever no longer applies:

    git -C third_party/wifi-sdr diff --ignore-submodules > patches/wifi-sdr-0001-playrx-stream.patch
    git -C third_party/wifi-sdr/external/devourer diff --ignore-submodules > patches/devourer-0001-playrx-usb.patch
