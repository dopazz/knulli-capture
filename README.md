# knulli-capture

Screen capture for [Knulli](https://github.com/knulli-cfw/knulli-linux)
handhelds: **knulli-screenshot** saves the screen to PNG and **knulli-record**
records it, with sound, to MKV or MP4.

Both tools read the display's scanout buffers directly, through DRM/KMS or
fbdev, rather than calling out to fbgrab or ffmpeg. They capture what is
actually on screen, including overlay planes such as the RG DS on-screen
keyboard, and they turn a sideways-mounted panel back upright.

## knulli-screenshot

```
knulli-screenshot [options] [file.png]
```

- Captures every active screen. Several screens are stacked into one PNG,
  or saved one per file with `-s`.
- Rebuilds each screen from all its planes (primary, overlays, cursor) at
  their position, scale and alpha. Falls back to `/dev/fb0` when DRM capture
  isn't possible.
- With no file name, writes
  `/userdata/screenshots/knulli-screenshot_<wwww>x<h>_<timestamp>.png`.
  Given `file.png`, writes `file_<wwww>x<h>.png`.
- `-l` lists screens, planes and framebuffers; `-c` picks screens by CRTC id
  or connector name (RG DS: `-c DSI-2` top, `-c DSI-1` bottom).

Needs zlib only.

## knulli-record

```
knulli-record [options] [file.mkv|file.mp4]
```

- Writes `/userdata/recordings/knulli-record_<wwww>x<h>_<timestamp>.mkv`
  unless given a file name.
- MKV with H.264 + Opus by default, which still plays if recording is cut
  short. `--mp4` (or a name ending in `.mp4`) writes MP4 with H.264 + AAC,
  which plays in web browsers and chat apps.
- Sound comes from PulseAudio (what the speakers play, by default). Without
  PulseAudio it records video only.
- Uses hardware for capture and colour conversion where it can: the
  Rockchip RGA (if `librga.so.2` is present) and the Allwinner display
  engine. `--no-hw` forces the CPU path.
- `-S` records only the screen EmulationStation is on; `-t` stops after a
  given time; `-v` prints timings and statistics.
- Stop it with SIGINT or SIGTERM to finish the file cleanly.

Run `knulli-record -h` for every option. Needs x264, opus and vo-aacenc;
libpulse-simple and librga are loaded at run time if present.

Both tools must run as root.

## Tested on

Screenshots: Anbernic RG DS, RG CubeXX, RG35XX SP, RG ARC S; TrimUI Hammer,
Smart Pro; Miniloong Pocket; Miyoo Flip; R36Max; BatleXP G350.
Recording: Anbernic RG DS, RG CubeXX, RG35XX SP; TrimUI Hammer;
Miniloong Pocket.

All aarch64.

## Building

```sh
make CC=aarch64-buildroot-linux-gnu-gcc
make install DESTDIR=/path/to/rootfs      # to $(DESTDIR)/usr/bin
```

To build a standalone `knulli-record` for a Knulli image that doesn't ship
x264, opus or vo-aacenc, use `make STATIC=1`. This links those libraries in
from the toolchain's sysroot. Build with a toolchain whose glibc is no newer
than the device's (`src/compat.c` covers the one known mismatch, `sqrtf`).

## Knulli / Buildroot package

`buildroot/knulli-capture/` contains the package for knulli-linux:

1. Copy it to `package/system/knulli-capture/`.
2. In the top-level `Config.in`, add:

   ```
   source "$BR2_EXTERNAL_KNULLI_PATH/package/system/knulli-capture/Config.in"
   ```

3. In `package/system/knulli-system/Config.in`, add:

   ```
   select BR2_PACKAGE_KNULLI_CAPTURE    if BR2_aarch64
   ```

4. Remove the scripts that would overwrite the binaries:
   - `board/fsoverlay/usr/bin/knulli-screenshot` and
     `board/fsoverlay/usr/bin/knulli-record`. The rootfs overlay is copied
     after all packages install, so these would replace the binaries.
   - In `package/system/knulli-resolution/knulli-resolution.mk`, skip the
     `knulli-screenshot` install and the `KNULLI_RESOLUTION_INSTALL_RECORDER`
     hook when `BR2_PACKAGE_KNULLI_CAPTURE` is `y`.

32-bit boards keep using knulli-resolution's scripts.

## Licence

GPL-3.0-or-later; see [LICENSE](LICENSE).

`src/rga.c` contains struct and constant definitions copied from Rockchip's
librga headers (im2d API v1.10.4). `src/sunxi.c` contains ioctl definitions
from the Allwinner 4.9 BSP kernel's display headers. Both are there only to
talk to those interfaces.
