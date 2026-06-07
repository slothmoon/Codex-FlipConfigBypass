#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

using NvApiQueryInterface = void* (__cdecl*)(unsigned int interfaceId);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE module, LPCSTR procName);

namespace
{
constexpr unsigned int NvApiD3D12SetFlipConfigId = 0xF3148C42;

std::atomic<NvApiQueryInterface> g_realNvApiQueryInterface{ nullptr };
std::atomic<GetProcAddressFn> g_realGetProcAddress{ ::GetProcAddress };
std::atomic<bool> g_running{ true };
std::mutex g_scanMutex;
std::unordered_set<HMODULE> g_scannedModules;

std::wstring joinPath(std::wstring_view dir, std::wstring_view name)
{
    std::wstring result(dir);
    if (!result.empty() && result.back() != L'\\')
        result.push_back(L'\\');
    result.append(name);
    return result;
}

void* __cdecl hookedNvApiQueryInterface(unsigned int interfaceId)
{
    if (interfaceId == NvApiD3D12SetFlipConfigId)
        return nullptr;

    NvApiQueryInterface realNvApiQueryInterface = g_realNvApiQueryInterface.load(std::memory_order_acquire);
    if (!realNvApiQueryInterface)
        return nullptr;

    return realNvApiQueryInterface(interfaceId);
}

FARPROC WINAPI hookedGetProcAddress(HMODULE module, LPCSTR procName)
{
    if (procName && reinterpret_cast<uintptr_t>(procName) > 0xFFFF &&
        std::strcmp(procName, "nvapi_QueryInterface") == 0)
    {
        GetProcAddressFn realGetProcAddress = g_realGetProcAddress.load(std::memory_order_acquire);
        NvApiQueryInterface realNvApiQueryInterface = g_realNvApiQueryInterface.load(std::memory_order_acquire);
        if (!realNvApiQueryInterface)
        {
            realNvApiQueryInterface = reinterpret_cast<NvApiQueryInterface>(realGetProcAddress(module, procName));
            g_realNvApiQueryInterface.store(realNvApiQueryInterface, std::memory_order_release);
        }

        return reinterpret_cast<FARPROC>(&hookedNvApiQueryInterface);
    }

    return g_realGetProcAddress.load(std::memory_order_acquire)(module, procName);
}

bool isReadablePeImage(HMODULE module)
{
    auto* base = reinterpret_cast<const std::uint8_t*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE;
}

bool patchImport(HMODULE module, const char* importedModuleName, const char* procName, void* replacement, void** original)
{
    if (!module || !isReadablePeImage(module))
        return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress || !importDir.Size)
        return false;

    bool patched = false;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; desc->Name; ++desc)
    {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, importedModuleName) != 0 || !desc->OriginalFirstThunk)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);

        for (; origThunk->u1.AddressOfData; ++origThunk, ++thunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal))
                continue;

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), procName) != 0)
                continue;

            void* current = reinterpret_cast<void*>(thunk->u1.Function);
            if (current == replacement)
                continue;

            DWORD oldProtect = 0;
            if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                if (original && !*original)
                    *original = current;
                thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                patched = true;
            }
        }
    }

    return patched;
}

void patchModuleImports(HMODULE module)
{
    void* originalGetProc = nullptr;
    if (patchImport(module, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&hookedGetProcAddress), &originalGetProc) && originalGetProc)
        g_realGetProcAddress.store(reinterpret_cast<GetProcAddressFn>(originalGetProc), std::memory_order_release);

    void* originalNvQuery = nullptr;
    if (patchImport(module, "nvapi64.dll", "nvapi_QueryInterface", reinterpret_cast<void*>(&hookedNvApiQueryInterface), &originalNvQuery) && originalNvQuery)
        g_realNvApiQueryInterface.store(reinterpret_cast<NvApiQueryInterface>(originalNvQuery), std::memory_order_release);

    originalNvQuery = nullptr;
    if (patchImport(module, "nvapi.dll", "nvapi_QueryInterface", reinterpret_cast<void*>(&hookedNvApiQueryInterface), &originalNvQuery) && originalNvQuery)
        g_realNvApiQueryInterface.store(reinterpret_cast<NvApiQueryInterface>(originalNvQuery), std::memory_order_release);
}

void scanAndPatchImports()
{
    HMODULE modules[1024]{};
    DWORD needed = 0;
    HANDLE process = GetCurrentProcess();
    using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);

    HMODULE psapi = GetModuleHandleW(L"psapi.dll");
    if (!psapi)
        psapi = LoadLibraryW(L"psapi.dll");

    auto enumProcessModules = psapi
        ? reinterpret_cast<EnumProcessModulesFn>(GetProcAddress(psapi, "EnumProcessModules"))
        : nullptr;

    if (!enumProcessModules || !enumProcessModules(process, modules, sizeof(modules), &needed))
        return;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(modules)));
    for (DWORD i = 0; i < count; ++i)
    {
        if (g_scannedModules.insert(modules[i]).second)
            patchModuleImports(modules[i]);
    }
}

DWORD WINAPI workerThread(void*)
{
    for (int i = 0; g_running && i < 600; ++i)
    {
        scanAndPatchImports();
        Sleep(500);
    }

    return 0;
}

HMODULE loadRealDxgi()
{
    static HMODULE realDxgi = [] {
        wchar_t systemDir[MAX_PATH]{};
        GetSystemDirectoryW(systemDir, MAX_PATH);
        std::wstring path = joinPath(systemDir, L"dxgi.dll");
        return LoadLibraryW(path.c_str());
    }();

    return realDxgi;
}

FARPROC realDxgiProc(const char* name)
{
    HMODULE dxgi = loadRealDxgi();
    return dxgi ? GetProcAddress(dxgi, name) : nullptr;
}

template <typename Fn, typename... Args>
auto callDxgi(const char* name, Args... args)
{
    auto fn = reinterpret_cast<Fn>(realDxgiProc(name));
    if (!fn)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return fn(args...);
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    return callDxgi<Fn>("CreateDXGIFactory", riid, factory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    return callDxgi<Fn>("CreateDXGIFactory1", riid, factory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    return callDxgi<Fn>("CreateDXGIFactory2", flags, riid, factory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    using Fn = HRESULT(WINAPI*)();
    return callDxgi<Fn>("DXGIDeclareAdapterRemovalSupport");
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    return callDxgi<Fn>("DXGIGetDebugInterface1", flags, riid, debug);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDisableVBlankVirtualization()
{
    using Fn = HRESULT(WINAPI*)();
    return callDxgi<Fn>("DXGIDisableVBlankVirtualization");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running = false;
    }

    return TRUE;
}
