<p align="center">
  <img src="title/sce_sys/icon0.png" width="160" alt="Kodi for PlayStation 5">
</p>

# Kodi for PlayStation 5

Welcome to Kodi for PS5! This is a native port of [Kodi](https://kodi.tv) 22,
the free and open source media center, to jailbroken PlayStation 5 consoles.
It runs as a regular home-screen title, renders its interface at 4K on the PS5's
GPU, decodes H.264, HEVC and VP9 on the console's video hardware, and plays your
library from the network or local storage.

It is built on the open-source [ps5-payload-dev](https://github.com/ps5-payload-dev)
toolchain and [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), and
is installed with ShadowMountPlus or a compatible loader.

[![Release](https://img.shields.io/github/v/release/VivaLaVent/kodi-ps5?label=release)](https://github.com/VivaLaVent/kodi-ps5/releases)
[![Kodi 22](https://img.shields.io/badge/Kodi-22%20BETA2-17b2e7)](https://kodi.tv)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/YB58bUZrqu)

> **Unofficial.** This project is not affiliated with or endorsed by Team Kodi,
> the XBMC Foundation or Sony. "Kodi" and the Kodi logo are trademarks of the
> XBMC Foundation. The repository contains no exploit, no Sony SDK code and no
> firmware files; the Sony library prototypes in `overlay/xbmc/platform/ps5/sce/`
> are clean-room declarations of the handful of functions Kodi needs.

## Give the code a test drive

Ready-to-install builds are on the [releases page](https://github.com/VivaLaVent/kodi-ps5/releases).

**You need** a jailbroken PS5 with a HEN (etaHEN or equivalent), ShadowMountPlus
(or a compatible folder-title loader), and an FTP server on the console. Tested
on firmware 10.01 (etaHEN + ShadowMountPlus + kstuff) and reported working on
4.03 (ItemzFlow + etaHEN 2.3b).

1. Extract the release zip into `/data/homebrew/` on the console over FTP; it
   creates `/data/homebrew/PPSA99420/`.
2. Register the title with ShadowMountPlus, then start Kodi from the home screen.
3. Add your media: *Videos → Files → Add videos… → Browse → Add network location…*,
   protocol **Windows network (SMB)** or **NFS**, and enter the server's **IP
   address** (Windows/NetBIOS names are not resolved). SMB2 and SMB3 work, SMB1 does not.

For updates, usually only `eboot.bin` changes (plus `sce_sys/` when the title
metadata changes, `share/` when Kodi's data files change). Kodi keeps its data in
`/data/kodi` when the loader lets a title reach `/data`, otherwise in the title
folder itself (`/data/homebrew/PPSA99420/kodi`).

## What works, what doesn't yet

| Works | Not yet |
| --- | --- |
| Estuary GUI rendered natively at 3840x2160 (OpenGL 4.6 on the PS5 GPU) | H.264 High 10 and HEVC 4:2:2/4:4:4 in hardware (FFmpeg takes them) |
| Hardware video decoding (VideoDec2): H.264, HEVC Main and **Main 10**, **VP9 Profile 0 and 2**; shown **zero-copy** (the GPU reads the decoder's frames directly) | HLG output (tone mapped, as on an SDR display) |
| Everything else in FFmpeg, including **AV1** (dav1d) | Fixed 24/25/50 Hz output modes (the PS5 refuses explicit rates from titles) |
| **HDR10 output** while PQ video plays (BT.2020 PQ 10-bit scanout, GUI composited in PQ) | VRR with the PS5's VRR setting off |
| Menus at 60 Hz; **VRR during playback** matched to the video's frame rate | Internet access via curl (add-on repository, online streams) |
| Interlaced streams decoded in hardware and deinterlaced with bwdif | Python add-ons (Python is not built yet) |
| Audio: 5.1/7.1 PCM (48 kHz, 8 channels); Dolby/DTS passthrough offered to Kodi's *Allow passthrough* | Binary add-ons (no `dlopen` in a title) |
| SMB2/3 and NFS network sources, UPnP; thumbnails, databases, settings | Listing under the Media tab (the GL driver fails in that sandbox) |
| DualSense navigation (as keyboard events) | The player debug overlay (L3) during VRR raises the rate to ~120 Hz |

## Settings that matter

### Display

Two PS5 settings (*Settings → Screen and Video → Video Output*) and two Kodi
settings (*Settings → Player → Videos*, settings level Advanced or Expert)
decide the output:

| PS5 **VRR** | Kodi *Adjust display refresh rate* | Menus | During a video |
| --- | --- | --- | --- |
| Off | any | 59.94 Hz fixed | 59.94 Hz fixed |
| On | Off, Always or On start | 60 Hz (paced on the VRR link) | 60 Hz |
| On | **On start/stop** | 60 Hz | **VRR at the video's rate** (table below) |

With the PS5's VRR on, the system runs Kodi on a VRR link and the TV refreshes
whenever Kodi presents a frame. Kodi paces its frames: 59.94 per second in the
menus, and during a video the lowest multiple of the frame rate within the
PS5's VRR range of 48–120 Hz:

| Video | VRR rate | Frames shown |
| --- | --- | --- |
| 23.976 fps (films) | 71.93 Hz | each 3× |
| 24 fps | 48 Hz | each 2× |
| 25 fps (PAL) | 50 Hz | each 2× |
| 29.97 / 30 fps | 59.94 / 60 Hz | each 2× |
| 50 / 59.94 / 60 fps | 50 / 59.94 / 60 Hz | each 1× |

Every frame is on screen equally long: no 3:2 judder, no speed change.
Stopping the video returns to 60 Hz.

- **Enable 120 Hz Output** on the PS5 should be **Automatic**: the system
  builds its VRR link from the high-refresh mode Kodi declares.
- **HDR.** The title declares HDR capability, so with the PS5's HDR at *On When
  Supported* the TV runs in HDR for the whole session: the PS5 maps the menus
  and SDR video into the HDR signal itself (brightness per its *Adjust HDR*
  calibration), and Kodi outputs HDR10 (PQ) video natively. For an SDR title
  instead, build with `KODI_HDR_TITLE=0 bash scripts/30-deploy.sh` (or set the
  PS5's HDR to *Off*): the TV stays SDR and HDR video is tone mapped by Kodi
  (video OSD: *Tone mapping*).
- **Sync playback to display** never changes the output rate. On the fixed
  59.94 Hz output it adjusts playback speed (and audio pitch) to the display's
  vblank clock; on the VRR link the display follows Kodi, so Kodi keeps its
  own clock and the setting has nothing to correct.
- The TV's own overlay (on an LG: the Game Dashboard) shows the rate: about
  60 in the menus, the table's rate during a video. Kodi's log records every
  decision (`[PS5] 25 fps: VRR at 2x = … 50.000Hz`, `display mode …: VRR on;
  presenting at 50.000 Hz on the VRR link`).

### Audio

Set *Settings → System → Audio → Number of channels* to **5.1** or **7.1** for
surround tracks; Kodi's default of 2.0 downmixes them (without the LFE). The PS5
downmixes further to what the display or receiver takes.

Dolby Digital, Dolby Digital Plus and DTS **passthrough** is offered to Kodi's
*Allow passthrough* setting (off by default) as IEC 61937 inside PCM. It needs
the PS5's *Audio Format (Priority)* at **Linear PCM** and a bit-exact path; try
it at low volume first, because a receiver that does not recognise the packets
plays them as noise.

### Switches

Create an empty file with one of these names in `/data/homebrew/PPSA99420/` and
start Kodi. The console protects files a title creates from outside processes,
so FTP cannot delete Kodi's data; the first two let Kodi do it.

| File | Effect |
| --- | --- |
| `kodi-reset` | wipe Kodi's data once, then start fresh |
| `kodi-uninstall` | wipe Kodi's data and quit; the title folder can then be deleted over FTP |
| `kodi-debug` | debug-level logging (slower; remove when done) |
| `kodi-swdecode` | software (FFmpeg) video decoding only, no hardware decoder |
| `kodi-no-zerocopy` | show video through the copying path instead of zero-copy (troubleshooting: flicker, black video) |
| `kodi-no-hdr` | keep the scanout SDR for HDR video too (Kodi then tone maps, if *Tone mapping* is set in the video OSD) |
| `kodi-stereo-only` | a 2-channel audio port and no passthrough offered |
| `kodi-multichannel-alt` | the other 8-channel order, if side and back speakers come out swapped |
| `kodi-hw-pipeline2` | *(experiment)* hardware decoder with two frames in flight, for 4K60 VP9/HEVC that stutters at the default depth of one; `kodi-debug` logs decode times every 5 seconds |
| `kodi-probe-hdr` | *(probe, for HDR development)* after start-up, switch the scanout buffers to the HDR format for 3 seconds and log the result |

## Building

Kodi is not forked. This repository is an **overlay**: a `ps5` platform directory
copied on top of a stock Kodi checkout, thirteen small Kodi patches, C shims that
fill gaps in what a title's system libraries provide, and the scripts that set
up the cross toolchain, configure, build and package.

**Host:** Linux, or Windows 11 with WSL2 (Ubuntu 24.04). 8+ cores and ~40 GB free
disk recommended; the toolchain and library set take 1–3 hours to build once,
Kodi itself about as long again. Builds live on the Linux filesystem.

```bash
git clone https://github.com/VivaLaVent/kodi-ps5.git ~/kodi-ps5-src
cd ~/kodi-ps5-src

# 0. Toolchain: ps5-payload-sdk + pacbrew libraries + ps5-opengl + native-app template (1–3 h)
bash scripts/00-setup-wsl.sh
#    If a pacbrew package fails on a flaky download, resume from it:
#    bash scripts/01-pacbrew-resume.sh <package>

# 1. Kodi source (master / 22.x; the patches are maintained against it)
git clone https://github.com/xbmc/xbmc.git ~/kodi

# 2. Host tools and the libraries nobody packages
bash scripts/10-build-host-tools.sh      # TexturePacker + JsonSchemaBuilder, native
bash scripts/11-build-tinyxml.sh         # TinyXML 2.6.2 into the sysroot
bash scripts/12-build-libuuid-shim.sh    # small libuuid (crossguid) + libprocstat stub (exiv2)
bash scripts/13-build-brotli.sh          # brotli for Kodi's internal exiv2
bash scripts/14-sysroot-pc-files.sh      # .pc files pacbrew does not install (sqlite3)
bash scripts/15-build-dav1d.sh           # dav1d AV1 decoder (needs nasm on the host)
bash scripts/16-build-ffmpeg.sh          # FFmpeg 7.1 with libdav1d (Kodi needs >= 7.1)
bash scripts/17-build-sce-stubs.sh       # link stubs: libSceVideodec2, extended libSceVideoOut
bash scripts/18-build-ps5-opengl.sh      # ps5-opengl SDK with Kodi's additions (patches/ps5-opengl)

# 3. Configure (applies overlay + patches), build, package
bash scripts/20-configure-kodi.sh
cmake --build ~/kodi-ps5-build -j$(nproc)
bash scripts/30-deploy.sh                # -> ~/kodi-ps5-stage/app/dist/PPSA99420/
```

The overlay is copied with a content check, the patch folder must match
`patches/kodi/manifest.txt` (a release zip extracted over an older checkout
never deletes files), and packaging refuses stale builds.

<details>
<summary><b>Repository layout</b></summary>

```
toolchain/ps5-kodi.cmake        wraps the SDK's prospero.cmake, selects CORE_SYSTEM_NAME=ps5
overlay/                        copied onto a Kodi checkout by scripts/20-configure-kodi.sh
  cmake/platform/ps5/           platform selection and dependency exclusions
  cmake/scripts/ps5/            ArchSetup / PathSetup / Install / Macros for the ps5 core system
  cmake/treedata/ps5/           which xbmc/ subdirectories are compiled
  xbmc/platform/ps5/            main.cpp, CPlatformPS5, CPU/GPU info, klog log sink, strptime
    audio/ input/ network/ storage/    AESinkPS5, PS5PadInput, NetworkPS5, PS5StorageProvider
    filesystem/                 smb:// over libsmb2 (SMB2Session, CSMB2File, CSMB2Directory)
    video/                      hardware decoder (CVideoDec2, CDVDVideoCodecPS5), zero-copy buffers and renderer
    sce/                        clean-room prototypes of the Sony libraries used
  xbmc/windowing/ps5/           CWinSystemPS5, CWinSystemPS5GLContext (EGL), VRR pacing, HDR output
patches/kodi/                   thirteen Kodi patches (charset, SMB hooks, log sink, renderer, refresh, HDR framebuffer) + manifest
patches/ps5-opengl/             Kodi's additions to the GL driver/runtime (zero-copy textures, HDR scanout switch)
patches/                        fix for older native-app template converters
shims/native-app/               C library gaps, compiled into the title
shims/libuuid/ shims/libprocstat/   minimal libraries for crossguid and exiv2
shims/sce_stubs/                link stub for libSceVideodec2 (the SDK has none)
pacbrew/ffmpeg/ pacbrew/dav1d/  PKGBUILDs for FFmpeg 7.1 and dav1d (scripts/15, 16)
scripts/                        00 setup · 01 pacbrew resume · 10–18 dependencies · 20 configure · 30 package
title/sce_sys/                  Kodi's icon
```
</details>

<details>
<summary><b>How it works</b></summary>

| Piece | What it does |
| --- | --- |
| Graphics | OpenGL 4.6 Core via ps5-opengl's Mesa/Gallium build; EGL default display |
| Video decoding | `CDVDVideoCodecPS5` on the hardware decoder (libSceVideodec2). Frames are shown zero-copy: GL textures lie over the decoder's frames (driver additions), a frame returns to the decoder when Kodi releases the picture. 10-bit output arrives lower-aligned in 16-bit words |
| HDR output | for PQ video the scanout buffers switch to the platform's HDR 10-bit format in place (`sceVideoOutSubmitChangeBufferAttribute2`); Kodi renders into a 10-bit target that a final pass packs into the 8-bit framebuffer; the GUI is composited in PQ with Kodi's own compositing path |
| Display timing | the system rate (59.94 Hz) always; on the PS5's VRR link Kodi paces presentation, at the video's VRR rate during playback |
| Audio | `AESinkPS5`: 48 kHz, 2 or 8 channels on the system audio port; the blocking write is the clock. Passthrough = IEC 61937 in 16-bit stereo PCM at 48/192 kHz |
| Input | `PS5PadInput`: DualSense polled at 125 Hz, mapped to Kodi keyboard events |
| Network sources | `smb://` on libsmb2, NFS on libnfs, UPnP |
| Logging | every log line goes to klog (`PS5InterfaceForCLog`) as well as `kodi.log` |
| C library gaps | `shims/native-app/`: resolver (`getaddrinfo` on `sceNetResolver`), locale, directory reading, time, 8 MiB thread stacks, direct-memory heap |
| Packaging | ps5-opengl's native-app template: `eboot.bin` + `sce_module/` + `sce_sys/` + Kodi's data in `share/` |
</details>

<details>
<summary><b>Platform notes</b> (things that differ from a FreeBSD desktop)</summary>

- **Missing modules.** Titles do not get `libScePosixForWebKit` or
  `libkernel_sys`; anything only they export jumps to address 0. Their link
  stubs are removed so such symbols fail at link time and get a shim instead.
- **Memory.** A title's *flexible* memory (`mmap`) is only 448 MiB; the malloc
  heap is carved out of *direct* memory instead (`heap_dmem.c`). Thread stacks
  default to 64 KiB and are raised to 8 MiB (`thread_stack.c`): 1 MiB overflowed
  during thumbnail extraction.
- **ABI mismatches with the system C library:** `struct lconv` field order
  (own `localeconv`), 16-bit `wchar_t` on the PlayStation compiler target
  (patch 0005), directories that need 64 KiB `getdents` buffers (own
  `opendir`/`readdir`).
- **Sandbox.** `lstat` on the title's own mount points fails (SQLite gets a
  patched system call); files the title creates cannot be deleted from outside;
  USB drives are not visible to a title.
- **Media category.** Media apps get half the page tables and a stricter
  sandbox in which the GL driver fails (`EGL_BAD_ALLOC`), so Kodi is a Games title.
- **Display.** The GL driver's render size is a build profile (2160p60 by
  default, `PS5_SCANOUT_HEIGHT` in `scripts/18-build-ps5-opengl.sh`). Explicit
  refresh rates through the mode API are refused (`UNSUPPORTED_OUTPUT_MODE`), so
  fixed 24/25/50 Hz are not available to titles. With the PS5's VRR on, the
  system keeps the title on a ~120 Hz VRR link that follows the title's
  presentation, so Kodi's VRR is frame pacing on that link (checked every 2
  seconds). The high-refresh preset's unpeg (`sceVideoOutVrrUnpegFromFixedRate`)
  returns `0x8029001c` on firmware 10.01, so Kodi does not use that route.
- **HDR.** Registering scanout buffers in the HDR format needs HDR-capable
  title metadata (`attribute` in `param.json`); with it, the PS5 keeps the TV in
  HDR for the whole session. Re-registering buffers in place is refused
  (`SLOT_OCCUPIED`); `sceVideoOutSubmitChangeBufferAttribute2` switches the
  format at the next flip.
- **GL driver.** 2D R8/RG8 textures are tiled and uploaded pixel by pixel, so
  video frames use rectangle textures (patch 0008); the driver reports wrong
  buffer ages, so Kodi redraws the whole screen each frame. Zero-copy video uses
  linear 2D textures over the decoder's memory (row pitch a multiple of 256
  bytes), which the driver additions make available as EGL images.
- **Dynamic linking.** A title cannot resolve symbols by name
  (`sceKernelDlsym` fails), and the loader leaves weak imports empty; functions
  the SDK's stubs lack (e.g. the VRR unpeg) come from an extended link stub.
</details>

## Debugging

```bash
nc <console-ip> 3232 | tee kodi-klog.txt          # capture while launching (klogsrv on the console)
grep -a "\[kodi" kodi-klog.txt | tail -100          # Kodi's log and the port's startup markers
```

A crash prints a report with `# backtrace:`. To turn its addresses into
function names (the build keeps symbols):

```bash
N=$(grep -a -n "# backtrace:" kodi-klog.txt | tail -1 | cut -d: -f1)
sed -n "$((N+1)),$((N+40))p" kodi-klog.txt | grep -a -o "^# [0-9a-f]\{16\}" | awk '{print $2}' |
  while read a; do printf '0x%x\n' $((0x$a - 0x400000 - 1)); done |
  llvm-symbolizer-18 --obj=$HOME/kodi-ps5-stage/app/build/llvm-pie.elf --demangle --inlining=false -p
```

## Roadmap

1. Internet access (curl/TLS): add-on repository, scrapers, online streams.
2. Python, then binary add-ons through a static registry.
3. The player debug overlay (L3) during VRR: keep the paced rate.
4. GL driver: cheaper clears and draws at 4K, runtime-selected render size.
5. A real DualSense joystick driver, on-screen keyboard.
6. TrueHD / DTS-HD passthrough (8 channels at 192 kHz), once the PCM path is
   confirmed bit-exact.

## Contributing

Bug reports with a klog capture (see Debugging) and your firmware/loader
versions are the most useful thing. Pull requests welcome. Keep line endings
LF (enforced by `.gitattributes`); the scripts are bash and break on CRLF.

Join the [Discord server](https://discord.gg/YB58bUZrqu) for help, test reports
and development news.

## Acknowledgements

- John Törnblom and contributors: [ps5-payload-dev](https://github.com/ps5-payload-dev)
  SDK, pacbrew-repo, ftpsrv, klogsrv.
- BlackBearReloaded: [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl),
  the PS5 native-app template, and the PS5 hardware video and audio decoding
  research (console-proven VideoDec2 modes, VP9, low-aligned 10-bit surfaces,
  the AudioOut formats).
- ProsperoLight: reference for direct-memory allocation, the high-refresh
  entitlement, VRR and the HDR scanout format on a PS5 title.
- The prosper project, for the VP9 codec value.
- Ronnie Sahlberg: [libsmb2](https://github.com/sahlberg/libsmb2).
- The PS5 SDL backend, whose observations of the audio and pad libraries the
  `sce/` headers restate.
- Team Kodi, for Kodi itself.

## License

GPL-2.0-or-later, the same license as Kodi (see [LICENSE](LICENSE)). Files under
`overlay/` carry Kodi's standard file header because they are written to be
upstreamed into Kodi's tree.

This project is for running free software on hardware you own. It does not
enable, and must not be used for, copyright infringement of any kind.
