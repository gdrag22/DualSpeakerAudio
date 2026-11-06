# DualSpeakerAudio

A lightweight Windows utility that plays your default system audio through a
second output at the same time. Use it to listen through two monitor speakers,
or another pair of Windows playback devices.

Written in native C++ using Windows WASAPI. No additional runtime installation
or administrator privileges are required.

## Download

Download **DualMonitorAudio.exe** from the **Assets** section of the
[latest release](https://github.com/gdrag22/DualSpeakerAudio/releases/latest).
The executable runs by itself; there is nothing to install.

**Requirements:** Windows 10 version 1703 or later, or Windows 11, on an x64 PC,
with two connected and enabled audio outputs.

## Getting started

1. In Windows sound settings, choose your main speaker or monitor as the default
   output device.
2. Run `DualMonitorAudio.exe`.
3. On first launch, enter the number for your secondary output from the list.
4. Play audio normally. Both outputs should now play the default device's audio.

Your secondary selection is saved automatically. Future launches start forwarding
audio using that selection. Keep the console open while listening; press
**Ctrl+C** or close the window to stop. Your Windows default output is unchanged.

The program does not launch automatically with Windows.

## Change the secondary output

Close the running copy, open PowerShell in the folder containing the executable,
and run:

```powershell
.\DualMonitorAudio.exe --configure
```

Only one copy can run per Windows session.

## Other commands

Run these from PowerShell in the executable's folder:

| Command | Action |
| --- | --- |
| `.\DualMonitorAudio.exe` | Start forwarding, or select a secondary output on first launch. |
| `.\DualMonitorAudio.exe --configure` | Select and save a different secondary output, then start forwarding. |
| `.\DualMonitorAudio.exe --list` | List active playback devices. |
| `.\DualMonitorAudio.exe --check` | Initialize the saved route without starting playback, then exit. Prompts for selection if none is saved. |
| `.\DualMonitorAudio.exe --help` | Show command-line help. |

## What to expect

- Supports one primary output and one secondary output.
- Captures audio played through the Windows default console-role playback device.
  Apps explicitly routed to another device are outside that mix.
- Handles different device sample rates and compensates for gradual clock drift.
- A small delay between outputs is possible. Independent devices and monitor
  audio processing cannot be perfectly synchronized.
- If a device becomes unavailable, the program waits and retries. Reconnecting
  the same device should allow forwarding to resume.
- If the selected secondary becomes the Windows default, forwarding pauses to
  avoid feedback. Change the default back or select a different secondary.

## Troubleshooting

**The second output is missing:** Make sure it is connected, enabled in Windows
sound settings, and has its volume turned up. Then run `--configure` again.

**The second output is silent:** Check both Windows and monitor volume/mute
settings, and confirm the source app plays through the Windows default output.
Protected content and exclusive-mode audio may not be captured.

**The saved device is no longer available:** Run `--configure` to select it again.
Driver changes can give an output a new device ID.

**Reset the saved selection:** Close the program and delete
`%LOCALAPPDATA%\DualMonitorAudio\device.txt`. The next launch prompts for a device.

**Windows displays an unfamiliar-app warning:** The executable is unsigned.
Download it from this repository's releases and verify the source before deciding
whether to run it.

For problems, [open an issue](https://github.com/gdrag22/DualSpeakerAudio/issues)
with your Windows version, output device names, and any terminal messages.

## Build from source

Install Visual Studio 2022 or later with **Desktop development with C++**, the
Windows SDK, and CMake 3.20 or later. From an x64 Developer Command Prompt in the
repository folder:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is created at `build\Release\DualMonitorAudio.exe`. MSVC builds
statically link the C++ runtime, so users do not need to install the Visual C++
redistributable.
