# WireMerge

WireMerge is a Windows audio mixer that merges multiple input sources,
including Android phones, into a single output stream. 
The main feature is that it uses just a USB cable and ADB.

<img width="1366" height="738" alt="image" src="https://github.com/user-attachments/assets/6a6446c1-7b53-4786-861c-7b8d60c00310" />

## Features

| Source | Description |
| --- | --- |
| Microphones | Any input device visible to your system |
| Android (USB) | Captures system/app audio from an Android 10+ phone over ADB, no root required |
| Mixer output | Combines every active source into one virtual output device |

WireMerge ships as a single static `.exe`. Standalone for simplicity.

## Requirements

- Windows 10 or later
- An Android 10+ phone if you want to mix in phone audio (Or phones before Android 8)
- USB debugging enabled on the phone (for the ADB source)

## Setup

1. Download the latest release from the [Releases page](https://github.com/zerrinsiya/WireMerge/releases)
2. Extract from the .zip
3. Run the only executable `WireMerge.exe`, no install step
4. Add your input sources from the Sources panel OR
5. Connect your phone by USB and enable USB debugging if you want to mix in Android audio
6. Press Start Output

## Building from source code

WireMerge is built with CMake (a fully static build)
`x64-mingw-static` binary.

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

Dependencies (via vcpkg):

```
vcpkg install portaudio libusb
```

## Project structure

| Path | Description |
| --- | --- |
| `src/audio_handler` | PortAudio device I/O |
| `src/adb_handler` | Android capture over ADB/USB |
| `src/usb_handler` | USB device detection |
| `src/mixer` | Combines input sources into the output stream |
| `src/gui` | Dear ImGui interface |
| `src/layout` | Window and panel layout |

## A note on distribution

Before getting into the specifics below: the goal here is to be fully upfront
with you about how WireMerge is currently distributed and why, rather than
leave anyone guessing.

WireMerge is not code-signed. Signing through [SignPath.io](https://signpath.io/)
was the plan, but it could not be completed due to insufficient funds on the
maintainer's end. As a result, some antivirus tools and Windows SmartScreen
may flag the regular build.

Because of this, WireMerge is distributed as two downloads from the
[Releases page](https://github.com/zerrinsiya/WireMerge/releases):

| Build | Description |
| --- | --- |
| `WireMerge` | The regular build, includes everything, including a feature that automatically downloads the small Android capture tools (adb.exe, sndcpy.apk) if they're missing. |
| `WireMerge-Stripped` | Same app, same features. The only difference is that it does not download those tools automatically; you place them in a `tools` folder yourself. This is the build most likely to avoid antivirus/SmartScreen flags, since the automatic download is what appears to trigger most of them. |

Both builds are fully functional. If you'd rather not have WireMerge reach
out to the internet on its own for anything, or your antivirus is flagging
the regular build, use the Stripped Build instead.

## Testers
Thanks to everyone who tested WireMerge:
- Brook - hailegna (tester) (Logo artist)
- urlate (tester)
- kldprm (tester)

*This project is run by Zerrin Siya as the sole Maintainer and reviewer.*
