// speed roll - lightweight native Win32 mouse-wheel enhancer.
// C++17, no third-party runtime, single source file.
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <imm.h>
#include <psapi.h>

#include "resource.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr ULONG_PTR kInjectedTag = 0x57414343;  // "WACC"
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kInjectMessage = WM_APP + 2;
constexpr UINT kToastTimerId = 1;
constexpr UINT_PTR kToastLifetimeMs = 1000;
constexpr UINT kHomeEndCooldownMs = 250;
constexpr int kWheelDelta = 120;
constexpr double kDefaultFastThreshold = 100.0;
constexpr int kDefaultMinFastTicks = 3;
constexpr double kDefaultMaxMultiplier = 8.0;
constexpr double kDefaultCurvePower = 1.8;
constexpr double kDefaultHorizontalMultiplier = 1.0;

// Settings-window geometry in 96-DPI logical pixels.
constexpr int kSettingsWidth = 940;
constexpr int kSettingsHeight = 680;
constexpr int kSidebarWidth = 210;
constexpr int kPageHeaderHeight = 76;
constexpr int kContentLeft = 234;
constexpr int kContentRight = 916;
constexpr int kSummaryTop = 92;
constexpr int kSummaryHeight = 80;
constexpr int kSummaryTileWidth = 220;
constexpr int kSummaryGap = 11;
constexpr int kActionTop = 186;
constexpr int kActionHeight = 64;
constexpr int kLowerTop = 264;
constexpr int kLowerHeight = 320;
constexpr int kLeftCardLeft = 234;
constexpr int kLeftCardRight = 640;
constexpr int kLeftCardTop = 264;
constexpr int kLeftCardBottom = 584;
constexpr int kRightCardLeft = 652;
constexpr int kRightCardRight = 916;
constexpr int kBehaviorTop = 264;
constexpr int kBehaviorBottom = 386;
constexpr int kBlacklistCardTop = 398;
constexpr int kBlacklistCardBottom = 584;
constexpr int kFooterBorderY = 594;
constexpr int kButtonY = 606;
constexpr int kButtonHeight = 42;
constexpr int kFieldX1 = 252;
constexpr int kFieldX2 = 443;
constexpr int kFieldWidth = 179;
constexpr int kFieldY1 = 312;
constexpr int kFieldY2 = 380;
constexpr int kFieldY3 = 448;
constexpr int kFieldEditOffset = 22;
constexpr int kFieldEditHeight = 30;
constexpr int kBlacklistEditX = 668;
constexpr int kBlacklistEditY = 448;
constexpr int kBlacklistEditWidth = 232;
constexpr int kBlacklistEditHeight = 120;
constexpr wchar_t kIniSection[] = L"speed roll";
constexpr wchar_t kRunKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"speed roll";
constexpr wchar_t kLegacyRunValue[] = L"WheelAccel";
constexpr wchar_t kMainClass[] = L"SpeedRoll.MainWindow";
constexpr wchar_t kSettingsClass[] = L"SpeedRoll.SettingsWindow";
constexpr wchar_t kToastClass[] = L"SpeedRoll.ToastWindow";

enum TrayCommand : UINT {
    IDM_OPEN_SETTINGS = 1001,
    IDM_TOGGLE_MODE = 1002,
    IDM_GLOBAL_DISABLE = 1003,
    IDM_DISABLE_CURRENT_APP = 1004,
    IDM_EXIT = 1005,
};

enum InjectAction : WPARAM {
    INJECT_WHEEL = 1,
    INJECT_HWHEEL = 2,
    INJECT_XBUTTON1 = 3,
    INJECT_XBUTTON2 = 4,
};

enum SettingsId : int {
    IDC_EDIT_FAST = 2001,
    IDC_EDIT_MIN_TICKS,
    IDC_EDIT_MAX,
    IDC_EDIT_CURVE,
    IDC_EDIT_HORIZONTAL,
    IDC_CHK_ZOOM,
    IDC_CHK_AUTOSTART,
    IDC_EDIT_BLACKLIST,
    IDC_BTN_SAVE,
    IDC_BTN_RESET,
    IDC_BTN_CLOSE,
};

struct Config {
    double fastThreshold = kDefaultFastThreshold;
    int minFastTicks = kDefaultMinFastTicks;
    double maxMultiplier = kDefaultMaxMultiplier;
    double curvePower = kDefaultCurvePower;
    double horizontalMultiplier = kDefaultHorizontalMultiplier;
    bool preventZoomOverrun = true;
    bool globalDisabled = false;
    std::vector<std::wstring> disabledProcesses;
};

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_settingsWindow = nullptr;
HWND g_tooltipWindow = nullptr;
HWND g_toastWindow = nullptr;
HHOOK g_mouseHook = nullptr;
HANDLE g_instanceMutex = nullptr;
HICON g_appIcon = nullptr;
HICON g_appIconSmall = nullptr;
HFONT g_uiFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_metricFont = nullptr;
HFONT g_tinyFont = nullptr;
HFONT g_subtitleFont = nullptr;
HFONT g_headerFont = nullptr;
HFONT g_hintFont = nullptr;
HFONT g_toastFont = nullptr;
HBRUSH g_cardBrush = nullptr;
HBRUSH g_editBrush = nullptr;
NOTIFYICONDATAW g_trayData{};
UINT g_taskbarCreatedMessage = 0;

Config g_config;
bool g_hyperMode = true;
bool g_autoStart = false;
std::wstring g_exePath;
std::wstring g_exeDir;
std::wstring g_exeName;
std::wstring g_configPath;
std::wstring g_lastActiveProcess;
std::wstring g_toastText;
int g_dpi = 96;
int g_focusedEditId = 0;

// PID -> process name cache. The low-level mouse hook must never open another
// process on the input path, so names are resolved once per PID and reused.
constexpr size_t kPidCacheLimit = 512;
std::unordered_map<DWORD, std::wstring> g_pidCache;
HWINEVENTHOOK g_foregroundHook = nullptr;

// Foreground cache filled by the WinEvent hook. The mouse hook only reads
// these two values, so scrolling never performs a window/process query.
std::wstring g_currentForegroundProc;
HWND g_currentForegroundWindow = nullptr;

// Opt-in latency diagnostics. Enabled when "diagnostics.on" sits next to the
// executable; the counters are updated from the hook with interlocked ops only
// and flushed to diagnostics.log from the message loop.
bool g_diagEnabled = false;
volatile LONG g_diagWheelCount = 0;
volatile LONG g_diagDelayBuckets[6] = {};
volatile LONG g_diagDelaySumMs = 0;
volatile LONG g_diagDelayMaxMs = 0;
volatile LONG g_diagWorkSumUs = 0;
volatile LONG g_diagWorkMaxUs = 0;

// Single-producer/single-consumer injection queue. The hook only enqueues and
// signals; a separate high-priority thread performs SendInput so the hook
// thread never blocks inside the input stack.
constexpr size_t kInjectQueueSize = 1024;
struct InjectEntry {
    InjectAction action;
    LONG_PTR data;
};
InjectEntry g_injectQueue[kInjectQueueSize]{};
std::atomic<size_t> g_injectHead{0};
std::atomic<size_t> g_injectTail{0};
HANDLE g_injectEvent = nullptr;

// Side-button state. XBUTTON1 = back, XBUTTON2 = forward.
bool g_x1Down = false;
bool g_x2Down = false;
bool g_x1UsedAsModifier = false;
bool g_x2UsedAsModifier = false;
bool g_comboLatched = false;
DWORD g_lastHomeEndTick = 0;

// Wheel acceleration state.
LARGE_INTEGER g_qpcFrequency{};
double g_lastWheelMs = 0.0;
int g_lastWheelDirection = 0;
double g_wheelRemainder = 0.0;
int g_fastStreak = 0;

int Scale(int value) {
    return MulDiv(value, g_dpi, 96);
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::wstring Trim(const std::wstring& value) {
    size_t begin = 0;
    while (begin < value.size() && std::iswspace(value[begin])) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::iswspace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool EndsWithNoCase(const std::wstring& value, const std::wstring& suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    return _wcsicmp(value.c_str() + (value.size() - suffix.size()), suffix.c_str()) == 0;
}

std::wstring NormalizeProcessName(std::wstring name) {
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        name = name.substr(slash + 1);
    }
    if (EndsWithNoCase(name, L".exe")) {
        name.resize(name.size() - 4);
    }
    return ToLower(Trim(name));
}

std::wstring GetModuleFilePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return std::wstring();
        }
        if (copied < buffer.size() - 1) {
            return std::wstring(buffer.data(), copied);
        }
        if (buffer.size() >= 32768) {
            return std::wstring(buffer.data(), copied);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring GetDirectoryName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

bool ParseInt(const std::wstring& text, int& value) {
    std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    long parsed = std::wcstol(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str()) {
        return false;
    }
    while (*end != L'\0' && std::iswspace(*end)) {
        ++end;
    }
    if (*end != L'\0' || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ParseDouble(const std::wstring& text, double& value) {
    std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    double parsed = std::wcstod(trimmed.c_str(), &end);
    if (end == trimmed.c_str()) {
        return false;
    }
    while (*end != L'\0' && std::iswspace(*end)) {
        ++end;
    }
    if (*end != L'\0' || errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

std::wstring FormatDouble(double value) {
    wchar_t buffer[64]{};
    std::swprintf(buffer, _countof(buffer), L"%.4f", value);
    std::wstring text(buffer);
    while (text.size() > 1 && text.back() == L'0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == L'.') {
        text.pop_back();
    }
    return text;
}

std::wstring GetControlText(HWND parent, int id) {
    HWND control = GetDlgItem(parent, id);
    if (!control) {
        return std::wstring();
    }
    int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return std::wstring();
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    GetWindowTextW(control, buffer.data(), length + 1);
    return std::wstring(buffer.data(), static_cast<size_t>(length));
}

void SetControlText(HWND parent, int id, const std::wstring& text) {
    SetDlgItemTextW(parent, id, text.c_str());
}

std::wstring GetProcessNameById(DWORD processId) {
    if (processId == 0) {
        return std::wstring();
    }
    auto cached = g_pidCache.find(processId);
    if (cached != g_pidCache.end()) {
        return cached->second;
    }

    std::wstring name;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process) {
        wchar_t path[1024]{};
        DWORD pathLength = _countof(path);
        if (QueryFullProcessImageNameW(process, 0, path, &pathLength)) {
            name = NormalizeProcessName(std::wstring(path, pathLength));
        }
        CloseHandle(process);
    }

    // A clear-all policy keeps the common case O(1) and bounds memory; it is
    // far cheaper than evicting individual entries on this path.
    if (g_pidCache.size() >= kPidCacheLimit) {
        g_pidCache.clear();
    }
    g_pidCache.emplace(processId, name);
    return name;
}

std::wstring GetWindowProcessName(HWND window) {
    if (!window) {
        return std::wstring();
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return GetProcessNameById(processId);
}

bool IsShellOrSelf(const std::wstring& name) {
    if (name.empty()) {
        return true;
    }
    return name == g_exeName || name == L"explorer" || name == L"shellexperiencehost" ||
           name == L"searchhost";
}

// Refreshes both the blacklist-facing foreground process and the tray-facing
// display name. Runs only on foreground changes, never on the wheel path.
void UpdateForegroundCache(HWND window) {
    if (!window) {
        return;
    }
    g_currentForegroundWindow = GetAncestor(window, GA_ROOT);
    if (!g_currentForegroundWindow) {
        g_currentForegroundWindow = window;
    }
    std::wstring foreground = GetWindowProcessName(window);
    if (foreground.empty()) {
        return;
    }
    g_currentForegroundProc = foreground;
    if (!IsShellOrSelf(foreground)) {
        g_lastActiveProcess = foreground;
    }
}

void UpdateLastActiveProcess() {
    UpdateForegroundCache(GetForegroundWindow());
}

// Runs on this thread's message loop for every foreground change; the mouse
// hook no longer polls the foreground window or opens processes per event.
void CALLBACK ForegroundEventProc(HWINEVENTHOOK, DWORD event, HWND window, LONG objectId,
                                  LONG childId, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && window && objectId == OBJID_WINDOW &&
        childId == CHILDID_SELF) {
        UpdateForegroundCache(window);
    }
}

bool IsBlacklisted(const std::wstring& processName) {
    if (processName.empty()) {
        return false;
    }
    for (const auto& entry : g_config.disabledProcesses) {
        if (entry == processName) {
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> ParseBlacklist(const std::wstring& text) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= text.size()) {
        size_t separator = text.find_first_of(L";\r\n", start);
        std::wstring token = separator == std::wstring::npos
                                 ? text.substr(start)
                                 : text.substr(start, separator - start);
        token = NormalizeProcessName(token);
        if (!token.empty() &&
            std::find(result.begin(), result.end(), token) == result.end()) {
            result.push_back(token);
        }
        if (separator == std::wstring::npos) {
            break;
        }
        start = separator + 1;
    }
    return result;
}

std::wstring SerializeBlacklist() {
    std::wstring result;
    for (size_t i = 0; i < g_config.disabledProcesses.size(); ++i) {
        if (i != 0) {
            result.push_back(L';');
        }
        result += g_config.disabledProcesses[i];
    }
    return result;
}

std::wstring BlacklistForEditor() {
    std::wstring result;
    for (size_t i = 0; i < g_config.disabledProcesses.size(); ++i) {
        if (i != 0) {
            result += L"\r\n";
        }
        result += g_config.disabledProcesses[i];
    }
    return result;
}

double Clamp(double value, double minimum, double maximum) {
    return std::max(minimum, std::min(maximum, value));
}

double ReadIniDouble(const wchar_t* key, double fallback) {
    wchar_t buffer[128]{};
    GetPrivateProfileStringW(kIniSection, key, L"", buffer, _countof(buffer),
                             g_configPath.c_str());
    double value = fallback;
    if (!ParseDouble(buffer, value)) {
        return fallback;
    }
    return value;
}

void LoadConfig() {
    g_config.fastThreshold = ReadIniDouble(L"FastThreshold", kDefaultFastThreshold);
    g_config.minFastTicks = static_cast<int>(
        GetPrivateProfileIntW(kIniSection, L"MinFastTicks", kDefaultMinFastTicks,
                              g_configPath.c_str()));
    g_config.maxMultiplier = ReadIniDouble(L"MaxMultiplier", kDefaultMaxMultiplier);
    g_config.curvePower = ReadIniDouble(L"CurvePower", kDefaultCurvePower);
    g_config.horizontalMultiplier =
        ReadIniDouble(L"HorizontalMultiplier", kDefaultHorizontalMultiplier);
    g_config.preventZoomOverrun =
        GetPrivateProfileIntW(kIniSection, L"PreventZoomOverrun", 1, g_configPath.c_str()) != 0;
    g_config.globalDisabled =
        GetPrivateProfileIntW(kIniSection, L"GlobalDisabled", 0, g_configPath.c_str()) != 0;

    std::vector<wchar_t> blacklist(32768);
    GetPrivateProfileStringW(kIniSection, L"DisabledProcesses", L"", blacklist.data(),
                             static_cast<DWORD>(blacklist.size()), g_configPath.c_str());
    g_config.disabledProcesses = ParseBlacklist(blacklist.data());

    g_config.fastThreshold = Clamp(g_config.fastThreshold, 1.0, 5000.0);
    g_config.minFastTicks = std::max(1, std::min(24, g_config.minFastTicks));
    g_config.maxMultiplier = Clamp(g_config.maxMultiplier, 1.0, 100.0);
    g_config.curvePower = Clamp(g_config.curvePower, 0.1, 10.0);
    g_config.horizontalMultiplier = Clamp(g_config.horizontalMultiplier, 0.1, 20.0);
}

bool WriteDouble(const wchar_t* key, double value) {
    wchar_t buffer[64]{};
    std::swprintf(buffer, _countof(buffer), L"%.6f", value);
    return WritePrivateProfileStringW(kIniSection, key, buffer, g_configPath.c_str()) != FALSE;
}

bool SaveConfig() {
    bool ok = true;
    wchar_t minTicks[32]{};
    std::swprintf(minTicks, _countof(minTicks), L"%d", g_config.minFastTicks);

    ok = WriteDouble(L"FastThreshold", g_config.fastThreshold) && ok;
    ok = WritePrivateProfileStringW(kIniSection, L"MinFastTicks", minTicks,
                                    g_configPath.c_str()) != FALSE && ok;
    ok = WriteDouble(L"MaxMultiplier", g_config.maxMultiplier) && ok;
    ok = WriteDouble(L"CurvePower", g_config.curvePower) && ok;
    ok = WriteDouble(L"HorizontalMultiplier", g_config.horizontalMultiplier) && ok;
    ok = WritePrivateProfileStringW(kIniSection, L"PreventZoomOverrun",
                                    g_config.preventZoomOverrun ? L"1" : L"0",
                                    g_configPath.c_str()) != FALSE && ok;
    ok = WritePrivateProfileStringW(kIniSection, L"GlobalDisabled",
                                    g_config.globalDisabled ? L"1" : L"0",
                                    g_configPath.c_str()) != FALSE && ok;
    std::wstring blacklist = SerializeBlacklist();
    ok = WritePrivateProfileStringW(kIniSection, L"DisabledProcesses", blacklist.c_str(),
                                    g_configPath.c_str()) != FALSE && ok;
    // Remove the obsolete pre-MinFastTicks key when upgrading an old config.
    WritePrivateProfileStringW(kIniSection, L"HyperMultiplier", nullptr, g_configPath.c_str());
    return ok;
}

bool QueryAutoStart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[2048]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    LONG result = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(value), &bytes);
    if (result != ERROR_SUCCESS) {
        // Migration fallback for the pre-rename autostart value.
        bytes = sizeof(value) - sizeof(wchar_t);
        result = RegQueryValueExW(key, kLegacyRunValue, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(value), &bytes);
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    value[_countof(value) - 1] = L'\0';
    return value[0] != L'\0';
}

bool SetAutoStart(bool enabled, DWORD* errorCode = nullptr) {
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (result == ERROR_FILE_NOT_FOUND) {
        result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                                 nullptr, &key, nullptr);
    }
    if (result != ERROR_SUCCESS) {
        if (errorCode) {
            *errorCode = static_cast<DWORD>(result);
        }
        return false;
    }
    if (enabled) {
        std::wstring command = L"\"" + g_exePath + L"\"";
        result = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        // Remove the value created by the previous application name.
        RegDeleteValueW(key, kLegacyRunValue);
    } else {
        result = RegDeleteValueW(key, kRunValue);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
        RegDeleteValueW(key, kLegacyRunValue);
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS && errorCode) {
        *errorCode = static_cast<DWORD>(result);
    }
    return result == ERROR_SUCCESS;
}

double NowMilliseconds() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return (static_cast<double>(counter.QuadPart) * 1000.0) /
           static_cast<double>(g_qpcFrequency.QuadPart);
}

void UpdateDiagnosticMax(volatile LONG* target, LONG value) {
    LONG current = *target;
    while (value > current) {
        LONG previous = InterlockedCompareExchange(target, value, current);
        if (previous == current) {
            return;
        }
        current = previous;
    }
}

// Called from the hook; interlocked counters only, no I/O on the input path.
void RecordWheelDiagnostics(DWORD delayMs, LONGLONG workUs) {
    InterlockedIncrement(&g_diagWheelCount);
    const LONG bucket = delayMs <= 1 ? 0
                      : delayMs <= 2 ? 1
                      : delayMs <= 5 ? 2
                      : delayMs <= 10 ? 3
                      : delayMs <= 20 ? 4
                                     : 5;
    InterlockedIncrement(&g_diagDelayBuckets[bucket]);
    InterlockedExchangeAdd(&g_diagDelaySumMs, static_cast<LONG>(delayMs));
    UpdateDiagnosticMax(&g_diagDelayMaxMs, static_cast<LONG>(delayMs));
    const LONG work = static_cast<LONG>(workUs);
    InterlockedExchangeAdd(&g_diagWorkSumUs, work);
    UpdateDiagnosticMax(&g_diagWorkMaxUs, work);
}

void DumpWheelDiagnostics() {
    if (!g_diagEnabled) {
        return;
    }
    const LONG count = g_diagWheelCount;
    if (count == 0) {
        return;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[512];
    std::snprintf(
        line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u count=%ld "
        "delay<=1:%ld <=2:%ld <=5:%ld <=10:%ld <=20:%ld >20:%ld "
        "avg=%.2fms max=%ldms work_avg=%.3fms work_max=%ldus\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, count,
        g_diagDelayBuckets[0], g_diagDelayBuckets[1], g_diagDelayBuckets[2],
        g_diagDelayBuckets[3], g_diagDelayBuckets[4], g_diagDelayBuckets[5],
        static_cast<double>(g_diagDelaySumMs) / count, g_diagDelayMaxMs,
        static_cast<double>(g_diagWorkSumUs) / count / 1000.0, g_diagWorkMaxUs);
    HANDLE file = CreateFileW((g_exeDir + L"\\diagnostics.log").c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        CloseHandle(file);
    }
}

void ResetWheelAcceleration() {
    g_wheelRemainder = 0.0;
    g_lastWheelDirection = 0;
    g_lastWheelMs = 0.0;
    g_fastStreak = 0;
}

// Modern native injection: SendInput reaches the input stack without the
// legacy mouse_event compatibility layer Windows 11 keeps for old binaries.
void SendMouseInput(DWORD flags, DWORD mouseData) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouseData;
    input.mi.dwExtraInfo = kInjectedTag;
    SendInput(1, &input, sizeof(input));
}

void SendWheelDelta(int delta) {
    SendMouseInput(MOUSEEVENTF_WHEEL, static_cast<DWORD>(delta));
}

void SendHorizontalWheelDelta(int delta) {
    SendMouseInput(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(delta));
}

void EnsureWindowForeground(HWND target) {
    if (!target || !IsWindow(target) || GetForegroundWindow() == target) {
        return;
    }
    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
    }

    const DWORD currentThread = GetCurrentThreadId();
    HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    bool attached = false;
    if (foregroundThread != 0 && foregroundThread != currentThread) {
        attached = AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    }

    SetForegroundWindow(target);
    BringWindowToTop(target);

    if (attached) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }

    // Let the target process its activation before the navigation keys arrive.
    if (GetForegroundWindow() == target) {
        Sleep(20);
    }
}

void SendDocumentExtremeBlocking(bool top, HWND target) {
    EnsureWindowForeground(target);

    // WPS' Qt engine and LibreOffice often miss the Ctrl modifier when it is
    // packed into the same SendInput batch as the navigation key. Send each
    // key separately with its hardware scan code and a short settle delay.
    const BYTE controlScan = static_cast<BYTE>(MapVirtualKeyW(VK_LCONTROL, 0));
    const WORD navigationKey = top ? VK_HOME : VK_END;
    const BYTE navigationScan = static_cast<BYTE>(MapVirtualKeyW(navigationKey, 0));

    keybd_event(VK_LCONTROL, controlScan, 0, kInjectedTag);
    Sleep(15);
    keybd_event(static_cast<BYTE>(navigationKey), navigationScan,
                KEYEVENTF_EXTENDEDKEY, kInjectedTag);
    Sleep(15);
    keybd_event(static_cast<BYTE>(navigationKey), navigationScan,
                KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, kInjectedTag);
    Sleep(15);
    keybd_event(VK_LCONTROL, controlScan, KEYEVENTF_KEYUP, kInjectedTag);
}

void StartDocumentExtremeAsync(bool top, HWND target) {
    // This helper is called from the low-level mouse hook. Never block the hook
    // with the 45 ms focus/settle sequence; run it on a detached worker.
    std::thread([top, target]() {
        SendDocumentExtremeBlocking(top, target);
    }).detach();
}

void ReplayXButton(WORD button) {
    SendMouseInput(MOUSEEVENTF_XDOWN, button);
    SendMouseInput(MOUSEEVENTF_XUP, button);
}

DWORD WINAPI InjectionThreadProc(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        WaitForSingleObject(g_injectEvent, INFINITE);
        for (;;) {
            const size_t tail = g_injectTail.load(std::memory_order_relaxed);
            if (tail == g_injectHead.load(std::memory_order_acquire)) {
                break;
            }
            const InjectEntry entry = g_injectQueue[tail];
            g_injectTail.store((tail + 1) % kInjectQueueSize, std::memory_order_release);
            switch (entry.action) {
                case INJECT_WHEEL:
                    SendWheelDelta(static_cast<int>(entry.data));
                    break;
                case INJECT_HWHEEL:
                    SendHorizontalWheelDelta(static_cast<int>(entry.data));
                    break;
                case INJECT_XBUTTON1:
                    ReplayXButton(XBUTTON1);
                    break;
                case INJECT_XBUTTON2:
                    ReplayXButton(XBUTTON2);
                    break;
            }
        }
    }
}

// Never call SendInput directly from the hook thread: the injected event
// re-enters the same hook chain and can stall input for the low-level hook
// timeout. Enqueue instead; the dedicated thread performs the injection.
void QueueInjection(InjectAction action, LONG_PTR data = 0) {
    if (!g_injectEvent) {
        if (g_mainWindow) {
            PostMessageW(g_mainWindow, kInjectMessage, static_cast<WPARAM>(action),
                         static_cast<LPARAM>(data));
        }
        return;
    }
    const size_t head = g_injectHead.load(std::memory_order_relaxed);
    const size_t next = (head + 1) % kInjectQueueSize;
    if (next == g_injectTail.load(std::memory_order_acquire)) {
        return;  // Queue full: drop rather than block the hook callback.
    }
    g_injectQueue[head] = InjectEntry{action, data};
    g_injectHead.store(next, std::memory_order_release);
    SetEvent(g_injectEvent);
}

void EnsureToastWindow();
void ShowToast(const std::wstring& text);
void UpdateTrayTooltip();

void ToggleHyperMode() {
    g_hyperMode = !g_hyperMode;
    ResetWheelAcceleration();
    ShowToast(g_hyperMode ? L"已切换到极速模式" : L"已切换到普通模式");
    UpdateTrayTooltip();
    if (g_settingsWindow) {
        InvalidateRect(g_settingsWindow, nullptr, TRUE);
    }
}

bool IsScrollDisabledFor(const std::wstring& processName) {
    return g_config.globalDisabled || IsBlacklisted(processName);
}

bool HandleWheel(const MSLLHOOKSTRUCT& info) {
    int rawDelta = static_cast<short>(HIWORD(info.mouseData));
    if (rawDelta == 0) {
        return false;
    }
    const int direction = rawDelta > 0 ? 1 : -1;

    // The foreground process comes from the WinEvent cache: no WindowFromPoint,
    // no GetWindowThreadProcessId and no process query on the wheel path.
    if (IsScrollDisabledFor(g_currentForegroundProc)) {
        ResetWheelAcceleration();
        return false;  // native wheel is passed through untouched
    }

    // Forward button held alone: wheel jumps to the top or bottom.
    if (g_x2Down && !g_x1Down) {
        g_x2UsedAsModifier = true;
        DWORD now = GetTickCount();
        if (static_cast<DWORD>(now - g_lastHomeEndTick) >= kHomeEndCooldownMs) {
            g_lastHomeEndTick = now;
            // Uses the cached top-level foreground window instead of a
            // WindowFromPoint lookup inside the hook callback.
            StartDocumentExtremeAsync(direction > 0, g_currentForegroundWindow);
            ShowToast(direction > 0 ? L"已直达顶部" : L"已直达底部");
        }
        ResetWheelAcceleration();
        return true;
    }

    // Back button held alone: wheel becomes horizontal scrolling.
    if (g_x1Down && !g_x2Down) {
        g_x1UsedAsModifier = true;
        int horizontalDelta = static_cast<int>(
            std::lround(-static_cast<double>(direction) * kWheelDelta *
                        g_config.horizontalMultiplier));
        if (horizontalDelta == 0) {
            horizontalDelta = -direction * kWheelDelta;
        }
        QueueInjection(INJECT_HWHEEL, horizontalDelta);
        ResetWheelAcceleration();
        return true;
    }

    // Both buttons held: this is the mode-switch chord; do not scroll.
    if (g_x1Down && g_x2Down) {
        return true;
    }

    // Normal mode is a strict native 1:1 wheel: never intercept or accelerate.
    if (!g_hyperMode) {
        ResetWheelAcceleration();
        return false;
    }

    const double nowMs = NowMilliseconds();
    const double deltaMs = g_lastWheelMs > 0.0 ? nowMs - g_lastWheelMs : 1.0e9;
    const bool reversed =
        g_lastWheelDirection != 0 && direction != g_lastWheelDirection;
    const bool slow = deltaMs >= g_config.fastThreshold;

    g_lastWheelDirection = direction;
    g_lastWheelMs = nowMs;

    // A pause, a slow tick, or a reversal ends the fast streak. The native
    // event is passed through untouched so Win11 keeps its smooth wheel path.
    if (slow || reversed) {
        g_fastStreak = 0;
        g_wheelRemainder = 0.0;
        return false;
    }

    if (g_config.preventZoomOverrun &&
        ((GetKeyState(VK_CONTROL) & 0x8000) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0)) {
        g_fastStreak = 0;
        g_wheelRemainder = 0.0;
        return false;
    }

    ++g_fastStreak;
    if (g_fastStreak > 1000000) {
        g_fastStreak = 1000000;
    }

    double multiplier = 1.0;
    if (g_fastStreak >= g_config.minFastTicks) {
        double ratio = (g_config.fastThreshold - deltaMs) / g_config.fastThreshold;
        ratio = Clamp(ratio, 0.0, 1.0);
        multiplier = 1.0 + (g_config.maxMultiplier - 1.0) *
                                std::pow(ratio, g_config.curvePower);
    } else {
        // Keep the first MinFastTicks ticks at exactly one native notch.
        g_wheelRemainder = 0.0;
    }

    const double totalClicks = multiplier + g_wheelRemainder;
    const double wholeClicks = std::floor(totalClicks);
    g_wheelRemainder = totalClicks - wholeClicks;

    if (wholeClicks >= 2.0) {
        int clicks = static_cast<int>(wholeClicks);
        if (clicks > 1000) {
            clicks = 1000;
        }
        QueueInjection(INJECT_WHEEL, direction * kWheelDelta * clicks);
        return true;  // swallow the native notch, the aggregated event replaces it
    }

    // A single notch needs no rewrite: hand the original hardware event back so
    // Windows 11's smooth scrolling pipeline is never interrupted.
    return false;
}

bool HandleXButtonDown(bool forwardButton) {
    if (forwardButton) {
        if (!g_x2Down) {
            g_x2UsedAsModifier = false;
        }
        g_x2Down = true;
    } else {
        if (!g_x1Down) {
            g_x1UsedAsModifier = false;
        }
        g_x1Down = true;
    }

    if (g_x1Down && g_x2Down && !g_comboLatched) {
        g_comboLatched = true;
        g_x1UsedAsModifier = true;
        g_x2UsedAsModifier = true;
        ToggleHyperMode();
    }
    return true;
}

bool HandleXButtonUp(bool forwardButton) {
    bool wasDown = forwardButton ? g_x2Down : g_x1Down;
    if (!wasDown) {
        return false;
    }

    bool used = forwardButton ? g_x2UsedAsModifier : g_x1UsedAsModifier;
    if (forwardButton) {
        g_x2Down = false;
    } else {
        g_x1Down = false;
    }

    if (!used) {
        QueueInjection(forwardButton ? INJECT_XBUTTON2 : INJECT_XBUTTON1);
    }

    if (!g_x1Down && !g_x2Down) {
        g_comboLatched = false;
        g_x1UsedAsModifier = false;
        g_x2UsedAsModifier = false;
    }
    return true;
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM message, LPARAM lParam) {
    // Ultra-fast reject: only wheel and side-button messages need any work.
    // Everything else (including injected traffic below) goes straight back.
    if (code < 0 || (message != WM_MOUSEWHEEL && message != WM_XBUTTONDOWN &&
                     message != WM_XBUTTONUP)) {
        return CallNextHookEx(g_mouseHook, code, message, lParam);
    }
    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (!info || info->dwExtraInfo == kInjectedTag) {
        return CallNextHookEx(g_mouseHook, code, message, lParam);
    }

    if (message == WM_MOUSEWHEEL) {
        LARGE_INTEGER start{};
        DWORD delayMs = 0;
        if (g_diagEnabled) {
            QueryPerformanceCounter(&start);
            delayMs = GetTickCount() - info->time;
        }
        const bool consumed = HandleWheel(*info);
        if (g_diagEnabled) {
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&end);
            const LONGLONG ticks = end.QuadPart - start.QuadPart;
            const LONGLONG workUs = g_qpcFrequency.QuadPart > 0
                                        ? ticks * 1000000 / g_qpcFrequency.QuadPart
                                        : 0;
            RecordWheelDiagnostics(delayMs, workUs);
        }
        if (consumed) {
            return 1;
        }
    } else if (message == WM_XBUTTONDOWN) {
        WORD button = HIWORD(info->mouseData);
        if (button == XBUTTON1 || button == XBUTTON2) {
            HandleXButtonDown(button == XBUTTON2);
            return 1;
        }
    } else if (message == WM_XBUTTONUP) {
        WORD button = HIWORD(info->mouseData);
        if (button == XBUTTON1 || button == XBUTTON2) {
            if (HandleXButtonUp(button == XBUTTON2)) {
                return 1;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, message, lParam);
}

HICON CreateAppIcon() {
    constexpr int size = 64;
    BITMAPV5HEADER bitmapInfo{};
    bitmapInfo.bV5Size = sizeof(bitmapInfo);
    bitmapInfo.bV5Width = size;
    bitmapInfo.bV5Height = -size;
    bitmapInfo.bV5Planes = 1;
    bitmapInfo.bV5BitCount = 32;
    bitmapInfo.bV5Compression = BI_BITFIELDS;
    bitmapInfo.bV5RedMask = 0x00FF0000;
    bitmapInfo.bV5GreenMask = 0x0000FF00;
    bitmapInfo.bV5BlueMask = 0x000000FF;
    bitmapInfo.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(memory, reinterpret_cast<BITMAPINFO*>(&bitmapInfo),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (color) {
            DeleteObject(color);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    auto* pixels = static_cast<uint32_t*>(bits);

    auto roundedRectDistance = [](double px, double py, double left, double top,
                                  double right, double bottom, double radius) {
        const double halfWidth = (right - left) * 0.5;
        const double halfHeight = (bottom - top) * 0.5;
        const double qx = std::abs(px - (left + halfWidth)) - (halfWidth - radius);
        const double qy = std::abs(py - (top + halfHeight)) - (halfHeight - radius);
        const double outside = std::sqrt(std::max(qx, 0.0) * std::max(qx, 0.0) +
                                         std::max(qy, 0.0) * std::max(qy, 0.0));
        return outside + std::min(std::max(qx, qy), 0.0) - radius;
    };
    auto capsuleDistance = [](double px, double py, double centerX, double top,
                              double bottom, double radius) {
        const double qx = std::abs(px - centerX);
        const double qy = std::abs(py - (top + bottom) * 0.5) - (bottom - top) * 0.5;
        const double outside = std::sqrt(std::max(qx, 0.0) * std::max(qx, 0.0) +
                                         std::max(qy, 0.0) * std::max(qy, 0.0));
        return outside + std::min(std::max(qx, qy), 0.0) - radius;
    };

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double px = x + 0.5;
            const double py = y + 0.5;
            const double tileDistance =
                roundedRectDistance(px, py, 4.0, 4.0, 60.0, 60.0, 14.0);
            const double tileCoverage = Clamp(0.5 - tileDistance, 0.0, 1.0);
            if (tileCoverage <= 0.0) {
                pixels[y * size + x] = 0;
                continue;
            }

            // Modern blue rounded tile with a soft top-to-bottom gradient.
            const double gradient = Clamp((py - 4.0) / 56.0, 0.0, 1.0);
            double r = 58.0 + (28.0 - 58.0) * gradient;
            double g = 132.0 + (76.0 - 132.0) * gradient;
            double b = 255.0 + (214.0 - 255.0) * gradient;

            if (tileDistance > -1.3 && tileDistance < 0.7) {
                r = std::min(255.0, r + 28.0);
                g = std::min(255.0, g + 22.0);
                b = std::min(255.0, b + 12.0);
            }

            // White scroll-wheel capsule.
            const double wheelDistance = capsuleDistance(px, py, 32.0, 17.0, 47.0, 5.4);
            const double wheelCoverage = Clamp(0.5 - wheelDistance, 0.0, 1.0);
            if (wheelCoverage > 0.0) {
                r = r * (1.0 - wheelCoverage) + 250.0 * wheelCoverage;
                g = g * (1.0 - wheelCoverage) + 252.0 * wheelCoverage;
                b = b * (1.0 - wheelCoverage) + 255.0 * wheelCoverage;
            }

            // Three recessed grip bands on the wheel.
            if (std::abs(px - 32.0) < 3.9) {
                for (double bandY : {24.0, 32.0, 40.0}) {
                    const double bandCoverage =
                        Clamp(1.35 - std::abs(py - bandY), 0.0, 1.0) * wheelCoverage;
                    if (bandCoverage > 0.0) {
                        r = r * (1.0 - bandCoverage) + 28.0 * bandCoverage;
                        g = g * (1.0 - bandCoverage) + 78.0 * bandCoverage;
                        b = b * (1.0 - bandCoverage) + 198.0 * bandCoverage;
                    }
                }
            }

            const auto alpha = static_cast<uint32_t>(std::lround(tileCoverage * 255.0));
            pixels[y * size + x] = (alpha << 24) |
                                   (static_cast<uint32_t>(std::lround(r)) << 16) |
                                   (static_cast<uint32_t>(std::lround(g)) << 8) |
                                   static_cast<uint32_t>(std::lround(b));
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmMask = mask;
    iconInfo.hbmColor = color;
    HICON icon = CreateIconIndirect(&iconInfo);
    if (mask) {
        DeleteObject(mask);
    }
    DeleteObject(color);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return icon;
}

// Prefer the multi-resolution icon embedded by speed-roll.rc so Explorer,
// the taskbar, the tray and the title bar all render a crisp native size.
HICON LoadAppIcon(int width, int height) {
    HICON icon = static_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, width, height, 0));
    if (icon) {
        return icon;
    }
    return CreateAppIcon();
}

// Windows 11 may demote a windowless tray process to an efficiency core and
// apply EcoQoS execution-speed throttling. Raise the scheduling class and
// explicitly opt out of power throttling so hook callbacks stay prompt.
void ConfigureProcessScheduling() {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    struct PowerThrottlingState {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling, version 1 and
    // PROCESS_POWER_THROTTLING_EXECUTION_SPEED. Resolved dynamically so the
    // binary still loads on systems that predate the API.
    constexpr int kProcessPowerThrottling = 4;
    constexpr ULONG kPowerThrottlingVersion = 1;
    constexpr ULONG kExecutionSpeed = 0x1;

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32) {
        FARPROC address = GetProcAddress(kernel32, "SetProcessInformation");
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        auto setProcessInformation =
            reinterpret_cast<SetProcessInformationFn>(address);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (setProcessInformation) {
            // StateMask 0 disables the execution-speed throttle.
            PowerThrottlingState state{kPowerThrottlingVersion, kExecutionSpeed, 0};
            setProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &state,
                                  sizeof(state));
        }
    }

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

void CreateFonts() {
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    if (g_dpi <= 0) {
        g_dpi = 96;
    }

    const wchar_t* family = L"Microsoft YaHei UI";
    g_uiFont = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_titleFont = CreateFontW(-MulDiv(18, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                              FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, family);
    g_metricFont = CreateFontW(-MulDiv(16, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, family);
    g_tinyFont = CreateFontW(-MulDiv(8, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, family);
    g_subtitleFont = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, family);
    g_headerFont = CreateFontW(-MulDiv(14, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, family);
    g_hintFont = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_toastFont = CreateFontW(-MulDiv(11, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    g_cardBrush = CreateSolidBrush(RGB(255, 255, 255));
    g_editBrush = CreateSolidBrush(RGB(255, 255, 255));
}

void DeleteFonts() {
    if (g_uiFont) {
        DeleteObject(g_uiFont);
        g_uiFont = nullptr;
    }
    if (g_titleFont) {
        DeleteObject(g_titleFont);
        g_titleFont = nullptr;
    }
    if (g_metricFont) {
        DeleteObject(g_metricFont);
        g_metricFont = nullptr;
    }
    if (g_tinyFont) {
        DeleteObject(g_tinyFont);
        g_tinyFont = nullptr;
    }
    if (g_subtitleFont) {
        DeleteObject(g_subtitleFont);
        g_subtitleFont = nullptr;
    }
    if (g_headerFont) {
        DeleteObject(g_headerFont);
        g_headerFont = nullptr;
    }
    if (g_hintFont) {
        DeleteObject(g_hintFont);
        g_hintFont = nullptr;
    }
    if (g_toastFont) {
        DeleteObject(g_toastFont);
        g_toastFont = nullptr;
    }
    if (g_cardBrush) {
        DeleteObject(g_cardBrush);
        g_cardBrush = nullptr;
    }
    if (g_editBrush) {
        DeleteObject(g_editBrush);
        g_editBrush = nullptr;
    }
}

LRESULT CALLBACK ToastWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wParam == kToastTimerId) {
                KillTimer(window, kToastTimerId);
                ShowWindow(window, SW_HIDE);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(RGB(15, 23, 42));
            FillRect(dc, &client, background);
            DeleteObject(background);
            RECT accent{0, 0, Scale(3), client.bottom};
            HBRUSH accentBrush = CreateSolidBrush(RGB(29, 78, 216));
            FillRect(dc, &accent, accentBrush);
            DeleteObject(accentBrush);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(248, 250, 252));
            HGDIOBJ oldFont = SelectObject(dc, g_toastFont ? g_toastFont : GetStockObject(DEFAULT_GUI_FONT));
            RECT textRect = client;
            textRect.left += Scale(14);
            textRect.right -= Scale(14);
            DrawTextW(dc, g_toastText.c_str(), -1, &textRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void EnsureToastWindow() {
    if (g_toastWindow) {
        return;
    }
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                          WS_EX_TRANSPARENT | WS_EX_LAYERED;
    g_toastWindow = CreateWindowExW(exStyle, kToastClass, L"", WS_POPUP, 0, 0, 0, 0,
                                    nullptr, nullptr, g_instance, nullptr);
    if (g_toastWindow) {
        SetLayeredWindowAttributes(g_toastWindow, 0, 230, LWA_ALPHA);
    }
}

void ShowToast(const std::wstring& text) {
    EnsureToastWindow();
    if (!g_toastWindow) {
        return;
    }
    g_toastText = text;

    HDC dc = GetDC(g_toastWindow);
    HGDIOBJ oldFont = SelectObject(dc, g_toastFont ? g_toastFont : GetStockObject(DEFAULT_GUI_FONT));
    SIZE textSize{};
    GetTextExtentPoint32W(dc, g_toastText.c_str(), static_cast<int>(g_toastText.size()), &textSize);
    SelectObject(dc, oldFont);
    ReleaseDC(g_toastWindow, dc);

    const int width = std::max(Scale(120), static_cast<int>(textSize.cx) + Scale(30));
    const int height = std::max(Scale(40), static_cast<int>(textSize.cy) + Scale(18));

    POINT cursor{};
    GetCursorPos(&cursor);
    int x = cursor.x + Scale(18);
    int y = cursor.y + Scale(18);

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        if (x + width > monitorInfo.rcWork.right) {
            x = monitorInfo.rcWork.right - width;
        }
        if (y + height > monitorInfo.rcWork.bottom) {
            y = monitorInfo.rcWork.bottom - height;
        }
        if (x < monitorInfo.rcWork.left) {
            x = monitorInfo.rcWork.left;
        }
        if (y < monitorInfo.rcWork.top) {
            y = monitorInfo.rcWork.top;
        }
    }

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, Scale(10), Scale(10));
    SetWindowRgn(g_toastWindow, region, TRUE);
    SetWindowPos(g_toastWindow, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_toastWindow, nullptr, TRUE);
    UpdateWindow(g_toastWindow);
    SetTimer(g_toastWindow, kToastTimerId, kToastLifetimeMs, nullptr);
}

void UpdateTrayTooltip() {
    std::wstring tip = L"speed roll | ";
    if (g_config.globalDisabled) {
        tip += L"已全局禁用";
    } else {
        tip += g_hyperMode ? L"极速模式" : L"普通模式";
        if (!g_config.disabledProcesses.empty()) {
            tip += L" | 黑名单 ";
            tip += std::to_wstring(g_config.disabledProcesses.size());
        }
    }
    lstrcpynW(g_trayData.szTip, tip.c_str(), _countof(g_trayData.szTip));
    if (g_mainWindow) {
        g_trayData.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_trayData);
    }
}

void AddTrayIcon() {
    if (!g_mainWindow) {
        return;
    }
    g_trayData.cbSize = sizeof(g_trayData);
    g_trayData.hWnd = g_mainWindow;
    g_trayData.uID = 1;
    g_trayData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayData.uCallbackMessage = kTrayMessage;
    g_trayData.hIcon = g_appIconSmall ? g_appIconSmall : g_appIcon;
    lstrcpynW(g_trayData.szTip, L"speed roll | 极速模式", _countof(g_trayData.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_trayData);
    UpdateTrayTooltip();
}

void RemoveTrayIcon() {
    if (g_trayData.hWnd) {
        g_trayData.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &g_trayData);
        g_trayData.hWnd = nullptr;
    }
}

void CreateSettingsWindow();
void RefreshSettingsControls();
void PopSettingsTooltip();

void ShowSettingsWindow() {
    if (!g_settingsWindow) {
        CreateSettingsWindow();
    }
    if (!g_settingsWindow) {
        return;
    }
    g_focusedEditId = 0;
    RefreshSettingsControls();
    ShowWindow(g_settingsWindow, SW_SHOWNORMAL);
    InvalidateRect(g_settingsWindow, nullptr, TRUE);
    SetForegroundWindow(g_settingsWindow);
}

void RestoreDefaultSettingsControls() {
    if (!g_settingsWindow) {
        return;
    }
    SetControlText(g_settingsWindow, IDC_EDIT_FAST, FormatDouble(kDefaultFastThreshold));
    SetControlText(g_settingsWindow, IDC_EDIT_MIN_TICKS,
                   std::to_wstring(kDefaultMinFastTicks));
    SetControlText(g_settingsWindow, IDC_EDIT_MAX, FormatDouble(kDefaultMaxMultiplier));
    SetControlText(g_settingsWindow, IDC_EDIT_CURVE, FormatDouble(kDefaultCurvePower));
    SetControlText(g_settingsWindow, IDC_EDIT_HORIZONTAL,
                   FormatDouble(kDefaultHorizontalMultiplier));
    CheckDlgButton(g_settingsWindow, IDC_CHK_ZOOM, BST_CHECKED);
    // Auto-start is a system registration, not an app tuning parameter. Keep
    // its current state when restoring the parameter defaults.
    SetControlText(g_settingsWindow, IDC_EDIT_BLACKLIST, L"");
    SetFocus(GetDlgItem(g_settingsWindow, IDC_BTN_SAVE));
}

void SaveSettingsFromUI() {
    double fast = 0.0, maximum = 0.0, curve = 0.0, horizontal = 0.0;
    int minTicks = 0;
    struct Field {
        int id;
        double* destination;
        double minimum;
        double maximum;
        const wchar_t* name;
    };
    Field fields[] = {
        {IDC_EDIT_FAST, &fast, 1.0, 5000.0, L"FastThreshold"},
        {IDC_EDIT_MAX, &maximum, 1.0, 100.0, L"MaxMultiplier"},
        {IDC_EDIT_CURVE, &curve, 0.1, 10.0, L"CurvePower"},
        {IDC_EDIT_HORIZONTAL, &horizontal, 0.1, 20.0, L"HorizontalMultiplier"},
    };

    for (const auto& field : fields) {
        if (!ParseDouble(GetControlText(g_settingsWindow, field.id), *field.destination) ||
            *field.destination < field.minimum || *field.destination > field.maximum) {
            wchar_t message[256]{};
            std::swprintf(message, _countof(message),
                          L"%ls 必须是 %.1f 到 %.1f 之间的数字。", field.name,
                          field.minimum, field.maximum);
            MessageBoxW(g_settingsWindow, message, L"参数错误", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(g_settingsWindow, field.id));
            return;
        }
    }

    if (!ParseInt(GetControlText(g_settingsWindow, IDC_EDIT_MIN_TICKS), minTicks) ||
        minTicks < 1 || minTicks > 24) {
        MessageBoxW(g_settingsWindow,
                    L"MinFastTicks 必须是 1 到 24 之间的整数。",
                    L"参数错误", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(g_settingsWindow, IDC_EDIT_MIN_TICKS));
        return;
    }

    g_config.fastThreshold = fast;
    g_config.minFastTicks = minTicks;
    g_config.maxMultiplier = maximum;
    g_config.curvePower = curve;
    g_config.horizontalMultiplier = horizontal;
    g_config.preventZoomOverrun =
        SendDlgItemMessageW(g_settingsWindow, IDC_CHK_ZOOM, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_config.disabledProcesses =
        ParseBlacklist(GetControlText(g_settingsWindow, IDC_EDIT_BLACKLIST));

    const bool autoStartWanted =
        SendDlgItemMessageW(g_settingsWindow, IDC_CHK_AUTOSTART, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (!SaveConfig()) {
        MessageBoxW(g_settingsWindow,
                    L"无法写入 config.ini，请确认程序所在目录可写。",
                    L"保存失败", MB_OK | MB_ICONERROR);
        return;
    }

    DWORD autoStartError = 0;
    const bool autoStartOk = SetAutoStart(autoStartWanted, &autoStartError);
    g_autoStart = autoStartWanted;

    ResetWheelAcceleration();
    UpdateTrayTooltip();
    PopSettingsTooltip();
    ShowWindow(g_settingsWindow, SW_HIDE);

    if (autoStartOk) {
        ShowToast(L"配置已保存并生效");
    } else {
        wchar_t warning[128]{};
        std::swprintf(warning, _countof(warning),
                      L"⚠️ 配置已保存，但开机自启设置失败 (%lu)", autoStartError);
        ShowToast(warning);
    }
}

void RefreshSettingsControls() {
    if (!g_settingsWindow) {
        return;
    }
    SetControlText(g_settingsWindow, IDC_EDIT_FAST, FormatDouble(g_config.fastThreshold));
    SetControlText(g_settingsWindow, IDC_EDIT_MIN_TICKS,
                   std::to_wstring(g_config.minFastTicks));
    SetControlText(g_settingsWindow, IDC_EDIT_MAX, FormatDouble(g_config.maxMultiplier));
    SetControlText(g_settingsWindow, IDC_EDIT_CURVE, FormatDouble(g_config.curvePower));
    SetControlText(g_settingsWindow, IDC_EDIT_HORIZONTAL,
                   FormatDouble(g_config.horizontalMultiplier));
    CheckDlgButton(g_settingsWindow, IDC_CHK_ZOOM,
                   g_config.preventZoomOverrun ? BST_CHECKED : BST_UNCHECKED);
    g_autoStart = QueryAutoStart();
    CheckDlgButton(g_settingsWindow, IDC_CHK_AUTOSTART, g_autoStart ? BST_CHECKED : BST_UNCHECKED);
    SetControlText(g_settingsWindow, IDC_EDIT_BLACKLIST, BlacklistForEditor());
}


void PopSettingsTooltip() {
    if (g_tooltipWindow) {
        SendMessageW(g_tooltipWindow, TTM_POP, 0, 0);
    }
}

bool CreateSettingsTooltip(HWND parent) {
    g_tooltipWindow = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, g_instance, nullptr);
    if (!g_tooltipWindow) {
        return false;
    }
    SendMessageW(g_tooltipWindow, TTM_SETMAXTIPWIDTH, 0, Scale(460));
    SendMessageW(g_tooltipWindow, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
    SendMessageW(g_tooltipWindow, TTM_SETTIPBKCOLOR, static_cast<WPARAM>(RGB(15, 23, 42)), 0);
    SendMessageW(g_tooltipWindow, TTM_SETTIPTEXTCOLOR,
                 static_cast<WPARAM>(RGB(248, 250, 252)), 0);
    SendMessageW(g_tooltipWindow, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    SendMessageW(g_tooltipWindow, TTM_ACTIVATE, TRUE, 0);
    return true;
}

void AddTooltip(HWND parent, HWND control, const wchar_t* text) {
    if (!g_tooltipWindow || !control || !text) {
        return;
    }
    TOOLINFOW toolInfo{};
    // Use the v2 size (through lParam) for compatibility with the v5
    // comctl32 tooltip control; v3 appends the reserved pointer.
    toolInfo.cbSize = TTTOOLINFOW_V2_SIZE;
    toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    toolInfo.hwnd = parent;
    toolInfo.uId = reinterpret_cast<UINT_PTR>(control);
    toolInfo.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(g_tooltipWindow, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&toolInfo));

}

void CreateField(HWND parent, const wchar_t* label, const wchar_t* value,
                 int x, int y, int width, int controlId,
                 const wchar_t* tooltipText) {
    HWND labelWindow = CreateWindowExW(
        0, L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
        Scale(x), Scale(y), Scale(width), Scale(16), parent, nullptr, g_instance,
        nullptr);
    HWND editWindow = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", value,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        Scale(x), Scale(y + kFieldEditOffset), Scale(width), Scale(kFieldEditHeight),
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), g_instance,
        nullptr);
    if (labelWindow) {
        SendMessageW(labelWindow, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        AddTooltip(parent, labelWindow, tooltipText);
    }
    if (editWindow) {
        SendMessageW(editWindow, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        SendMessageW(editWindow, EM_SETLIMITTEXT, 32, 0);
        SendMessageW(editWindow, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Scale(6), Scale(6)));
        AddTooltip(parent, editWindow, tooltipText);
    }
}

void FillColorRect(HDC dc, int left, int top, int right, int bottom, COLORREF color) {
    RECT rect{left, top, right, bottom};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

RECT GetEditFocusRect(int id) {
    int x = 0;
    int y = 0;
    int w = kFieldWidth;
    int h = kFieldEditHeight;
    switch (id) {
        case IDC_EDIT_FAST:
            x = kFieldX1;
            y = kFieldY1 + kFieldEditOffset;
            break;
        case IDC_EDIT_MIN_TICKS:
            x = kFieldX2;
            y = kFieldY1 + kFieldEditOffset;
            break;
        case IDC_EDIT_MAX:
            x = kFieldX1;
            y = kFieldY2 + kFieldEditOffset;
            break;
        case IDC_EDIT_CURVE:
            x = kFieldX2;
            y = kFieldY2 + kFieldEditOffset;
            break;
        case IDC_EDIT_HORIZONTAL:
            x = kFieldX1;
            y = kFieldY3 + kFieldEditOffset;
            break;
        case IDC_EDIT_BLACKLIST:
            x = kBlacklistEditX;
            y = kBlacklistEditY;
            w = kBlacklistEditWidth;
            h = kBlacklistEditHeight;
            break;
        default:
            return RECT{};
    }
    return RECT{Scale(x - 1), Scale(y - 1), Scale(x + w + 1), Scale(y + h + 1)};
}

constexpr COLORREF kCanvas = RGB(248, 250, 252);
constexpr COLORREF kSidebar = RGB(15, 23, 42);
constexpr COLORREF kPanel = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(226, 232, 240);
constexpr COLORREF kPrimary = RGB(29, 78, 216);
constexpr COLORREF kPrimarySoft = RGB(239, 246, 255);
constexpr COLORREF kPrimaryBorder = RGB(219, 234, 254);
constexpr COLORREF kText = RGB(15, 23, 42);
constexpr COLORREF kBody = RGB(51, 65, 85);
constexpr COLORREF kMuted = RGB(100, 116, 139);
constexpr COLORREF kSubtle = RGB(148, 163, 184);
constexpr COLORREF kSuccess = RGB(34, 197, 94);

void DrawPanel(HDC dc, int left, int top, int right, int bottom, int radius,
               COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = border == CLR_INVALID ? nullptr : CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen ? pen : GetStockObject(NULL_PEN));
    RoundRect(dc, left, top, right, bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (pen) {
        DeleteObject(pen);
    }
    DeleteObject(brush);
}

void DrawTextLine(HDC dc, HFONT font, COLORREF color, const std::wstring& text,
                  const RECT& rect, UINT flags) {
    RECT target = rect;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &target, flags | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void DrawGlyphIcon(HDC dc, int x, int y, int size, int kind, COLORREF color) {
    const double s = static_cast<double>(size) / 24.0;
    auto X = [&](double value) { return x + static_cast<int>(std::lround(value * s)); };
    auto Y = [&](double value) { return y + static_cast<int>(std::lround(value * s)); };

    HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), color);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    SetBkMode(dc, TRANSPARENT);

    switch (kind) {
        case 0: {  // acceleration
            POINT points[6] = {{X(13), Y(3)},  {X(7), Y(13)},  {X(11), Y(13)},
                               {X(9), Y(21)},  {X(18), Y(10)}, {X(13), Y(10)}};
            Polygon(dc, points, 6);
            break;
        }
        case 1: {  // sliders
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            MoveToEx(dc, X(4), Y(6), nullptr);
            LineTo(dc, X(20), Y(6));
            MoveToEx(dc, X(4), Y(12), nullptr);
            LineTo(dc, X(20), Y(12));
            MoveToEx(dc, X(4), Y(18), nullptr);
            LineTo(dc, X(20), Y(18));
            SelectObject(dc, brush);
            Ellipse(dc, X(7), Y(4), X(12), Y(9));
            Ellipse(dc, X(13), Y(10), X(18), Y(15));
            Ellipse(dc, X(9), Y(16), X(14), Y(21));
            break;
        }
        case 2: {  // shield
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            POINT points[6] = {{X(12), Y(3)},  {X(20), Y(6)},  {X(19), Y(14)},
                               {X(12), Y(21)}, {X(5), Y(14)},  {X(4), Y(6)}};
            Polygon(dc, points, 6);
            break;
        }
        case 3: {  // list
            SelectObject(dc, brush);
            Ellipse(dc, X(4), Y(5), X(7), Y(8));
            Ellipse(dc, X(4), Y(11), X(7), Y(14));
            Ellipse(dc, X(4), Y(17), X(7), Y(20));
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            MoveToEx(dc, X(10), Y(6), nullptr);
            LineTo(dc, X(20), Y(6));
            MoveToEx(dc, X(10), Y(12), nullptr);
            LineTo(dc, X(20), Y(12));
            MoveToEx(dc, X(10), Y(18), nullptr);
            LineTo(dc, X(20), Y(18));
            break;
        }
        case 4: {  // mode toggle
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, X(3), Y(7), X(21), Y(17), X(10) - X(0), X(10) - X(0));
            SelectObject(dc, brush);
            Ellipse(dc, X(14), Y(9), X(19), Y(15));
            break;
        }
        case 5: {  // threshold gauge
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Arc(dc, X(3), Y(4), X(21), Y(22), X(21), Y(13), X(3), Y(13));
            MoveToEx(dc, X(12), Y(14), nullptr);
            LineTo(dc, X(17), Y(8));
            SelectObject(dc, brush);
            Ellipse(dc, X(10), Y(12), X(14), Y(16));
            break;
        }
        case 6: {  // max multiplier
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            MoveToEx(dc, X(12), Y(21), nullptr);
            LineTo(dc, X(12), Y(5));
            MoveToEx(dc, X(6), Y(11), nullptr);
            LineTo(dc, X(12), Y(5));
            LineTo(dc, X(18), Y(11));
            break;
        }
        case 7: {  // mouse wheel
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, X(7), Y(3), X(17), Y(21), X(6), X(6));
            MoveToEx(dc, X(12), Y(7), nullptr);
            LineTo(dc, X(12), Y(12));
            break;
        }
        default:
            break;
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawIconTile(HDC dc, int x, int y, int size, int kind) {
    DrawPanel(dc, x, y, x + size, y + size, Scale(7), kPrimarySoft, kPrimaryBorder);
    DrawGlyphIcon(dc, x + Scale(4), y + Scale(4), size - Scale(8), kind, kPrimary);
}

void DrawCard(HDC dc, int left, int top, int right, int bottom) {
    DrawPanel(dc, left, top, right, bottom, Scale(14), kPanel, kBorder);
}

void DrawSummaryTile(HDC dc, int left, int top, int width, int height, int kind,
                     const std::wstring& label, const std::wstring& value,
                     const std::wstring& detail) {
    DrawPanel(dc, left, top, left + width, top + height, Scale(12), kPanel, kBorder);
    DrawIconTile(dc, left + Scale(14), top + Scale(14), Scale(30), kind);

    const int textLeft = left + Scale(54);
    const int textWidth = width - Scale(64);
    RECT labelRect{textLeft, top + Scale(10), textLeft + textWidth, top + Scale(28)};
    DrawTextLine(dc, g_hintFont, kMuted, label, labelRect,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT valueRect{textLeft, top + Scale(26), textLeft + textWidth, top + Scale(52)};
    DrawTextLine(dc, g_metricFont, kind == 4 ? kPrimary : kText, value, valueRect,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT detailRect{textLeft, top + Scale(54), textLeft + textWidth, top + Scale(70)};
    DrawTextLine(dc, g_tinyFont, kMuted, detail, detailRect,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void DrawAccelerationCurve(HDC dc, int left, int top, int right, int bottom,
                           double maxMultiplier, double curvePower) {
    DrawPanel(dc, left, top, right, bottom, Scale(10), kCanvas, kBorder);

    const std::wstring title = L"加速曲线";
    RECT titleRect{left + Scale(12), top + Scale(5), right - Scale(90), top + Scale(19)};
    DrawTextLine(dc, g_tinyFont, kMuted, title, titleRect,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);

    const std::wstring axis = L"1×  →  " + FormatDouble(maxMultiplier) + L"×";
    RECT axisRect{right - Scale(104), top + Scale(5), right - Scale(12), top + Scale(19)};
    DrawTextLine(dc, g_tinyFont, kMuted, axis, axisRect,
                 DT_RIGHT | DT_TOP | DT_SINGLELINE);

    const int chartLeft = left + Scale(12);
    const int chartTop = top + Scale(22);
    const int chartRight = right - Scale(12);
    const int chartBottom = bottom - Scale(8);
    if (chartRight <= chartLeft || chartBottom <= chartTop) {
        return;
    }

    HPEN gridPen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(dc, gridPen);
    for (int i = 0; i <= 3; ++i) {
        const int y = chartTop + (chartBottom - chartTop) * i / 3;
        MoveToEx(dc, chartLeft, y, nullptr);
        LineTo(dc, chartRight, y);
    }
    for (int i = 0; i <= 4; ++i) {
        const int x = chartLeft + (chartRight - chartLeft) * i / 4;
        MoveToEx(dc, x, chartTop, nullptr);
        LineTo(dc, x, chartBottom);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);

    const double maximum = std::max(1.0, maxMultiplier);
    const int steps = 48;
    std::vector<POINT> points;
    points.reserve(steps);
    for (int i = 0; i < steps; ++i) {
        const double t = static_cast<double>(i) / (steps - 1);
        const double normalized = std::pow(t, std::max(0.1, curvePower));
        const int x = chartLeft + static_cast<int>(std::lround(t * (chartRight - chartLeft)));
        const int y = chartBottom - static_cast<int>(
            std::lround(normalized * (chartBottom - chartTop)));
        points.push_back(POINT{x, y});
    }

    if (maximum > 1.0) {
        std::vector<POINT> filled;
        filled.reserve(points.size() + 2);
        filled.push_back(POINT{chartLeft, chartBottom});
        filled.insert(filled.end(), points.begin(), points.end());
        filled.push_back(POINT{chartRight, chartBottom});
        HBRUSH fillBrush = CreateSolidBrush(RGB(219, 234, 254));
        HGDIOBJ oldBrush = SelectObject(dc, fillBrush);
        HGDIOBJ oldFillPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, filled.data(), static_cast<int>(filled.size()));
        SelectObject(dc, oldFillPen);
        SelectObject(dc, oldBrush);
        DeleteObject(fillBrush);
    }

    HPEN curvePen = CreatePen(PS_SOLID, std::max(1, Scale(2)), kPrimary);
    oldPen = SelectObject(dc, curvePen);
    Polyline(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, oldPen);
    DeleteObject(curvePen);

    HBRUSH dotBrush = CreateSolidBrush(kPrimary);
    HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
    HGDIOBJ oldDotPen = SelectObject(dc, GetStockObject(NULL_PEN));
    const POINT& end = points.back();
    Ellipse(dc, end.x - Scale(2), end.y - Scale(2), end.x + Scale(2), end.y + Scale(2));
    SelectObject(dc, oldDotPen);
    SelectObject(dc, oldBrush);
    DeleteObject(dotBrush);
}

void DrawSidebar(HDC dc, const RECT& client) {
    const int width = Scale(kSidebarWidth);
    FillColorRect(dc, 0, 0, width, client.bottom, kSidebar);
    FillColorRect(dc, width - 1, 0, width, client.bottom, RGB(30, 41, 59));

    DrawIconEx(dc, Scale(24), Scale(22), g_appIcon, Scale(40), Scale(40), 0, nullptr,
               DI_NORMAL);

    RECT brandRect{Scale(74), Scale(14), Scale(196), Scale(42)};
    DrawTextLine(dc, g_headerFont, RGB(255, 255, 255), L"speed roll", brandRect,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT taglineRect{Scale(74), Scale(42), Scale(196), Scale(58)};
    DrawTextLine(dc, g_tinyFont, kSubtle, L"滚轮增强", taglineRect,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    FillColorRect(dc, Scale(20), Scale(88), Scale(190), Scale(89), RGB(30, 41, 59));

    DrawPanel(dc, Scale(20), Scale(104), Scale(190), Scale(196), Scale(12),
              RGB(17, 28, 51), RGB(30, 41, 59));
    RECT modeLabel{Scale(34), Scale(112), Scale(176), Scale(128)};
    DrawTextLine(dc, g_tinyFont, kSubtle, L"当前模式", modeLabel,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT modeValue{Scale(34), Scale(128), Scale(176), Scale(162)};
    DrawTextLine(dc, g_titleFont, RGB(255, 255, 255),
                 g_hyperMode ? L"极速" : L"普通", modeValue,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
    HBRUSH statusBrush = CreateSolidBrush(kSuccess);
    HGDIOBJ oldBrush = SelectObject(dc, statusBrush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, Scale(164), Scale(116), Scale(172), Scale(124));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(statusBrush);
    RECT modeHint{Scale(34), Scale(166), Scale(176), Scale(182)};
    DrawTextLine(dc, g_tinyFont, kSubtle, L"前进 + 后退 切换", modeHint,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT tipsTitle{Scale(20), Scale(218), Scale(190), Scale(236)};
    DrawTextLine(dc, g_uiFont, RGB(226, 232, 240), L"快速提示", tipsTitle,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
    const wchar_t* tips[] = {L"Ctrl / Alt 强制 1x", L"侧键可随时切换模式",
                             L"黑名单按进程生效"};
    int tipY = 244;
    for (const wchar_t* tip : tips) {
        HBRUSH tipBrush = CreateSolidBrush(RGB(96, 165, 250));
        HGDIOBJ tipOldBrush = SelectObject(dc, tipBrush);
        HGDIOBJ tipOldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, Scale(20), Scale(tipY + 4), Scale(24), Scale(tipY + 8));
        SelectObject(dc, tipOldPen);
        SelectObject(dc, tipOldBrush);
        DeleteObject(tipBrush);
        RECT tipRect{Scale(32), Scale(tipY), Scale(190), Scale(tipY + 16)};
        DrawTextLine(dc, g_tinyFont, kSubtle, tip, tipRect,
                     DT_LEFT | DT_TOP | DT_SINGLELINE);
        tipY += 26;
    }

    FillColorRect(dc, Scale(20), Scale(330), Scale(190), Scale(331), RGB(30, 41, 59));
    RECT configLabel{Scale(20), Scale(346), Scale(190), Scale(360)};
    DrawTextLine(dc, g_tinyFont, kSubtle, L"配置文件", configLabel,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT configValue{Scale(20), Scale(360), Scale(190), Scale(376)};
    DrawTextLine(dc, g_tinyFont, RGB(203, 213, 225), L"config.ini", configValue,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);

    HPEN wheelPen = CreatePen(PS_SOLID, 1, RGB(30, 41, 59));
    HGDIOBJ wheelOldPen = SelectObject(dc, wheelPen);
    HGDIOBJ wheelOldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, Scale(40), Scale(400), Scale(160), Scale(520));
    RoundRect(dc, Scale(92), Scale(432), Scale(108), Scale(488), Scale(8), Scale(8));
    SelectObject(dc, wheelOldBrush);
    SelectObject(dc, wheelOldPen);
    DeleteObject(wheelPen);

    FillColorRect(dc, Scale(20), Scale(600), Scale(190), Scale(601), RGB(30, 41, 59));
    HBRUSH runBrush = CreateSolidBrush(kSuccess);
    HGDIOBJ runOldBrush = SelectObject(dc, runBrush);
    HGDIOBJ runOldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, Scale(24), Scale(622), Scale(32), Scale(630));
    SelectObject(dc, runOldPen);
    SelectObject(dc, runOldBrush);
    DeleteObject(runBrush);
    RECT runningRect{Scale(38), Scale(616), Scale(180), Scale(634)};
    DrawTextLine(dc, g_tinyFont, kSubtle, L"运行中", runningRect,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT versionRect{Scale(24), Scale(646), Scale(180), Scale(664)};
    DrawTextLine(dc, g_tinyFont, kSubtle, L"v1.0.4 · Win32", versionRect,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void DrawPageHeader(HDC dc, const RECT& client) {
    FillColorRect(dc, Scale(kSidebarWidth), 0, client.right, Scale(kPageHeaderHeight),
                  kCanvas);
    FillColorRect(dc, Scale(kSidebarWidth), Scale(kPageHeaderHeight - 1), client.right,
                  Scale(kPageHeaderHeight), kBorder);

    RECT titleRect{Scale(234), Scale(8), Scale(720), Scale(42)};
    DrawTextLine(dc, g_titleFont, kText, L"偏好设置", titleRect,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT subtitleRect{Scale(236), Scale(43), Scale(720), Scale(64)};
    DrawTextLine(dc, g_hintFont, kMuted, L"调整滚轮加速、侧键行为与例外程序",
                 subtitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawPanel(dc, Scale(760), Scale(24), Scale(916), Scale(52), Scale(14), kPanel,
              kBorder);
    HBRUSH dotBrush = CreateSolidBrush(kSuccess);
    HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, Scale(772), Scale(35), Scale(780), Scale(43));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(dotBrush);
    RECT statusRect{Scale(786), Scale(28), Scale(906), Scale(48)};
    DrawTextLine(dc, g_hintFont, kBody, L"配置已加载", statusRect,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void DrawActionStrip(HDC dc) {
    DrawPanel(dc, Scale(234), Scale(186), Scale(916), Scale(250), Scale(12), kPanel,
              kBorder);
    DrawPanel(dc, Scale(690), Scale(186), Scale(916), Scale(250), Scale(12), kPrimary,
              kPrimary);
    FillColorRect(dc, Scale(470), Scale(198), Scale(471), Scale(238), kBorder);

    const struct {
        int x;
        const wchar_t* title;
        const wchar_t* detail;
    } items[] = {
        {252, L"前进键 + 滚轮", L"到顶 / 到底"},
        {480, L"后退键 + 滚轮", L"左右横向滚动"},
    };
    for (const auto& item : items) {
        RECT titleRect{Scale(item.x), Scale(196), Scale(item.x + 206), Scale(216)};
        DrawTextLine(dc, g_uiFont, kText, item.title, titleRect,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT detailRect{Scale(item.x), Scale(216), Scale(item.x + 206), Scale(234)};
        DrawTextLine(dc, g_tinyFont, kMuted, item.detail, detailRect,
                     DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    RECT highlightTitle{Scale(706), Scale(196), Scale(898), Scale(216)};
    DrawTextLine(dc, g_uiFont, RGB(255, 255, 255), L"前进 + 后退键", highlightTitle,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT highlightDetail{Scale(706), Scale(216), Scale(898), Scale(234)};
    DrawTextLine(dc, g_tinyFont, RGB(219, 234, 254), L"切换极速 / 普通模式",
                 highlightDetail, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void PaintSettingsBackground(HDC dc, const RECT& client) {
    FillColorRect(dc, client.left, client.top, client.right, client.bottom, kCanvas);
    DrawSidebar(dc, client);
    DrawPageHeader(dc, client);

    const std::wstring modeValue = g_hyperMode ? L"极速" : L"普通";
    const std::wstring thresholdValue = FormatDouble(g_config.fastThreshold) + L" ms";
    const std::wstring maxValue = FormatDouble(g_config.maxMultiplier) + L"×";
    const int tileTop = Scale(kSummaryTop);
    const int tileWidth = Scale(kSummaryTileWidth);
    const int tileHeight = Scale(kSummaryHeight);
    const int gap = Scale(kSummaryGap);
    const int tile1 = Scale(kContentLeft);
    const int tile2 = tile1 + tileWidth + gap;
    const int tile3 = tile2 + tileWidth + gap;
    DrawSummaryTile(dc, tile1, tileTop, tileWidth, tileHeight, 4, L"当前模式", modeValue,
                    L"侧键可切换");
    DrawSummaryTile(dc, tile2, tileTop, tileWidth, tileHeight, 5, L"快滚阈值",
                    thresholdValue, L"FastThreshold");
    DrawSummaryTile(dc, tile3, tileTop, tileWidth, tileHeight, 6, L"最大倍率", maxValue,
                    L"MaxMultiplier");

    DrawActionStrip(dc);

    DrawCard(dc, Scale(234), Scale(kLeftCardTop), Scale(640), Scale(kLeftCardBottom));
    DrawCard(dc, Scale(652), Scale(kBehaviorTop), Scale(916), Scale(kBehaviorBottom));
    DrawCard(dc, Scale(652), Scale(kBlacklistCardTop), Scale(916),
             Scale(kBlacklistCardBottom));

    DrawIconTile(dc, Scale(250), Scale(276), Scale(24), 1);
    RECT accelTitle{Scale(282), Scale(270), Scale(620), Scale(294)};
    DrawTextLine(dc, g_headerFont, kText, L"滚轮加速", accelTitle,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT accelSubtitle{Scale(282), Scale(294), Scale(620), Scale(308)};
    DrawTextLine(dc, g_tinyFont, kMuted, L"调整触发阈值、倍率与曲线", accelSubtitle,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT accelCount{Scale(540), Scale(278), Scale(622), Scale(294)};
    DrawTextLine(dc, g_tinyFont, kMuted, L"5 项参数", accelCount,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    DrawIconTile(dc, Scale(668), Scale(276), Scale(24), 2);
    RECT behaviorTitle{Scale(700), Scale(270), Scale(904), Scale(294)};
    DrawTextLine(dc, g_headerFont, kText, L"行为与兼容", behaviorTitle,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT behaviorSubtitle{Scale(700), Scale(294), Scale(904), Scale(308)};
    DrawTextLine(dc, g_tinyFont, kMuted, L"防误触与启动选项", behaviorSubtitle,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);

    DrawIconTile(dc, Scale(668), Scale(410), Scale(24), 3);
    RECT blacklistTitle{Scale(700), Scale(404), Scale(904), Scale(428)};
    DrawTextLine(dc, g_headerFont, kText, L"进程黑名单", blacklistTitle,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawAccelerationCurve(dc, Scale(252), Scale(512), Scale(622), Scale(568),
                          g_config.maxMultiplier, g_config.curvePower);

    FillColorRect(dc, Scale(kSidebarWidth), Scale(kFooterBorderY), client.right,
                  Scale(kFooterBorderY + 1), kBorder);
    RECT footerHint{Scale(608), Scale(616), Scale(820), Scale(638)};
    DrawTextLine(dc, g_tinyFont, kMuted, L"保存后立即生效 · 写入 config.ini",
                 footerHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (g_focusedEditId != 0) {
        RECT focus = GetEditFocusRect(g_focusedEditId);
        if (focus.right > focus.left) {
            HPEN focusPen = CreatePen(PS_SOLID, 1, kPrimary);
            HGDIOBJ focusOldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            HGDIOBJ focusOldPen = SelectObject(dc, focusPen);
            RoundRect(dc, focus.left, focus.top, focus.right, focus.bottom, Scale(3),
                      Scale(3));
            SelectObject(dc, focusOldPen);
            SelectObject(dc, focusOldBrush);
            DeleteObject(focusPen);
            FillColorRect(dc, focus.left - Scale(4), focus.top + Scale(2),
                          focus.left - Scale(1), focus.bottom - Scale(2), kPrimary);
        }
    }
}


LRESULT CALLBACK BlacklistTabSubclass(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_GETDLGCODE) {
        // A multiline edit normally consumes Tab. Let the dialog manager move
        // focus instead so keyboard users can reach Save/Reset/Close.
        return DefSubclassProc(window, message, wParam, lParam) & ~DLGC_WANTTAB;
    }
    if (message == WM_CHAR && wParam == VK_TAB) {
        const BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        HWND next = GetNextDlgTabItem(GetParent(window), window, shift);
        if (next) {
            SetFocus(next);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, BlacklistTabSubclass, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK FlatButtonSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR subclassId, DWORD_PTR) {
    const bool hot = GetWindowLongPtrW(window, GWLP_USERDATA) != 0;
    switch (message) {
        case WM_MOUSEMOVE:
            if (!hot) {
                SetWindowLongPtrW(window, GWLP_USERDATA, 1);
                InvalidateRect(window, nullptr, TRUE);
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
            }
            break;
        case WM_MOUSELEAVE:
            if (hot) {
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                InvalidateRect(window, nullptr, TRUE);
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(window, nullptr, TRUE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(window, FlatButtonSubclass, subclassId);
            break;
        default:
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void DrawOwnerButton(const DRAWITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_BUTTON) {
        return;
    }
    const int id = static_cast<int>(item->CtlID);
    const bool hot = GetWindowLongPtrW(item->hwndItem, GWLP_USERDATA) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;

    COLORREF fill = RGB(241, 245, 249);
    COLORREF border = RGB(226, 232, 240);
    COLORREF text = RGB(51, 65, 85);
    if (id == IDC_BTN_SAVE) {
        fill = pressed ? RGB(30, 64, 175) : (hot ? RGB(37, 99, 235) : RGB(29, 78, 216));
        border = fill;
        text = RGB(255, 255, 255);
    } else if (id == IDC_BTN_CLOSE) {
        fill = pressed ? RGB(241, 245, 249) : (hot ? RGB(248, 250, 252) : RGB(255, 255, 255));
        border = RGB(226, 232, 240);
        text = RGB(71, 85, 105);
    } else if (hot) {
        fill = pressed ? RGB(203, 213, 225) : RGB(226, 232, 240);
    } else if (pressed) {
        fill = RGB(203, 213, 225);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
    HGDIOBJ oldPen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right,
              item->rcItem.bottom, Scale(10), Scale(10));
    SelectObject(item->hDC, oldPen);
    SelectObject(item->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t label[64]{};
    GetWindowTextW(item->hwndItem, label, _countof(label));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, text);
    HGDIOBJ oldFont = SelectObject(item->hDC, g_uiFont);
    RECT textRect = item->rcItem;
    DrawTextW(item->hDC, label, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item->hDC, oldFont);

    if (focused) {
        RECT focusRect = item->rcItem;
        InflateRect(&focusRect, -Scale(3), -Scale(3));
        HPEN focusPen = CreatePen(
            PS_SOLID, std::max(1, Scale(2)),
            id == IDC_BTN_SAVE ? RGB(255, 255, 255) : RGB(29, 78, 216));
        HGDIOBJ focusOldBrush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
        HGDIOBJ focusOldPen = SelectObject(item->hDC, focusPen);
        RoundRect(item->hDC, focusRect.left, focusRect.top, focusRect.right,
                  focusRect.bottom, Scale(8), Scale(8));
        SelectObject(item->hDC, focusOldPen);
        SelectObject(item->hDC, focusOldBrush);
        DeleteObject(focusPen);
    }
}

void ApplyNativeWindowChrome(HWND window) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) {
        return;
    }
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    DwmSetWindowAttributeFn setAttribute = nullptr;
    FARPROC proc = GetProcAddress(dwm, "DwmSetWindowAttribute");
    static_assert(sizeof(setAttribute) == sizeof(proc), "function pointer size mismatch");
    std::memcpy(&setAttribute, &proc, sizeof(setAttribute));
    if (setAttribute) {
        BOOL dark = TRUE;
        setAttribute(window, 20, &dark, sizeof(dark));
        const COLORREF caption = RGB(15, 23, 42);
        setAttribute(window, 35, &caption, sizeof(caption));
        const COLORREF text = RGB(255, 255, 255);
        setAttribute(window, 36, &text, sizeof(text));
        const COLORREF border = RGB(15, 23, 42);
        setAttribute(window, 34, &border, sizeof(border));
    }
    FreeLibrary(dwm);
}

void CreateSettingsWindow() {
    const int clientWidth = Scale(kSettingsWidth);
    const int clientHeight = Scale(kSettingsHeight);
    RECT rect{0, 0, clientWidth, clientHeight};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int windowWidth = rect.right - rect.left;
    const int windowHeight = rect.bottom - rect.top;
    const int x = workArea.left + ((workArea.right - workArea.left) - windowWidth) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - windowHeight) / 2;

    g_settingsWindow = CreateWindowExW(
        exStyle, kSettingsClass, L"speed roll 参数设置", style, x, y, windowWidth, windowHeight,
        nullptr, nullptr, g_instance, nullptr);
    if (!g_settingsWindow) {
        return;
    }
    ApplyNativeWindowChrome(g_settingsWindow);
    CreateSettingsTooltip(g_settingsWindow);

    CreateField(g_settingsWindow, L"快滚阈值 (ms)", L"100",
                kFieldX1, kFieldY1, kFieldWidth, IDC_EDIT_FAST,
                L"FastThreshold：单格滚轮事件的最大间隔（毫秒）。小于该值才计入快滚；"
                L"默认 100。实际快滚不触发加速时可适当调大。");
    CreateField(g_settingsWindow, L"触发格数", L"3",
                kFieldX2, kFieldY1, kFieldWidth, IDC_EDIT_MIN_TICKS,
                L"MinFastTicks：极速模式下触发加速所需的连续同向快滚格数。默认 3；"
                L"数值越大越不容易误触。");
    CreateField(g_settingsWindow, L"最大倍率 (倍)", L"8",
                kFieldX1, kFieldY2, kFieldWidth, IDC_EDIT_MAX,
                L"MaxMultiplier：快速滚动时允许的最大倍率。8 表示最多一次滚动 8 格；"
                L"默认 8。");
    CreateField(g_settingsWindow, L"曲线指数", L"1.8",
                kFieldX2, kFieldY2, kFieldWidth, IDC_EDIT_CURVE,
                L"CurvePower：加速曲线指数。数值越大，只有越快的滚动才接近最大倍率；"
                L"默认 1.8。");
    CreateField(g_settingsWindow, L"横向倍率 (倍)", L"1",
                kFieldX1, kFieldY3, kFieldWidth, IDC_EDIT_HORIZONTAL,
                L"HorizontalMultiplier：按住后退键滚动时的横向滚动倍率。"
                L"1 表示与原生横向滚动一致。");

    HWND horizontalHint = CreateWindowExW(
        0, L"STATIC", L"按住后退键滚动时\r\n按此倍率横向滚动",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY, Scale(444), Scale(450),
        Scale(174), Scale(44), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(3010)), g_instance, nullptr);
    SendMessageW(horizontalHint, WM_SETFONT, reinterpret_cast<WPARAM>(g_hintFont), TRUE);

    HWND zoomCheck = CreateWindowExW(
        0, L"BUTTON", L"防缩放失控 (Ctrl/Alt 1x)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, Scale(668), Scale(310),
        Scale(232), Scale(24), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_ZOOM)), g_instance, nullptr);
    SendMessageW(zoomCheck, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    AddTooltip(g_settingsWindow, zoomCheck,
               L"勾选后，按住 Ctrl 或 Alt 滚动会跳过倍速并强制 1x，"
               L"避免文档和表格缩放失控。");

    HWND autoStartCheck = CreateWindowExW(
        0, L"BUTTON", L"开机自动静默启动",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, Scale(668), Scale(340),
        Scale(232), Scale(24), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_AUTOSTART)), g_instance, nullptr);
    SendMessageW(autoStartCheck, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    AddTooltip(g_settingsWindow, autoStartCheck,
               L"勾选并保存后，把当前 exe 写入当前用户的开机启动项，"
               L"实现登录后静默启动。");

    HWND blacklistHint = CreateWindowExW(
        0, L"STATIC", L"每行一个进程名，不含 .exe",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY, Scale(668), Scale(430),
        Scale(232), Scale(14), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(3011)), g_instance, nullptr);
    SendMessageW(blacklistHint, WM_SETFONT, reinterpret_cast<WPARAM>(g_tinyFont), TRUE);

    HWND blacklistEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN | ES_LEFT,
        Scale(kBlacklistEditX), Scale(kBlacklistEditY), Scale(kBlacklistEditWidth),
        Scale(kBlacklistEditHeight), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_BLACKLIST)), g_instance, nullptr);
    SendMessageW(blacklistEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    SendMessageW(blacklistEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(Scale(6), Scale(6)));
    SetWindowSubclass(blacklistEdit, BlacklistTabSubclass, 1, 0);
    AddTooltip(g_settingsWindow, blacklistEdit,
               L"每行填写一个进程名（不含 .exe）。列出的程序内滚轮加速和"
               L"侧键+滚轮功能不生效，普通滚轮保持原生。");

    HWND saveButton = CreateWindowExW(
        0, L"BUTTON", L"保存并立即生效",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Scale(234), Scale(kButtonY),
        Scale(190), Scale(kButtonHeight), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE)), g_instance, nullptr);
    SetWindowSubclass(saveButton, FlatButtonSubclass, 1, 0);

    HWND resetButton = CreateWindowExW(
        0, L"BUTTON", L"恢复默认值",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Scale(438), Scale(kButtonY),
        Scale(150), Scale(kButtonHeight), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_RESET)), g_instance, nullptr);
    SetWindowSubclass(resetButton, FlatButtonSubclass, 1, 0);

    HWND closeButton = CreateWindowExW(
        0, L"BUTTON", L"关闭",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Scale(832), Scale(kButtonY),
        Scale(84), Scale(kButtonHeight), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CLOSE)), g_instance, nullptr);
    SetWindowSubclass(closeButton, FlatButtonSubclass, 1, 0);

    RefreshSettingsControls();
}

LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
            PaintSettingsBackground(memory, client);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HWND control = reinterpret_cast<HWND>(lParam);
            const int id = GetDlgCtrlID(control);
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam),
                         id >= 3000 && id < 3100 ? RGB(100, 116, 139)
                                                 : RGB(51, 65, 85));
            return reinterpret_cast<LRESULT>(g_cardBrush);
        }
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(51, 65, 85));
            return reinterpret_cast<LRESULT>(g_cardBrush);
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(15, 23, 42));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(g_editBrush);
        case WM_DRAWITEM:
            DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_CLOSE:
            g_focusedEditId = 0;
            PopSettingsTooltip();
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS) {
                g_focusedEditId = LOWORD(wParam);
                InvalidateRect(window, nullptr, TRUE);
            } else if (HIWORD(wParam) == EN_KILLFOCUS) {
                if (g_focusedEditId == LOWORD(wParam)) {
                    g_focusedEditId = 0;
                    InvalidateRect(window, nullptr, TRUE);
                }
            }
            switch (LOWORD(wParam)) {
                case IDC_BTN_SAVE:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        SaveSettingsFromUI();
                        return 0;
                    }
                    break;
                case IDC_BTN_RESET:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        RestoreDefaultSettingsControls();
                        return 0;
                    }
                    break;
                case IDC_BTN_CLOSE:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        g_focusedEditId = 0;
                        PopSettingsTooltip();
                        ShowWindow(window, SW_HIDE);
                        return 0;
                    }
                    break;
                case IDCANCEL:
                    g_focusedEditId = 0;
                    ShowWindow(window, SW_HIDE);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_DESTROY:
            if (g_tooltipWindow) {
                DestroyWindow(g_tooltipWindow);
                g_tooltipWindow = nullptr;
            }
            g_settingsWindow = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ToggleCurrentProcessBlacklist() {
    if (g_lastActiveProcess.empty()) {
        return;
    }
    const std::wstring name = g_lastActiveProcess;
    auto it = std::find(g_config.disabledProcesses.begin(), g_config.disabledProcesses.end(), name);
    if (it == g_config.disabledProcesses.end()) {
        g_config.disabledProcesses.push_back(name);
        SaveConfig();
        ShowToast(L"已在 " + name + L" 中禁用加速");
    } else {
        g_config.disabledProcesses.erase(it);
        SaveConfig();
        ShowToast(L"已恢复 " + name + L" 的滚轮加速");
    }
    ResetWheelAcceleration();
    UpdateTrayTooltip();
}

void ShowTrayMenu() {
    UpdateLastActiveProcess();

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppendMenuW(menu, MF_STRING, IDM_OPEN_SETTINGS, L"打开参数设置");
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_MODE, L"切换模式 (普通 / 极速)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_config.globalDisabled ? MF_CHECKED : MF_UNCHECKED),
                IDM_GLOBAL_DISABLE, L"禁用所有程序 (Disable for All Apps)");

    if (!g_lastActiveProcess.empty()) {
        std::wstring label = L"在 " + g_lastActiveProcess + L" 中禁用加速";
        UINT flags = MF_STRING;
        if (IsBlacklisted(g_lastActiveProcess)) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(menu, flags, IDM_DISABLE_CURRENT_APP, label.c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出程序");
    SetMenuDefaultItem(menu, IDM_OPEN_SETTINGS, FALSE);

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_mainWindow);
    UINT command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_TOPALIGN |
                                             TPM_LEFTALIGN,
                                    cursor.x, cursor.y, g_mainWindow, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_mainWindow, WM_NULL, 0, 0);

    switch (command) {
        case IDM_OPEN_SETTINGS:
            ShowSettingsWindow();
            break;
        case IDM_TOGGLE_MODE:
            ToggleHyperMode();
            break;
        case IDM_GLOBAL_DISABLE:
            g_config.globalDisabled = !g_config.globalDisabled;
            SaveConfig();
            ResetWheelAcceleration();
            ShowToast(g_config.globalDisabled ? L"已禁用所有程序的滚轮加速"
                                              : L"已恢复所有程序的滚轮加速");
            UpdateTrayTooltip();
            break;
        case IDM_DISABLE_CURRENT_APP:
            ToggleCurrentProcessBlacklist();
            break;
        case IDM_EXIT:
            DestroyWindow(g_mainWindow);
            break;
        default:
            break;
    }
}

void CleanupAndQuit() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    RemoveTrayIcon();
    if (g_settingsWindow) {
        DestroyWindow(g_settingsWindow);
        g_settingsWindow = nullptr;
    }
    if (g_tooltipWindow) {
        DestroyWindow(g_tooltipWindow);
        g_tooltipWindow = nullptr;
    }
    if (g_toastWindow) {
        DestroyWindow(g_toastWindow);
        g_toastWindow = nullptr;
    }
    PostQuitMessage(0);
}

LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        AddTrayIcon();
        return 0;
    }

    switch (message) {
        case kTrayMessage: {
            const UINT event = LOWORD(lParam);
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                ShowTrayMenu();
            } else if (event == WM_LBUTTONDBLCLK) {
                ShowSettingsWindow();
            }
            return 0;
        }
        case kInjectMessage:
            switch (static_cast<InjectAction>(wParam)) {
                case INJECT_WHEEL:
                    SendWheelDelta(static_cast<int>(lParam));
                    break;
                case INJECT_HWHEEL:
                    SendHorizontalWheelDelta(static_cast<int>(lParam));
                    break;
                case INJECT_XBUTTON1:
                    ReplayXButton(XBUTTON1);
                    break;
                case INJECT_XBUTTON2:
                    ReplayXButton(XBUTTON2);
                    break;
                default:
                    break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_mainWindow = nullptr;
            CleanupAndQuit();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterWindowClasses() {
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = g_instance;
    mainClass.hIcon = g_appIcon;
    mainClass.hIconSm = g_appIconSmall;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.lpszClassName = kMainClass;
    if (!RegisterClassExW(&mainClass)) {
        return false;
    }

    WNDCLASSEXW settingsClass{};
    settingsClass.cbSize = sizeof(settingsClass);
    settingsClass.lpfnWndProc = SettingsWindowProc;
    settingsClass.hInstance = g_instance;
    settingsClass.hIcon = g_appIcon;
    settingsClass.hIconSm = g_appIconSmall;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    settingsClass.lpszClassName = kSettingsClass;
    if (!RegisterClassExW(&settingsClass)) {
        return false;
    }

    WNDCLASSEXW toastClass{};
    toastClass.cbSize = sizeof(toastClass);
    toastClass.lpfnWndProc = ToastWindowProc;
    toastClass.hInstance = g_instance;
    toastClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    toastClass.lpszClassName = kToastClass;
    if (!RegisterClassExW(&toastClass)) {
        return false;
    }
    return true;
}

bool CreateMainWindow() {
    g_mainWindow = CreateWindowExW(0, kMainClass, L"speed roll", WS_OVERLAPPED, 0, 0, 0, 0,
                                   nullptr, nullptr, g_instance, nullptr);
    return g_mainWindow != nullptr;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    // The settings controls only accept ASCII numbers and process names. Keep
    // third-party IME/TSF modules out of this tiny process.
    ImmDisableIME(static_cast<DWORD>(-1));
    g_instance = instance;

    g_instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\speed_roll_SingleInstance_7B39C7E6");
    if (!g_instanceMutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return 0;
    }

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&commonControls);

    g_exePath = GetModuleFilePath();
    g_exeDir = GetDirectoryName(g_exePath);
    g_exeName = NormalizeProcessName(g_exePath);
    g_configPath = g_exeDir + L"\\config.ini";
    g_diagEnabled =
        GetFileAttributesW((g_exeDir + L"\\diagnostics.on").c_str()) != INVALID_FILE_ATTRIBUTES;

    QueryPerformanceFrequency(&g_qpcFrequency);
    LoadConfig();
    g_autoStart = QueryAutoStart();

    g_appIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    if (!g_appIcon) {
        g_appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    const int smallIconSize = GetSystemMetrics(SM_CXSMICON);
    g_appIconSmall = LoadAppIcon(smallIconSize, smallIconSize);

    CreateFonts();
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (!RegisterWindowClasses() || !CreateMainWindow()) {
        MessageBoxW(nullptr, L"speed roll 初始化失败。", L"speed roll",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    AddTrayIcon();
    if (g_diagEnabled) {
        // Snapshot from a helper thread so log I/O never runs on the hook thread.
        std::thread([]() {
            for (;;) {
                Sleep(5000);
                DumpWheelDiagnostics();
            }
        }).detach();
    }

    g_injectEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_injectEvent) {
        HANDLE injectThread =
            CreateThread(nullptr, 0, InjectionThreadProc, nullptr, 0, nullptr);
        if (injectThread) {
            CloseHandle(injectThread);
        }
    }

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance, 0);
    if (!g_mouseHook) {
        MessageBoxW(nullptr, L"无法安装全局鼠标钩子。", L"speed roll",
                    MB_OK | MB_ICONERROR);
        DestroyWindow(g_mainWindow);
        return 1;
    }

    // Foreground tracking is event-driven, so the mouse hook never polls the
    // foreground window or resolves a process path on the input path.
    UpdateLastActiveProcess();
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                       ForegroundEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    // Keep the hook thread out of Win11's EcoQoS / efficiency-core demotion.
    ConfigureProcessScheduling();

    // Trim initial working set; the app otherwise keeps a tiny, stable footprint.
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                             static_cast<SIZE_T>(-1));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (g_settingsWindow && IsWindowVisible(g_settingsWindow)) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
                HWND focus = GetFocus();
                HWND blacklist = GetDlgItem(g_settingsWindow, IDC_EDIT_BLACKLIST);
                if (focus != blacklist) {
                    if (focus == GetDlgItem(g_settingsWindow, IDC_BTN_RESET)) {
                        RestoreDefaultSettingsControls();
                    } else if (focus == GetDlgItem(g_settingsWindow, IDC_BTN_CLOSE)) {
                        PopSettingsTooltip();
                        ShowWindow(g_settingsWindow, SW_HIDE);
                    } else {
                        SaveSettingsFromUI();
                    }
                    continue;
                }
            }
            if (IsDialogMessageW(g_settingsWindow, &message)) {
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DumpWheelDiagnostics();
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_foregroundHook) {
        UnhookWinEvent(g_foregroundHook);
        g_foregroundHook = nullptr;
    }
    RemoveTrayIcon();
    if (g_settingsWindow) {
        DestroyWindow(g_settingsWindow);
    }
    if (g_toastWindow) {
        DestroyWindow(g_toastWindow);
    }
    if (g_appIconSmall) {
        DestroyIcon(g_appIconSmall);
        g_appIconSmall = nullptr;
    }
    if (g_appIcon) {
        DestroyIcon(g_appIcon);
        g_appIcon = nullptr;
    }
    DeleteFonts();
    if (g_instanceMutex) {
        ReleaseMutex(g_instanceMutex);
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
    return 0;
}


