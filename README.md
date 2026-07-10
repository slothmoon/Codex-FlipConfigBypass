# Flip Config Bypass

A small Windows x64 tray utility that prevents selected applications from obtaining NVIDIA's `NvAPI_D3D12_SetFlipConfig` interface.

The launcher watches a whitelist, injects a lightweight payload into the first matching x64 process, and then stops process scanning until that process exits. The payload blocks only this NVAPI interface ID:

```text
0xF3148C42 - NvAPI_D3D12_SetFlipConfig
```

All unrelated NVAPI queries and `GetProcAddress` calls are forwarded normally.

## Requirements

- Windows on a native AMD64/x64 host
- An x64 target application
- An NVIDIA driver that provides `nvapi64.dll`
- `FlipConfigBypass.exe` and `FlipConfigPayload.dll` in the same folder

ARM64 hosts and 32-bit target processes are intentionally unsupported.

## Download

GitHub Actions produces the current binaries:

1. Open the repository's **Actions** tab.
2. Select a successful **Build** run.
3. Download the `FlipConfigBypass-x64` artifact.
4. Extract both files into the same folder:

```text
FlipConfigBypass.exe
FlipConfigPayload.dll
```

The workflow also supports a manual **Run workflow** build.

## Quick Start

1. Run `FlipConfigBypass.exe`.
2. Right-click its tray icon and select **Edit Whitelist...**.
3. Add the executable name of the game, for example `Cyberpunk2077.exe`.
4. Save the whitelist.
5. Start the game.
6. Check the tray tooltip or `FlipConfigBypass.log` to confirm injection.

The app creates these files beside the executable when needed:

```text
whitelist.txt
FlipConfigBypass.log
```

Only one launcher instance can run at a time.

## Whitelist

Add one executable name or full path per line:

```text
# Lines beginning with # are comments.
Cyberpunk2077.exe
witcher3.exe
C:\Games\Example\Game.exe
```

Matching is case-insensitive. Forward slashes in full paths are normalized to backslashes.

Use executable names for normal setups. Use a full path only when two installations share the same executable name and only one should be targeted.

Changes made in the whitelist editor take effect without restarting the app.

## Tray Controls and Status

Right-click the tray icon to:

- edit the whitelist
- open the log
- pause or resume watching
- enable or disable Start with Windows
- exit

Hover over the icon to see the current watcher state:

```text
Watching 3 apps
Paused
Scanning stopped: Game.exe is running
```

After a successful injection, the launcher waits directly on that game process instead of waking every two seconds to enumerate processes. Scanning resumes automatically when the process fully exits.

If a game remains stuck in the background, the tooltip identifies the executable keeping the watcher stopped. End that process or restart the launcher to resume scanning.

While one injected game is running, another whitelisted process will not be detected. This is intentional: the launcher is optimized for one active game and minimal work during gameplay.

## How It Works

### Launcher

While watching, the launcher checks running processes every two seconds. It:

1. Matches executable names without allocating a temporary lowercase string for every process.
2. Uses the process creation time to distinguish reused process IDs.
3. Records an injection attempt before making it, whether the attempt succeeds or fails.
4. Confirms that the target is x64.
5. Loads `FlipConfigPayload.dll` with a remote `LoadLibraryW` thread.
6. On success, holds a synchronization handle and sleeps until the process exits.

Each process instance receives exactly one injection attempt. A failed attempt is not retried because the game may already have resolved and cached the NVAPI interface by the time a retry occurs.

Remote path memory is released only after the loader thread has definitely finished. If waiting fails or times out, the allocation is deliberately left in the target rather than racing a thread that may still be reading it.

### Payload

The payload patches import address table entries for:

- direct imports of `nvapi64.dll!nvapi_QueryInterface`
- `GetProcAddress` imports used for dynamic NVAPI lookup

API-set imports that resolve to `GetProcAddress` are handled as well.

The hooked `nvapi_QueryInterface` returns `nullptr` only for `0xF3148C42`. Every other interface ID is forwarded to the real function. The `GetProcAddress` hook similarly forwards unrelated modules and function names through its fast path.

The payload performs one initial module scan at normal priority, then scans once per second at below-normal priority for about two minutes. It patches only newly observed modules, reuses its module buffers, handles unloaded module addresses, and exits its worker after the scan window. Installed hooks remain active for the lifetime of the process.

Neither hook performs logging, file access, locking, allocation, or module enumeration.

## Performance Characteristics

The design intentionally avoids permanent monitoring work inside the game:

- launcher process scans occur only while looking for a target
- launcher scans stop for the entire lifetime of a successfully injected game
- payload scans are limited to roughly two minutes
- previously scanned modules are skipped
- the payload worker exits after its scan window
- hook fast paths perform only the checks needed to forward unrelated calls

The tray UI remains responsive while the watcher waits for the game to exit.

## Logs

The log contains meaningful injection results only:

```text
[14:32:01] Cyberpunk2077.exe (PID 9432) - injected OK
[14:42:55] ProtectedGame.exe (PID 2216) - failed, access denied
```

Non-whitelisted processes are not logged. At startup, the log is cleared if it exceeds 2 MB.

For injection failures, check that:

- the executable name or path is correct
- both program files are in the same folder
- the target is x64
- the launcher and target are running at compatible privilege levels
- antivirus or anti-cheat software is not blocking process access

## Start With Windows

The startup option writes the launcher path to:

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
```

It does not install a service. If the program folder is moved, disable and re-enable **Start with Windows** to update the stored path.

## Antivirus and Anti-Cheat

This project performs user-mode DLL injection, which security products may flag even when the binary is built from this source.

If an exclusion is necessary for personal use, keep it narrowly limited to the program folder. Do not create broad exclusions for an entire detection family or for unrelated game directories.

The project does not attempt to bypass protected processes, antivirus software, or anti-cheat systems.

## Limitations

- native AMD64/x64 Windows hosts only
- x64 target processes only
- `nvapi64.dll` only
- one active injected game at a time
- one injection attempt per process instance, with no retries
- protected or anti-cheat-enabled processes may reject injection
- an NVAPI pointer cached before injection may remain unaffected
- modules first loaded after the two-minute payload scan window are not patched
- a background process that has not fully exited keeps launcher scanning stopped

## Building

The GitHub Actions workflow builds Release x64 with the Microsoft Visual C++ toolchain using:

```text
/std:c++17 /O2 /W4 /WX /permissive- /MT /EHsc
```

The workflow builds and uploads:

```text
bin\Release\FlipConfigBypass.exe
bin\Release\FlipConfigPayload.dll
```

For local builds, install Visual Studio or Visual Studio Build Tools with the x64 C++ toolchain and follow the commands in [`.github/workflows/build.yml`](.github/workflows/build.yml).

## Project Layout

```text
.github/workflows/build.yml   Release x64 build
src/app/main.cpp              Tray UI, whitelist watcher, and injector
src/app/app.rc                Windows resources
src/payload/payload.cpp       Injected IAT-hook payload
src/shared/flip_config.h      Blocked NVAPI interface ID
```

The project deliberately has no NVIDIA SDK dependency, external hook library, overlay, service, or game-file replacement.
