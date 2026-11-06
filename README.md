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

### Adjust the audio buffer

If the secondary output has too much delay, or you hear crackling or dropouts,
you can adjust the application's buffer target. This requires the source code
and the build tools above; there is no command-line buffer setting.

The buffer provides headroom for variations in audio delivery. It does not need
to match a device's sample rate. Smaller targets can reduce delay but make
dropouts more likely; larger targets provide more headroom at the cost of delay.

1. Close the running application.
2. Open `src/main.cpp` and find this line:

   ```cpp
   const auto target = static_cast<size_t>(sampleRate_ * std::max(0.015, periodSeconds));
   ```

3. Change **`0.015`**, which is a duration in seconds. For example:

   | Value | Buffer target | When to try it |
   | --- | --- | --- |
   | `0.010` | 10 ms | Try less delay if playback is stable. |
   | `0.015` | 15 ms | Current default. |
   | `0.020` | 20 ms | Try more headroom for crackling or dropouts. |
   | `0.030` | 30 ms | Try more headroom if 20 ms is insufficient. |

   Keep `periodSeconds` in the expression. `std::max` uses whichever is larger:
   your chosen duration or one device period. A slower device can therefore
   prevent a smaller value from reducing the effective target.

4. Save the file, then run these commands from the repository folder:

   ```bat
   cmake -S . -B build -A x64
   cmake --build build --config Release
   ```

5. Launch the newly built **`build\Release\DualMonitorAudio.exe`**. An executable
   downloaded earlier or copied to another folder will still use its old setting.
   The startup message shows the effective jitter-buffer target in milliseconds.

Change one value at a time and test with normal playback, including games and
pausing/resuming audio. If a smaller target causes problems, restore the previous
value and rebuild. Leave the FIFO capacity expression on the following line
unchanged; it limits queued audio rather than setting the normal target delay.

The displayed buffer target is only part of the total delay. Windows audio
processing and monitor hardware add latency too, so adjusting this value cannot
guarantee perfect synchronization. Repeated startup messages indicate the audio
route is restarting; increasing the buffer does not address that cause.

### If `cmake` is not recognized

This means the terminal cannot find `cmake.exe`. It is a build-tool setup issue;
people downloading the ready-to-run executable do not need CMake.

If CMake is installed with Visual Studio, its `bin` folder may not be on your
PATH. For example, Visual Studio 2022 Professional may install it at:

```text
C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
```

Your path may differ by Visual Studio version, edition, and installation location.
Confirm that the folder contains `cmake.exe` before adding it.

To make it available in future terminals:

1. Search Windows Start for **Edit environment variables for your account**.
2. Under **User variables**, select **Path**, then click **Edit**.
3. Click **New**, paste the folder containing `cmake.exe`, and confirm with **OK**
   in each dialog. Add the folder, without quotes; do not replace existing entries.
4. Close and reopen your terminal. If using VS Code's integrated terminal, fully
   restart VS Code so it receives the updated PATH.
5. Run `cmake --version` to verify that Windows can find it, then retry the build
   commands above.

If CMake is not installed, add **C++ CMake tools for Windows** through the Visual
Studio Installer, or install CMake separately and select its option to add CMake
to PATH. CMake alone does not install the C++ compiler or Windows SDK.
