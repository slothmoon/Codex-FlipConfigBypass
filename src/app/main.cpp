#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "resource.h"

static_assert(sizeof(void*) == 8, "FlipConfigBypass must be built as x64.");

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4)
#endif

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace
{
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kIdEditWhitelist = 1001;
constexpr UINT kIdViewLog = 1002;
constexpr UINT kIdPauseWatching = 1003;
constexpr UINT kIdStartWithWindows = 1004;
constexpr UINT kIdExit = 1005;
constexpr UINT kIdSave = 2001;
constexpr UINT kIdCancel = 2002;
constexpr UINT kIdAddExe = 2003;
constexpr UINT kIdRemoveExe = 2005;
constexpr UINT kIdEntry = 2007;
constexpr UINT kIdAddEntry = 2008;
constexpr wchar_t kWindowClass[] = L"FlipConfigBypassTray";
constexpr wchar_t kEditorClass[] = L"FlipConfigBypassEditor";
constexpr wchar_t kRunValueName[] = L"FlipConfigBypass";
constexpr wchar_t kInstanceMutexName[] = L"Local\\FlipConfigBypass.Instance";
constexpr LONGLONG kMaxLogBytes = 2ll * 1024ll * 1024ll;
constexpr DWORD kWatcherScanIntervalMs = 2000;

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
NOTIFYICONDATAW g_tray{};
HANDLE g_instanceMutex = nullptr;
HANDLE g_stopEvent = nullptr;
UINT g_taskbarCreatedMessage = 0;
std::atomic<bool> g_running{ true };
std::atomic<bool> g_paused{ false };
std::thread g_watcherThread;
std::mutex g_stateMutex;
std::atomic<std::uint64_t> g_whitelistVersion{ 1 };
std::vector<std::wstring> g_whitelist;
std::unordered_set<std::wstring> g_whitelistNames;
std::unordered_set<std::wstring> g_whitelistPaths;
std::unordered_set<std::wstring> g_whitelistPathNames;
bool g_hasPathWhitelist = false;
// Owned by watcherLoop only; add synchronization before touching this from UI code.
std::unordered_map<DWORD, ULONGLONG> g_attemptedProcesses;
std::wstring g_exePath;
std::wstring g_exeDir;
std::wstring g_payloadPath;
std::wstring g_whitelistPath;
std::wstring g_logPath;
HFONT g_uiFont = nullptr;
HFONT g_titleFont = nullptr;
HICON g_appIcon = nullptr;
bool g_ownsAppIcon = false;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_fieldBrush = nullptr;
COLORREF g_textColor = RGB(24, 31, 39);
COLORREF g_windowColor = RGB(248, 250, 252);
COLORREF g_fieldColor = RGB(255, 255, 255);
UINT g_dpi = 96;

HMENU menuId(UINT id)
{
    return reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id));
}

void initFonts()
{
    g_uiFont = CreateFontW(-MulDiv(10, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_titleFont = CreateFontW(-MulDiv(13, static_cast<int>(g_dpi), 72), 0, 0, 0, 600, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void setFont(HWND hwnd, HFONT font = g_uiFont)
{
    if (hwnd && font)
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND createButton(HWND parent, UINT id, const wchar_t* text, DWORD style = 0)
{
    HWND button = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_FLAT | style,
        0, 0, 0, 0, parent, menuId(id), g_instance, nullptr);
    setFont(button);
    return button;
}

int scale(int value)
{
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

void enableDpiAwareness()
{
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto setProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setProcessDpiAwarenessContext &&
        setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        return;
    }

    SetProcessDPIAware();
}

UINT systemDpi()
{
    HDC dc = GetDC(nullptr);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc)
        ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

std::wstring toLower(std::wstring value)
{
    for (wchar_t& ch : value)
        ch = static_cast<wchar_t>(towlower(ch));
    return value;
}

std::wstring trim(std::wstring value)
{
    const wchar_t* whitespace = L" \t\r\n";
    const size_t first = value.find_first_not_of(whitespace);
    if (first == std::wstring::npos)
        return {};
    const size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::wstring fileNameFromPath(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool isPathEntry(const std::wstring& value)
{
    return value.find(L'\\') != std::wstring::npos || value.find(L'/') != std::wstring::npos;
}

std::wstring normalizeWhitelistValue(std::wstring value)
{
    value = toLower(trim(std::move(value)));
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name)
{
    std::wstring result = dir;
    if (!result.empty() && result.back() != L'\\')
        result.push_back(L'\\');
    result += name;
    return result;
}

std::string wideToUtf8(const std::wstring& text)
{
    if (text.empty())
        return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& text)
{
    if (text.empty())
        return {};

    const char* data = text.data();
    int byteCount = static_cast<int>(text.size());
    if (byteCount >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF)
    {
        data += 3;
        byteCount -= 3;
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, data, byteCount, nullptr, 0);
    if (size <= 0)
        return {};

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, byteCount, result.data(), size);
    return result;
}

std::wstring readTextFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0)
    {
        CloseHandle(file);
        return {};
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    bytes.resize(read);
    CloseHandle(file);
    return utf8ToWide(bytes);
}

bool writeTextFile(const std::wstring& path, const std::wstring& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const std::string bytes = wideToUtf8(text);
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && static_cast<size_t>(written) == bytes.size();
}

void appendLog(const std::wstring& message)
{
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    wchar_t prefix[32]{};
    swprintf_s(prefix, L"[%02u:%02u:%02u] ",
        static_cast<unsigned int>(localTime.wHour),
        static_cast<unsigned int>(localTime.wMinute),
        static_cast<unsigned int>(localTime.wSecond));

    std::wstring line = prefix;
    line += message;
    line += L"\r\n";
    const std::string bytes = wideToUtf8(line);
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void trimLogIfTooLarge()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &data))
        return;

    LARGE_INTEGER size{};
    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    size.LowPart = data.nFileSizeLow;
    if (size.QuadPart > kMaxLogBytes)
        writeTextFile(g_logPath, L"");
}

void ensureDefaultFiles()
{
    if (GetFileAttributesW(g_whitelistPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        writeTextFile(g_whitelistPath,
            L"# One executable name or full path per line.\r\n"
            L"# Example:\r\n"
            L"# GTA5.exe\r\n");
    }

    if (GetFileAttributesW(g_logPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        writeTextFile(g_logPath, L"");
}

void loadWhitelist()
{
    std::vector<std::wstring> entries;
    std::unordered_set<std::wstring> names;
    std::unordered_set<std::wstring> paths;
    std::unordered_set<std::wstring> pathNames;
    std::wstring content = readTextFile(g_whitelistPath);
    size_t start = 0;
    while (start <= content.size())
    {
        const size_t end = content.find_first_of(L"\r\n", start);
        std::wstring line = trim(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!line.empty() && line[0] != L'#')
        {
            std::wstring normalized = normalizeWhitelistValue(line);
            if (isPathEntry(normalized))
            {
                if (paths.insert(normalized).second)
                {
                    std::wstring pathName = fileNameFromPath(normalized);
                    if (!pathName.empty())
                        pathNames.insert(std::move(pathName));
                    entries.push_back(line);
                }
            }
            else
            {
                if (names.insert(normalized).second)
                    entries.push_back(line);
            }
        }

        if (end == std::wstring::npos)
            break;
        start = content.find_first_not_of(L"\r\n", end);
        if (start == std::wstring::npos)
            break;
    }

    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_whitelist = std::move(entries);
    g_whitelistNames = std::move(names);
    g_whitelistPaths = std::move(paths);
    g_whitelistPathNames = std::move(pathNames);
    g_hasPathWhitelist = !g_whitelistPaths.empty();
    g_whitelistVersion.fetch_add(1, std::memory_order_release);
}

std::vector<std::wstring> whitelistSnapshot()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_whitelist;
}

struct WhitelistMatchSnapshot
{
    std::unordered_set<std::wstring> names;
    std::unordered_set<std::wstring> paths;
    std::unordered_set<std::wstring> pathNames;
    bool hasPathEntries = false;
};

void refreshWhitelistMatchSnapshot(
    WhitelistMatchSnapshot& snapshot,
    std::uint64_t& snapshotVersion)
{
    const std::uint64_t currentVersion = g_whitelistVersion.load(std::memory_order_acquire);
    if (snapshotVersion == currentVersion)
        return;

    std::lock_guard<std::mutex> lock(g_stateMutex);
    snapshot.names = g_whitelistNames;
    snapshot.paths = g_whitelistPaths;
    snapshot.pathNames = g_whitelistPathNames;
    snapshot.hasPathEntries = g_hasPathWhitelist;
    snapshotVersion = g_whitelistVersion.load(std::memory_order_relaxed);
}

constexpr ULONGLONG kUnknownProcessCreationTime = 0;

bool processInstanceWasAttempted(DWORD pid, ULONGLONG creationTime)
{
    const auto it = g_attemptedProcesses.find(pid);
    return it != g_attemptedProcesses.end() &&
        (it->second == kUnknownProcessCreationTime || it->second == creationTime);
}

bool pidHasUnknownAttempt(DWORD pid)
{
    const auto it = g_attemptedProcesses.find(pid);
    return it != g_attemptedProcesses.end() && it->second == kUnknownProcessCreationTime;
}

bool hasAttemptedProcesses()
{
    return !g_attemptedProcesses.empty();
}

void markProcessAttempted(DWORD pid, ULONGLONG creationTime)
{
    g_attemptedProcesses[pid] = creationTime;
}

std::unordered_set<DWORD> attemptedPidsSnapshot()
{
    std::unordered_set<DWORD> pids;
    pids.reserve(g_attemptedProcesses.size());
    for (const auto& attempted : g_attemptedProcesses)
        pids.insert(attempted.first);
    return pids;
}

void removeAttemptedProcesses(const std::unordered_set<DWORD>& pids)
{
    for (DWORD pid : pids)
        g_attemptedProcesses.erase(pid);
}

bool matchesWhitelistedPath(const WhitelistMatchSnapshot& whitelist, const std::wstring& fullPath)
{
    if (fullPath.empty())
        return false;

    return whitelist.paths.find(normalizeWhitelistValue(fullPath)) != whitelist.paths.end();
}

struct ProcessMetadata
{
    std::wstring fullPath;
    ULONGLONG creationTime = kUnknownProcessCreationTime;
};

bool queryProcessMetadata(DWORD pid, bool includePath, ProcessMetadata& metadata)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    bool ok = GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime) == TRUE;
    if (ok)
    {
        ULARGE_INTEGER value{};
        value.LowPart = creation.dwLowDateTime;
        value.HighPart = creation.dwHighDateTime;
        metadata.creationTime = value.QuadPart;
    }

    if (ok && includePath)
    {
        wchar_t path[MAX_PATH * 4]{};
        DWORD size = static_cast<DWORD>(std::size(path));
        ok = QueryFullProcessImageNameW(process, 0, path, &size) == TRUE;
        if (ok)
            metadata.fullPath.assign(path, size);
    }

    CloseHandle(process);
    return ok;
}

using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);

IsWow64Process2Fn isWow64Process2Fn()
{
    static const auto fn = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    return fn;
}

bool supportedX64Host()
{
    IsWow64Process2Fn isWow64Process2 = isWow64Process2Fn();
    if (!isWow64Process2)
        return true;

    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    return isWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) &&
        nativeMachine == IMAGE_FILE_MACHINE_AMD64;
}

bool isX64ProcessCompat(HANDLE process, bool& isX64)
{
    isX64 = false;
    IsWow64Process2Fn isWow64Process2 = isWow64Process2Fn();
    if (isWow64Process2)
    {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine))
            return false;

        const USHORT effectiveMachine = processMachine == IMAGE_FILE_MACHINE_UNKNOWN
            ? nativeMachine
            : processMachine;
        isX64 = effectiveMachine == IMAGE_FILE_MACHINE_AMD64;
        return true;
    }

    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64))
        return false;
    isX64 = wow64 == FALSE;
    return true;
}

std::wstring lastErrorText(DWORD error)
{
    wchar_t* buffer = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring text = buffer ? trim(buffer) : L"unknown error";
    if (buffer)
        LocalFree(buffer);
    return text;
}

bool injectPayload(DWORD pid, const std::wstring& exeName)
{
    if (GetFileAttributesW(g_payloadPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, payload DLL missing");
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
        FALSE,
        pid);
    if (!process)
    {
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, " + lastErrorText(GetLastError()));
        return false;
    }

    bool isX64 = false;
    if (!isX64ProcessCompat(process, isX64))
    {
        CloseHandle(process);
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, could not verify architecture");
        return false;
    }

    if (!isX64)
    {
        CloseHandle(process);
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, payload architecture mismatch");
        return false;
    }

    const size_t bytes = (g_payloadPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath)
    {
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, " + lastErrorText(GetLastError()));
        CloseHandle(process);
        return false;
    }

    bool ok = false;
    bool remotePathMayBeInUse = false;
    bool remoteThreadTimedOut = false;
    bool remoteThreadWaitFailed = false;
    DWORD error = ERROR_SUCCESS;
    if (!WriteProcessMemory(process, remotePath, g_payloadPath.c_str(), bytes, nullptr))
    {
        error = GetLastError();
    }
    else
    {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (!loadLibrary)
        {
            error = ERROR_PROC_NOT_FOUND;
        }
        else
        {
            HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
            if (!thread)
            {
                error = GetLastError();
            }
            else
            {
                const DWORD waitResult = WaitForSingleObject(thread, 10000);
                if (waitResult == WAIT_OBJECT_0)
                {
                    DWORD exitCode = 0;
                    if (!GetExitCodeThread(thread, &exitCode))
                        error = GetLastError();
                    else if (!exitCode)
                        error = ERROR_DLL_INIT_FAILED;
                    else
                        ok = true;
                }
                else
                {
                    remotePathMayBeInUse = true;
                    remoteThreadTimedOut = waitResult == WAIT_TIMEOUT;
                    remoteThreadWaitFailed = !remoteThreadTimedOut;
                    error = remoteThreadTimedOut ? WAIT_TIMEOUT : GetLastError();
                    if (remoteThreadWaitFailed && error == ERROR_SUCCESS)
                        error = ERROR_GEN_FAILURE;
                }
                CloseHandle(thread);
            }
        }
    }

    if (!remotePathMayBeInUse)
    {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    }
    else
    {
        // The remote thread may still be reading remotePath; leave it allocated rather than racing the target.
    }
    CloseHandle(process);

    if (ok)
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - injected OK");
    else if (remoteThreadTimedOut)
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, remote thread timed out; remote path cleanup skipped");
    else if (remoteThreadWaitFailed)
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, remote thread wait failed; remote path cleanup skipped, " + lastErrorText(error));
    else
        appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, " + lastErrorText(error));

    return ok;
}

void watcherLoop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    WhitelistMatchSnapshot whitelist;
    std::uint64_t whitelistVersion = 0;

    while (g_running.load(std::memory_order_relaxed))
    {
        if (!g_paused.load(std::memory_order_relaxed))
        {
            refreshWhitelistMatchSnapshot(whitelist, whitelistVersion);
            const bool shouldPruneAttemptedPids = hasAttemptedProcesses();
            const bool hasWhitelistEntries = !whitelist.names.empty() || whitelist.hasPathEntries;
            if (hasWhitelistEntries || shouldPruneAttemptedPids)
            {
                std::unordered_set<DWORD> missingAttemptedPids;
                if (shouldPruneAttemptedPids)
                    missingAttemptedPids = attemptedPidsSnapshot();

                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snapshot != INVALID_HANDLE_VALUE)
                {
                    PROCESSENTRY32W entry{};
                    entry.dwSize = static_cast<DWORD>(sizeof(entry));
                    if (Process32FirstW(snapshot, &entry))
                    {
                        do
                        {
                            const DWORD pid = entry.th32ProcessID;
                            if (shouldPruneAttemptedPids)
                                missingAttemptedPids.erase(pid);

                            if (!hasWhitelistEntries)
                                continue;

                            const std::wstring exeLower = toLower(std::wstring(entry.szExeFile));
                            const bool nameMatched = whitelist.names.find(exeLower) != whitelist.names.end();
                            const bool pathNameMatched = !nameMatched && whitelist.hasPathEntries &&
                                whitelist.pathNames.find(exeLower) != whitelist.pathNames.end();
                            if (!nameMatched && !pathNameMatched)
                                continue;

                            if (pidHasUnknownAttempt(pid))
                                continue;

                            ProcessMetadata metadata{};
                            if (!queryProcessMetadata(pid, pathNameMatched, metadata))
                            {
                                if (nameMatched)
                                {
                                    const std::wstring exeName = entry.szExeFile;
                                    markProcessAttempted(pid, kUnknownProcessCreationTime);
                                    appendLog(exeName + L" (PID " + std::to_wstring(pid) + L") - failed, could not identify process instance");
                                }
                                continue;
                            }

                            if (processInstanceWasAttempted(pid, metadata.creationTime))
                                continue;

                            if (!nameMatched && !matchesWhitelistedPath(whitelist, metadata.fullPath))
                                continue;

                            const std::wstring exeName = entry.szExeFile;
                            markProcessAttempted(pid, metadata.creationTime);
                            injectPayload(pid, exeName);
                        } while (Process32NextW(snapshot, &entry));
                    }

                    CloseHandle(snapshot);
                    if (shouldPruneAttemptedPids)
                        removeAttemptedProcesses(missingAttemptedPids);
                }
            }
        }

        if (g_stopEvent)
        {
            if (WaitForSingleObject(g_stopEvent, kWatcherScanIntervalMs) == WAIT_OBJECT_0)
                break;
        }
        else
        {
            Sleep(kWatcherScanIntervalMs);
        }
    }
}

bool startWithWindowsEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    wchar_t value[MAX_PATH * 4]{};
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(sizeof(value));
    const LONG result = RegQueryValueExW(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);

    const std::wstring expected = L"\"" + g_exePath + L"\"";
    return result == ERROR_SUCCESS && type == REG_SZ && std::wstring(value) == expected;
}

void setStartWithWindows(bool enabled)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    if (enabled)
    {
        std::wstring command = L"\"" + g_exePath + L"\"";
        RegSetValueExW(key, kRunValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, kRunValueName);
    }

    RegCloseKey(key);
}

void updateTrayTip()
{
    const size_t watched = whitelistSnapshot().size();
    std::wstring tip = L"Flip Config Bypass\r\n";
    tip += g_paused.load(std::memory_order_relaxed) ? L"Paused" : L"Watching " + std::to_wstring(watched) + L" apps";
    wcsncpy_s(g_tray.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

struct EditorState
{
    HWND title = nullptr;
    HWND list = nullptr;
    HWND hint = nullptr;
    HWND entry = nullptr;
};

void resizeEditor(HWND hwnd, EditorState* state)
{
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const int margin = scale(22);
    const int gap = scale(10);
    const int addWidth = scale(78);
    const int browseWidth = scale(100);
    const int buttonY = rect.bottom - scale(52);
    MoveWindow(state->title, margin, scale(10), rect.right - margin * 2, scale(28), TRUE);
    MoveWindow(state->hint, margin, scale(42), rect.right - margin * 2, scale(24), TRUE);
    MoveWindow(state->entry, margin, scale(74), rect.right - margin * 2 - addWidth - browseWidth - gap * 2, scale(32), TRUE);
    MoveWindow(GetDlgItem(hwnd, kIdAddEntry), rect.right - margin - addWidth - browseWidth - gap, scale(74), addWidth, scale(32), TRUE);
    MoveWindow(GetDlgItem(hwnd, kIdAddExe), rect.right - margin - browseWidth, scale(74), browseWidth, scale(32), TRUE);
    MoveWindow(state->list, margin, scale(118), rect.right - margin * 2, buttonY - scale(132), TRUE);
    MoveWindow(GetDlgItem(hwnd, kIdRemoveExe), margin, buttonY, scale(92), scale(32), TRUE);
    MoveWindow(GetDlgItem(hwnd, kIdSave), rect.right - scale(184), buttonY, scale(78), scale(32), TRUE);
    MoveWindow(GetDlgItem(hwnd, kIdCancel), rect.right - scale(94), buttonY, scale(78), scale(32), TRUE);
}

void populateWhitelistList(HWND list)
{
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const std::wstring& entry : whitelistSnapshot())
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.c_str()));
}

bool saveWhitelistList(HWND list)
{
    const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    if (count == LB_ERR)
        return false;

    std::wstring content;
    for (int i = 0; i < count; ++i)
    {
        const int len = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, i, 0));
        if (len == LB_ERR)
            continue;

        std::wstring item(static_cast<size_t>(len) + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, i, reinterpret_cast<LPARAM>(item.data()));
        item.resize(static_cast<size_t>(len));
        if (!content.empty())
            content += L"\r\n";
        content += item;
    }
    if (!content.empty())
        content += L"\r\n";

    if (!writeTextFile(g_whitelistPath, content))
        return false;

    loadWhitelist();
    return true;
}

bool listContainsNormalizedEntry(HWND list, const std::wstring& entry)
{
    const std::wstring normalizedEntry = normalizeWhitelistValue(entry);
    const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        const int len = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, i, 0));
        if (len == LB_ERR)
            continue;

        std::wstring item(static_cast<size_t>(len) + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, i, reinterpret_cast<LPARAM>(item.data()));
        item.resize(static_cast<size_t>(len));
        if (normalizeWhitelistValue(item) == normalizedEntry)
            return true;
    }

    return false;
}

void addEntryFromEdit(EditorState* state)
{
    const int len = GetWindowTextLengthW(state->entry);
    if (len <= 0)
        return;

    std::wstring item(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(state->entry, item.data(), len + 1);
    item.resize(static_cast<size_t>(len));
    item = trim(item);
    if (item.empty())
        return;

    if (!listContainsNormalizedEntry(state->list, item))
        SendMessageW(state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    SetWindowTextW(state->entry, L"");
    SetFocus(state->entry);
}

LRESULT CALLBACK editorProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<EditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        return TRUE;
    case WM_CREATE:
        state = reinterpret_cast<EditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        state->title = CreateWindowW(L"STATIC", L"Whitelisted executables", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, nullptr, g_instance, nullptr);
        setFont(state->title, g_titleFont);
        state->hint = CreateWindowW(L"STATIC", L"Add executable names or full paths. Only these processes are touched.",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, g_instance, nullptr);
        setFont(state->hint);
        state->entry = CreateWindowExW(0, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, menuId(kIdEntry), g_instance, nullptr);
        setFont(state->entry);
        SendMessageW(state->entry, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Example: 007FirstLight.exe"));
        state->list = CreateWindowExW(0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd, nullptr, g_instance, nullptr);
        setFont(state->list);
        populateWhitelistList(state->list);
        createButton(hwnd, kIdAddEntry, L"Add");
        createButton(hwnd, kIdAddExe, L"Browse...");
        createButton(hwnd, kIdRemoveExe, L"Remove");
        createButton(hwnd, kIdSave, L"Save", BS_DEFPUSHBUTTON);
        createButton(hwnd, kIdCancel, L"Cancel");
        resizeEditor(hwnd, state);
        return 0;
    case WM_SIZE:
        if (state && state->list)
            resizeEditor(hwnd, state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == kIdAddEntry)
        {
            addEntryFromEdit(state);
        }
        else if (LOWORD(wparam) == kIdAddExe)
        {
            wchar_t file[MAX_PATH * 4]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = static_cast<DWORD>(sizeof(ofn));
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Executable Files (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = static_cast<DWORD>(std::size(file));
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn))
            {
                if (!listContainsNormalizedEntry(state->list, file))
                    SendMessageW(state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(file));
            }
        }
        else if (LOWORD(wparam) == kIdRemoveExe)
        {
            const int sel = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            if (sel != LB_ERR)
                SendMessageW(state->list, LB_DELETESTRING, sel, 0);
        }
        else if (LOWORD(wparam) == kIdSave)
        {
            if (saveWhitelistList(state->list))
            {
                updateTrayTip();
                DestroyWindow(hwnd);
            }
            else
            {
                MessageBoxW(hwnd, L"Could not save whitelist.txt.", L"Flip Config Bypass", MB_ICONERROR | MB_OK);
            }
        }
        else if (LOWORD(wparam) == kIdCancel)
        {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_textColor);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case WM_CTLCOLORLISTBOX:
    {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, g_textColor);
        SetBkColor(dc, g_fieldColor);
        return reinterpret_cast<LRESULT>(g_fieldBrush);
    }
    case WM_CTLCOLOREDIT:
    {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, g_textColor);
        SetBkColor(dc, g_fieldColor);
        return reinterpret_cast<LRESULT>(g_fieldBrush);
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void showEditor(HWND owner)
{
    EditorState state{};
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kEditorClass, L"Flip Config Bypass - Whitelist",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, scale(620), scale(460),
        nullptr, nullptr, g_instance, &state);
    if (!hwnd)
        return;

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG msg{};
    BOOL messageResult = FALSE;
    while (IsWindow(hwnd) && (messageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (messageResult == 0)
        PostQuitMessage(0);
}

void openLog()
{
    ensureDefaultFiles();
    ShellExecuteW(nullptr, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showTrayMenu(HWND hwnd)
{
    updateTrayTip();

    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    const size_t watched = whitelistSnapshot().size();
    const bool paused = g_paused.load(std::memory_order_relaxed);
    const std::wstring status = (paused ? L"Paused - " : L"Running - ") + std::to_wstring(watched) + L" apps watched";

    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"Flip Config Bypass");
    AppendMenuW(menu, MF_STRING | MF_DISABLED | (paused ? 0 : MF_CHECKED), 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdEditWhitelist, L"Edit Whitelist...");
    AppendMenuW(menu, MF_STRING, kIdViewLog, L"Open Log...");
    AppendMenuW(menu, MF_STRING | (paused ? MF_CHECKED : 0), kIdPauseWatching, L"Pause Watching");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (startWithWindowsEnabled() ? MF_CHECKED : 0), kIdStartWithWindows, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdExit, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void addTrayIcon(HWND hwnd)
{
    g_tray.cbSize = static_cast<DWORD>(sizeof(g_tray));
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = g_appIcon;
    wcsncpy_s(g_tray.szTip, L"Flip Config Bypass", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    updateTrayTip();
}

void removeTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (g_taskbarCreatedMessage && msg == g_taskbarCreatedMessage)
    {
        addTrayIcon(hwnd);
        return 0;
    }

    switch (msg)
    {
    case kTrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP)
            showTrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case kIdEditWhitelist:
            showEditor(hwnd);
            return 0;
        case kIdViewLog:
            openLog();
            return 0;
        case kIdPauseWatching:
            g_paused.store(!g_paused.load(std::memory_order_relaxed), std::memory_order_relaxed);
            updateTrayTip();
            return 0;
        case kIdStartWithWindows:
            setStartWithWindows(!startWithWindowsEnabled());
            return 0;
        case kIdExit:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        g_running.store(false, std::memory_order_relaxed);
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        removeTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool registerClasses()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = g_appIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_windowBrush;
    if (!RegisterClassW(&wc))
        return false;

    wc = {};
    wc.lpfnWndProc = editorProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = kEditorClass;
    wc.hIcon = g_appIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_windowBrush;
    if (!RegisterClassW(&wc))
        return false;

    return true;
}

bool initPaths()
{
    wchar_t path[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || static_cast<size_t>(length) >= std::size(path))
        return false;

    g_exePath.assign(path, length);
    const size_t slash = g_exePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return false;

    g_exeDir = g_exePath.substr(0, slash);
    g_payloadPath = joinPath(g_exeDir, L"FlipConfigPayload.dll");
    g_whitelistPath = joinPath(g_exeDir, L"whitelist.txt");
    g_logPath = joinPath(g_exeDir, L"FlipConfigBypass.log");
    return true;
}

bool acquireSingleInstance()
{
    g_instanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
    if (!g_instanceMutex)
        return false;

    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return true;

    CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
    return false;
}

void releaseSingleInstance()
{
    if (!g_instanceMutex)
        return;

    ReleaseMutex(g_instanceMutex);
    CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
}

void closeStopEvent()
{
    if (!g_stopEvent)
        return;

    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    enableDpiAwareness();
    g_dpi = systemDpi();
    g_instance = instance;
    if (!supportedX64Host())
    {
        MessageBoxW(nullptr,
            L"Flip Config Bypass supports only native x64 Windows. ARM64 hosts are not supported.",
            L"Flip Config Bypass",
            MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!initPaths())
        return 1;
    if (!acquireSingleInstance())
        return 0;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    initFonts();
    g_windowBrush = CreateSolidBrush(g_windowColor);
    g_fieldBrush = CreateSolidBrush(g_fieldColor);
    g_appIcon = reinterpret_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    g_ownsAppIcon = g_appIcon != nullptr;
    if (!g_appIcon)
        g_appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    ensureDefaultFiles();
    trimLogIfTooLarge();
    loadWhitelist();

    if (!registerClasses())
    {
        closeStopEvent();
        releaseSingleInstance();
        return 1;
    }

    g_window = CreateWindowW(kWindowClass, L"Flip Config Bypass", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, g_instance, nullptr);
    if (!g_window)
    {
        closeStopEvent();
        releaseSingleInstance();
        return 1;
    }

    addTrayIcon(g_window);
    g_watcherThread = std::thread(watcherLoop);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running.store(false, std::memory_order_relaxed);
    if (g_stopEvent)
        SetEvent(g_stopEvent);
    if (g_watcherThread.joinable())
        g_watcherThread.join();

    if (g_appIcon && g_ownsAppIcon)
        DestroyIcon(g_appIcon);
    if (g_uiFont)
        DeleteObject(g_uiFont);
    if (g_titleFont)
        DeleteObject(g_titleFont);
    if (g_windowBrush)
        DeleteObject(g_windowBrush);
    if (g_fieldBrush)
        DeleteObject(g_fieldBrush);
    closeStopEvent();
    releaseSingleInstance();

    return 0;
}
