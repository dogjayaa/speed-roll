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
#include <cerrno>
#include <climits>
#include <cmath>
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
constexpr int kSettingsWidth = 640;
constexpr int kSettingsHeight = 640;
constexpr int kHeaderHeight = 96;
constexpr int kCardLeft = 16;
constexpr int kCardTop = 80;
constexpr int kCardRight = 624;
constexpr int kCardBottom = 552;
constexpr int kRowX = 36;
constexpr int kRowStartY = 190;
constexpr int kRowGap = 32;
constexpr int kRowHeight = 30;
constexpr int kEditX = 386;
constexpr int kEditWidth = 190;
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

// PID -> process name cache. The low-level mouse hook must never open another
// process on the input path, so names are resolved once per PID and reused.
constexpr size_t kPidCacheLimit = 512;
std::unordered_map<DWORD, std::wstring> g_pidCache;
HWINEVENTHOOK g_foregroundHook = nullptr;

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

std::wstring GetProcessNameAtPoint(POINT point) {
    HWND window = WindowFromPoint(point);
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

void UpdateLastActiveProcessFromWindow(HWND window) {
    std::wstring foreground = GetWindowProcessName(window);
    if (!foreground.empty() && !IsShellOrSelf(foreground)) {
        g_lastActiveProcess = foreground;
    }
}

void UpdateLastActiveProcess() {
    UpdateLastActiveProcessFromWindow(GetForegroundWindow());
}

// Runs on this thread's message loop for every foreground change; the mouse
// hook no longer polls the foreground window or opens processes per event.
void CALLBACK ForegroundEventProc(HWINEVENTHOOK, DWORD event, HWND window, LONG objectId,
                                  LONG childId, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && window && objectId == OBJID_WINDOW &&
        childId == CHILDID_SELF) {
        UpdateLastActiveProcessFromWindow(window);
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

void ResetWheelAcceleration() {
    g_wheelRemainder = 0.0;
    g_lastWheelDirection = 0;
    g_lastWheelMs = 0.0;
    g_fastStreak = 0;
}

void SendWheelDelta(int delta) {
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(delta), kInjectedTag);
}

void SendHorizontalWheelDelta(int delta) {
    mouse_event(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(delta), kInjectedTag);
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
    mouse_event(MOUSEEVENTF_XDOWN, 0, 0, button, kInjectedTag);
    mouse_event(MOUSEEVENTF_XUP, 0, 0, button, kInjectedTag);
}

// Never call mouse_event/SendInput directly from inside a WH_MOUSE_LL callback.
// The injected mouse event shares the active hook chain and can block the
// callback until the low-level hook timeout (~300 ms). Queue the injection and
// let the message loop run it immediately after the callback returns.
void QueueInjection(InjectAction action, LONG_PTR data = 0) {
    if (g_mainWindow) {
        PostMessageW(g_mainWindow, kInjectMessage, static_cast<WPARAM>(action),
                     static_cast<LPARAM>(data));
    }
}

void EnsureToastWindow();
void ShowToast(const std::wstring& text);
void UpdateTrayTooltip();

void ToggleHyperMode() {
    g_hyperMode = !g_hyperMode;
    ResetWheelAcceleration();
    ShowToast(g_hyperMode ? L"⚡ 滚轮：极速模式" : L"🖱️ 滚轮：普通模式");
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
    const std::wstring targetProcess = GetProcessNameAtPoint(info.pt);

    if (IsScrollDisabledFor(targetProcess)) {
        ResetWheelAcceleration();
        return false;  // native wheel is passed through untouched
    }

    // Forward button held alone: wheel jumps to the top or bottom.
    if (g_x2Down && !g_x1Down) {
        g_x2UsedAsModifier = true;
        DWORD now = GetTickCount();
        if (static_cast<DWORD>(now - g_lastHomeEndTick) >= kHomeEndCooldownMs) {
            g_lastHomeEndTick = now;
            HWND targetRoot = GetAncestor(WindowFromPoint(info.pt), GA_ROOT);
            // Start the keyboard worker first; the no-activate toast is created
            // afterwards so it cannot race the target window's focus change.
            StartDocumentExtremeAsync(direction > 0, targetRoot);
            ShowToast(direction > 0 ? L"⬆️ 已直达顶部" : L"⬇️ 已直达底部");
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

    // A pause, a slow tick, or a reversal ends the fast streak. The first
    // tick of the new gesture is always delivered at exactly 1x.
    if (slow || reversed) {
        g_fastStreak = 0;
        g_wheelRemainder = 0.0;
        QueueInjection(INJECT_WHEEL, direction * kWheelDelta);
        return true;
    }

    if (g_config.preventZoomOverrun &&
        ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000))) {
        g_fastStreak = 0;
        g_wheelRemainder = 0.0;
        QueueInjection(INJECT_WHEEL, direction * kWheelDelta);
        return true;
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

    if (wholeClicks >= 1.0) {
        int clicks = static_cast<int>(wholeClicks);
        if (clicks > 1000) {
            clicks = 1000;
        }
        QueueInjection(INJECT_WHEEL, direction * kWheelDelta * clicks);
    }
    return true;
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
        if (HandleWheel(*info)) {
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
    g_titleFont = CreateFontW(-MulDiv(21, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
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
    g_editBrush = CreateSolidBrush(RGB(248, 250, 253));
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
            HBRUSH background = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(dc, &client, background);
            DeleteObject(background);
            RECT accent{0, 0, Scale(3), client.bottom};
            HBRUSH accentBrush = CreateSolidBrush(RGB(47, 107, 255));
            FillRect(dc, &accent, accentBrush);
            DeleteObject(accentBrush);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(250, 250, 250));
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
        ShowToast(L"✅ 配置已保存并生效");
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
    SendMessageW(g_tooltipWindow, TTM_SETTIPBKCOLOR, static_cast<WPARAM>(RGB(32, 36, 43)), 0);
    SendMessageW(g_tooltipWindow, TTM_SETTIPTEXTCOLOR,
                 static_cast<WPARAM>(RGB(245, 247, 250)), 0);
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

void CreateLabeledEdit(HWND parent, const wchar_t* label, const wchar_t* value, int x, int y,
                       int labelWidth, int editX, int editWidth, int controlId,
                       const wchar_t* tooltipText) {
    HWND labelWindow = CreateWindowExW(
        0, L"STATIC", label,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
        Scale(x), Scale(y), Scale(labelWidth), Scale(kRowHeight), parent, nullptr,
        g_instance, nullptr);
    HWND editWindow = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", value,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        Scale(editX), Scale(y), Scale(editWidth), Scale(kRowHeight), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), g_instance, nullptr);
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

void PaintSettingsBackground(HDC dc, const RECT& client) {
    FillColorRect(dc, client.left, client.top, client.right, client.bottom, RGB(238, 242, 248));

    const int header = Scale(kHeaderHeight);
    for (int y = 0; y < header; ++y) {
        const double t = static_cast<double>(y) / std::max(1, header - 1);
        const COLORREF color = RGB(
            static_cast<int>(42.0 + (90.0 - 42.0) * t),
            static_cast<int>(99.0 + (142.0 - 99.0) * t),
            static_cast<int>(246.0 + (255.0 - 246.0) * t));
        FillColorRect(dc, 0, y, client.right, y + 1, color);
    }

    // Subtle decorative arcs in the header.
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN arcPen1 = CreatePen(PS_SOLID, std::max(1, Scale(1)), RGB(105, 154, 255));
    HPEN arcPen2 = CreatePen(PS_SOLID, std::max(1, Scale(1)), RGB(128, 171, 255));
    HGDIOBJ oldPen = SelectObject(dc, arcPen1);
    Ellipse(dc, Scale(470), Scale(-70), Scale(700), Scale(160));
    SelectObject(dc, arcPen2);
    Ellipse(dc, Scale(540), Scale(-145), Scale(780), Scale(95));
    SelectObject(dc, oldPen);

    DrawIconEx(dc, Scale(24), Scale(22), g_appIcon, Scale(44), Scale(44), 0, nullptr,
               DI_NORMAL);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g_titleFont);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT titleRect{Scale(82), Scale(12), Scale(500), Scale(52)};
    DrawTextW(dc, L"speed roll", -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, g_subtitleFont);
    SetTextColor(dc, RGB(222, 235, 255));
    RECT subtitleRect{Scale(84), Scale(54), Scale(500), Scale(80)};
    DrawTextW(dc, L"轻量滚轮增强 · 参数设置", -1, &subtitleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Mode badge.
    RECT badgeRect{Scale(520), Scale(30), Scale(624), Scale(60)};
    HBRUSH badgeBrush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ badgeOldBrush = SelectObject(dc, badgeBrush);
    HGDIOBJ badgeOldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, badgeRect.left, badgeRect.top, badgeRect.right, badgeRect.bottom,
              Scale(16), Scale(16));
    SelectObject(dc, g_uiFont);
    SetTextColor(dc, RGB(47, 107, 255));
    DrawTextW(dc, g_hyperMode ? L"极速模式" : L"普通模式", -1, &badgeRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, badgeOldPen);
    SelectObject(dc, badgeOldBrush);
    DeleteObject(badgeBrush);

    // Soft card shadow and card.
    RECT shadowRect{Scale(kCardLeft + 2), Scale(kCardTop + 2), Scale(kCardRight + 2),
                    Scale(kCardBottom + 4)};
    HBRUSH shadowBrush = CreateSolidBrush(RGB(224, 231, 242));
    HGDIOBJ shadowOldBrush = SelectObject(dc, shadowBrush);
    HGDIOBJ shadowOldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, shadowRect.left, shadowRect.top, shadowRect.right, shadowRect.bottom,
              Scale(18), Scale(18));
    SelectObject(dc, shadowOldPen);
    SelectObject(dc, shadowOldBrush);
    DeleteObject(shadowBrush);

    RECT cardRect{Scale(kCardLeft), Scale(kCardTop), Scale(kCardRight), Scale(kCardBottom)};
    HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(220, 227, 240));
    HGDIOBJ cardOldBrush = SelectObject(dc, g_cardBrush);
    HGDIOBJ cardOldPen = SelectObject(dc, cardPen);
    RoundRect(dc, cardRect.left, cardRect.top, cardRect.right, cardRect.bottom, Scale(18),
              Scale(18));
    SelectObject(dc, cardOldPen);
    SelectObject(dc, cardOldBrush);
    DeleteObject(cardPen);

    FillColorRect(dc, Scale(24), Scale(102), Scale(28), Scale(120), RGB(47, 107, 255));

    SelectObject(dc, g_headerFont);
    SetTextColor(dc, RGB(27, 35, 50));
    RECT sectionRect{Scale(36), Scale(94), Scale(330), Scale(120)};
    DrawTextW(dc, L"滚轮加速参数", -1, &sectionRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Highlight the mode-switch chord instead of using a faint hint.
    RECT modePill{Scale(272), Scale(90), Scale(604), Scale(120)};
    HBRUSH modePillBrush = CreateSolidBrush(RGB(47, 107, 255));
    HPEN modePillPen = CreatePen(PS_SOLID, 1, RGB(91, 143, 255));
    HGDIOBJ modeOldBrush = SelectObject(dc, modePillBrush);
    HGDIOBJ modeOldPen = SelectObject(dc, modePillPen);
    RoundRect(dc, modePill.left, modePill.top, modePill.right, modePill.bottom,
              Scale(15), Scale(15));
    SelectObject(dc, modeOldPen);
    SelectObject(dc, modeOldBrush);
    DeleteObject(modePillPen);
    DeleteObject(modePillBrush);

    SelectObject(dc, g_uiFont);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"前进+后退键同时按：切换极速模式/普通模式", -1, &modePill,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Side-button action guide: keep the two wheel gestures explicit.
    RECT actionPanel{Scale(36), Scale(122), Scale(604), Scale(178)};
    HBRUSH actionBrush = CreateSolidBrush(RGB(242, 247, 255));
    HPEN actionPen = CreatePen(PS_SOLID, 1, RGB(220, 232, 255));
    HGDIOBJ actionOldBrush = SelectObject(dc, actionBrush);
    HGDIOBJ actionOldPen = SelectObject(dc, actionPen);
    RoundRect(dc, actionPanel.left, actionPanel.top, actionPanel.right,
              actionPanel.bottom, Scale(12), Scale(12));
    SelectObject(dc, actionOldPen);
    SelectObject(dc, actionOldBrush);
    DeleteObject(actionPen);
    DeleteObject(actionBrush);

    FillColorRect(dc, Scale(320), Scale(132), Scale(321), Scale(168), RGB(220, 232, 255));

    SelectObject(dc, g_uiFont);
    SetTextColor(dc, RGB(47, 107, 255));
    RECT forwardTitle{Scale(52), Scale(127), Scale(306), Scale(147)};
    DrawTextW(dc, L"前进键 + 滚轮 ↑ / ↓", -1, &forwardTitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT backTitle{Scale(336), Scale(127), Scale(590), Scale(147)};
    DrawTextW(dc, L"后退键 + 滚轮 ← / →", -1, &backTitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, g_hintFont);
    SetTextColor(dc, RGB(112, 122, 138));
    RECT forwardDesc{Scale(52), Scale(149), Scale(306), Scale(169)};
    DrawTextW(dc, L"直达文档 / 表格顶部或底部", -1, &forwardDesc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT backDesc{Scale(336), Scale(149), Scale(590), Scale(169)};
    DrawTextW(dc, L"左右横向滚动", -1, &backDesc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    FillColorRect(dc, Scale(36), Scale(184), Scale(604), Scale(185), RGB(236, 240, 247));

    SelectObject(dc, oldFont);
    DeleteObject(arcPen1);
    DeleteObject(arcPen2);
    SelectObject(dc, oldBrush);
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

    COLORREF fill = RGB(238, 242, 249);
    COLORREF border = RGB(214, 222, 236);
    COLORREF text = RGB(48, 58, 74);
    if (id == IDC_BTN_SAVE) {
        fill = pressed ? RGB(35, 85, 220) : (hot ? RGB(70, 124, 255) : RGB(47, 107, 255));
        border = fill;
        text = RGB(255, 255, 255);
    } else if (id == IDC_BTN_CLOSE) {
        fill = pressed ? RGB(234, 239, 247) : (hot ? RGB(244, 247, 252) : RGB(255, 255, 255));
        border = RGB(220, 227, 238);
        text = RGB(92, 103, 120);
    } else if (hot) {
        fill = pressed ? RGB(218, 227, 241) : RGB(229, 235, 246);
    } else if (pressed) {
        fill = RGB(218, 227, 241);
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
        InflateRect(&focusRect, -Scale(4), -Scale(4));
        DrawFocusRect(item->hDC, &focusRect);
    }
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
    CreateSettingsTooltip(g_settingsWindow);

    CreateLabeledEdit(g_settingsWindow, L"快速滚动阈值 FastThreshold (ms)", L"100",
                      kRowX, kRowStartY, 330, kEditX, kEditWidth, IDC_EDIT_FAST,
                      L"单格滚轮事件的最大间隔（毫秒）。小于该值才计入快滚；默认 100。"
                      L"实际快滚不触发加速时可适当调大。");
    CreateLabeledEdit(g_settingsWindow, L"触发快滚格数 MinFastTicks", L"3",
                      kRowX, kRowStartY + kRowGap, 330, kEditX, kEditWidth,
                      IDC_EDIT_MIN_TICKS,
                      L"极速模式下触发加速所需的连续同向快滚格数。默认 3；"
                      L"数值越大越不容易误触。");
    CreateLabeledEdit(g_settingsWindow, L"最大加速倍率 MaxMultiplier", L"8",
                      kRowX, kRowStartY + kRowGap * 2, 330, kEditX, kEditWidth,
                      IDC_EDIT_MAX,
                      L"快速滚动时允许的最大倍率。8 表示最多一次滚动 8 格；默认 8。");
    CreateLabeledEdit(g_settingsWindow, L"加速曲线指数 CurvePower", L"1.8",
                      kRowX, kRowStartY + kRowGap * 3, 330, kEditX, kEditWidth,
                      IDC_EDIT_CURVE,
                      L"加速曲线指数。数值越大，只有越快的滚动才接近最大倍率；默认 1.8。");
    CreateLabeledEdit(g_settingsWindow, L"横向滚动倍率 HorizontalMultiplier", L"1",
                      kRowX, kRowStartY + kRowGap * 4, 330, kEditX, kEditWidth,
                      IDC_EDIT_HORIZONTAL,
                      L"按住后退键滚动时的横向滚动倍率。1 表示与原生横向滚动一致。");

    HWND zoomCheck = CreateWindowExW(
        0, L"BUTTON", L"防缩放失控 (按住 Ctrl/Alt 恢复 1x)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, Scale(kRowX), Scale(360),
        Scale(540), Scale(26), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_ZOOM)), g_instance, nullptr);
    SendMessageW(zoomCheck, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    AddTooltip(g_settingsWindow, zoomCheck,
               L"勾选后，按住 Ctrl 或 Alt 滚动会跳过倍速并强制 1x，"
               L"避免文档和表格缩放失控。");

    HWND autoStartCheck = CreateWindowExW(
        0, L"BUTTON", L"开机自动静默启动",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, Scale(kRowX), Scale(392),
        Scale(400), Scale(26), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_AUTOSTART)), g_instance, nullptr);
    SendMessageW(autoStartCheck, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    AddTooltip(g_settingsWindow, autoStartCheck,
               L"勾选并保存后，把当前 exe 写入当前用户的开机启动项，"
               L"实现登录后静默启动。");

    HWND blacklistLabel = CreateWindowExW(
        0, L"STATIC", L"进程黑名单（每行一个进程名，不含 .exe；不区分大小写）",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, Scale(kRowX),
        Scale(422), Scale(568), Scale(22), g_settingsWindow, nullptr, g_instance, nullptr);
    SendMessageW(blacklistLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    AddTooltip(g_settingsWindow, blacklistLabel,
               L"每行填写一个进程名（不含 .exe）。列出的程序内滚轮加速和"
               L"侧键+滚轮功能不生效，普通滚轮保持原生。");

    HWND blacklistEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN | ES_LEFT,
        Scale(kRowX), Scale(446), Scale(568), Scale(80), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_BLACKLIST)), g_instance, nullptr);
    SendMessageW(blacklistEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    SendMessageW(blacklistEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(Scale(6), Scale(6)));
    AddTooltip(g_settingsWindow, blacklistEdit,
               L"每行填写一个进程名（不含 .exe）。列出的程序内滚轮加速和"
               L"侧键+滚轮功能不生效，普通滚轮保持原生。");

    HWND saveButton = CreateWindowExW(
        0, L"BUTTON", L"保存并立即生效",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Scale(36), Scale(570),
        Scale(190), Scale(40), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE)), g_instance, nullptr);
    SetWindowSubclass(saveButton, FlatButtonSubclass, 1, 0);

    HWND resetButton = CreateWindowExW(
        0, L"BUTTON", L"恢复默认值",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Scale(240), Scale(570),
        Scale(150), Scale(40), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_RESET)), g_instance, nullptr);
    SetWindowSubclass(resetButton, FlatButtonSubclass, 1, 0);

    HWND closeButton = CreateWindowExW(
        0, L"BUTTON", L"关闭",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Scale(520), Scale(570),
        Scale(84), Scale(40), g_settingsWindow,
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
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(58, 67, 82));
            return reinterpret_cast<LRESULT>(g_cardBrush);
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(58, 67, 82));
            return reinterpret_cast<LRESULT>(g_cardBrush);
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(29, 36, 51));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(248, 250, 253));
            return reinterpret_cast<LRESULT>(g_editBrush);
        case WM_DRAWITEM:
            DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_CLOSE:
            PopSettingsTooltip();
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_COMMAND:
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
                        PopSettingsTooltip();
                        ShowWindow(window, SW_HIDE);
                        return 0;
                    }
                    break;
                case IDCANCEL:
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
        ShowToast(L"🚫 已在 " + name + L" 中禁用加速");
    } else {
        g_config.disabledProcesses.erase(it);
        SaveConfig();
        ShowToast(L"✅ 已恢复 " + name + L" 的滚轮加速");
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
            ShowToast(g_config.globalDisabled ? L"🌐 已禁用所有程序的滚轮加速"
                                              : L"🌐 已恢复所有程序的滚轮加速");
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

    // The hook callback runs on this thread; raise its scheduling priority so
    // a busy system cannot add latency to global mouse input handling.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

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


