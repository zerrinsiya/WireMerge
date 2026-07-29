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

## Code signing policy

This app isn't signed yet but is planned to get apply for a certificat from [SignPath.io](https://signpath.io/),
certificate hopefully to be provided by SignPath Foundation.

## Testers
Thanks to everyone who tested WirMerge:
- Brook - hailegna (tester) (Logo artist)
- urlate (tester)
- kldprm (tester)

*This project is run by Zerrin Siya as the sole Maintainer and reviewer.*
