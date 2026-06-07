# DisableFlipMetering

Tiny `dxgi.dll` proxy that forwards DXGI to the real Windows DLL and intercepts `nvapi_QueryInterface`.

When the game asks NVAPI for `NvAPI_D3D12_SetFlipConfig`, the proxy returns `nullptr`. This matches the behavior of the larger projects that disable NVIDIA flip metering by hiding that NVAPI entry point.

## Build

GitHub Actions builds the DLL directly with the Visual C++ compiler on a Windows runner. Open the repo's **Actions** tab, run **Build DLL**, then download the `DisableFlipMetering-dxgi` artifact.

The DLL will be here:

```text
bin\Release\dxgi.dll
```

## Use

Copy `dxgi.dll` next to the game's `.exe`.

If the game does not load `dxgi.dll`, try the loader name the game already probes. The usual name is `dxgi.dll`; `dxig.dll` is normally a typo unless the game specifically loads that filename.

No config file is needed. The DLL hardcodes `NvAPI_D3D12_SetFlipConfig` as `0xF3148C42`.

## Notes

This is intentionally small:

- no overlay
- no settings UI
- no graphics hooks
- no MinHook dependency
- no NVIDIA SDK dependency
- no config or log file

Some games, especially multiplayer games with anti-cheat, may block local DLL proxy loading or treat it as unsupported.
