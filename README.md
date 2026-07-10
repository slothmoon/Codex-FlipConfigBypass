# Flip Config Bypass

A minimal Windows x64 tray utility that disables NVIDIA flip metering for selected x64 applications.

The app watches a whitelist, injects `FlipConfigPayload.dll` into matching processes, and blocks one NVAPI query:

```text
0xF3148C42 - NvAPI_D3D12_SetFlipConfig
```

Everything else is intentionally left alone.

## What It Does

- Watches only executables listed in `whitelist.txt`
- Injects only into x64 processes
- Attempts each matching process instance only once
- Stops process scanning while a successfully injected game is running
- Hooks `nvapi64.dll!nvapi_QueryInterface`
- Returns `nullptr` only for `0xF3148C42`
- Forwards all other NVAPI query IDs to the real NVAPI function
- Logs injection successes and useful failures
- Restores the tray icon if Windows Explorer restarts

## What It Does Not Do

- No `dxgi.dll` proxy
- No game file replacement
- No overlay
- No NVIDIA SDK dependency
- No external hook library
- No 32-bit payload
- No anti-cheat or protected-process bypass

## Download

Builds are made by GitHub Actions.

Open the repo's **Actions** tab, run **Build**, then download the `FlipConfigBypass-x64` artifact.

The artifact contains:

```text
FlipConfigBypass.exe
FlipConfigPayload.dll
```

Keep both files in the same folder.

## Usage

Run `FlipConfigBypass.exe`. The app creates these files beside itself if needed:

```text
whitelist.txt
FlipConfigBypass.log
```

Right-click the tray icon to:

- edit the whitelist
- open the log with your default `.log` handler
- pause or resume watching
- toggle Start with Windows
- exit

Only one instance can run at a time.

## Whitelist

Add one executable name or full path per line:

```text
Cyberpunk2077.exe
witcher3.exe
C:\Games\Example\Game.exe
```

Executable-name entries are case-insensitive. Full paths are also normalized for case and slash direction.

Filename entries are usually best. Use full paths only when you need to target one exact install.

## Logs

The log records meaningful events only:

```text
[14:32:01] Cyberpunk2077.exe (PID 9432) - injected OK
[14:42:55] ProtectedGame.exe (PID 2216) - failed, access denied
```

Non-whitelisted processes are not logged.

At startup, `FlipConfigBypass.log` is cleared if it is larger than `2 MB`.

## Start With Windows

The startup toggle uses the current user's Run key:

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
```

It does not install a service and should not require administrator permission.

If you move the app folder, toggle Start with Windows off and on again.

## How The Payload Works

Inside a whitelisted x64 process, the payload patches:

- direct imports of `nvapi64.dll!nvapi_QueryInterface`
- `GetProcAddress` imports used for dynamic NVAPI lookup

When `nvapi_QueryInterface` is called, the payload blocks only:

```text
0xF3148C42
```

All other NVAPI query IDs are forwarded.

The payload scans newly loaded modules every `1 second` for about `2 minutes`, then stops. Hooks already installed remain active for the process lifetime.

## False Positives

This project uses normal user-mode DLL injection. Antivirus products may flag that behavior.

For personal use, prefer a narrow exclusion for the folder containing:

```text
FlipConfigBypass.exe
FlipConfigPayload.dll
whitelist.txt
FlipConfigBypass.log
```

Avoid broad exclusions for a whole detection name.

## Limitations

- Windows x64 only
- ARM64 hosts are rejected rather than running through x64 emulation
- Target processes must be x64
- Only `nvapi64.dll` is handled
- Protected or anti-cheat-enabled games may block injection
- NVAPI pointers resolved before injection may stay cached and unaffected
- Modules loaded after the payload scan window may not be patched
- A failed injection is not retried for the same process instance
- While an injected game is running, additional whitelisted processes are not detected

## Build

The GitHub Actions workflow builds with the Visual C++ toolchain on a Windows runner.

Local builds require the Visual Studio C++ build tools.
