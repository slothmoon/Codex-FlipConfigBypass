#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cwchar>
#include <cstdint>
#include <cstring>

#include "../shared/flip_config.h"

using NvApiQueryInterface = void* (__cdecl*)(unsigned int interfaceId);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE module, LPCSTR procName);

namespace
{
std::atomic<NvApiQueryInterface> g_realNvApiQueryInterface{ nullptr };
std::atomic<GetProcAddressFn> g_realGetProcAddress{ ::GetProcAddress };
std::atomic<bool> g_running{ true };
HMODULE g_scannedModules[512]{};
unsigned int g_scannedModuleCount = 0;

struct PeImageView
{
    std::uint8_t* base = nullptr;
    DWORD size = 0;
};

bool isNvApiModule(HMODULE module)
{
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH))
        return false;

    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"nvapi64.dll") == 0;
}

bool rememberModule(HMODULE module)
{
    for (unsigned int i = 0; i < g_scannedModuleCount; ++i)
    {
        if (g_scannedModules[i] == module)
            return false;
    }

    if (g_scannedModuleCount >= static_cast<unsigned int>(sizeof(g_scannedModules) / sizeof(g_scannedModules[0])))
        return false;

    g_scannedModules[g_scannedModuleCount++] = module;

    return true;
}

void seedRealNvApiQueryInterface()
{
    if (g_realNvApiQueryInterface.load(std::memory_order_acquire))
        return;

    GetProcAddressFn realGetProcAddress = g_realGetProcAddress.load(std::memory_order_acquire);
    HMODULE module = GetModuleHandleW(L"nvapi64.dll");
    if (module)
    {
        auto queryInterface = reinterpret_cast<NvApiQueryInterface>(
            realGetProcAddress(module, "nvapi_QueryInterface"));
        if (queryInterface)
        {
            g_realNvApiQueryInterface.store(queryInterface, std::memory_order_release);
        }
    }
}

void* __cdecl hookedNvApiQueryInterface(unsigned int interfaceId)
{
    if (interfaceId == kNvApiD3D12SetFlipConfigId)
        return nullptr;

    NvApiQueryInterface realNvApiQueryInterface = g_realNvApiQueryInterface.load(std::memory_order_acquire);
    return realNvApiQueryInterface ? realNvApiQueryInterface(interfaceId) : nullptr;
}

FARPROC WINAPI hookedGetProcAddress(HMODULE module, LPCSTR procName)
{
    GetProcAddressFn realGetProcAddress = g_realGetProcAddress.load(std::memory_order_acquire);
    FARPROC proc = realGetProcAddress(module, procName);

    if (procName && reinterpret_cast<uintptr_t>(procName) > 0xFFFF &&
        std::strcmp(procName, "nvapi_QueryInterface") == 0 &&
        proc && isNvApiModule(module))
    {
        g_realNvApiQueryInterface.store(reinterpret_cast<NvApiQueryInterface>(proc), std::memory_order_release);
        return reinterpret_cast<FARPROC>(&hookedNvApiQueryInterface);
    }

    return proc;
}

bool rvaRangeInImage(const PeImageView& image, DWORD rva, SIZE_T bytes)
{
    return rva && rva < image.size && bytes <= image.size - rva;
}

template <typename T>
T* rvaToPtr(const PeImageView& image, DWORD rva, SIZE_T bytes = sizeof(T))
{
    return rvaRangeInImage(image, rva, bytes) ? reinterpret_cast<T*>(image.base + rva) : nullptr;
}

bool getPeImageView(HMODULE module, PeImageView& image)
{
    if (!module)
        return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || !nt->OptionalHeader.SizeOfImage)
        return false;

    image.base = base;
    image.size = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool patchThunk(IMAGE_THUNK_DATA* thunk, void* replacement, void** original)
{
    void* current = reinterpret_cast<void*>(thunk->u1.Function);
    if (!current || current == replacement)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;

    if (original && !*original)
        *original = current;
    thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
    DWORD ignoredProtect = 0;
    VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &ignoredProtect);
    FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(void*));
    return true;
}

bool patchImportUnsafe(HMODULE module, const char* importedModuleName, const char* procName, void* replacement, void** original)
{
    PeImageView image{};
    if (!getPeImageView(module, image))
        return false;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image.base + dos->e_lfanew);
    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!rvaRangeInImage(image, importDir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR)))
        return false;

    bool patched = false;
    GetProcAddressFn realGetProcAddress = g_realGetProcAddress.load(std::memory_order_acquire);
    HMODULE importedModule = GetModuleHandleA(importedModuleName);
    void* targetProc = importedModule ? reinterpret_cast<void*>(realGetProcAddress(importedModule, procName)) : nullptr;

    const DWORD maxDescriptors = importDir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    auto* desc = rvaToPtr<IMAGE_IMPORT_DESCRIPTOR>(image, importDir.VirtualAddress);
    for (DWORD descIndex = 0; desc && descIndex < maxDescriptors && desc->Name; ++descIndex, ++desc)
    {
        const char* dllName = rvaToPtr<char>(image, desc->Name, 1);
        if (!dllName || _stricmp(dllName, importedModuleName) != 0 || !desc->FirstThunk)
            continue;

        auto* thunk = rvaToPtr<IMAGE_THUNK_DATA>(image, desc->FirstThunk);
        auto* origThunk = desc->OriginalFirstThunk ? rvaToPtr<IMAGE_THUNK_DATA>(image, desc->OriginalFirstThunk) : nullptr;
        if (!thunk)
            continue;

        for (DWORD thunkIndex = 0; thunkIndex < 4096; ++thunkIndex, ++thunk)
        {
            if (!rvaRangeInImage(image, desc->FirstThunk + thunkIndex * sizeof(IMAGE_THUNK_DATA), sizeof(IMAGE_THUNK_DATA)) ||
                !thunk->u1.Function)
            {
                break;
            }

            bool shouldPatch = false;
            if (origThunk &&
                rvaRangeInImage(image, desc->OriginalFirstThunk + thunkIndex * sizeof(IMAGE_THUNK_DATA), sizeof(IMAGE_THUNK_DATA)) &&
                origThunk[thunkIndex].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL(origThunk[thunkIndex].u1.Ordinal))
            {
                auto* importByName = rvaToPtr<IMAGE_IMPORT_BY_NAME>(image, static_cast<DWORD>(origThunk[thunkIndex].u1.AddressOfData));
                shouldPatch = importByName && std::strcmp(reinterpret_cast<const char*>(importByName->Name), procName) == 0;
            }
            else if (targetProc)
            {
                shouldPatch = reinterpret_cast<void*>(thunk->u1.Function) == targetProc;
            }

            if (shouldPatch)
                patched = patchThunk(thunk, replacement, original) || patched;
        }
    }

    return patched;
}

bool patchImport(HMODULE module, const char* importedModuleName, const char* procName, void* replacement, void** original)
{
    __try
    {
        return patchImportUnsafe(module, importedModuleName, procName, replacement, original);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void patchModuleImports(HMODULE module)
{
    void* originalGetProc = nullptr;
    const char* loaderModules[] = {
        "KERNEL32.dll",
        "KERNELBASE.dll"
    };

    for (const char* loaderModule : loaderModules)
    {
        originalGetProc = nullptr;
        if (patchImport(module, loaderModule, "GetProcAddress", reinterpret_cast<void*>(&hookedGetProcAddress), &originalGetProc) && originalGetProc)
            g_realGetProcAddress.store(reinterpret_cast<GetProcAddressFn>(originalGetProc), std::memory_order_release);
    }

    void* originalNvQuery = nullptr;
    if (patchImport(module, "nvapi64.dll", "nvapi_QueryInterface", reinterpret_cast<void*>(&hookedNvApiQueryInterface), &originalNvQuery) && originalNvQuery)
        g_realNvApiQueryInterface.store(reinterpret_cast<NvApiQueryInterface>(originalNvQuery), std::memory_order_release);
}

void scanAndPatchImports()
{
    seedRealNvApiQueryInterface();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            HMODULE module = reinterpret_cast<HMODULE>(entry.modBaseAddr);
            if (rememberModule(module))
                patchModuleImports(module);
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

DWORD WINAPI workerThread(void*)
{
    seedRealNvApiQueryInterface();

    for (int i = 0; g_running && i < 600; ++i)
    {
        scanAndPatchImports();
        Sleep(500);
    }

    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        HMODULE pinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&DllMain),
            &pinned);

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
