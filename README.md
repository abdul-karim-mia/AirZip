# AirZip

A lightweight Windows archive extractor with zero runtime dependencies.

## Overview

AirZip is a standalone archive extractor built with raw Win32 C++ and 7-Zip's COM interfaces. It ships as a single 2.0 MB executable with no managed runtime, .NET, or external dependencies beyond Windows system DLLs.

Originally developed as a .NET 8 WinForms application (7.4 MB, requiring the .NET 8 Desktop Runtime), it was rewritten in C++/Win32 to eliminate the runtime requirement and reduce the binary size by 73%.

## Key Properties

- Single executable, ~2.0 MB
- Static C runtime (no CRT installation needed)
- Windows system DLLs only: ole32, oleaut32, shell32, dwmapi, uxtheme, winmm, advapi32, user32, gdi32, kernel32
- 7z.dll (1.8 MB) embedded as an RCDATA resource and unpacked to %TEMP%\AirZip on first run
- Per-monitor DPI aware, long-path aware, UTF-8 code page aware

## Features

- **Smart folder extraction**: Archives with a single root item extract beside the source file. Archives with multiple root items extract into a subfolder named after the archive.
- **Multi-volume support**: Handles numbered split archives (.7z.001, .7z.002, etc.) transparently through a spanning stream.
- **Theme integration**: Follows Windows light/dark theme with live switching. Progress bar accent color reads from DWM settings.
- **Delayed UI**: Progress window appears only if extraction is still running after 400 ms, avoiding flashes on fast operations.
- **Progress feedback**: Displays current filename, percentage complete, and bytes done/total.
- **Completion chime**: System sound (SystemAsterisk) respects the user's Sound scheme settings.
- **Password protection**: Dialog prompt for encrypted archives (code path present; end-to-end testing pending).
- **Zip-slip defense**: Path validation rejects absolute paths and ".." escape attempts.

## Usage

Extract an archive by passing its path, or run one of the installation commands. The executable is a GUI-subsystem binary, so cmd.exe does not wait for it. Use `start /wait` (cmd) or `Start-Process -Wait` (PowerShell) when the exit code matters.

```cmd
AirZip.exe <archive>
AirZip.exe --install
AirZip.exe --silent-install
AirZip.exe --uninstall
AirZip.exe --silent-uninstall
AirZip.exe --version
AirZip.exe --help
```

| Option | Result |
|--------|--------|
| `<archive>` | Extract the archive |
| `--install` | Install and register file types (prompts for elevation) |
| `--silent-install` | Install with no dialogs |
| `--uninstall` | Uninstall and restore previous handlers (prompts for elevation and confirmation) |
| `--silent-uninstall` | Uninstall with no dialogs |
| `--version` | Print version |
| `--help` | Print usage |

**Exit codes**: 0 (success), 1 (failed), 2 (bad usage), 3 (file not found), 4 (not elevated).

**GUI mode caveat**: When installed in Program Files, uninstall is asynchronous. The running exe cannot delete itself, so the work is handed to a temporary clone and this process returns immediately.

## Installation and Uninstallment

Running without arguments (double-click) or with `--install` registers AirZip as the handler for 12 archive extensions: .zip, .rar, .7z, .tar, .gz, .bz2, .xz, .iso, .cab, .lzma, .wim, .arj.

On install, the previous handler for each extension is backed up in HKEY_LOCAL_MACHINE\SOFTWARE\AirZip\Associations. On uninstall, the handlers are restored from that backup, or if the backup is missing, AirZip attempts to restore a classic ProgID from OpenWithProgids (e.g., CompressedFolder for .zip).

An entry is added to Add/Remove Programs (HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AirZip).

## Building from Source

### Requirements
- Visual Studio 2022 Build Tools (or Community/Professional/Enterprise)
- C++ workload installed
- Windows SDK

### Build

```cmd
cd native
build.cmd
```

The script probes standard Visual Studio installation roots (avoiding vswhere due to cmd.exe parentheses parsing issues) and produces `native\out\AirZip.exe`.

**Build prerequisites**: The 7z.dll file must exist at the repository root (it is embedded into the exe by the resource compiler).

## Known Limitations

### Untested
- **Password-protected archives**: The password prompt code path is present and renders, but has never been tested end-to-end due to lack of an encrypted sample archive.
- **.rar, .iso, .cab, .wim, .arj extraction**: Wired up via 7-Zip handlers but untested (no sample archives available).

### Verified Formats
Confirmed to work in testing: .zip, .7z (including split volumes), .tar, .tar.gz.

### Platform
x64 only.

### Caveat on Uninstall
On uninstall, .arj, .lzma, and .wim may end up unassociated if only an AppX package identity was available to restore (AppX identities do not work as plain file-type defaults). This is a limitation of the restore heuristic, not of extraction.
