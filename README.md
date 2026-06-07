# Flip Config Bypass

## About

Flip Config Bypass is a small Windows tray utility for disabling NVIDIA flip metering in whitelisted x64 applications.

It watches a user-managed whitelist, injects a tiny payload DLL into matching processes, and makes `NvAPI_D3D12_SetFlipConfig` unavailable by returning `nullptr` for NVAPI interface ID `0xF3148C42`.

The tool is intentionally narrow:

- no `dxgi.dll` proxy
- no overlay
- no game file replacement
- no NVIDIA SDK dependency
- no external hook library
- no anti-cheat or protected-process bypass

## Download

Builds are produced by GitHub Actions.

Open the repo's **Actions** tab, run **Build**, then download the `FlipConfigBypass-x64` artifact.

The artifact contains:

```text
FlipConfigBypass.exe
FlipConfigPayload.dll
```

Keep both files in the same folder.

## Usage

Run `FlipConfigBypass.exe`. The app sits in the system tray and creates these files beside the EXE if needed:

```text
whitelist.txt
FlipConfigBypass.log
```

Right-click the tray icon to:

- edit the whitelist
- open the log in your default editor
- pause watching
- toggle Start with Windows
- exit

Whitelist entries can be executable names or full paths:

```text
GTA5.exe
C:\Games\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe
```

Filename entries are usually easiest. Full-path entries are supported when you want to target one exact install location.

## Logging

The log records meaningful events only, such as:

```text
[14:32:01] GTA5.exe (PID 9432) - injected OK
[14:42:55] ProtectedGame.exe (PID 2216) - failed, access denied
```

Non-whitelisted processes are not logged.

## Start With Windows

`Start with Windows` uses the current user's registry Run key:

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
```

It does not install a service and should not require administrator permission.

If you move the app folder, toggle Start with Windows off and on again so Windows points to the new EXE path.

## False Positives

This tool uses normal user-mode DLL injection into whitelisted processes. Antivirus products may flag that behavior even though the source is small and auditable.

If you use it personally, prefer a narrow exclusion for the folder containing:

```text
FlipConfigBypass.exe
FlipConfigPayload.dll
whitelist.txt
FlipConfigBypass.log
```

Do not allow a broad detection name globally.

## Limitations

- Windows x64 only
- Targets must be x64
- Protected or anti-cheat-enabled games may block injection
- The payload scans for newly loaded modules for a limited startup window
- If a game resolved and cached NVAPI before injection, this tool may not affect that cached pointer

## Build

The GitHub Actions workflow builds directly with the Visual C++ compiler on a Windows runner.

Manual local builds require the Visual Studio C++ toolchain.

