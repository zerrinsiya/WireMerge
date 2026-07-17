# WireMerge

A lightweight Windows utility that lets you plug in USB audio sources
(phones, TVs, USB sound cards, etc.) and mix them into your PC's output,
similar to how Bluetooth audio pairing works, but over USB, and without
needing a full DAW or resource-heavy mixing app running.

## How it works:

The original plan called for reading raw audio directly off the USB bus
with `libusb`. In practice, once Windows recognizes a plugged-in device as
a USB Audio Class device, it already exposes it as a normal recording
device, so WireMerge captures audio through **PortAudio** (via WASAPI),
the same way any other Windows audio app would. `libusb` is used
separately, only for detecting when a USB device is plugged in / unplugged
so the GUI can tell you, and for basic vendor/product info. This is both
more realistic to build and lighter on the CPU than reimplementing USB
Audio Class transfers from scratch.

**Volume control:** by design, the external device (your phone/TV) is the
primary volume control, matching the Bluetooth-like behavior described in
the project goal. WireMerge passes audio through at unity gain by default;
each source also has an optional software "Trim" slider in the GUI for
balancing sources against each other, not for normal day-to-day volume use.

### A note on phones specifically

Plugging an Android phone in via USB and pressing play on Spotify does
**not** make it show up as a USB audio input — regular phones don't route
app playback over the USB audio-class endpoints the way a USB mic or DAC
does. That's not a WireMerge bug; it's how Android's USB stack works.

To actually capture a phone's app audio (Spotify, YouTube, etc.), WireMerge
uses the same technique [scrcpy](https://github.com/Genymobile/scrcpy) and
its companion tool [sndcpy](https://github.com/rom1v/sndcpy) use: `adb`
(Android Debug Bridge, over the same USB cable) installs a small helper
app that uses Android 10+'s Playback Capture API and streams the audio
back over an `adb forward` tunnel. This is a genuinely different mechanism
from the USB-audio-device path above, with its own setup step (below) and
its own caveats:

- Requires **Android 10 or newer** on the phone.
- Requires **USB debugging** enabled (Settings → About Phone → tap Build
  Number 7 times → Developer Options → USB debugging) and a one-time
  on-device authorization tap.
- Each time capture starts, Android shows its own on-device prompt asking
  you to confirm screen/audio capture, this is normal OS behavior, not
  something WireMerge can skip.
- Capture is opt-in per app. Most mainstream apps work; some DRM-guarded
  streaming apps block it regardless of what WireMerge or sndcpy do.
- iOS has no equivalent capability exposed to third-party tools, so this
  path is Android-only.
- This path has noticeably more latency (~500ms) than USB mic/DAC input,
  by design, it rides over an `adb forward` TCP tunnel rather than a
  native audio driver, which has real jitter that needs a bigger buffer
  to absorb cleanly. If a source's "Underruns" indicator (Active Sources
  panel) is climbing steadily, that's audible stutter from that source
  specifically; occasional small increases are normal, but if it's
  climbing fast, check the phone hasn't locked/backgrounded the capture.

**Setup for this feature** (optional — everything else works without it):

WireMerge ships this as a `tools/` folder sitting next to `WireMerge.exe`.
**If you're packaging a release to hand out to others, do this once per
release** so end users never have to touch adb or sndcpy themselves:

1. Download **platform-tools** (contains `adb.exe`) from
   https://developer.android.com/tools/releases/platform-tools — grab the
   Windows zip, extract it, and copy exactly `adb.exe`, `AdbWinApi.dll`,
   and `AdbWinUsbApi.dll` (the rest of that folder isn't needed).
2. Download the latest sndcpy release from
   https://github.com/rom1v/sndcpy/releases and extract `sndcpy.apk`.
3. Put all four files in `tools/`, next to `WireMerge.exe`:
   ```
   bin/
   ├── WireMerge.exe
   └── tools/
       ├── adb.exe
       ├── AdbWinApi.dll
       ├── AdbWinUsbApi.dll
       └── sndcpy.apk
   ```
4. Zip/distribute the whole `bin/` folder (or however you package
   releases) — `tools/` travels with it. Anyone you give this to just runs
   `WireMerge.exe`; they never download or configure anything.

**Safety net:** if `tools/` is missing or incomplete on launch (e.g. you
handed out a stripped build, or someone deleted the folder), WireMerge
will attempt a one-time automatic download of both files before falling
back to showing manual instructions in the Android panel. This requires
internet access and is meant as a rare-case fallback, not the primary
path, packaging `tools/` yourself (above) means end users should never
actually see it trigger.

## Project layout

```
WireMerge/
├── src/
│   ├── main.cpp              Entry point / startup sequence
│   ├── audio_handler.*       PortAudio wrapper (device I/O)
│   ├── usb_handler.*         libusb enumeration + hotplug detection
│   ├── adb_handler.*         Android app-audio capture via adb + sndcpy
│   ├── mixer.*                Thread-safe multi-source mixing
│   ├── gui.*                  Dear ImGui (Win32 + DX11) interface
│   ├── check_dependencies.*  Runtime dependency checks
│   └── utils.*                 Logging + simple config file
├── include/portaudio/         (fallback location if not using vcpkg)
├── include/libusb/            (fallback location if not using vcpkg)
├── CMakeLists.txt
└── README.md
```

## Setup

### 1. Install toolchain
- [MinGW-w64](https://www.mingw-w64.org/) (or MSVC — either works with CMake)
- [CMake](https://cmake.org/download/) 3.15+
- [vcpkg](https://github.com/microsoft/vcpkg) — recommended for dependencies

### 2. Install dependencies via vcpkg
```powershell
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install portaudio libusb
```

If you'd rather not use vcpkg, download prebuilt PortAudio and libusb
headers/libs manually and drop them into `include/portaudio/` and
`include/libusb/` respectively (matching the fallback paths in
`CMakeLists.txt`).

Dear ImGui is fetched automatically by CMake (`FetchContent`) — no manual
setup needed.

### 3. Configure and build
```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[path to vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The executable will be at `build/bin/WireMerge.exe`.

### 4. Run
On first launch, WireMerge checks that `portaudio.dll` and
`libusb-1.0.dll` are reachable (next to the exe or on PATH) and tells you
exactly what to install if not, it will not silently download or run
installers on your behalf.

## Usage
1. Plug in your USB audio source (phone/TV/USB sound card). Windows should
   install a driver automatically and show it as a recording device.
2. Open WireMerge, pick your speakers/headphones under **Output**, click
   **Start Output**.
3. Under **Input**, select the newly connected device and click
   **Add Source**. Repeat for additional sources.
4. Control volume from the source device itself, same as you would over
   Bluetooth.

## License
See [`LICENSE`](https://github.com/zerrinsiya/WireMerge/blob/3700f7db4441e250e11a3fbdb8e0a797f4465369/LICENSE).
