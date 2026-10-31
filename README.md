# PicoET-Enhance-MouthLink

English | [简体中文](README_zh.md)

> **This is a fork of [WolalaQAQ/PicoET-Enhance](https://github.com/WolalaQAQ/PicoET-Enhance)**
> (branch `mouthlink`, module version `2.4.2-ml4`): upstream v2.4.2 plus an opt-in
> **MouthLink** source for custom mouth tracking.
>
> - Raw mouth-camera stream (400×400 IR, 90 Hz) to a PC over UDP, captured inside the
>   existing payload — no second injector and no extra camera client.
> - Optional per-frame stock mouth labels (network output, unsmoothed and smoothed
>   values) carrying the same timestamp as the camera frame, for training a custom model.
> - Disabled by default. Eye tracking and stock mouth output are unchanged.
> - Only private-network PCs may subscribe by default; an allow list narrows it to your PC,
>   and a running stream cannot be taken over by another address (ml4).
>
> The PC side (receiver, recording, dataset builder, training) is
> [MouthLink](https://github.com/UndefinedSignal/MouthLink). Details:
> [Optional MouthLink RAW mouth-camera stream](#optional-mouthlink-raw-mouth-camera-stream).
> The rest of this README is the upstream documentation. Please report MouthLink issues
> in this fork, not upstream.

An eye-tracking enhancement Magisk module for the PICO 4 Pro: it opens the eye-tracking service's per-eye gate for real pupil data, decouples the facial expression model so each eye is processed on its own, and targets 90 Hz for both eye and mouth expressions by default instead of the stock rate of about 24 Hz. Use it with [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) and [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule).

> v2.4.2 · PICO 4 Pro / PICO OS 5.13.7 · [Changelog](CHANGELOG.md)

## What it does

The PICO 4 Pro firmware tracks each eye, but its facial expression model mixes the two eyes before output, so blink, look direction, wideness and brows for one eye depend on the other eye's inputs. It also updates these expressions only about 24 times a second, although the eye cameras run at 90 Hz. This module changes three things at runtime:

- **Real pupil data**: it opens the per-eye gate inside the eye-tracking service, so real pupil diameter data is written to shared memory.
- **Independent eyes**: it removes the expression model's cross-eye coupling. EyeLook, Blink, Wide and the brow channels are computed and filtered per eye, each with its own quality window, reference image and history. The filtered right inner brow and the per-eye quality states are exported as an `EPI1` extension, which PicoFacialBridge forwards as `X` packets.
- **90/90 Hz eye and mouth by default**: the eye and mouth expression paths each run on every eligible fresh camera input, and the two inferences for a frame run in parallel. Lower rates are optional ([Eye and mouth rates](#eye-and-mouth-rates)). To carry the high rate all the way to VRChat, see [About 90 Hz OSC](#about-90-hz-osc).

Smoothing, mouth baseline adaptation and quality windows follow each source's own timestamps: a long gap pauses the quality window, and a repeated or backward source does not advance anything.

The inference models and combined gaze stay stock. Each eye keeps blink endpoints, filtering and the within-eye inner/outer brow linkage, and mouth postprocessing keeps the stock rules. Dark-image admission, reference protection and model quantization remain enabled. On the PC, eye direction and eyelid openness use EyeLook and `1−Blink`; native eye data supplies measured pupil diameter only.

The enhancement runs as hooks in the eye-tracking service and is unloaded when that service restarts.

## How it fits together

```text
PicoET-Enhance (headset, this module)
        ↓ active at runtime, advertises the mode
PicoFacialBridge (headset)
        ↓ BridgeSplit, LAN UDP 9030
UnifiedPicoModule (PC)
        ↓
VRCFaceTracking → VRChat OSC
```

PicoFacialBridge reads the module's transient mode properties and automatically tells the PC whether normal or enhanced mode is active.

The high-rate path uses `EPI1` / `X` v2 and needs PicoFacialBridge 0.5.0 or later and UnifiedPicoModule 0.4.0 or later. The earlier PicoET-Enhance 2.2.0 with PicoFacialBridge 0.4.0 and UnifiedPicoModule 0.3.0 provides per-eye independence at the stock expression cadence using v1. Older Bridge versions (0.3 and up) still work in normal mode without this module.

### About 90 Hz OSC

The eye path delivers about 90 Hz to the PC, but the official VRCFaceTracking host passes it on to OSC at a lower rate: it reads module output and writes OSC on fixed 10 ms waits. We measured this on a PICO 4 Pro streaming through Virtual Desktop, replaying 2,700 recorded eye frames at their original 90 Hz timing and counting new eye directions at the OSC output:

| VRCFT host (5.4.5, Steam build `7add148`) | New eye directions per second |
|---|---:|
| Official, unmodified | about 61–78 Hz |
| Patched, 4 ms waits | about 87–88 Hz |

Windows 11 may ignore the 1 ms timer resolution the official host asks for. When it does, each 10 ms wait takes about 15 ms, which is why the official result varies from run to run.

These figures came from a Ryzen 7 9700X, RTX 4070 Ti SUPER, 64 GB RAM and Windows 11 Pro for Workstations on the Ultimate Performance power plan. Other PCs may see different numbers.

The module works with the official VRCFaceTracking as is. If you want OSC close to 90 Hz, use [VRCFT-Enhance](https://github.com/WolalaQAQ/VRCFT-Enhance). It has the host and module processes keep Windows from ignoring their timer resolution, turns the two fixed waits into settings (4 ms by default), and fixes a few unsynchronized data exchanges in the host that shorter waits make more likely to collide. It runs from a separate copy of VRCFaceTracking; releases include the two patched DLLs, and the repository also keeps the source patch if you prefer to build them yourself.

## Requirements

| Item | Requirement |
|---|---|
| Headset | PICO 4 Pro |
| Firmware | PICO OS 5.13.7 (Android 10 / API 29, arm64-v8a) |
| Root | Magisk installed |
| Companion components | PicoFacialBridge and UnifiedPicoModule, to feed the data into VRCFaceTracking |

The installer verifies the SHA-256 of the device's eye-tracking libraries and stops if the firmware build does not match.

## Installation

1. Download `magisk-picoet-enhance-v<version>.zip` from [Releases](https://github.com/WolalaQAQ/PicoET-Enhance/releases).
2. Flash it in the Magisk app under Install from storage, or from a command line:

   ```sh
    adb push magisk-picoet-enhance-v2.4.2.zip /data/local/tmp/
    adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.4.2.zip'
   ```

3. Reboot the headset.

Fresh installs enable enhanced mode (`dual/on`) at 90/90 Hz after reboot. If you switched to normal mode before upgrading, it stays off.

## Usage

Run as root on the headset:

```sh
sh /data/adb/modules/picoet-enhance/picoet.sh status
sh /data/adb/modules/picoet-enhance/picoet.sh dual
sh /data/adb/modules/picoet-enhance/picoet.sh off
sh /data/adb/modules/picoet-enhance/picoet.sh toggle
```

| Command | Effect |
|---|---|
| `status` | Shows the saved and applied mode, injection state, service PID, configured effective eye/mouth targets and the current `ro.pxr.externalfunc` value |
| `dual` | Enhanced mode: sets `dual/on` together, opening the pupil gate and enabling the per-eye independent expressions |
| `off` | Normal mode: sets `off/off` together and restarts the service without injection; fully stock |
| `toggle` | Switches to normal mode if enhancement is ready; otherwise attempts to enable enhancement |

Switching restarts the eye-tracking service and disconnects eye and face tracking clients; command-line switching requires `--force` while clients are connected. The Magisk app's Action button switches directly between enhanced and normal mode. After an enhancement failure, pressing it again retries enabling.

`on` is accepted as a synonym for `dual`. The state files are `/data/local/tmp/picoet-mode` and `/data/local/tmp/picoet-gate`, and the log is `/data/local/tmp/picoet-hook.log`.

### Automatic loading and recovery

Automatic injection waits for `sys.boot_completed=1` and a Binder response from the same eye-tracking service instance. Before starting Bridge or another tracking client in enhanced mode, check that `picoet.sh status` shows `phase=ready applied_mode=dual applied_gate=on`.

`ready` confirms the installed hooks and configured target rates. Engines initialized before the hooks are kept running: profile 0 and startup ET settings are not applied retroactively, and the enhancement's own state warms up again rather than forcing an engine rebuild.

If no safe executor is available, or a temporary signal stop interrupts preparation, the injector returns `3`. The watchdog releases the control lock and waits 30 seconds before retrying, without consuming either budget. Synchronous and asynchronous unsafe failures share one automatic recovery per boot or explicit mode command. A separate limit permits injection into at most three consecutive unstable service instances; only the same instance remaining continuously `ready` for 60 seconds replenishes that limit. When a budget is exhausted, automatic injection stops until the next boot or an explicit mode command.

### Eye and mouth rates

The defaults are 90 Hz for eyes and 90 Hz for mouth, and no configuration is needed to use them. To lower either one, write a positive integer to its file:

| File | Default | Effect |
|---|---:|---|
| `/data/local/tmp/picoet-eye-hz` | 90 | Eye-expression rate; 90 processes every eligible fresh camera input |
| `/data/local/tmp/picoet-mouth-hz` | 90 | Mouth-expression rate; cannot exceed the eye rate, because every mouth frame also computes the eyes |

Values above 90 are capped at 90, and a missing or invalid file uses the default. Frames outside the chosen rate are skipped rather than computed. The files are read once when the service starts, so apply a change by stopping tracking clients such as Bridge and re-running `dual`, which restarts the service. For example, to keep eyes at 90 Hz and run the mouth at the stock 24 Hz:

```sh
echo 90 > /data/local/tmp/picoet-eye-hz
echo 24 > /data/local/tmp/picoet-mouth-hz
sh /data/adb/modules/picoet-enhance/picoet.sh dual
sh /data/adb/modules/picoet-enhance/picoet.sh status
```

`status` shows the configured rates as `effective_eye_hz` and `effective_mouth_hz`; these are the targets in effect, not measured output rates. To return to 90/90, delete both files and run `dual` again.

Rates set in 2.4.0 or later survive reinstalls and upgrades. Only the first migration from a version before 2.4.0 clears old overrides to start at 90/90. Normal mode (`off`) is fully stock regardless of these files.

## Optional MouthLink RAW mouth-camera stream

This fork adds an opt-in MouthLink source to the existing payload; it does not attach a second injector or open another camera client. The existing FaceTrackor dispatch supplies the already-paired lower-face `cv::Mat` and its original mouth source timestamp. One-channel `CV_8U` and `CV_16U` frames are copied with row padding removed and sent as uncompressed MouthLink `GRAY8` or `GRAY16LE`. Full frames and the subscription ROI are supported; fragments, CRC32, PING/PONG, DISCOVER/HELLO and the three-second subscription lease follow MouthLink protocol v1.

MouthLink is disabled by default and no image is sent before `SUBSCRIBE`. It requires enhanced `dual` mode because capture uses the expression hook. Enabling or disabling it restarts tracking:

```sh
adb shell su -c 'sh /data/adb/modules/picoet-enhance/picoet.sh dual --force'
adb shell su -c 'sh /data/adb/modules/picoet-enhance/picoet.sh mouthlink-on --force'
adb shell su -c 'sh /data/adb/modules/picoet-enhance/picoet.sh status'
```

Then run `MouthLink.Receiver --host <headset-IP>` on the PC and open its viewer (by default `http://localhost:8080/`). Use `mouthlink-off --force` to disable the source. The control port defaults to UDP 9040 and can be changed only before service startup with `/data/local/tmp/picoet-mouthlink-port`.

### Who may subscribe (ml4)

The camera stream is a live image of your face, so control packets (`SUBSCRIBE`, `UNSUBSCRIBE`, `PING`, `DISCOVER`) are accepted only from an allow list read at service start from `/data/local/tmp/picoet-mouthlink-allow`:

- `private` (default, also when the file is absent): 10/8, 172.16/12, 192.168/16, 169.254/16, 127/8 and 100.64/10 (CGNAT, Tailscale). A public source address is never answered, which also keeps a spoofed 16-byte `SUBSCRIBE` from aiming the ~118 Mbit/s stream at a public host.
- IPv4 addresses and CIDR ranges, separated by commas or spaces, e.g. `192.168.1.10` or `192.168.1.0/24,10.0.0.5`. Recommended: just your PC.
- `any`: explicit opt-out of the filter.
- A list that does not parse allows nothing (fail closed); the hook log says so.

```sh
adb shell su -c 'sh /data/adb/modules/picoet-enhance/picoet.sh mouthlink-allow 192.168.1.10 --force'
adb shell su -c 'sh /data/adb/modules/picoet-enhance/picoet.sh mouthlink-allow private --force'
```

The command saves the list and, when MouthLink is on, restarts tracking to apply it; `status` shows `mouthlink_allow=`. While one address holds the stream (its lease is renewed every second), a `SUBSCRIBE` from another allowed address is ignored instead of redirecting the stream, and `UNSUBSCRIBE` is accepted only from the subscriber. After the owner stops, the next PC can subscribe within 3 seconds. Rejected packets are counted (`rejected=` in the hook statistics) and logged at most once per 10 seconds.

The stream itself is not encrypted: anyone who can read the Wi-Fi traffic (for example with its password) can see it. Use a trusted network and never forward UDP 9040/9041 from the internet.

Network sending runs on a worker thread. The camera thread uses a non-blocking slot handoff and never waits for UDP; overload drops frames rather than delaying inference. Frames larger than 4 MiB, non-monochrome or other OpenCV depths are rejected. No JPEG/LZ4 conversion is performed. Raw images remain biometric data: do not commit recordings.

The host-only synthetic transport test does not need Android or PICO libraries:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ihook/src \
  hook/src/mouthlink/mouthlink.cpp hook/tests/mouthlink_test.cpp \
  -pthread -o /tmp/mouthlink_test
/tmp/mouthlink_test && /tmp/mouthlink_test default-allow && /tmp/mouthlink_test fail-closed
```

The subscription frame-rate limit (`SUBSCRIBE` fps) sends every camera frame at 0 or ≥ 90; lower rates use a source-clock grid with a quarter-period slack, so a 90 Hz camera gives exactly 60, 45 or 30 fps (ml2).

### STOCK labels (ml3)

A subscriber that sets bit 0 of `SUBSCRIBE` byte 14 also receives one 424-byte `STOCK` datagram for every newly accepted mouth source. It carries the stock network output (35 values), the 33 mouth values after stock post-processing but before temporal smoothing, the 33 smoothed values that are exported to the bridge, and the valid / warm-up / quality state. Its timestamp equals the capture timestamp of the camera frame, so frames and labels pair exactly. Records are queued from the expression thread under `trylock` and sent by the MouthLink thread; tracking never waits, and the stock outputs themselves are not changed. `HELLO` is 9 bytes, with capability bit 0 = STOCK. Older subscribers that send a 14-byte `SUBSCRIBE` never receive STOCK.

### Firmware and validation

The firmware guards remain unchanged: this build targets the same library hashes as PicoET-Enhance 2.4.2 (PICO OS 5.13.7). It was validated on a PICO 4 Pro running PICO OS 5.13.3S, whose eye-tracking libraries are byte-identical (the installer's hash check passes). Do not flash it onto another firmware merely because an older PicoET build was injectable there; the matching source offsets and hashes must be ported first.

Measured on that headset: mouth camera 400×400 GRAY8 at 90 fps, ≈118 Mbit/s uncompressed with ~20 ms latency next to Virtual Desktop; stock eye and mouth expressions stay at 90/90 Hz; STOCK labels paired with 100 % of received frames.

## About the enterprise gate

The firmware uses `PXR::EyeUtil::isBusinessDev()` to control per-eye output, including the real pupil diameter. This module makes the function return true inside the eye-tracking service. The device-wide enterprise-mode property `ro.pxr.externalfunc` keeps its existing value.

## Compatibility and verification

The startup-safety changes were validated with a local NDK 26.1.10909125 build on PICO 4 Pro / PICO OS 5.13.7 with SELinux permissive: normal boot, upgrade, mode switching, repeated control commands and targeted failure branches passed. A 45-second worn session confirmed live eye and mouth updates, blink and mouth response, profile 0 initialization without retries, and normal tracking shutdown without a service restart. The stock libraries were unchanged. SELinux enforcing has not been tested. The 2.4.2 workflow explicitly selects NDK 26.1.10909125 and CMake 3.22.1.

The following 90/90 measurements are from the earlier 2.4.0 validation, not a new throughput test for 2.4.2. The PICO 4 Pro (PICO OS 5.13.7) was awake with tracking ready and not worn. The test replayed 2,700 recorded camera inputs (about 30 seconds) through a test harness that drives the module's expression-processing algorithm, with PicoFacialBridge 0.5.0, UnifiedPicoModule 0.4.0, VRCFT-Enhance and desktop VRChat:

| Stage | Rate |
|---|---:|
| Eye and mouth computation on the headset | 89.80 Hz (2,694 of 2,700 inputs; the other six hit the stock full-queue drop rule, with no inference failures) |
| Bridge → PC (`X` packets) | 89.84 Hz |
| VRCFT → OSC, eye and mouth from the same source | 87.80 Hz (2,633 sources, 97.5% of inputs) |

For every source that reached OSC, VRChat sent back at least one eye and one mouth value equal to the one written. Values matched their source at every stage; this covers the supported fields the test avatar actually sends. The loopback packet capture dropped packets, so the PC-side counts are verifiable lower bounds. From `X` arrival at the PC to OSC output, the median is 9.1 ms and the 95th percentile 12.5 ms. The replay was not run through the installed Magisk package, and it does not include live camera copying or Virtual Desktop decoding and rendering.

On version 2.3.0 (mouth at about 24 Hz), official VRCFT 5.4.5 was also run for 84 seconds worn on Virtual Desktop with Cymple mouth tracking and PICO eyes: about 89.7 `X` packets per second reached the PC, each with a new eye source.

Not yet covered: single-eye quality failure, staggered reference rebuild, a live mouth-output comparison and long live-camera sessions. Combined gaze remains unchanged, and the host-side brow parameters used by current avatars average the two sides even though the extension carries both.

## Disabling and uninstalling

Run `picoet.sh off` to disable enhancement and restore the stock eye-tracking service. To remove the module, uninstall it in Magisk and reboot the headset. Cleanup removes known state files without recursively entering unexpected directories or nested mounts, so those directories may remain.

## Building from source

Building requires the Android NDK and CMake/Ninja. The scripts look for the toolchain in the `-Ndk` / `-CmakeBin` parameters, the `PICOET_NDK` / `PICOET_CMAKE_BIN` environment variables, or a standard layout under `ANDROID_HOME`.

```powershell
pwsh -File hook\build-hook.ps1                 # build the injector and payload
pwsh -File build-picoet-enhance.ps1            # package the Magisk zip
```

The output is `artifacts/magisk-picoet-enhance-v<version>.zip`, containing the module scripts, injector and hook payload. Packaging checks the ELF file list and stock-library hashes.

Pushing a `v<version>` tag makes GitHub Actions build on a Windows runner and publish the zip with a checksum.

## TODO

- [ ] Tongue-direction support: PICO currently exposes only the scalar `TongueOut`, while VRCFaceTracking already defines direction channels such as `TongueLeft/Right/Up/Down`. The plan is to capture raw frames from the lower-face camera, auto-label them with visual segmentation foundation models, train a small model, and let users collect their own data for a local online fine-tune.
- [ ] Unfiltered face output: make the on-device FaceTrackor temporal smoothing optional so the module can forward per-frame raw face data. Filtering then moves to the PC side, where it can be tuned as needed.

## Feedback

Issues are welcome. Please include the headset model, PICO OS version, module version, the current mode, the output of `picoet.sh status`, and relevant log excerpts.

## License

The original code and scripts of this project are licensed under [MIT](LICENSE). The vendored ShadowHook 2.0.1 sources include MIT- and BSD-licensed dependencies; the Magisk ZIP includes their notices and the project license. This project contains and distributes no PICO binary; it only references the device's own libraries by numeric offset at runtime. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
