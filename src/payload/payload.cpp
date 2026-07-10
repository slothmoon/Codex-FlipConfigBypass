#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../shared/flip_config.h"

using NvApiQueryInterface = void* (__cdecl*)(unsigned int interfaceId);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE module, LPCSTR procName);
using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE process, HMODULE* modules, DWORD bytes, LPDWORD bytesNeeded);

static_assert(sizeof(void*) == 8, "FlipConfigPayload must be built as x64.");

namespace
{
constexpr int kModuleScanIterations = 120;
constexpr DWORD kModuleScanIntervalMs = 1000;
constexpr std::size_t kInitialModuleCapacity = 512;

std::atomic<NvApiQueryInterface> g_realNvApiQueryInterface{ nullptr };
std::atomic<GetProcAddressFn> g_realGetProcAddress{ ::GetProcAddress };
std::atomic<HMODULE> g_nvapiModule{ nullptr };
std::atomic<void*> g_kernel32GetProcAddress{ nullptr };
std::atomic<void*> g_kernelbaseGetProcAddress{ nullptr };
std::atomic<bool> g_running{ true };

void* __cdecl hookedNvApiQueryInterface(unsigned int interfaceId);

struct PeImageView
{
    std::uint8_t* base = nullptr;
    DWORD size = 0;
};

struct ImportPatchTarget
{
    const char* procName = nullptr;
    void* targetProc = nullptr;
    void* replacement = nullptr;
    void** original = nullptr;
};

struct ModuleScanState
{
    std::vector<HMODULE> modules;
    std::vector<HMODULE> scannedModules;
};

void cacheNvApiModule(HMODULE module)
{
    if (!module)
        return;

    HMODULE expected = nullptr;
    g_nvapiModule.compare_exchange_strong(expected, module, std::memory_order_relaxed, std::memory_order_relaxed);
}

void cachePointer(std::atomic<void*>& cache, void* value)
{
    if (!value)
        return;

    void* expected = nullptr;
    cache.compare_exchange_strong(expected, value, std::memory_order_relaxed, std::memory_order_relaxed);
}

bool moduleAddressLess(HMODULE left, HMODULE right)
{
    return reinterpret_cast<std::uintptr_t>(left) < reinterpret_cast<std::uintptr_t>(right);
}

void storeRealNvApiQueryInterfaceIfUnset(NvApiQueryInterface queryInterface)
{
    if (!queryInterface || queryInterface == &hookedNvApiQueryInterface)
        return;

    NvApiQueryInterface expected = nullptr;
    g_realNvApiQueryInterface.compare_exchange_strong(expected, queryInterface, std::memory_order_release, std::memory_order_relaxed);
}

void* procAddressFrom(const char* moduleName, const char* procName, GetProcAddressFn realGetProcAddress)
{
    HMODULE module = GetModuleHandleA(moduleName);
    return module ? reinterpret_cast<void*>(realGetProcAddress(module, procName)) : nullptr;
}

void* cachedProcAddressFrom(std::atomic<void*>& cache, const char* moduleName, const char* procName, GetProcAddressFn realGetProcAddress)
{
    void* cached = cache.load(std::memory_order_relaxed);
    if (cached)
        return cached;

    void* resolved = procAddressFrom(moduleName, procName, realGetProcAddress);
    cachePointer(cache, resolved);
    return cache.load(std::memory_order_relaxed);
}

void* nvApiQueryTarget(GetProcAddressFn realGetProcAddress)
{
    NvApiQueryInterface cached = g_realNvApiQueryInterface.load(std::memory_order_relaxed);
    if (cached)
        return reinterpret_cast<void*>(cached);

    auto queryInterface = reinterpret_cast<NvApiQueryInterface>(
        procAddressFrom("nvapi64.dll", "nvapi_QueryInterface", realGetProcAddress));
    storeRealNvApiQueryInterfaceIfUnset(queryInterface);
    return reinterpret_cast<void*>(g_realNvApiQueryInterface.load(std::memory_order_relaxed));
}

void seedRealNvApiQueryInterface()
{
    HMODULE module = g_nvapiModule.load(std::memory_order_relaxed);
    if (!module)
    {
        module = GetModuleHandleW(L"nvapi64.dll");
        cacheNvApiModule(module);
    }

    if (g_realNvApiQueryInterface.load(std::memory_order_relaxed))
        return;

    GetProcAddressFn realGetProcAddress = g_realGetProcAddress.load(std::memory_order_relaxed);
    if (module)
    {
        auto queryInterface = reinterpret_cast<NvApiQueryInterface>(
            realGetProcAddress(module, "nvapi_QueryInterface"));
        storeRealNvApiQueryInterfaceIfUnset(queryInterface);
    }
}

void* __cdecl hookedNvApiQueryInterface(unsigned int interfaceId)
{
    if (interfaceId == kNvApiD3D12SetFlipConfigId)
        return nullptr;

    NvApiQueryInterface realNvApiQueryInterface = g_realNvApiQueryInterface.load(std::memory_order_relaxed);
    return realNvApiQueryInterface ? realNvApiQueryInterface(interfaceId) : nullptr;
}

FARPROC WINAPI hookedGetProcAddress(HMODULE module, LPCSTR procName)
{
    GetProcAddressFn realGetProcAddress = g_realGetProcAddress.load(std::memory_order_relaxed);
    FARPROC proc = realGetProcAddress(module, procName);
    if (!proc)
        return nullptr;

    HMODULE nvapiModule = g_nvapiModule.load(std::memory_order_relaxed);
    if (nvapiModule && module != nvapiModule)
        return proc;

    if (!procName || reinterpret_cast<uintptr_t>(procName) <= 0xFFFF ||
        std::strcmp(procName, "nvapi_QueryInterface") != 0)
        return proc;

    if (!nvapiModule)
    {
        nvapiModule = GetModuleHandleW(L"nvapi64.dll");
        cacheNvApiModule(nvapiModule);
    }

    if (module != nvapiModule)
        return proc;

    storeRealNvApiQueryInterfaceIfUnset(reinterpret_cast<NvApiQueryInterface>(proc));
    return reinterpret_cast<FARPROC>(&hookedNvApiQueryInterface);
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
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)))
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        !nt->OptionalHeader.SizeOfImage)
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

    void* previous = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&thunk->u1.Function),
        replacement,
        current);
    DWORD ignoredProtect = 0;
    VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &ignoredProtect);
    if (previous != current)
        return false;

    if (original && !*original)
        *original = current;
    return true;
}

bool importPatchTargetForDll(const char* dllName, GetProcAddressFn realGetProcAddress, void*& originalNvQuery, ImportPatchTarget& target)
{
    if (_stricmp(dllName, "KERNEL32.dll") == 0)
    {
        target.procName = "GetProcAddress";
        target.targetProc = cachedProcAddressFrom(g_kernel32GetProcAddress, "KERNEL32.dll", "GetProcAddress", realGetProcAddress);
        target.replacement = reinterpret_cast<void*>(&hookedGetProcAddress);
        return true;
    }

    if (_stricmp(dllName, "KERNELBASE.dll") == 0)
    {
        target.procName = "GetProcAddress";
        target.targetProc = cachedProcAddressFrom(g_kernelbaseGetProcAddress, "KERNELBASE.dll", "GetProcAddress", realGetProcAddress);
        target.replacement = reinterpret_cast<void*>(&hookedGetProcAddress);
        return true;
    }

    if (_stricmp(dllName, "nvapi64.dll") == 0)
    {
        target.procName = "nvapi_QueryInterface";
        target.targetProc = nvApiQueryTarget(realGetProcAddress);
        target.replacement = reinterpret_cast<void*>(&hookedNvApiQueryInterface);
        target.original = &originalNvQuery;
        return true;
    }

    return false;
}

void patchMatchingThunks(const PeImageView& image, IMAGE_IMPORT_DESCRIPTOR* desc, const ImportPatchTarget& target)
{
    auto* thunk = rvaToPtr<IMAGE_THUNK_DATA>(image, desc->FirstThunk);
    auto* origThunk = desc->OriginalFirstThunk ? rvaToPtr<IMAGE_THUNK_DATA>(image, desc->OriginalFirstThunk) : nullptr;
    if (!thunk)
        return;

    const DWORD thunkCapacity = static_cast<DWORD>(
        (image.size - desc->FirstThunk) / sizeof(IMAGE_THUNK_DATA));
    const DWORD maxThunks = thunkCapacity < 4096 ? thunkCapacity : 4096;
    const DWORD maxOrigThunks = origThunk && desc->OriginalFirstThunk < image.size
        ? static_cast<DWORD>((image.size - desc->OriginalFirstThunk) / sizeof(IMAGE_THUNK_DATA))
        : 0;

    for (DWORD thunkIndex = 0; thunkIndex < maxThunks && thunk->u1.Function; ++thunkIndex, ++thunk)
    {
        bool shouldPatch = false;
        if (origThunk &&
            thunkIndex < maxOrigThunks &&
            origThunk[thunkIndex].u1.AddressOfData &&
            !IMAGE_SNAP_BY_ORDINAL(origThunk[thunkIndex].u1.Ordinal))
        {
            auto* importByName = rvaToPtr<IMAGE_IMPORT_BY_NAME>(image, static_cast<DWORD>(origThunk[thunkIndex].u1.AddressOfData));
            shouldPatch = importByName && target.targetProc &&
                reinterpret_cast<void*>(thunk->u1.Function) == target.targetProc &&
                std::strcmp(reinterpret_cast<const char*>(importByName->Name), target.procName) == 0;
        }
        else if (target.targetProc)
        {
            shouldPatch = reinterpret_cast<void*>(thunk->u1.Function) == target.targetProc;
        }

        if (shouldPatch)
            patchThunk(thunk, target.replacement, target.original);
    }
}

void patchModuleImportsUnsafe(HMODULE module)
{
    PeImageView image{};
    if (!getPeImageView(module, image))
        return;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!rvaRangeInImage(image, importDir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR)))
        return;

    GetProcAddressFn realGetProcAddress = g_realGetProcAddress.load(std::memory_order_relaxed);
    void* originalNvQuery = nullptr;

    const DWORD maxDescriptors = static_cast<DWORD>(
        importDir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR));
    auto* desc = rvaToPtr<IMAGE_IMPORT_DESCRIPTOR>(image, importDir.VirtualAddress);
    for (DWORD descIndex = 0; desc && descIndex < maxDescriptors && desc->Name; ++descIndex, ++desc)
    {
        const char* dllName = rvaToPtr<char>(image, desc->Name, 1);
        if (!dllName || !desc->FirstThunk)
            continue;

        ImportPatchTarget target{};
        if (!importPatchTargetForDll(dllName, realGetProcAddress, originalNvQuery, target))
            continue;

        patchMatchingThunks(image, desc, target);
    }

    if (originalNvQuery)
        storeRealNvApiQueryInterfaceIfUnset(reinterpret_cast<NvApiQueryInterface>(originalNvQuery));
}

void patchModuleImports(HMODULE module)
{
    __try
    {
        patchModuleImportsUnsafe(module);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
#ifdef _DEBUG
        OutputDebugStringW(L"FlipConfigPayload: skipped module after import patch exception.\n");
#endif
        // Ignore unusual or transient module layouts rather than crashing the target process.
    }
}

EnumProcessModulesFn enumProcessModulesFn()
{
    static EnumProcessModulesFn fn = reinterpret_cast<EnumProcessModulesFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules"));
    return fn;
}

bool enumerateProcessModules(ModuleScanState& state, std::size_t& moduleCount)
{
    EnumProcessModulesFn enumModules = enumProcessModulesFn();
    if (!enumModules)
        return false;

    if (state.modules.empty())
        state.modules.resize(kInitialModuleCapacity);

    for (;;)
    {
        const std::size_t bufferBytes = state.modules.size() * sizeof(HMODULE);
        if (bufferBytes > MAXDWORD)
            return false;

        DWORD bytesNeeded = 0;
        if (!enumModules(
            GetCurrentProcess(),
            state.modules.data(),
            static_cast<DWORD>(bufferBytes),
            &bytesNeeded))
        {
            return false;
        }

        const std::size_t requiredModules =
            (static_cast<std::size_t>(bytesNeeded) + sizeof(HMODULE) - 1) / sizeof(HMODULE);
        if (requiredModules <= state.modules.size())
        {
            moduleCount = requiredModules;
            return true;
        }

        state.modules.resize(requiredModules);
    }
}

void scanAndPatchImports(ModuleScanState& state)
{
    seedRealNvApiQueryInterface();

    std::size_t moduleCount = 0;
    if (!enumerateProcessModules(state, moduleCount))
        return;

    auto liveEnd = state.modules.begin() + moduleCount;
    std::sort(state.modules.begin(), liveEnd, moduleAddressLess);
    if (state.scannedModules.capacity() < state.modules.size())
        state.scannedModules.reserve(state.modules.size());

    HMODULE nvapiModule = g_nvapiModule.load(std::memory_order_relaxed);
    if (nvapiModule && !std::binary_search(
        state.modules.begin(), liveEnd, nvapiModule, moduleAddressLess))
    {
        g_realNvApiQueryInterface.store(nullptr, std::memory_order_release);
        g_nvapiModule.store(nullptr, std::memory_order_relaxed);
    }

    auto scannedIt = state.scannedModules.begin();
    const auto scannedEnd = state.scannedModules.end();
    for (auto liveIt = state.modules.begin(); liveIt != liveEnd; ++liveIt)
    {
        while (scannedIt != scannedEnd && moduleAddressLess(*scannedIt, *liveIt))
            ++scannedIt;

        if (scannedIt != scannedEnd && *scannedIt == *liveIt)
        {
            ++scannedIt;
            continue;
        }

        patchModuleImports(*liveIt);
    }

    state.scannedModules.assign(state.modules.begin(), liveEnd);
}

DWORD workerThreadMain()
{
    ModuleScanState state;
    state.modules.resize(kInitialModuleCapacity);
    state.scannedModules.reserve(kInitialModuleCapacity);

    if (g_running.load(std::memory_order_relaxed))
        scanAndPatchImports(state);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (int i = 1; g_running.load(std::memory_order_relaxed) && i < kModuleScanIterations; ++i)
    {
        Sleep(kModuleScanIntervalMs);
        if (g_running.load(std::memory_order_relaxed))
            scanAndPatchImports(state);
    }

    return 0;
}

DWORD WINAPI workerThread(void*) noexcept
{
    try
    {
        return workerThreadMain();
    }
    catch (...)
    {
#ifdef _DEBUG
        OutputDebugStringW(L"FlipConfigPayload: worker stopped after allocation failure.\n");
#endif
        return 0;
    }
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            // FROM_ADDRESS treats this as an address inside the payload, not as a string.
            reinterpret_cast<LPCWSTR>(workerThread),
            &pinned))
        {
            return FALSE;
        }

        HANDLE thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
        if (!thread)
            return FALSE;

        CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // The DLL is pinned; detach is expected only during process teardown.
        // Do not wait for the worker from DllMain.
        g_running = false;
    }

    return TRUE;
}
