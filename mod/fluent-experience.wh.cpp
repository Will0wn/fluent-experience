// ==WindhawkMod==
// @id              fluent-experience
// @name            Fluent Experience for Windows 11
// @description     Turns the Windows 11 taskbar into a compact, floating taskbar built on what Windows provides
// @version         0.16
// @author          Will0wn
// @github          https://github.com/Will0wn
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lshcore -lole32 -loleaut32 -lruntimeobject -ldwmapi -lshell32 -lgdi32 -lcomctl32 -lwinhttp
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Fluent Experience for Windows 11

A compact, floating taskbar built on top of the native Windows 11 taskbar. It
doesn't replace it: Start, search, previews, menus and Windows updates keep
working.

## Philosophy

Fluent Experience is built on what Windows already provides, and adds custom
code only when there's no native way to do something. The taskbar is the real
Windows taskbar, not a replacement: Start, search, previews, menus and tray
icons are the ones Windows draws. Materials, rounded corners, borders and
shadows are Windows 11's own, and Windows settings such as light/dark mode,
Transparency effects, Animation effects and Show smaller taskbar buttons keep
working on the taskbar.

This keeps Fluent Experience working across Windows updates and upgrades: when
Microsoft changes how something looks or behaves, the taskbar follows, instead
of being stuck with a copy of an older design.

## Features

* **Compact taskbar**: the taskbar is actually narrowed and centered, not just
  clipped. Clicks, menus and corners follow it.
* **Dynamic width**: each taskbar grows and shrinks on its own to fit its icons,
  on every monitor, animated when Windows animation effects are on.
* **Floating taskbar**: the taskbar sits a few pixels above the bottom edge of
  the screen.
* **Windows materials**: the taskbar can use the Windows 11 materials (Acrylic,
  Mica, Mica Alt) with the rounded corners and shadow Windows gives windows,
  following the light/dark theme and the transparency effects setting. With
  the default background the rounded corners are optional, and the window
  border can be hidden.
* **Reserved space**: maximized windows stop above the taskbar.
* **Settings button**: a button on the taskbar, between the tray icons
  and the clock, opens the Fluent Experience settings window.
* **Updates**: Fluent Experience checks for new versions on GitHub at startup
  and once a day (this can be turned off). When one is available, the settings
  button shows it the way Windows shows an app that needs attention, and the
  update is installed from the settings window, never on its own.

Windows settings still apply: "Show smaller taskbar buttons" and "Animation
effects" work as on the original taskbar.

Every feature can be turned off; disabling Fluent Experience restores the
original Windows taskbar.

## Settings

Fluent Experience is configured in its own settings window, not on this page.
Open it with the settings button on the taskbar, before the clock, or here:
[Open Fluent Experience Settings](fluent-experience-settings:).

Changes apply immediately, no restart needed.
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <inspectable.h>
#include <propsys.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winhttp.h>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace winrt::Windows::UI::Xaml;

// The Windows 11 materials, or the taskbar's default background.
enum class Material {
    windowsDefault,
    acrylic,
    mica,
    micaAlt,
};

// The settings live in HKCU, written by the settings window without elevation.
constexpr WCHAR kSettingsSubKey[] = LR"(Software\FluentExperience)";

struct Settings {
    bool enabled;
    Material material;
    bool border;
    bool roundedCorners;
    bool dynamicWidth;
    int width;
    int padding;
    int animationDuration;
    int margin;
    bool settingsButton;
    bool hiddenIconMenu;
    bool checkUpdates;
    std::wstring language;
    std::wstring logFile;
};

Settings g_settings;

std::atomic<bool> g_unloading;

// Whether the mod changes the taskbar: loaded, and enabled in the settings.
bool IsActive() {
    return !g_unloading && g_settings.enabled;
}
std::atomic<bool> g_applyingSettings;
std::atomic<int> g_hookCallCounter;
std::atomic<bool> g_taskbarViewDllHooked;

// Dock width per monitor, in pixels. While animating, current moves from
// "from" to "target".
struct DockWidth {
    int current;
    int from;
    int target;
    ULONGLONG startTick;
};

std::mutex g_dockWidthsMutex;
std::unordered_map<HMONITOR, DockWidth> g_dockWidths;

const UINT g_dockWidthMsg = RegisterWindowMessage(L"FluentExperience_SetWidth");
const UINT g_applyStyleMsg = RegisterWindowMessage(L"FluentExperience_ApplyStyle");

constexpr UINT_PTR kWidthAnimationTimerId = 0xD0C8;

std::mutex g_logMutex;

void DebugLog(PCWSTR format, ...) {
    WCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    int len = _vsnwprintf(buffer, ARRAYSIZE(buffer) - 1, format, args);
    va_end(args);
    buffer[len < 0 ? ARRAYSIZE(buffer) - 1 : len] = L'\0';

    Wh_Log(L"%s", buffer);

    std::lock_guard<std::mutex> guard(g_logMutex);
    if (g_settings.logFile.empty()) {
        return;
    }

    HANDLE file = CreateFile(g_settings.logFile.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR line[1100];
    int lineLen = _snwprintf(line, ARRAYSIZE(line) - 1,
                             L"%02d:%02d:%02d.%03d %s\r\n", st.wHour,
                             st.wMinute, st.wSecond, st.wMilliseconds, buffer);
    if (lineLen > 0) {
        char utf8[3300];
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line, lineLen, utf8,
                                        sizeof(utf8), nullptr, nullptr);
        DWORD written;
        WriteFile(file, utf8, bytes, &written, nullptr);
    }

    CloseHandle(file);
}

int ReadIntSetting(HKEY key, PCWSTR name, int defaultValue) {
    DWORD value;
    DWORD size = sizeof(value);
    if (!key || RegGetValue(key, nullptr, name, RRF_RT_REG_DWORD, nullptr,
                            &value, &size) != ERROR_SUCCESS) {
        return defaultValue;
    }

    return (int)value;
}

std::wstring ReadStringSetting(HKEY key, PCWSTR name, PCWSTR defaultValue) {
    WCHAR value[MAX_PATH];
    DWORD size = sizeof(value);
    if (!key || RegGetValue(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value,
                            &size) != ERROR_SUCCESS) {
        return defaultValue;
    }

    return value;
}

// Missing values fall back to the defaults, which the settings window shows
// too.
Settings ReadSettings() {
    HKEY key = nullptr;
    RegOpenKeyEx(HKEY_CURRENT_USER, kSettingsSubKey, 0, KEY_READ, &key);

    Settings settings;
    settings.enabled = ReadIntSetting(key, L"enabled", 1);

    std::wstring material = ReadStringSetting(key, L"material", L"acrylic");
    settings.material = material == L"default" ? Material::windowsDefault
                        : material == L"mica"  ? Material::mica
                        : material == L"micaAlt"
                            ? Material::micaAlt
                            : Material::acrylic;

    settings.border = ReadStringSetting(key, L"border", L"none") != L"none";
    settings.roundedCorners = ReadIntSetting(key, L"roundedCorners", 1);
    settings.dynamicWidth = ReadIntSetting(key, L"dynamicWidth", 1);
    settings.width = std::max(ReadIntSetting(key, L"width", 800), 200);
    settings.padding = std::max(ReadIntSetting(key, L"padding", 16), 0);
    settings.animationDuration =
        std::clamp(ReadIntSetting(key, L"animationDuration", 200), 1, 2000);
    settings.margin = std::clamp(ReadIntSetting(key, L"margin", 8), 0, 200);
    settings.settingsButton = ReadIntSetting(key, L"settingsButton", 1);
    settings.hiddenIconMenu = ReadIntSetting(key, L"hiddenIconMenu", 1);
    settings.checkUpdates = ReadIntSetting(key, L"checkUpdates", 1);
    settings.language = ReadStringSetting(key, L"language", L"en");
    settings.logFile = ReadStringSetting(key, L"logFile", L"");

    if (key) {
        RegCloseKey(key);
    }

    return settings;
}

void LoadSettings() {
    Settings settings = ReadSettings();

    std::lock_guard<std::mutex> guard(g_logMutex);
    g_settings = settings;
}

// The Fluent Experience Settings app of earlier versions registered its path;
// the settings window replaced it.
void RemoveSettingsAppLeftovers() {
    RegDeleteKeyValue(HKEY_CURRENT_USER, kSettingsSubKey, L"settingsAppPath");
}

constexpr WCHAR kSettingsLinkKey[] =
    LR"(Software\Classes\fluent-experience-settings)";
constexpr WCHAR kOpenSettingsValue[] = L"openSettings";

// The fluent-experience-settings: link in the readme opens the settings
// window, also with the settings button off. With no exe of its own, the link
// runs reg.exe to write a request to the settings key, which the settings
// watcher sees. Registered while the mod is loaded.
void RegisterSettingsLink() {
    WCHAR systemDir[MAX_PATH];
    GetSystemDirectory(systemDir, ARRAYSIZE(systemDir));
    std::wstring command = L"\"" + std::wstring(systemDir) +
                           LR"(\reg.exe" add "HKCU\)" + kSettingsSubKey +
                           L"\" /v " + kOpenSettingsValue +
                           L" /t REG_DWORD /d 1 /f";
    std::wstring commandKey = std::wstring(kSettingsLinkKey) + LR"(\shell\open\command)";

    constexpr WCHAR kName[] = L"URL:Fluent Experience Settings";
    RegSetKeyValue(HKEY_CURRENT_USER, kSettingsLinkKey, nullptr, REG_SZ, kName,
                   sizeof(kName));
    RegSetKeyValue(HKEY_CURRENT_USER, kSettingsLinkKey, L"URL Protocol", REG_SZ,
                   L"", sizeof(WCHAR));
    RegSetKeyValue(HKEY_CURRENT_USER, commandKey.c_str(), nullptr, REG_SZ,
                   command.c_str(),
                   (DWORD)((command.size() + 1) * sizeof(WCHAR)));
}

void UnregisterSettingsLink() {
    RegDeleteTree(HKEY_CURRENT_USER, kSettingsLinkKey);
}

// Whether the link asked for the settings window; the request is removed.
bool TakeOpenSettingsRequest() {
    if (RegGetValue(HKEY_CURRENT_USER, kSettingsSubKey, kOpenSettingsValue,
                    RRF_RT_REG_DWORD, nullptr, nullptr,
                    nullptr) != ERROR_SUCCESS) {
        return false;
    }

    RegDeleteKeyValue(HKEY_CURRENT_USER, kSettingsSubKey, kOpenSettingsValue);
    return true;
}

bool GetMonitorRect(HMONITOR monitor, RECT* rc) {
    MONITORINFO monitorInfo{
        .cbSize = sizeof(MONITORINFO),
    };
    if (!GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    *rc = monitorInfo.rcMonitor;
    return true;
}

int GetMonitorIndex(HMONITOR monitor);

int GetDockWidthForMonitor(HMONITOR monitor, const RECT& monitorRect) {
    int width = 0;
    if (g_settings.dynamicWidth) {
        std::lock_guard<std::mutex> guard(g_dockWidthsMutex);
        auto it = g_dockWidths.find(monitor);
        if (it != g_dockWidths.end()) {
            width = it->second.current;
        }
    }

    // Until the icons are measured, a dynamic dock is as wide as the screen,
    // like the original taskbar, so Windows doesn't shrink the buttons for
    // lack of room. The monitor index keeps the taskbars' widths distinct.
    if (!width && g_settings.dynamicWidth) {
        width = monitorRect.right - monitorRect.left - GetMonitorIndex(monitor);
    }

    if (!width) {
        UINT dpiX = 96;
        UINT dpiY = 96;
        GetDpiForMonitor(monitor, MDT_DEFAULT, &dpiX, &dpiY);
        width = MulDiv(g_settings.width, dpiX, 96) + GetMonitorIndex(monitor);
    }

    return std::min(width, (int)(monitorRect.right - monitorRect.left));
}

// Narrows and centers a horizontal bottom taskbar rect. The rect of an
// auto-hidden taskbar is mostly below the monitor, so the monitor is taken from
// the top edge.
void NarrowBottomTaskbarRect(RECT* rc) {
    if (rc->right - rc->left <= rc->bottom - rc->top) {
        return;
    }

    HMONITOR monitor = MonitorFromPoint({(rc->left + rc->right) / 2, rc->top},
                                        MONITOR_DEFAULTTONEAREST);
    RECT monitorRect;
    if (!GetMonitorRect(monitor, &monitorRect)) {
        return;
    }

    // Bottom taskbar only.
    if (rc->top < (monitorRect.top + monitorRect.bottom) / 2) {
        return;
    }

    int width = GetDockWidthForMonitor(monitor, monitorRect);
    rc->left =
        monitorRect.left + (monitorRect.right - monitorRect.left - width) / 2;
    rc->right = rc->left + width;

    // Lift the dock off the bottom edge. An auto-hidden taskbar (mostly below
    // the monitor) stays where Windows puts it. The rect may already be lifted,
    // so it's placed from the monitor edge to keep this idempotent.
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(monitor, MDT_DEFAULT, &dpiX, &dpiY);
    int margin = MulDiv(g_settings.margin, dpiY, 96);
    if (margin > 0 && (rc->bottom == monitorRect.bottom ||
                       rc->bottom == monitorRect.bottom - margin)) {
        int height = rc->bottom - rc->top;
        rc->bottom = monitorRect.bottom - margin;
        rc->top = rc->bottom - height;
    }
}

// Whether the rect is the dock's (narrowed and centered at the bottom).
bool IsDockRect(const RECT* rc) {
    RECT fullWidth = *rc;
    HMONITOR monitor = MonitorFromPoint({(rc->left + rc->right) / 2, rc->top},
                                        MONITOR_DEFAULTTONEAREST);
    RECT monitorRect;
    if (!GetMonitorRect(monitor, &monitorRect)) {
        return false;
    }

    fullWidth.left = monitorRect.left;
    fullWidth.right = monitorRect.right;
    NarrowBottomTaskbarRect(&fullWidth);
    return EqualRect(&fullWidth, rc) &&
           rc->right - rc->left < monitorRect.right - monitorRect.left;
}

// Explorer computes the work area as "monitor minus taskbar" with
// SubtractRect, which only subtracts when the taskbar spans the full width.
// For that computation the dock is widened to the monitor edges, so maximized
// windows stop above the dock.
using SubtractRect_t = decltype(&SubtractRect);
SubtractRect_t SubtractRect_Original;
BOOL WINAPI SubtractRect_Hook(LPRECT lprcDst,
                              const RECT* lprcSrc1,
                              const RECT* lprcSrc2) {
    if (IsActive() && lprcSrc1 && lprcSrc2 && IsDockRect(lprcSrc2)) {
        RECT widened = *lprcSrc2;
        widened.left = std::min(widened.left, lprcSrc1->left);
        widened.right = std::max(widened.right, lprcSrc1->right);
        widened.bottom = std::max(widened.bottom, lprcSrc1->bottom);
        return SubtractRect_Original(lprcDst, lprcSrc1, &widened);
    }

    return SubtractRect_Original(lprcDst, lprcSrc1, lprcSrc2);
}

bool IsTaskbarWindow(HWND hWnd) {
    WCHAR szClassName[32];
    if (!GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName))) {
        return false;
    }

    return _wcsicmp(szClassName, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(szClassName, L"Shell_SecondaryTrayWnd") == 0;
}

// The hidden icon menu, opened by the arrow before the tray icons: a window of
// its own with a XAML island.
constexpr WCHAR kHiddenIconMenuClass[] = L"TopLevelWindowForOverflowXamlIsland";

bool IsHiddenIconMenuWindow(HWND hWnd) {
    WCHAR szClassName[64];
    return GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName)) &&
           _wcsicmp(szClassName, kHiddenIconMenuClass) == 0;
}

////////////////////////////////////////////////////////////////////////////////
// Dynamic width

FrameworkElement EnumChildElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback) {
    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);

    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (child && enumCallback(child)) {
            return child;
        }
    }

    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    return EnumChildElements(element, [name](FrameworkElement child) {
        return child.Name() == name;
    });
}

FrameworkElement FindChildByClassName(FrameworkElement element,
                                      PCWSTR className) {
    return EnumChildElements(element, [className](FrameworkElement child) {
        return winrt::get_class_name(child) == className;
    });
}

////////////////////////////////////////////////////////////////////////////////
// Dock style

// DWM attributes, numbered explicitly since older SDK headers lack some.
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr DWORD kDwmwaSystemBackdropType = 38;

constexpr DWORD kDwmColorDefault = 0xFFFFFFFF;
constexpr DWORD kDwmColorNone = 0xFFFFFFFE;

constexpr DWORD kDwmwcpDefault = 0;
constexpr DWORD kDwmwcpRound = 2;

constexpr DWORD kDwmsbtAuto = 0;
constexpr DWORD kDwmsbtMainWindow = 2;       // Mica
constexpr DWORD kDwmsbtTransientWindow = 3;  // Acrylic
constexpr DWORD kDwmsbtTabbedWindow = 4;     // Mica Alt

bool UseWindowsMaterial() {
    return IsActive() && g_settings.material != Material::windowsDefault;
}

// The hidden icon menu takes the taskbar's material when its setting is on.
bool StyleHiddenIconMenu() {
    return UseWindowsMaterial() && g_settings.hiddenIconMenu;
}

// Whether a window gets the Windows material: a taskbar, or the hidden icon
// menu.
bool UseWindowsMaterialFor(HWND hWnd) {
    return IsHiddenIconMenuWindow(hWnd) ? StyleHiddenIconMenu()
                                        : UseWindowsMaterial();
}

bool IsTaskbarLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    RegGetValue(HKEY_CURRENT_USER,
                LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
                L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                &size);
    return value != 0;
}

// Explorer gives the taskbar window an accent policy (its own transparency
// effect), which takes precedence over the system backdrop. With the native
// material it's turned off; the last one Explorer asked for is kept so it can
// be put back.
constexpr int kWcaAccentPolicy = 19;
constexpr int kAccentDisabled = 0;

struct AccentPolicy {
    int accentState;
    int accentFlags;
    DWORD gradientColor;
    int animationId;
};

struct WindowCompositionAttribData {
    int attrib;
    PVOID pvData;
    SIZE_T cbData;
};

using SetWindowCompositionAttribute_t =
    BOOL(WINAPI*)(HWND hWnd, WindowCompositionAttribData* data);
SetWindowCompositionAttribute_t SetWindowCompositionAttribute_Original;

using GetWindowCompositionAttribute_t =
    BOOL(WINAPI*)(HWND hWnd, WindowCompositionAttribData* data);
GetWindowCompositionAttribute_t pGetWindowCompositionAttribute;

std::mutex g_savedAccentsMutex;
std::unordered_map<HWND, AccentPolicy> g_savedAccents;

bool SetAccent(HWND hWnd, AccentPolicy accent) {
    WindowCompositionAttribData data{kWcaAccentPolicy, &accent, sizeof(accent)};
    return SetWindowCompositionAttribute_Original &&
           SetWindowCompositionAttribute_Original(hWnd, &data);
}

void DisableExplorerAccent(HWND hWnd) {
    {
        std::lock_guard<std::mutex> guard(g_savedAccentsMutex);
        if (!g_savedAccents.contains(hWnd)) {
            AccentPolicy current{};
            WindowCompositionAttribData data{kWcaAccentPolicy, &current,
                                             sizeof(current)};
            if (pGetWindowCompositionAttribute &&
                pGetWindowCompositionAttribute(hWnd, &data)) {
                g_savedAccents[hWnd] = current;
                DebugLog(L"Saved accent %p: state %d color %08X", hWnd,
                         current.accentState, current.gradientColor);
            } else {
                DebugLog(L"Can't read accent %p: %u", hWnd, GetLastError());
            }
        }
    }

    bool disabled = SetAccent(hWnd, AccentPolicy{kAccentDisabled});
    DebugLog(L"Disable accent %p: %d", hWnd, disabled);
}

void RestoreExplorerAccent(HWND hWnd) {
    AccentPolicy saved;
    {
        std::lock_guard<std::mutex> guard(g_savedAccentsMutex);
        auto it = g_savedAccents.find(hWnd);
        if (it == g_savedAccents.end()) {
            return;
        }
        saved = it->second;
        g_savedAccents.erase(it);
    }

    SetAccent(hWnd, saved);
}

BOOL WINAPI SetWindowCompositionAttribute_Hook(HWND hWnd,
                                               WindowCompositionAttribData* data) {
    if (data && data->attrib == kWcaAccentPolicy && data->pvData &&
        data->cbData >= sizeof(AccentPolicy) && IsTaskbarWindow(hWnd) &&
        UseWindowsMaterial()) {
        {
            std::lock_guard<std::mutex> guard(g_savedAccentsMutex);
            g_savedAccents[hWnd] = *(AccentPolicy*)data->pvData;
        }

        AccentPolicy disabled{kAccentDisabled};
        WindowCompositionAttribData replaced{kWcaAccentPolicy, &disabled,
                                             sizeof(disabled)};
        return SetWindowCompositionAttribute_Original(hWnd, &replaced);
    }

    return SetWindowCompositionAttribute_Original(hWnd, data);
}

// DWM draws the translucent system backdrops only on active windows, and a
// flat color on inactive ones. The taskbar is never the active window, so it's
// told it is; this only affects the frame rendering.
void ShowBackdropAsActive(HWND hWnd) {
    DefWindowProc(hWnd, WM_NCACTIVATE, TRUE, 0);
}

// Windows deactivates the taskbar through paths that don't all reach the
// TrayUI window procedure, so a subclass sees every deactivation and undoes it.
constexpr UINT_PTR kActivationSubclassId = 0xD0C8;

LRESULT CALLBACK ActivationSubclassProc(HWND hWnd,
                                        UINT msg,
                                        WPARAM wParam,
                                        LPARAM lParam,
                                        UINT_PTR,
                                        DWORD_PTR) {
    LRESULT result = DefSubclassProc(hWnd, msg, wParam, lParam);

    if (msg == WM_NCACTIVATE && !wParam && UseWindowsMaterialFor(hWnd)) {
        DebugLog(L"Deactivated %p, shown as active again", hWnd);
        ShowBackdropAsActive(hWnd);
    }

    return result;
}

// Must run on the taskbar thread.
void WatchActivation(HWND hWnd, bool watch) {
    if (watch) {
        SetWindowSubclass(hWnd, ActivationSubclassProc, kActivationSubclassId, 0);
    } else {
        RemoveWindowSubclass(hWnd, ActivationSubclassProc,
                             kActivationSubclassId);
    }
}

// Gives the taskbar window the Windows 11 system backdrop and rounded corners,
// the same treatment DWM gives regular windows. Possible because the dock
// window really is only as wide as the dock.
void ApplyWindowMaterial(HWND hWnd) {
    bool native = UseWindowsMaterial();

    if (native) {
        DisableExplorerAccent(hWnd);
    } else {
        RestoreExplorerAccent(hWnd);
    }

    // Always rounded with a material; with the default background it's the
    // Rounded corners setting.
    bool rounded = native || (IsActive() && g_settings.roundedCorners);
    DWORD corner = rounded ? kDwmwcpRound : kDwmwcpDefault;

    DWORD backdrop = kDwmsbtAuto;
    if (native) {
        switch (g_settings.material) {
            case Material::acrylic:
                backdrop = kDwmsbtTransientWindow;
                break;
            case Material::mica:
                backdrop = kDwmsbtMainWindow;
                break;
            case Material::micaAlt:
                backdrop = kDwmsbtTabbedWindow;
                break;
            case Material::windowsDefault:
                break;
        }
    }

    BOOL dark = native && !IsTaskbarLightTheme();

    // The Windows 11 window border, or none: both are Windows options.
    DWORD borderColor =
        rounded && !g_settings.border ? kDwmColorNone : kDwmColorDefault;

    // The backdrop shows through the client area only when the frame is
    // extended over it, and the window is transparent to DWM.
    MARGINS margins = native ? MARGINS{-1, -1, -1, -1} : MARGINS{};

    HRGN emptyRegion = CreateRectRgn(0, 0, -1, -1);
    DWM_BLURBEHIND blurBehind{
        .dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION,
        .fEnable = native,
        .hRgnBlur = emptyRegion,
    };

    HRESULT hrCorner = DwmSetWindowAttribute(
        hWnd, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
    HRESULT hrDark = DwmSetWindowAttribute(hWnd, kDwmwaUseImmersiveDarkMode,
                                           &dark, sizeof(dark));
    HRESULT hrBorder = DwmSetWindowAttribute(hWnd, kDwmwaBorderColor,
                                             &borderColor, sizeof(borderColor));
    HRESULT hrBackdrop = DwmSetWindowAttribute(hWnd, kDwmwaSystemBackdropType,
                                               &backdrop, sizeof(backdrop));
    HRESULT hrFrame = DwmExtendFrameIntoClientArea(hWnd, &margins);
    HRESULT hrBlur = DwmEnableBlurBehindWindow(hWnd, &blurBehind);
    DeleteObject(emptyRegion);

    WatchActivation(hWnd, native);
    if (native) {
        ShowBackdropAsActive(hWnd);
    }

    DebugLog(L"Material %p (native=%d, backdrop %u): corner %08X dark %08X "
             L"border %08X backdrop %08X frame %08X blur %08X",
             hWnd, native, backdrop, hrCorner, hrDark, hrBorder, hrBackdrop,
             hrFrame, hrBlur);
}

FrameworkElement FindDescendantByName(FrameworkElement element,
                                      PCWSTR name,
                                      int depth = 6) {
    if (depth == 0) {
        return nullptr;
    }

    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (!child) {
            continue;
        }

        if (child.Name() == name) {
            return child;
        }

        if (auto found = FindDescendantByName(child, name, depth - 1)) {
            return found;
        }
    }

    return nullptr;
}

// With a Windows material the taskbar's own background is made invisible so
// the system backdrop shows through. Otherwise its value is cleared back to
// what the taskbar template sets.
void ApplyBackgroundStyle(FrameworkElement taskbarFrame) {
    bool hide = UseWindowsMaterial();

    for (PCWSTR name : {L"BackgroundFill", L"BackgroundStroke"}) {
        auto element = FindDescendantByName(taskbarFrame, name);
        if (!element) {
            continue;
        }

        if (hide) {
            if (element.Opacity() != 0) {
                element.Opacity(0);
            }
        } else if (element.ReadLocalValue(UIElement::OpacityProperty()) !=
                   DependencyProperty::UnsetValue()) {
            element.ClearValue(UIElement::OpacityProperty());
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Hidden icon menu
//
// Windows draws the menu's surface (OverflowFlyoutBackgroundBorder) with
// in-app Acrylic, which blurs what's behind it inside the menu: nothing, so it
// looks flat. With its setting on, a layer with the host backdrop (the desktop
// behind the window, provided by Windows) goes right below the surface, with
// its shape, so Windows' own Acrylic has the desktop to blur. The menu window
// is given the DWM permission the host backdrop needs
// (DWMWA_USE_HOSTBACKDROPBRUSH) and is shown as active, as the host backdrop
// is flat on inactive windows. The layer is in the menu's content, so Windows'
// animation moves it with the surface. Used only from the taskbar thread.

bool g_hiddenIconMenuStyled;
winrt::weak_ref<FrameworkElement> g_hiddenIconMenuContent;

constexpr WCHAR kHiddenIconMenuSurface[] = L"OverflowFlyoutBackgroundBorder";
constexpr DWORD kDwmwaUseHostBackdropBrush = 17;

// The light layer of the surface's Acrylic, the closest match to the taskbar's
// material over white, grays, black and colors (measured on screen against the
// taskbar, 2026-10-22): Windows' tint (light theme: 252,252,252) scaled to 203,
// at 0.8.
constexpr double kHiddenIconMenuTintLightness = 203.0 / 252.0;
constexpr double kHiddenIconMenuLuminosity = 0.8;

// The layer added below the surface, in the same panel.
winrt::weak_ref<Controls::Border> g_hiddenIconMenuBackdrop;

// The surface's background and border, with the values to put back.
winrt::weak_ref<Controls::Border> g_hiddenIconMenuSurface;
winrt::Windows::Foundation::IInspectable g_hiddenIconMenuSurfaceBackground{nullptr};
winrt::Windows::Foundation::IInspectable g_hiddenIconMenuSurfaceBorder{nullptr};

void RestoreSurfaceValue(Controls::Border surface,
                         DependencyProperty property,
                         winrt::Windows::Foundation::IInspectable saved) {
    if (!saved) {
        return;
    }
    if (saved == DependencyProperty::UnsetValue()) {
        surface.ClearValue(property);
    } else {
        surface.SetValue(property, saved);
    }
}

void RemoveMenuBackdrop() {
    if (auto surface = g_hiddenIconMenuSurface.get()) {
        RestoreSurfaceValue(surface, Controls::Border::BackgroundProperty(),
                            g_hiddenIconMenuSurfaceBackground);
        RestoreSurfaceValue(surface, Controls::Border::BorderBrushProperty(),
                            g_hiddenIconMenuSurfaceBorder);
    }
    g_hiddenIconMenuSurface = nullptr;
    g_hiddenIconMenuSurfaceBackground = nullptr;
    g_hiddenIconMenuSurfaceBorder = nullptr;

    if (auto backdrop = g_hiddenIconMenuBackdrop.get()) {
        if (auto panel = Media::VisualTreeHelper::GetParent(backdrop)
                             .try_as<Controls::Panel>()) {
            uint32_t index;
            if (panel.Children().IndexOf(backdrop, index)) {
                panel.Children().RemoveAt(index);
            }
        }
    }
    g_hiddenIconMenuBackdrop = nullptr;
}

// Returns false while the surface isn't there yet.
bool AddMenuBackdrop(FrameworkElement root) {
    auto surface = FindDescendantByName(root, kHiddenIconMenuSurface, 8)
                       .try_as<Controls::Border>();
    if (!surface || surface.ActualWidth() <= 0) {
        return false;
    }

    auto panel =
        Media::VisualTreeHelper::GetParent(surface).try_as<Controls::Panel>();
    if (!panel) {
        DebugLog(L"Hidden icon menu surface: no panel");
        return true;
    }

    // Same place and shape as the surface.
    Controls::Border backdrop;
    backdrop.Name(L"FluentExperienceMenuBackdrop");
    backdrop.CornerRadius(surface.CornerRadius());
    backdrop.Margin(surface.Margin());
    backdrop.HorizontalAlignment(surface.HorizontalAlignment());
    backdrop.VerticalAlignment(surface.VerticalAlignment());
    backdrop.Width(surface.Width());
    backdrop.Height(surface.Height());
    backdrop.IsHitTestVisible(false);
    Controls::Grid::SetRow(backdrop, Controls::Grid::GetRow(surface));
    Controls::Grid::SetColumn(backdrop, Controls::Grid::GetColumn(surface));
    Controls::Grid::SetRowSpan(backdrop, Controls::Grid::GetRowSpan(surface));
    Controls::Grid::SetColumnSpan(backdrop, Controls::Grid::GetColumnSpan(surface));

    // Added last, so Windows' elements keep their positions in the panel, and
    // drawn below the surface by its z-index.
    Controls::Canvas::SetZIndex(backdrop, Controls::Canvas::GetZIndex(surface) - 1);
    panel.Children().Append(backdrop);
    g_hiddenIconMenuBackdrop = winrt::make_weak(backdrop);

    // The host backdrop as the layer's own composition visual, with the
    // surface's rounded corners; it follows the layer's size.
    try {
        namespace wuc = winrt::Windows::UI::Composition;
        auto compositor =
            Hosting::ElementCompositionPreview::GetElementVisual(backdrop).Compositor();
        wuc::SpriteVisual visual = compositor.CreateSpriteVisual();
        visual.Brush(compositor.CreateHostBackdropBrush());

        float radius = (float)surface.CornerRadius().TopLeft;
        auto shape = compositor.CreateRoundedRectangleGeometry();
        shape.CornerRadius({radius, radius});
        visual.Clip(compositor.CreateGeometricClip(shape));

        auto resize = [visual, shape](float width, float height) {
            visual.Size({width, height});
            shape.Size({width, height});
        };
        resize((float)surface.ActualWidth(), (float)surface.ActualHeight());
        backdrop.SizeChanged([resize](const auto&, const SizeChangedEventArgs& args) {
            resize(args.NewSize().Width, args.NewSize().Height);
        });

        Hosting::ElementCompositionPreview::SetElementChildVisual(backdrop, visual);
    } catch (const winrt::hresult_error& ex) {
        DebugLog(L"Hidden icon menu backdrop: %08X %s", ex.code().value,
                 ex.message().c_str());
    }

    g_hiddenIconMenuSurface = winrt::make_weak(surface);

    // The thin border Windows draws around the menu goes with the taskbar's
    // "Hide window border" setting.
    if (!g_settings.border) {
        g_hiddenIconMenuSurfaceBorder =
            surface.ReadLocalValue(Controls::Border::BorderBrushProperty());
        surface.BorderBrush(
            Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    }

    // Windows' Acrylic stays on the surface, now with the desktop to blur, with
    // its light layer set to match the taskbar's material.
    if (auto acrylic = surface.Background().try_as<Media::AcrylicBrush>()) {
        auto tint = acrylic.TintColor();
        winrt::Windows::UI::Color light{
            tint.A,
            (BYTE)std::lround(tint.R * kHiddenIconMenuTintLightness),
            (BYTE)std::lround(tint.G * kHiddenIconMenuTintLightness),
            (BYTE)std::lround(tint.B * kHiddenIconMenuTintLightness),
        };

        Media::AcrylicBrush matched;
        matched.BackgroundSource(acrylic.BackgroundSource());
        matched.TintColor(light);
        matched.TintOpacity(0);
        matched.TintLuminosityOpacity(kHiddenIconMenuLuminosity);
        matched.FallbackColor(acrylic.FallbackColor());

        g_hiddenIconMenuSurfaceBackground =
            surface.ReadLocalValue(Controls::Border::BackgroundProperty());
        surface.Background(matched);
    }

    DebugLog(L"Hidden icon menu backdrop added");
    return true;
}

HWND FindHiddenIconMenuWindow() {
    HWND found = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            if (IsHiddenIconMenuWindow(hWnd)) {
                *(HWND*)lParam = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&found);
    return found;
}

void ApplyHiddenIconMenuStyle() {
    HWND hWnd = FindHiddenIconMenuWindow();
    auto content = g_hiddenIconMenuContent.get();
    bool style = StyleHiddenIconMenu();
    if (!hWnd || !content || style == g_hiddenIconMenuStyled) {
        return;
    }

    auto xamlRoot = content.XamlRoot();
    auto root = xamlRoot ? xamlRoot.Content().try_as<FrameworkElement>() : nullptr;

    if (style) {
        // The permission first, then the layer that needs it.
        BOOL allow = TRUE;
        HRESULT hr = DwmSetWindowAttribute(hWnd, kDwmwaUseHostBackdropBrush,
                                           &allow, sizeof(allow));
        DebugLog(L"Hidden icon menu %p: host backdrop allowed %08X", hWnd, hr);
        if (FAILED(hr) || !root || !AddMenuBackdrop(root)) {
            return;
        }
    } else {
        RemoveMenuBackdrop();
        BOOL allow = FALSE;
        DwmSetWindowAttribute(hWnd, kDwmwaUseHostBackdropBrush, &allow,
                              sizeof(allow));
    }

    WatchActivation(hWnd, style);
    DefWindowProc(hWnd, WM_NCACTIVATE, style, 0);

    g_hiddenIconMenuStyled = style;
    DebugLog(L"Hidden icon menu %p: styled %d", hWnd, style);
}

HWND FindCurrentProcessTaskbarWnd();

// The menu's content is measured when the menu is created: its window exists
// then. The style is applied after the layout pass, from the taskbar thread's
// message.
using NotificationAreaOverflow_MeasureOverride_t =
    int(WINAPI*)(void* pThis,
                 winrt::Windows::Foundation::Size size,
                 winrt::Windows::Foundation::Size* resultSize);
NotificationAreaOverflow_MeasureOverride_t
    NotificationAreaOverflow_MeasureOverride_Original;
int WINAPI NotificationAreaOverflow_MeasureOverride_Hook(
    void* pThis,
    winrt::Windows::Foundation::Size size,
    winrt::Windows::Foundation::Size* resultSize) {
    g_hookCallCounter++;

    int ret = NotificationAreaOverflow_MeasureOverride_Original(pThis, size,
                                                                resultSize);

    // The function may be shared with other elements: the class is checked.
    try {
        FrameworkElement content = nullptr;
        ((IUnknown*)pThis)
            ->QueryInterface(winrt::guid_of<FrameworkElement>(),
                             winrt::put_abi(content));
        if (content &&
            winrt::get_class_name(content) == L"SystemTray.NotificationAreaOverflow") {
            g_hiddenIconMenuContent = winrt::make_weak(content);
            if (StyleHiddenIconMenu() != g_hiddenIconMenuStyled) {
                if (HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd()) {
                    PostMessage(hTaskbarWnd, g_applyStyleMsg, 1, 0);
                }
            }
        }
    } catch (const winrt::hresult_error& ex) {
        DebugLog(L"Error %08X: %s", ex.code().value, ex.message().c_str());
    }

    g_hookCallCounter--;

    return ret;
}

bool HookSystemTrayDllSymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::NotificationAreaOverflow,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::MeasureOverride(struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))"},
            &NotificationAreaOverflow_MeasureOverride_Original,
            NotificationAreaOverflow_MeasureOverride_Hook,
        },
    };

    return WindhawkUtils::HookSymbols(module, symbolHooks,
                                      ARRAYSIZE(symbolHooks));
}

////////////////////////////////////////////////////////////////////////////////
// Update state, set by the update checker (see Updates) and shown by the
// settings button and window.

enum class UpdateState {
    notChecked,
    checking,
    upToDate,
    available,
    checkFailed,
    installing,
};

struct UpdateInfo {
    UpdateState state = UpdateState::notChecked;
    // The latest release, without the leading "v" of its tag.
    std::wstring version;
    std::wstring dllUrl;
    std::wstring sourceUrl;
    // Local time of the last check; wYear is 0 before the first one.
    SYSTEMTIME lastChecked{};
};

std::mutex g_updateMutex;
UpdateInfo g_update;

// Set to ask the update thread for a check or an install.
HANDLE g_updateCheckEvent;
HANDLE g_updateInstallEvent;

UpdateInfo GetUpdateInfo() {
    std::lock_guard<std::mutex> guard(g_updateMutex);
    return g_update;
}

bool IsUpdateAvailable() {
    return GetUpdateInfo().state == UpdateState::available;
}

////////////////////////////////////////////////////////////////////////////////
// Settings button

// Size of the settings button, in DIPs.
constexpr double kSettingsButtonSize = 40;

// A taskbar frame seen so far, with what was added to it. Used only from the
// taskbar thread.
struct TrackedFrame {
    winrt::weak_ref<FrameworkElement> frame;
    Controls::Button button{nullptr};
    winrt::event_token clickToken{};
    // The line under the button's icon while an update is available.
    Controls::Border attentionIndicator{nullptr};
    // Failed attempts to add the button, e.g. while the clock area is still
    // being built. Retried a few times, not on every layout pass.
    int addFailures = 0;
    // The clock area panel the button was put in and, when it's a grid, the
    // column added for it.
    Controls::Panel trayPanel{nullptr};
    Controls::ColumnDefinition column{nullptr};
    int columnIndex = -1;
    // The clock area's size changes (tray icons coming and going, the button)
    // don't re-measure the taskbar frame, so the dock width is updated here.
    FrameworkElement trayFrame{nullptr};
    winrt::event_token traySizeChangedToken{};
    // The icons, moved right so Start is as far from the dock's left edge as
    // from its top and bottom edges.
    FrameworkElement repeater{nullptr};
    winrt::Windows::Foundation::IInspectable savedRepeaterMargin{nullptr};
};

// Extra space before Start, in DIPs: the dock is that much wider, and the gap
// left of the Start logo matches the gap above and below it.
constexpr double kStartInset = 2;

void InsetStartButton(TrackedFrame& tracked, FrameworkElement taskbarFrame) {
    if (tracked.repeater) {
        return;
    }

    auto rootGrid = FindChildByName(taskbarFrame, L"RootGrid");
    auto repeater =
        rootGrid ? FindChildByName(rootGrid, L"TaskbarFrameRepeater") : nullptr;
    if (!repeater) {
        return;
    }

    tracked.savedRepeaterMargin =
        repeater.ReadLocalValue(FrameworkElement::MarginProperty());
    Thickness margin = repeater.Margin();
    margin.Left += kStartInset;
    repeater.Margin(margin);
    tracked.repeater = repeater;
}

void RestoreStartButton(TrackedFrame& tracked) {
    if (!tracked.repeater) {
        return;
    }

    if (tracked.savedRepeaterMargin == DependencyProperty::UnsetValue()) {
        tracked.repeater.ClearValue(FrameworkElement::MarginProperty());
    } else {
        tracked.repeater.SetValue(FrameworkElement::MarginProperty(),
                                  tracked.savedRepeaterMargin);
    }

    tracked.repeater = nullptr;
    tracked.savedRepeaterMargin = nullptr;
}

std::vector<TrackedFrame> g_taskbarFrames;
std::atomic<bool> g_frameCustomizationPending;

bool ShowSettingsButton() {
    return IsActive() && g_settings.settingsButton;
}

// Glyph of the settings button and window: Equalizer.
constexpr WCHAR kSettingsGlyph[] = L"\uE9E9";

// While an update is available the button looks like a taskbar app that needs
// attention: a tinted background and a line under the icon.
constexpr double kAttentionIndicatorWidth = 16;
constexpr double kAttentionIndicatorHeight = 3;
constexpr winrt::Windows::UI::Color kAttentionIndicatorColor{255, 0xD2, 0x47, 0x26};
constexpr winrt::Windows::UI::Color kAttentionBackgroundColor{115, 0xFF, 0x8C, 0x5A};

constexpr WCHAR kSettingsButtonToolTip[] = L"Fluent Experience Settings";
constexpr WCHAR kSettingsButtonUpdateToolTip[] =
    L"Fluent Experience Settings - Update available";

void ShowUpdateAttention(TrackedFrame& tracked) {
    if (!tracked.button) {
        return;
    }

    bool attention = IsUpdateAvailable();
    tracked.attentionIndicator.Visibility(attention ? Visibility::Visible
                                                    : Visibility::Collapsed);
    tracked.button.Background(Media::SolidColorBrush(
        attention ? kAttentionBackgroundColor
                  : winrt::Windows::UI::Colors::Transparent()));
    Controls::ToolTipService::SetToolTip(
        tracked.button,
        winrt::box_value(attention ? kSettingsButtonUpdateToolTip
                                   : kSettingsButtonToolTip));
}

void OpenSettingsWindow();

FrameworkElement GetTrayFrame(FrameworkElement taskbarFrame) {
    auto xamlRoot = taskbarFrame.XamlRoot();
    auto content =
        xamlRoot ? xamlRoot.Content().try_as<FrameworkElement>() : nullptr;
    return content ? FindChildByClassName(content, L"SystemTray.SystemTrayFrame")
                   : nullptr;
}

void LogElementTree(FrameworkElement element, int depth = 0) {
    if (depth > 4) {
        return;
    }

    EnumChildElements(element, [depth](FrameworkElement child) {
        DebugLog(L"  %*s%s #%s column %d", depth * 2, L"",
                 winrt::get_class_name(child).c_str(), child.Name().c_str(),
                 Controls::Grid::GetColumn(child));
        LogElementTree(child, depth + 1);
        return false;
    });
}

constexpr int kMaxSettingsButtonAddFailures = 5;

// Shifts the grid children from a column on by delta columns.
void ShiftGridColumns(Controls::Grid grid, int fromColumn, int delta) {
    EnumChildElements(grid, [fromColumn, delta](FrameworkElement child) {
        int column = Controls::Grid::GetColumn(child);
        if (column >= fromColumn) {
            Controls::Grid::SetColumn(child, column + delta);
        }
        return false;
    });
}

// The button goes in the clock area right before the clock
// (NotificationCenterButton): after the tray icons on the primary taskbar, and
// right before the clock on the others. The clock area then includes the
// button in its own width. The area is a horizontal StackPanel on current
// builds, a grid with a column per element on others.
void AddSettingsButton(TrackedFrame& tracked, FrameworkElement taskbarFrame) {
    auto trayFrame = GetTrayFrame(taskbarFrame);
    auto trayPanel =
        trayFrame ? FindDescendantByName(trayFrame, L"SystemTrayFrameGrid")
                        .try_as<Controls::Panel>()
                  : nullptr;
    auto clock = trayPanel
                     ? FindChildByName(trayPanel, L"NotificationCenterButton")
                     : nullptr;
    uint32_t clockIndex;

    if (!clock || !trayPanel.Children().IndexOf(clock, clockIndex)) {
        // Logged once, with the clock area's tree to see what changed.
        if (++tracked.addFailures == kMaxSettingsButtonAddFailures) {
            DebugLog(L"Settings button: clock not found");
            if (trayFrame) {
                LogElementTree(trayFrame);
            }
        }
        return;
    }

    Controls::ColumnDefinition column{nullptr};
    int columnIndex = -1;
    auto trayGrid = trayPanel.try_as<Controls::Grid>();
    if (trayGrid && trayGrid.ColumnDefinitions().Size() > 0) {
        columnIndex = Controls::Grid::GetColumn(clock);
        ShiftGridColumns(trayGrid, columnIndex, 1);
        column = Controls::ColumnDefinition();
        column.Width(GridLengthHelper::Auto());
        trayGrid.ColumnDefinitions().InsertAt(columnIndex, column);
    }

    Controls::FontIcon icon;
    icon.Glyph(kSettingsGlyph);
    icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
    icon.FontSize(16);
    icon.HorizontalAlignment(HorizontalAlignment::Center);
    icon.VerticalAlignment(VerticalAlignment::Center);

    Controls::Border attentionIndicator;
    attentionIndicator.Width(kAttentionIndicatorWidth);
    attentionIndicator.Height(kAttentionIndicatorHeight);
    attentionIndicator.CornerRadius(CornerRadius{1.5, 1.5, 1.5, 1.5});
    attentionIndicator.Background(Media::SolidColorBrush(kAttentionIndicatorColor));
    attentionIndicator.HorizontalAlignment(HorizontalAlignment::Center);
    attentionIndicator.VerticalAlignment(VerticalAlignment::Bottom);
    attentionIndicator.Margin(Thickness{0, 0, 0, 2});
    attentionIndicator.Visibility(Visibility::Collapsed);

    Controls::Grid content;
    content.Width(kSettingsButtonSize);
    content.Height(kSettingsButtonSize);
    content.Children().Append(icon);
    content.Children().Append(attentionIndicator);

    Controls::Button button;
    button.Name(L"FluentExperienceSettingsButton");
    button.Content(content);
    button.Width(kSettingsButtonSize);
    button.Height(kSettingsButtonSize);
    button.Padding(Thickness{});
    button.BorderThickness(Thickness{});
    button.CornerRadius(CornerRadius{4, 4, 4, 4});
    button.Background(Media::SolidColorBrush(
        winrt::Windows::UI::Colors::Transparent()));
    button.VerticalAlignment(VerticalAlignment::Center);
    Controls::ToolTipService::SetToolTip(button,
                                         winrt::box_value(kSettingsButtonToolTip));
    if (column) {
        Controls::Grid::SetColumn(button, columnIndex);
    }

    tracked.clickToken =
        button.Click([](const auto&, const auto&) { OpenSettingsWindow(); });
    trayPanel.Children().InsertAt(clockIndex, button);

    tracked.button = button;
    tracked.attentionIndicator = attentionIndicator;
    tracked.trayPanel = trayPanel;
    tracked.column = column;
    tracked.columnIndex = columnIndex;

    DebugLog(L"Settings button added at %u", clockIndex);
}

void RemoveSettingsButton(TrackedFrame& tracked) {
    if (!tracked.button) {
        return;
    }

    tracked.button.Click(tracked.clickToken);

    if (auto trayPanel = tracked.trayPanel) {
        uint32_t index;
        if (trayPanel.Children().IndexOf(tracked.button, index)) {
            trayPanel.Children().RemoveAt(index);
        }

        auto trayGrid = trayPanel.try_as<Controls::Grid>();
        if (trayGrid && tracked.column) {
            if (trayGrid.ColumnDefinitions().IndexOf(tracked.column, index)) {
                trayGrid.ColumnDefinitions().RemoveAt(index);
            }
            ShiftGridColumns(trayGrid, tracked.columnIndex + 1, -1);
        }
    }

    tracked.button = nullptr;
    tracked.clickToken = {};
    tracked.attentionIndicator = nullptr;
    tracked.trayPanel = nullptr;
    tracked.column = nullptr;
    tracked.columnIndex = -1;

    DebugLog(L"Settings button removed");
}

void WatchTraySize(TrackedFrame& tracked, FrameworkElement taskbarFrame) {
    if (tracked.trayFrame) {
        return;
    }

    auto trayFrame = GetTrayFrame(taskbarFrame);
    if (!trayFrame) {
        return;
    }

    auto weakFrame = tracked.frame;
    tracked.traySizeChangedToken = trayFrame.SizeChanged(
        [weakFrame](const auto&, const SizeChangedEventArgs& args) {
            if (args.PreviousSize().Width != args.NewSize().Width) {
                if (auto frame = weakFrame.get()) {
                    frame.InvalidateMeasure();
                }
            }
        });
    tracked.trayFrame = trayFrame;
}

void UnwatchTraySize(TrackedFrame& tracked) {
    if (tracked.trayFrame) {
        tracked.trayFrame.SizeChanged(tracked.traySizeChangedToken);
        tracked.trayFrame = nullptr;
        tracked.traySizeChangedToken = {};
    }
}

HWND FindCurrentProcessTaskbarWnd();

// Called during layout: the tree can't be changed there, so adding or removing
// the button is queued to the taskbar thread.
void TrackTaskbarFrame(FrameworkElement taskbarFrame) {
    TrackedFrame* tracked = nullptr;
    for (auto& entry : g_taskbarFrames) {
        if (entry.frame.get() == taskbarFrame) {
            tracked = &entry;
            break;
        }
    }

    if (!tracked) {
        tracked = &g_taskbarFrames.emplace_back();
        tracked->frame = winrt::make_weak(taskbarFrame);
    }

    bool needsUpdate =
        (tracked->button
             ? !ShowSettingsButton()
             : ShowSettingsButton() &&
                   tracked->addFailures < kMaxSettingsButtonAddFailures) ||
        IsActive() != (bool)tracked->trayFrame ||
        IsActive() != (bool)tracked->repeater;

    if (needsUpdate && !g_frameCustomizationPending.exchange(true)) {
        if (HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd()) {
            PostMessage(hTaskbarWnd, g_applyStyleMsg, 1, 0);
        } else {
            g_frameCustomizationPending = false;
        }
    }
}

void ApplyCustomizationsToAllFrames() {
    g_frameCustomizationPending = false;

    std::erase_if(g_taskbarFrames,
                  [](const auto& entry) { return !entry.frame.get(); });

    for (auto& tracked : g_taskbarFrames) {
        auto taskbarFrame = tracked.frame.get();
        if (!taskbarFrame) {
            continue;
        }

        try {
            ApplyBackgroundStyle(taskbarFrame);

            if (ShowSettingsButton() && !tracked.button) {
                AddSettingsButton(tracked, taskbarFrame);
            } else if (!ShowSettingsButton() && tracked.button) {
                RemoveSettingsButton(tracked);
            }
            ShowUpdateAttention(tracked);

            if (IsActive()) {
                WatchTraySize(tracked, taskbarFrame);
                InsetStartButton(tracked, taskbarFrame);
            } else {
                UnwatchTraySize(tracked);
                RestoreStartButton(tracked);
            }
        } catch (const winrt::hresult_error& ex) {
            DebugLog(L"Error %08X: %s", ex.code().value, ex.message().c_str());
        }
    }

    try {
        ApplyHiddenIconMenuStyle();
    } catch (const winrt::hresult_error& ex) {
        DebugLog(L"Error %08X: %s", ex.code().value, ex.message().c_str());
    }
}

// The monitor index, added to the width: two taskbars with the same icons stay
// distinguishable by the width of their XAML island.
int GetMonitorIndex(HMONITOR monitor) {
    struct Param {
        HMONITOR target;
        int index;
        int result;
    } param{monitor, 0, 0};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto& param = *reinterpret_cast<Param*>(lParam);
            if (hMonitor == param.target) {
                param.result = param.index;
                return FALSE;
            }
            param.index++;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&param));

    return param.result;
}

// The taskbar a XAML island belongs to, matched by width: the island is as
// wide as the taskbar window.
HWND FindTaskbarWindowByIslandWidth(double islandWidth) {
    struct Param {
        double islandWidth;
        HWND found;
        int matches;
    } param{islandWidth, nullptr, 0};

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto& param = *reinterpret_cast<Param*>(lParam);
            DWORD processId;
            if (!GetWindowThreadProcessId(hWnd, &processId) ||
                processId != GetCurrentProcessId() || !IsTaskbarWindow(hWnd)) {
                return TRUE;
            }

            RECT rc;
            GetClientRect(hWnd, &rc);
            double width = (rc.right - rc.left) * 96.0 / GetDpiForWindow(hWnd);
            if (std::abs(width - param.islandWidth) < 0.75) {
                param.found = hWnd;
                param.matches++;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&param));

    return param.matches == 1 ? param.found : nullptr;
}

void UpdateDockWidthFromFrame(FrameworkElement taskbarFrame) {
    auto xamlRoot = taskbarFrame.XamlRoot();
    if (!xamlRoot) {
        return;
    }

    FrameworkElement repeater = nullptr;
    if (auto rootGrid = FindChildByName(taskbarFrame, L"RootGrid")) {
        repeater = FindChildByName(rootGrid, L"TaskbarFrameRepeater");
    }
    if (!repeater) {
        DebugLog(L"TaskbarFrameRepeater not found");
        return;
    }

    double iconsWidth = repeater.DesiredSize().Width;

    // Includes the settings button, which sits in the clock area.
    double trayWidth = 0;
    if (auto trayFrame = GetTrayFrame(taskbarFrame)) {
        trayWidth = trayFrame.DesiredSize().Width;
    }

    double islandWidth = xamlRoot.Size().Width;
    double neededWidth = iconsWidth + trayWidth + g_settings.padding;

    HWND hWnd = FindTaskbarWindowByIslandWidth(islandWidth);

    DebugLog(L"Measure: island=%.1f icons=%.1f tray=%.1f -> %.1f (hwnd %p)",
             islandWidth, iconsWidth, trayWidth, neededWidth, hWnd);

    if (hWnd) {
        PostMessage(hWnd, g_dockWidthMsg,
                    (WPARAM)std::lround(neededWidth * 100), 0);
    }
}

// Moves the taskbar window to the dock rect for the current width.
void MoveToDockRect(HWND hWnd) {
    RECT rc;
    GetWindowRect(hWnd, &rc);
    NarrowBottomTaskbarRect(&rc);

    SetWindowPos(hWnd, nullptr, rc.left, rc.top, rc.right - rc.left,
                 rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

// "Animation effects" in the Windows settings (Accessibility > Visual effects).
bool AreWindowsAnimationsOn() {
    BOOL on = TRUE;
    SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on;
}

void StepWidthAnimation(HWND hWnd);

void CALLBACK WidthAnimationTimerProc(HWND hWnd, UINT, UINT_PTR, DWORD) {
    StepWidthAnimation(hWnd);
}

// Applies the measured width (in DIPs * 100) to the taskbar window.
void ApplyDockWidth(HWND hWnd, WPARAM widthDip100) {
    if (!IsActive() || !g_settings.dynamicWidth) {
        return;
    }

    HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    int width = (int)std::ceil(widthDip100 / 100.0 * GetDpiForWindow(hWnd) /
                               96.0) +
                GetMonitorIndex(monitor);

    bool animate;
    bool firstMeasurement;
    {
        std::lock_guard<std::mutex> guard(g_dockWidthsMutex);
        auto& dockWidth = g_dockWidths[monitor];
        if (dockWidth.target == width) {
            return;
        }

        // The first measurement is applied at once.
        firstMeasurement = !dockWidth.current;
        animate = AreWindowsAnimationsOn() && !firstMeasurement;
        dockWidth.target = width;
        if (animate) {
            dockWidth.from = dockWidth.current;
            dockWidth.startTick = GetTickCount64();
        } else {
            dockWidth.current = width;
        }
    }

    DebugLog(L"New width %d px for %p", width, hWnd);

    // A new taskbar (e.g. on a connected monitor) gets its style here.
    if (firstMeasurement) {
        ApplyWindowMaterial(hWnd);
    }

    // The timer calls back directly: on the primary taskbar Explorer handles
    // WM_TIMER before its window procedure is reached.
    if (animate) {
        SetTimer(hWnd, kWidthAnimationTimerId, 10, WidthAnimationTimerProc);
    } else {
        MoveToDockRect(hWnd);
    }
}

// One frame of the width animation, with an ease-out cubic curve.
void StepWidthAnimation(HWND hWnd) {
    HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);

    bool done = true;
    {
        std::lock_guard<std::mutex> guard(g_dockWidthsMutex);
        auto it = g_dockWidths.find(monitor);
        if (it != g_dockWidths.end()) {
            auto& dockWidth = it->second;
            double t = (double)(GetTickCount64() - dockWidth.startTick) /
                       g_settings.animationDuration;
            if (t < 1 && IsActive()) {
                double eased = 1 - std::pow(1 - t, 3);
                dockWidth.current =
                    dockWidth.from +
                    (int)std::lround((dockWidth.target - dockWidth.from) *
                                     eased);
                done = false;
            } else {
                dockWidth.current = dockWidth.target;
            }
        }
    }

    if (done) {
        KillTimer(hWnd, kWidthAnimationTimerId);
    }

    if (IsActive()) {
        MoveToDockRect(hWnd);
    }
}

using TaskbarFrame_MeasureOverride_t =
    int(WINAPI*)(void* pThis,
                 winrt::Windows::Foundation::Size size,
                 winrt::Windows::Foundation::Size* resultSize);
TaskbarFrame_MeasureOverride_t TaskbarFrame_MeasureOverride_Original;
int WINAPI TaskbarFrame_MeasureOverride_Hook(
    void* pThis,
    winrt::Windows::Foundation::Size size,
    winrt::Windows::Foundation::Size* resultSize) {
    g_hookCallCounter++;

    bool measureNatural = IsActive() && g_settings.dynamicWidth;

    // Measure with plenty of room, so the icons report their natural width
    // instead of the one squeezed into the current space.
    auto measureSize = size;
    if (measureNatural) {
        measureSize.Width = std::max(size.Width, 10000.0f);
    }

    int ret =
        TaskbarFrame_MeasureOverride_Original(pThis, measureSize, resultSize);

    if (measureNatural && resultSize->Width > size.Width) {
        resultSize->Width = size.Width;
    }

    if (IsActive()) {
        try {
            FrameworkElement taskbarFrame = nullptr;
            ((IUnknown*)pThis)
                ->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                 winrt::put_abi(taskbarFrame));
            if (taskbarFrame) {
                TrackTaskbarFrame(taskbarFrame);
                ApplyBackgroundStyle(taskbarFrame);

                if (measureNatural) {
                    UpdateDockWidthFromFrame(taskbarFrame);
                }
            }
        } catch (const winrt::hresult_error& ex) {
            DebugLog(L"Error %08X: %s", ex.code().value, ex.message().c_str());
        }
    }

    g_hookCallCounter--;

    return ret;
}

bool HookTaskbarViewDllSymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::MeasureOverride(struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))"},
            &TaskbarFrame_MeasureOverride_Original,
            TaskbarFrame_MeasureOverride_Hook,
        },
    };

    return WindhawkUtils::HookSymbols(module, symbolHooks,
                                      ARRAYSIZE(symbolHooks));
}

HMODULE GetTaskbarViewModuleHandle() {
    HMODULE module = GetModuleHandle(L"Taskbar.View.dll");
    if (!module) {
        module = GetModuleHandle(L"ExplorerExtensions.dll");
    }

    return module;
}

void HandleLoadedModuleIfTaskbarView(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_taskbarViewDllHooked && GetTaskbarViewModuleHandle() == module &&
        !g_taskbarViewDllHooked.exchange(true)) {
        DebugLog(L"Loaded %s", lpLibFileName);

        if (HookTaskbarViewDllSymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

std::atomic<bool> g_systemTrayDllHooked;

// SystemTray.dll holds the hidden icon menu.
void HandleLoadedModuleIfSystemTray(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_systemTrayDllHooked && GetModuleHandle(L"SystemTray.dll") == module &&
        !g_systemTrayDllHooked.exchange(true)) {
        DebugLog(L"Loaded %s", lpLibFileName);

        if (HookSystemTrayDllSymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModuleIfTaskbarView(module, lpLibFileName);
        HandleLoadedModuleIfSystemTray(module, lpLibFileName);
    }

    return module;
}

////////////////////////////////////////////////////////////////////////////////
// Taskbar geometry

using TrayUI__StuckTrayChange_t = void(WINAPI*)(void* pThis);
TrayUI__StuckTrayChange_t TrayUI__StuckTrayChange_Original;

using TrayUI__HandleSettingChange_t = void(WINAPI*)(void* pThis,
                                                    void* param1,
                                                    void* param2,
                                                    void* param3,
                                                    void* param4);
TrayUI__HandleSettingChange_t TrayUI__HandleSettingChange_Original;
void WINAPI TrayUI__HandleSettingChange_Hook(void* pThis,
                                             void* param1,
                                             void* param2,
                                             void* param3,
                                             void* param4) {
    Wh_Log(L">");

    TrayUI__HandleSettingChange_Original(pThis, param1, param2, param3, param4);

    if (g_applyingSettings && TrayUI__StuckTrayChange_Original) {
        TrayUI__StuckTrayChange_Original(pThis);
    }
}

using TrayUI_MakeStuckRect_t = void(WINAPI*)(void* pThis,
                                             RECT* rect,
                                             RECT* param2,
                                             SIZE param3,
                                             DWORD taskbarPos);
TrayUI_MakeStuckRect_t TrayUI_MakeStuckRect_Original;
void WINAPI TrayUI_MakeStuckRect_Hook(void* pThis,
                                      RECT* rect,
                                      RECT* param2,
                                      SIZE param3,
                                      DWORD taskbarPos) {
    TrayUI_MakeStuckRect_Original(pThis, rect, param2, param3, taskbarPos);

    if (IsActive() && taskbarPos == ABE_BOTTOM) {
        NarrowBottomTaskbarRect(rect);
    }
}

using TrayUI_GetDockedRect_t = DWORD(WINAPI*)(void* pThis,
                                              RECT* rect,
                                              BOOL param2);
TrayUI_GetDockedRect_t TrayUI_GetDockedRect_Original;
DWORD WINAPI TrayUI_GetDockedRect_Hook(void* pThis, RECT* rect, BOOL param2) {
    DWORD ret = TrayUI_GetDockedRect_Original(pThis, rect, param2);

    if (IsActive()) {
        NarrowBottomTaskbarRect(rect);
    }

    return ret;
}

using CSecondaryTray__ComputeHiddenRect_t = void(WINAPI*)(void* pThis,
                                                          RECT* rect);
CSecondaryTray__ComputeHiddenRect_t CSecondaryTray__ComputeHiddenRect_Original;
void WINAPI CSecondaryTray__ComputeHiddenRect_Hook(void* pThis, RECT* rect) {
    CSecondaryTray__ComputeHiddenRect_Original(pThis, rect);

    if (IsActive()) {
        NarrowBottomTaskbarRect(rect);
    }
}

// Last line of defense: any move or resize of the taskbar window is brought back
// to the dock's width before Windows applies it.
void TaskbarWndProcPostProcess(HWND hWnd, UINT Msg, LPARAM lParam) {
    if (Msg != WM_WINDOWPOSCHANGING || !IsActive()) {
        return;
    }

    auto* windowpos = (WINDOWPOS*)lParam;
    if ((windowpos->flags & (SWP_NOSIZE | SWP_NOMOVE)) ==
        (SWP_NOSIZE | SWP_NOMOVE)) {
        return;
    }

    RECT currentRect;
    GetWindowRect(hWnd, &currentRect);

    RECT rc;
    rc.left = (windowpos->flags & SWP_NOMOVE) ? currentRect.left : windowpos->x;
    rc.top = (windowpos->flags & SWP_NOMOVE) ? currentRect.top : windowpos->y;
    int cx = (windowpos->flags & SWP_NOSIZE)
                 ? currentRect.right - currentRect.left
                 : windowpos->cx;
    int cy = (windowpos->flags & SWP_NOSIZE)
                 ? currentRect.bottom - currentRect.top
                 : windowpos->cy;
    rc.right = rc.left + cx;
    rc.bottom = rc.top + cy;

    RECT narrowed = rc;
    NarrowBottomTaskbarRect(&narrowed);
    if (EqualRect(&narrowed, &rc)) {
        return;
    }

    windowpos->x = narrowed.left;
    windowpos->y = narrowed.top;
    windowpos->cx = narrowed.right - narrowed.left;
    windowpos->cy = narrowed.bottom - narrowed.top;
    windowpos->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
}

// Messages of our own sent to the taskbar windows. Returns true if handled.
bool HandleDockMessage(HWND hWnd, UINT Msg, WPARAM wParam) {
    if (Msg == g_dockWidthMsg) {
        ApplyDockWidth(hWnd, wParam);
        return true;
    }

    // wParam 1: only the XAML customizations, queued from layout.
    if (Msg == g_applyStyleMsg) {
        if (wParam != 1) {
            ApplyWindowMaterial(hWnd);
        }
        ApplyCustomizationsToAllFrames();

        // The timer calls into the mod, so it can't outlive it.
        if (!IsActive()) {
            KillTimer(hWnd, kWidthAnimationTimerId);
        }
        return true;
    }

    return false;
}

// The dark mode attribute follows the taskbar theme.
void HandleThemeChange(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (Msg == WM_SETTINGCHANGE && lParam && UseWindowsMaterial() &&
        wcscmp((PCWSTR)lParam, L"ImmersiveColorSet") == 0) {
        ApplyWindowMaterial(hWnd);
    }
}

using TrayUI_WndProc_t = LRESULT(WINAPI*)(void* pThis,
                                          HWND hWnd,
                                          UINT Msg,
                                          WPARAM wParam,
                                          LPARAM lParam,
                                          bool* flag);
TrayUI_WndProc_t TrayUI_WndProc_Original;
LRESULT WINAPI TrayUI_WndProc_Hook(void* pThis,
                                   HWND hWnd,
                                   UINT Msg,
                                   WPARAM wParam,
                                   LPARAM lParam,
                                   bool* flag) {
    if (HandleDockMessage(hWnd, Msg, wParam)) {
        return 0;
    }

    g_hookCallCounter++;

    LRESULT ret =
        TrayUI_WndProc_Original(pThis, hWnd, Msg, wParam, lParam, flag);

    TaskbarWndProcPostProcess(hWnd, Msg, lParam);
    HandleThemeChange(hWnd, Msg, wParam, lParam);

    g_hookCallCounter--;

    return ret;
}

using CSecondaryTray_v_WndProc_t = LRESULT(
    WINAPI*)(void* pThis, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
CSecondaryTray_v_WndProc_t CSecondaryTray_v_WndProc_Original;
LRESULT WINAPI CSecondaryTray_v_WndProc_Hook(void* pThis,
                                             HWND hWnd,
                                             UINT Msg,
                                             WPARAM wParam,
                                             LPARAM lParam) {
    if (HandleDockMessage(hWnd, Msg, wParam)) {
        return 0;
    }

    g_hookCallCounter++;

    LRESULT ret =
        CSecondaryTray_v_WndProc_Original(pThis, hWnd, Msg, wParam, lParam);

    TaskbarWndProcPostProcess(hWnd, Msg, lParam);
    HandleThemeChange(hWnd, Msg, wParam, lParam);

    g_hookCallCounter--;

    return ret;
}

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Failed to load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {
            {LR"(public: void __cdecl TrayUI::_StuckTrayChange(void))"},
            &TrayUI__StuckTrayChange_Original,
        },
        {
            {LR"(public: void __cdecl TrayUI::_HandleSettingChange(struct HWND__ *,unsigned int,unsigned __int64,__int64))"},
            &TrayUI__HandleSettingChange_Original,
            TrayUI__HandleSettingChange_Hook,
        },
        {
            {LR"(public: virtual unsigned int __cdecl TrayUI::GetDockedRect(struct tagRECT *,int))"},
            &TrayUI_GetDockedRect_Original,
            TrayUI_GetDockedRect_Hook,
        },
        {
            {LR"(public: virtual void __cdecl TrayUI::MakeStuckRect(struct tagRECT *,struct tagRECT const *,struct tagSIZE,unsigned int))"},
            &TrayUI_MakeStuckRect_Original,
            TrayUI_MakeStuckRect_Hook,
        },
        {
            {LR"(public: virtual __int64 __cdecl TrayUI::WndProc(struct HWND__ *,unsigned int,unsigned __int64,__int64,bool *))"},
            &TrayUI_WndProc_Original,
            TrayUI_WndProc_Hook,
        },
        {
            {LR"(private: virtual __int64 __cdecl CSecondaryTray::v_WndProc(struct HWND__ *,unsigned int,unsigned __int64,__int64))"},
            &CSecondaryTray_v_WndProc_Original,
            CSecondaryTray_v_WndProc_Hook,
        },
        {
            {LR"(private: void __cdecl CSecondaryTray::_ComputeHiddenRect(struct tagRECT *))"},
            &CSecondaryTray__ComputeHiddenRect_Original,
            CSecondaryTray__ComputeHiddenRect_Hook,
            true,  // Only needed for auto-hide on secondary monitors.
        },
    };

    return WindhawkUtils::HookSymbols(module, taskbarDllHooks,
                                      ARRAYSIZE(taskbarDllHooks));
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

// Makes Explorer recompute the geometry of all taskbars.
void ApplySettings() {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        return;
    }

    g_applyingSettings = true;

    // Triggers TrayUI::_HandleSettingChange, which in turn calls
    // TrayUI::_StuckTrayChange.
    SendMessage(hTaskbarWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);

    EnumThreadWindows(
        GetWindowThreadProcessId(hTaskbarWnd, nullptr),
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            if (IsTaskbarWindow(hWnd)) {
                // Secondary taskbars reposition themselves on every setting
                // change.
                if (hWnd != (HWND)lParam) {
                    SendMessage(hWnd, WM_SETTINGCHANGE,
                                SPI_SETLOGICALDPIOVERRIDE, 0);
                }

                // Forces element sizes and positions to be recalculated.
                SetWindowRgn(hWnd, nullptr, TRUE);

                SendMessage(hWnd, g_applyStyleMsg, 0, 0);
            }

            return TRUE;
        },
        (LPARAM)hTaskbarWnd);

    g_applyingSettings = false;
}

////////////////////////////////////////////////////////////////////////////////
// Settings window
//
// Opened by the settings button. It runs on its own thread, with its own XAML
// island and message loop, and writes to the settings key: the watcher applies
// the changes as for any other write.

struct SettingsText {
    PCWSTR id;
    PCWSTR en;
    PCWSTR it;
};

// English is the default; Italian is selectable in the window.
constexpr SettingsText kSettingsTexts[] = {
    {L"WindowTitle", L"Fluent Experience Settings",
     L"Impostazioni di Fluent Experience"},

    {L"Enabled", L"Fluent Experience", L"Fluent Experience"},
    {L"EnabledDesc", L"Turn off to restore the original Windows taskbar",
     L"Disattiva per tornare alla barra originale di Windows"},

    {L"Appearance", L"Appearance", L"Aspetto"},
    {L"Material", L"Material", L"Materiale"},
    {L"MaterialDesc",
     L"Windows 11 materials, drawn by Windows: they follow the light/dark mode "
     L"and Transparency effects",
     L"Materiali di Windows 11, disegnati da Windows: seguono la modalità "
     L"chiara/scura e gli Effetti di trasparenza"},
    {L"MaterialDefault", L"Default", L"Predefinito"},
    {L"Acrylic", L"Acrylic", L"Acrilico"},
    {L"Mica", L"Mica", L"Mica"},
    {L"MicaAlt", L"Mica Alt", L"Mica Alt"},
    {L"RoundedCorners", L"Rounded corners", L"Angoli arrotondati"},
    {L"RoundedCornersDesc",
     L"Windows 11 rounded corners with the Default material. Always on with "
     L"the others",
     L"Angoli arrotondati di Windows 11 con il materiale Predefinito. Sempre "
     L"attivi con gli altri"},
    {L"Border", L"Hide window border", L"Nascondi bordo della finestra"},
    {L"BorderDesc",
     L"Removes the thin line Windows 11 draws around the taskbar, as around "
     L"every window",
     L"Toglie la linea sottile che Windows 11 disegna attorno alla barra delle "
     L"applicazioni, come attorno a ogni finestra"},
    {L"Margin", L"Bottom margin", L"Margine dal bordo"},
    {L"MarginDesc",
     L"Gap between the taskbar and the bottom edge of the screen",
     L"Spazio tra la barra delle applicazioni e il bordo inferiore dello "
     L"schermo"},
    {L"SettingsButton", L"Settings button", L"Pulsante impostazioni"},
    {L"SettingsButtonDesc",
     L"A button on the taskbar, before the clock, that opens this window",
     L"Un pulsante sulla barra delle applicazioni, prima dell'orologio, che "
     L"apre questa finestra"},

    {L"HiddenIconMenu", L"Hidden icon menu", L"Menu icone nascoste"},
    {L"HiddenIconMenuDesc",
     L"The menu opened by the arrow before the tray icons shows its Windows "
     L"Acrylic instead of a flat color, like the taskbar",
     L"Il menu che si apre con la freccia prima delle icone della barra di "
     L"sistema mostra il suo Acrylic di Windows invece di un colore pieno, come "
     L"la barra delle applicazioni"},

    {L"MultipleDisplays", L"Multiple displays", L"Più schermi"},
    {L"AppsWhereOpen",
     L"Show taskbar apps on the taskbar where the window is open",
     L"Mostra le app sulla barra delle applicazioni in cui è aperta la "
     L"finestra"},
    {L"AppsWhereOpenDesc",
     L"The Windows setting for multiple displays: each taskbar shows only the "
     L"apps with a window on its display",
     L"L'impostazione di Windows per più schermi: ogni barra delle "
     L"applicazioni mostra solo le app con una finestra sul suo schermo"},

    {L"Size", L"Size", L"Dimensioni"},
    {L"DynamicWidth", L"Dynamic width", L"Larghezza dinamica"},
    {L"DynamicWidthDesc", L"The taskbar grows and shrinks to fit its icons",
     L"La barra delle applicazioni si allarga e si stringe in base alle "
     L"icone"},
    {L"Width", L"Fixed width", L"Larghezza fissa"},
    {L"WidthDesc", L"Used when dynamic width is off",
     L"Usata quando la larghezza dinamica è disattivata"},
    {L"Padding", L"Extra space", L"Spazio extra"},
    {L"PaddingDesc", L"Added to the measured width",
     L"Aggiunto alla larghezza misurata"},

    {L"Animation", L"Animation", L"Animazione"},
    {L"Duration", L"Animation duration", L"Durata dell'animazione"},
    {L"DurationDesc",
     L"How long the taskbar takes to resize. It's animated only when "
     L"Animation effects are on in Windows",
     L"Quanto ci mette la barra delle applicazioni a ridimensionarsi. Si anima "
     L"solo se in Windows sono attivi gli Effetti di animazione"},

    {L"General", L"General", L"Generale"},
    {L"Language", L"Language", L"Lingua"},
    {L"LanguageDesc", L"Language of this window", L"Lingua di questa finestra"},
    {L"English", L"English", L"English"},
    {L"Italian", L"Italiano", L"Italiano"},
    {L"Reset", L"Reset to defaults", L"Ripristina valori predefiniti"},
    {L"ResetDesc", L"Restores every setting to its default value",
     L"Riporta ogni impostazione al valore predefinito"},
    {L"ResetButton", L"Reset", L"Ripristina"},

    {L"Updates", L"Updates", L"Aggiornamenti"},
    {L"UpdateNotChecked", L"Fluent Experience updates",
     L"Aggiornamenti di Fluent Experience"},
    {L"UpdateChecking", L"Checking for updates...",
     L"Verifica della disponibilità di aggiornamenti in corso..."},
    {L"UpdateUpToDate", L"You're up to date", L"Sei aggiornato"},
    {L"UpdateAvailable", L"Version %s is available",
     L"È disponibile la versione %s"},
    {L"UpdateCheckFailed", L"Couldn't check for updates",
     L"Impossibile verificare la disponibilità di aggiornamenti"},
    {L"UpdateInstalling", L"Installing version %s...",
     L"Installazione della versione %s in corso..."},
    {L"InstalledVersion", L"Installed version: %s",
     L"Versione installata: %s"},
    {L"LastChecked", L"Last checked: %s", L"Ultimo controllo: %s"},
    {L"Today", L"today", L"oggi"},
    {L"CheckButton", L"Check for updates",
     L"Verifica disponibilità aggiornamenti"},
    {L"UpdateButton", L"Update now", L"Aggiorna ora"},
    {L"CheckUpdates", L"Check for updates automatically",
     L"Verifica automaticamente gli aggiornamenti"},
    {L"CheckUpdatesDesc",
     L"Looks for a new version on GitHub at startup and once a day. Updates "
     L"are installed only when you choose to",
     L"Cerca una nuova versione su GitHub all'avvio e una volta al giorno. Gli "
     L"aggiornamenti vengono installati solo quando lo scegli tu"},

    {L"About", L"About", L"Informazioni"},
    {L"AboutDesc",
     L"A compact, floating taskbar built on top of the native Windows 11 "
     L"taskbar",
     L"Una barra delle applicazioni compatta e sospesa, costruita sulla barra "
     L"nativa di Windows 11"},
    {L"Version", L"Version", L"Versione"},
    {L"By", L"by", L"di"},

    {L"On", L"On", L"Attivato"},
    {L"Off", L"Off", L"Disattivato"},
};

// The values the reset removes; the language and the development log stay.
constexpr PCWSTR kResettableSettings[] = {
    L"enabled", L"material",          L"border", L"roundedCorners",
    L"dynamicWidth", L"width",        L"padding", L"animationDuration",
    L"margin",  L"settingsButton",    L"hiddenIconMenu", L"checkUpdates",
};

constexpr WCHAR kSettingsWindowClass[] = L"FluentExperienceSettingsWindow";

// Its own taskbar button, instead of being grouped with File Explorer.
constexpr WCHAR kSettingsWindowAppId[] = L"Will0wn.FluentExperience.Settings";
constexpr PROPERTYKEY kAppUserModelIdKey = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
    5};

HANDLE g_settingsWindowThread;
std::atomic<HWND> g_settingsWindow;

// Posted to the settings window when the update state changes.
constexpr UINT kUpdateChangedMsg = WM_APP + 1;

// Used only on the settings window thread.
struct {
    Controls::Grid root{nullptr};
    HWND islandWnd;
    HICON smallIcon;
    HICON bigIcon;
    bool italian;
    bool loading;
} g_settingsUi;

PCWSTR Text(PCWSTR id) {
    for (const auto& text : kSettingsTexts) {
        if (wcscmp(text.id, id) == 0) {
            return g_settingsUi.italian ? text.it : text.en;
        }
    }

    return id;
}

void WriteIntSetting(PCWSTR name, int value) {
    DWORD data = value;
    RegSetKeyValue(HKEY_CURRENT_USER, kSettingsSubKey, name, REG_DWORD, &data,
                   sizeof(data));
}

void WriteStringSetting(PCWSTR name, PCWSTR value) {
    RegSetKeyValue(HKEY_CURRENT_USER, kSettingsSubKey, name, REG_SZ, value,
                   (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
}

// Windows' "When using multiple displays, show my taskbar apps on" setting,
// where 2 is "Taskbar where window is open". The toggle shows and changes the
// Windows setting itself; off is the Windows default (no value).
constexpr WCHAR kExplorerAdvancedKey[] =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)";
constexpr WCHAR kMultipleDisplaysTaskbarMode[] = L"MMTaskbarMode";
constexpr DWORD kTaskbarWhereWindowIsOpen = 2;

bool IsTaskbarAppsWhereWindowOpen() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    RegGetValue(HKEY_CURRENT_USER, kExplorerAdvancedKey,
                kMultipleDisplaysTaskbarMode, RRF_RT_REG_DWORD, nullptr, &value,
                &size);
    return value == kTaskbarWhereWindowIsOpen;
}

void SetTaskbarAppsWhereWindowOpen(bool on) {
    if (on) {
        DWORD value = kTaskbarWhereWindowIsOpen;
        RegSetKeyValue(HKEY_CURRENT_USER, kExplorerAdvancedKey,
                       kMultipleDisplaysTaskbarMode, REG_DWORD, &value,
                       sizeof(value));
    } else {
        RegDeleteKeyValue(HKEY_CURRENT_USER, kExplorerAdvancedKey,
                          kMultipleDisplaysTaskbarMode);
    }

    // What Windows Settings sends: the taskbars apply it right away.
    SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                       (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 1000,
                       nullptr);
}

// A settings card, as in the Windows Settings app: icon, header and
// description, and the control on the right.
std::wstring CardXaml(PCWSTR name, PCWSTR glyph, const std::wstring& control) {
    std::wstring n = name;
    return LR"(<Grid x:Name=")" + n + LR"(Card" Style="{StaticResource CardStyle}">)"
           LR"(<Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/>)"
           LR"(<ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/>)"
           LR"(</Grid.ColumnDefinitions>)"
           LR"(<FontIcon Glyph="&#x)" + glyph + LR"(;" Style="{StaticResource CardIconStyle}"/>)"
           LR"(<StackPanel Grid.Column="1" VerticalAlignment="Center" Margin="0,0,16,0">)"
           LR"(<TextBlock x:Name=")" + n + LR"(Header" TextWrapping="Wrap"/>)"
           LR"(<TextBlock x:Name=")" + n + LR"(Desc" Style="{StaticResource DescriptionStyle}"/>)"
           LR"(</StackPanel>)"
           LR"(<StackPanel Grid.Column="2" Orientation="Horizontal" Spacing="12" VerticalAlignment="Center">)" +
           control + LR"(</StackPanel></Grid>)";
}

std::wstring ToggleXaml(PCWSTR name) {
    return LR"(<ToggleSwitch x:Name=")" + std::wstring(name) + LR"(Toggle"/>)";
}

std::wstring SliderXaml(PCWSTR name, int minimum, int maximum, int step) {
    std::wstring n = name;
    return LR"(<TextBlock x:Name=")" + n + LR"(Value" VerticalAlignment="Center" MinWidth="60" TextAlignment="Right"/>)"
           LR"(<Slider x:Name=")" + n + LR"(Slider" Width="200" VerticalAlignment="Center" Minimum=")" +
           std::to_wstring(minimum) + LR"(" Maximum=")" + std::to_wstring(maximum) +
           LR"(" StepFrequency=")" + std::to_wstring(step) + LR"("/>)";
}

std::wstring SectionXaml(PCWSTR name) {
    return LR"(<TextBlock x:Name=")" + std::wstring(name) +
           LR"(Section" Style="{StaticResource SectionStyle}"/>)";
}

// The page uses the Windows 11 controls and styles (WinUI 2), which Windows
// ships and Explorer already loads for the taskbar.
std::wstring SettingsPageXaml() {
    std::wstring xaml =
        LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation")"
        LR"( xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml")"
        LR"( xmlns:muxc="using:Microsoft.UI.Xaml.Controls" Background="Transparent">)"
        LR"(<Grid.Resources><ResourceDictionary>)"
        LR"(<ResourceDictionary.MergedDictionaries><muxc:XamlControlsResources/></ResourceDictionary.MergedDictionaries>)"
        // Text takes the island's app theme by default, which follows the app
        // mode instead of the window's theme.
        LR"(<Style TargetType="TextBlock">)"
        LR"(<Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/>)"
        LR"(</Style>)"
        LR"(<Style x:Key="CardStyle" TargetType="Grid">)"
        LR"(<Setter Property="Background" Value="{ThemeResource CardBackgroundFillColorDefaultBrush}"/>)"
        LR"(<Setter Property="BorderBrush" Value="{ThemeResource CardStrokeColorDefaultBrush}"/>)"
        LR"(<Setter Property="BorderThickness" Value="1"/>)"
        LR"(<Setter Property="CornerRadius" Value="{ThemeResource ControlCornerRadius}"/>)"
        LR"(<Setter Property="Padding" Value="16,12,16,12"/>)"
        LR"(<Setter Property="MinHeight" Value="68"/>)"
        LR"(</Style>)"
        LR"(<Style x:Key="CardIconStyle" TargetType="FontIcon">)"
        LR"(<Setter Property="FontFamily" Value="Segoe Fluent Icons"/>)"
        LR"(<Setter Property="FontSize" Value="20"/>)"
        LR"(<Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/>)"
        LR"(<Setter Property="Margin" Value="4,0,20,0"/>)"
        LR"(<Setter Property="VerticalAlignment" Value="Center"/>)"
        LR"(</Style>)"
        LR"(<Style x:Key="DescriptionStyle" TargetType="TextBlock" BasedOn="{StaticResource CaptionTextBlockStyle}">)"
        LR"(<Setter Property="TextWrapping" Value="Wrap"/>)"
        LR"(<Setter Property="Foreground" Value="{ThemeResource TextFillColorSecondaryBrush}"/>)"
        LR"(</Style>)"
        LR"(<Style x:Key="SectionStyle" TargetType="TextBlock" BasedOn="{StaticResource BodyStrongTextBlockStyle}">)"
        LR"(<Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/>)"
        LR"(<Setter Property="Margin" Value="1,26,0,6"/>)"
        LR"(</Style>)"
        LR"(<Style x:Key="UpdateButtonStyle" TargetType="Button" BasedOn="{StaticResource AccentButtonStyle}"/>)"
        LR"(</ResourceDictionary></Grid.Resources>)"
        LR"(<ScrollViewer Padding="36,0,36,0">)"
        LR"(<StackPanel MaxWidth="1000" Spacing="4" Margin="0,16,0,36">)"
        LR"(<TextBlock Text="Fluent Experience" Style="{StaticResource TitleTextBlockStyle}")"
        LR"( Foreground="{ThemeResource TextFillColorPrimaryBrush}" Margin="0,0,0,20"/>)";

    xaml += CardXaml(L"Enabled", L"E7C4", ToggleXaml(L"Enabled"));

    xaml += SectionXaml(L"Appearance");
    xaml += CardXaml(L"Material", L"F4A5",
                     LR"(<ComboBox x:Name="MaterialCombo" MinWidth="240"/>)");
    xaml += CardXaml(L"RoundedCorners", L"F16B", ToggleXaml(L"RoundedCorners"));
    xaml += CardXaml(L"Border", L"E81E", ToggleXaml(L"Border"));
    xaml += CardXaml(L"Margin", L"E74B", SliderXaml(L"Margin", 0, 40, 1));
    xaml += CardXaml(L"SettingsButton", L"E9E9", ToggleXaml(L"SettingsButton"));
    xaml += CardXaml(L"HiddenIconMenu", L"E70E", ToggleXaml(L"HiddenIconMenu"));

    xaml += SectionXaml(L"MultipleDisplays");
    xaml += CardXaml(L"AppsWhereOpen", L"E7F4", ToggleXaml(L"AppsWhereOpen"));

    xaml += SectionXaml(L"Size");
    xaml += CardXaml(L"DynamicWidth", L"E9A6", ToggleXaml(L"DynamicWidth"));
    // The NumberBox type isn't in the C++ headers: its value is bound to a
    // hidden slider, which the code reads and writes.
    xaml += CardXaml(
        L"Width", L"E740",
        LR"(<Slider x:Name="WidthSlider" Visibility="Collapsed" Minimum="200" Maximum="4000" StepFrequency="1"/>)"
        LR"(<muxc:NumberBox x:Name="WidthBox" MinWidth="160" Minimum="200" Maximum="4000")"
        LR"( SmallChange="10" LargeChange="100" SpinButtonPlacementMode="Inline")"
        LR"( Value="{Binding Value, ElementName=WidthSlider, Mode=TwoWay}"/>)");
    xaml += CardXaml(L"Padding", L"E76F", SliderXaml(L"Padding", 0, 64, 1));

    xaml += SectionXaml(L"Animation");
    xaml += CardXaml(L"Duration", L"E823", SliderXaml(L"Duration", 50, 1000, 10));

    xaml += SectionXaml(L"General");
    xaml += CardXaml(L"Language", L"F2B7",
                     LR"(<ComboBox x:Name="LanguageCombo" MinWidth="160"/>)");
    xaml += CardXaml(L"Reset", L"E777", LR"(<Button x:Name="ResetButton"/>)");

    xaml += SectionXaml(L"Updates");
    xaml += CardXaml(L"Update", L"E895",
                     LR"(<ProgressRing x:Name="UpdateProgress" Width="20" Height="20" IsActive="False"/>)"
                     LR"(<Button x:Name="UpdateButton" MinWidth="120"/>)");
    xaml += CardXaml(L"CheckUpdates", L"E81C", ToggleXaml(L"CheckUpdates"));

    xaml += SectionXaml(L"About");
    xaml +=
        LR"(<muxc:Expander IsExpanded="True" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch">)"
        LR"(<muxc:Expander.Header><Grid Padding="0,12,0,12">)"
        LR"(<Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/>)"
        LR"(<ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/>)"
        LR"(</Grid.ColumnDefinitions>)"
        LR"(<FontIcon Glyph="&#xE946;" Style="{StaticResource CardIconStyle}"/>)"
        LR"(<StackPanel Grid.Column="1" VerticalAlignment="Center" Margin="0,0,16,0">)"
        LR"(<TextBlock x:Name="AboutHeader" TextWrapping="Wrap"/>)"
        LR"(<TextBlock x:Name="AboutDesc" Style="{StaticResource DescriptionStyle}"/>)"
        LR"(</StackPanel>)"
        LR"(<TextBlock x:Name="VersionText" Grid.Column="2" VerticalAlignment="Center")"
        LR"( Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>)"
        LR"(</Grid></muxc:Expander.Header>)"
        LR"(<StackPanel Orientation="Horizontal" Spacing="4" Margin="44,0,0,0">)"
        LR"(<TextBlock x:Name="ByText" VerticalAlignment="Center"/>)"
        LR"(<HyperlinkButton x:Name="AuthorLink" Content="Will0wn"/>)"
        LR"(</StackPanel></muxc:Expander>)";

    xaml += LR"(</StackPanel></ScrollViewer></Grid>)";
    return xaml;
}

template <typename T>
T FindSettingsElement(const std::wstring& name) {
    return g_settingsUi.root.FindName(name).as<T>();
}

void SetSettingsText(const std::wstring& name, PCWSTR text) {
    FindSettingsElement<Controls::TextBlock>(name).Text(text);
}

void SetCardTexts(PCWSTR name, PCWSTR header, PCWSTR description) {
    std::wstring n = name;
    SetSettingsText(n + L"Header", header);
    SetSettingsText(n + L"Desc", description);
}

Controls::ToggleSwitch FindToggle(PCWSTR name) {
    return FindSettingsElement<Controls::ToggleSwitch>(std::wstring(name) + L"Toggle");
}

Controls::Slider FindSlider(PCWSTR name) {
    return FindSettingsElement<Controls::Slider>(std::wstring(name) + L"Slider");
}

void FillCombo(PCWSTR name,
               std::initializer_list<std::pair<PCWSTR, PCWSTR>> items) {
    auto combo = FindSettingsElement<Controls::ComboBox>(name);
    combo.Items().Clear();
    for (const auto& [tag, textId] : items) {
        Controls::ComboBoxItem item;
        item.Content(winrt::box_value(winrt::hstring(Text(textId))));
        item.Tag(winrt::box_value(winrt::hstring(tag)));
        combo.Items().Append(item);
    }
}

std::wstring SelectedTag(PCWSTR name) {
    auto combo = FindSettingsElement<Controls::ComboBox>(name);
    auto item = combo.SelectedItem().try_as<Controls::ComboBoxItem>();
    return item ? std::wstring(winrt::unbox_value<winrt::hstring>(item.Tag()))
                : std::wstring();
}

void SelectTag(PCWSTR name, PCWSTR tag) {
    auto combo = FindSettingsElement<Controls::ComboBox>(name);
    int32_t index = 0;
    for (uint32_t i = 0; i < combo.Items().Size(); i++) {
        auto item = combo.Items().GetAt(i).as<Controls::ComboBoxItem>();
        if (winrt::unbox_value<winrt::hstring>(item.Tag()) == tag) {
            index = i;
            break;
        }
    }
    combo.SelectedIndex(index);
}

// A disabled card is dimmed and its control can't be used, as in the Windows
// Settings app.
void SetCardEnabled(PCWSTR name, const std::wstring& controlName, bool enabled) {
    FindSettingsElement<Controls::Grid>(std::wstring(name) + L"Card")
        .Opacity(enabled ? 1.0 : 0.5);
    FindSettingsElement<Controls::Control>(controlName).IsEnabled(enabled);
}

// Settings that only matter when another one is on.
void UpdateSettingsUiStates() {
    bool enabled = FindToggle(L"Enabled").IsOn();
    bool defaultMaterial = SelectedTag(L"MaterialCombo") == L"default";
    bool dynamicWidth = FindToggle(L"DynamicWidth").IsOn();

    SetCardEnabled(L"Material", L"MaterialCombo", enabled);
    SetCardEnabled(L"RoundedCorners", L"RoundedCornersToggle",
                   enabled && defaultMaterial);
    SetCardEnabled(L"Border", L"BorderToggle",
                   enabled && (!defaultMaterial ||
                               FindToggle(L"RoundedCorners").IsOn()));
    SetCardEnabled(L"Margin", L"MarginSlider", enabled);
    SetCardEnabled(L"SettingsButton", L"SettingsButtonToggle", enabled);
    SetCardEnabled(L"HiddenIconMenu", L"HiddenIconMenuToggle",
                   enabled && !defaultMaterial);
    SetCardEnabled(L"DynamicWidth", L"DynamicWidthToggle", enabled);
    SetCardEnabled(L"Width", L"WidthBox", enabled && !dynamicWidth);
    SetCardEnabled(L"Padding", L"PaddingSlider", enabled && dynamicWidth);
    SetCardEnabled(L"Duration", L"DurationSlider", enabled && dynamicWidth);
}

void UpdateSettingsUiLabels() {
    SetSettingsText(L"MarginValue",
                    (std::to_wstring((int)FindSlider(L"Margin").Value()) + L" px").c_str());
    SetSettingsText(L"PaddingValue",
                    (std::to_wstring((int)FindSlider(L"Padding").Value()) + L" px").c_str());
    SetSettingsText(L"DurationValue",
                    (std::to_wstring((int)FindSlider(L"Duration").Value()) + L" ms").c_str());
}

// A text with one %s, filled.
std::wstring FormatText(PCWSTR id, const std::wstring& value) {
    std::wstring text = Text(id);
    size_t pos = text.find(L"%s");
    if (pos != std::wstring::npos) {
        text.replace(pos, 2, value);
    }
    return text;
}

// "today, 10:12" or the date and time, in the user's formats.
std::wstring FormatCheckTime(const SYSTEMTIME& time) {
    WCHAR timeText[64] = L"";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr,
                    timeText, ARRAYSIZE(timeText));

    SYSTEMTIME now;
    GetLocalTime(&now);
    std::wstring day;
    if (now.wYear == time.wYear && now.wMonth == time.wMonth &&
        now.wDay == time.wDay) {
        day = Text(L"Today");
    } else {
        WCHAR dateText[64] = L"";
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr,
                        dateText, ARRAYSIZE(dateText), nullptr);
        day = dateText;
    }

    return day + L", " + timeText;
}

// The Updates card, from the update state.
void UpdateUpdateUi() {
    UpdateInfo info = GetUpdateInfo();

    std::wstring header;
    switch (info.state) {
        case UpdateState::notChecked:
            header = Text(L"UpdateNotChecked");
            break;
        case UpdateState::checking:
            header = Text(L"UpdateChecking");
            break;
        case UpdateState::upToDate:
            header = Text(L"UpdateUpToDate");
            break;
        case UpdateState::available:
            header = FormatText(L"UpdateAvailable", info.version);
            break;
        case UpdateState::checkFailed:
            header = Text(L"UpdateCheckFailed");
            break;
        case UpdateState::installing:
            header = FormatText(L"UpdateInstalling", info.version);
            break;
    }

    std::wstring description = FormatText(L"InstalledVersion", WH_MOD_VERSION);
    if (info.lastChecked.wYear && info.state != UpdateState::available &&
        info.state != UpdateState::installing) {
        description = FormatText(L"LastChecked", FormatCheckTime(info.lastChecked));
    }

    SetCardTexts(L"Update", header.c_str(), description.c_str());

    bool busy = info.state == UpdateState::checking ||
                info.state == UpdateState::installing;
    FindSettingsElement<Controls::ProgressRing>(L"UpdateProgress").IsActive(busy);

    auto button = FindSettingsElement<Controls::Button>(L"UpdateButton");
    bool available = info.state == UpdateState::available ||
                     info.state == UpdateState::installing;
    button.Content(winrt::box_value(
        winrt::hstring(Text(available ? L"UpdateButton" : L"CheckButton"))));
    button.IsEnabled(!busy);
    // The accent style marks the action to take, as in Windows Update.
    button.Style(available
                     ? g_settingsUi.root.Resources()
                           .Lookup(winrt::box_value(L"UpdateButtonStyle"))
                           .as<Style>()
                     : nullptr);
}

// Fills texts and controls from the current language and settings.
void LoadSettingsUi() {
    g_settingsUi.loading = true;

    Settings settings = ReadSettings();
    g_settingsUi.italian = settings.language == L"it";

    if (HWND hWnd = g_settingsWindow) {
        SetWindowText(hWnd, Text(L"WindowTitle"));
    }

    SetCardTexts(L"Enabled", Text(L"Enabled"), Text(L"EnabledDesc"));
    SetSettingsText(L"AppearanceSection", Text(L"Appearance"));
    SetCardTexts(L"Material", Text(L"Material"), Text(L"MaterialDesc"));
    SetCardTexts(L"RoundedCorners", Text(L"RoundedCorners"),
                 Text(L"RoundedCornersDesc"));
    SetCardTexts(L"Border", Text(L"Border"), Text(L"BorderDesc"));
    SetCardTexts(L"Margin", Text(L"Margin"), Text(L"MarginDesc"));
    SetCardTexts(L"SettingsButton", Text(L"SettingsButton"),
                 Text(L"SettingsButtonDesc"));
    SetCardTexts(L"HiddenIconMenu", Text(L"HiddenIconMenu"),
                 Text(L"HiddenIconMenuDesc"));
    SetSettingsText(L"MultipleDisplaysSection", Text(L"MultipleDisplays"));
    SetCardTexts(L"AppsWhereOpen", Text(L"AppsWhereOpen"),
                 Text(L"AppsWhereOpenDesc"));
    SetSettingsText(L"SizeSection", Text(L"Size"));
    SetCardTexts(L"DynamicWidth", Text(L"DynamicWidth"),
                 Text(L"DynamicWidthDesc"));
    SetCardTexts(L"Width", Text(L"Width"), Text(L"WidthDesc"));
    SetCardTexts(L"Padding", Text(L"Padding"), Text(L"PaddingDesc"));
    SetSettingsText(L"AnimationSection", Text(L"Animation"));
    SetCardTexts(L"Duration", Text(L"Duration"), Text(L"DurationDesc"));
    SetSettingsText(L"GeneralSection", Text(L"General"));
    SetCardTexts(L"Language", Text(L"Language"), Text(L"LanguageDesc"));
    SetCardTexts(L"Reset", Text(L"Reset"), Text(L"ResetDesc"));
    FindSettingsElement<Controls::Button>(L"ResetButton")
        .Content(winrt::box_value(winrt::hstring(Text(L"ResetButton"))));
    SetSettingsText(L"UpdatesSection", Text(L"Updates"));
    SetCardTexts(L"CheckUpdates", Text(L"CheckUpdates"),
                 Text(L"CheckUpdatesDesc"));
    UpdateUpdateUi();
    SetSettingsText(L"AboutSection", Text(L"About"));
    SetCardTexts(L"About", L"Fluent Experience for Windows 11",
                 Text(L"AboutDesc"));
    SetSettingsText(L"VersionText",
                    (std::wstring(Text(L"Version")) + L" " + WH_MOD_VERSION).c_str());
    SetSettingsText(L"ByText", Text(L"By"));

    for (PCWSTR name : {L"Enabled", L"RoundedCorners", L"Border",
                        L"SettingsButton", L"HiddenIconMenu", L"AppsWhereOpen",
                        L"DynamicWidth",
                        L"CheckUpdates"}) {
        auto toggle = FindToggle(name);
        toggle.OnContent(winrt::box_value(winrt::hstring(Text(L"On"))));
        toggle.OffContent(winrt::box_value(winrt::hstring(Text(L"Off"))));
    }

    FillCombo(L"MaterialCombo", {{L"default", L"MaterialDefault"},
                                 {L"acrylic", L"Acrylic"},
                                 {L"mica", L"Mica"},
                                 {L"micaAlt", L"MicaAlt"}});
    FillCombo(L"LanguageCombo", {{L"en", L"English"}, {L"it", L"Italian"}});

    PCWSTR material = settings.material == Material::windowsDefault ? L"default"
                      : settings.material == Material::mica         ? L"mica"
                      : settings.material == Material::micaAlt      ? L"micaAlt"
                                                                    : L"acrylic";

    FindToggle(L"Enabled").IsOn(settings.enabled);
    SelectTag(L"MaterialCombo", material);
    FindToggle(L"RoundedCorners").IsOn(settings.roundedCorners);
    FindToggle(L"Border").IsOn(!settings.border);
    FindSlider(L"Margin").Value(settings.margin);
    FindToggle(L"SettingsButton").IsOn(settings.settingsButton);
    FindToggle(L"HiddenIconMenu").IsOn(settings.hiddenIconMenu);
    FindToggle(L"AppsWhereOpen").IsOn(IsTaskbarAppsWhereWindowOpen());
    FindToggle(L"DynamicWidth").IsOn(settings.dynamicWidth);
    FindToggle(L"CheckUpdates").IsOn(settings.checkUpdates);
    FindSlider(L"Width").Value(settings.width);
    FindSlider(L"Padding").Value(settings.padding);
    FindSlider(L"Duration").Value(settings.animationDuration);
    SelectTag(L"LanguageCombo", g_settingsUi.italian ? L"it" : L"en");

    UpdateSettingsUiStates();
    UpdateSettingsUiLabels();

    g_settingsUi.loading = false;
}

void SaveFromSettingsUi(std::function<void()> write) {
    if (g_settingsUi.loading) {
        return;
    }

    write();
    UpdateSettingsUiStates();
    UpdateSettingsUiLabels();
}

void ConnectToggle(PCWSTR name, PCWSTR setting) {
    FindToggle(name).Toggled([name, setting](const auto&, const auto&) {
        SaveFromSettingsUi(
            [name, setting] { WriteIntSetting(setting, FindToggle(name).IsOn()); });
    });
}

void ConnectSlider(PCWSTR name, PCWSTR setting) {
    FindSlider(name).ValueChanged([name, setting](const auto&, const auto&) {
        SaveFromSettingsUi([name, setting] {
            WriteIntSetting(setting, (int)FindSlider(name).Value());
        });
    });
}

void ConnectSettingsUi() {
    ConnectToggle(L"Enabled", L"enabled");
    ConnectToggle(L"RoundedCorners", L"roundedCorners");
    ConnectToggle(L"SettingsButton", L"settingsButton");
    ConnectToggle(L"DynamicWidth", L"dynamicWidth");
    ConnectToggle(L"CheckUpdates", L"checkUpdates");
    ConnectToggle(L"HiddenIconMenu", L"hiddenIconMenu");
    ConnectSlider(L"Margin", L"margin");
    ConnectSlider(L"Width", L"width");
    ConnectSlider(L"Padding", L"padding");
    ConnectSlider(L"Duration", L"animationDuration");

    FindToggle(L"Border").Toggled([](const auto&, const auto&) {
        SaveFromSettingsUi([] {
            WriteStringSetting(L"border",
                               FindToggle(L"Border").IsOn() ? L"none" : L"default");
        });
    });

    FindSettingsElement<Controls::ComboBox>(L"MaterialCombo")
        .SelectionChanged([](const auto&, const auto&) {
            SaveFromSettingsUi([] {
                std::wstring tag = SelectedTag(L"MaterialCombo");
                WriteStringSetting(L"material",
                                   tag.empty() ? L"acrylic" : tag.c_str());
            });
        });

    FindSettingsElement<Controls::ComboBox>(L"LanguageCombo")
        .SelectionChanged([](const auto&, const auto&) {
            if (g_settingsUi.loading) {
                return;
            }

            std::wstring tag = SelectedTag(L"LanguageCombo");
            WriteStringSetting(L"language", tag.empty() ? L"en" : tag.c_str());
            LoadSettingsUi();
        });

    FindSettingsElement<Controls::Button>(L"ResetButton")
        .Click([](const auto&, const auto&) {
            for (PCWSTR name : kResettableSettings) {
                RegDeleteKeyValue(HKEY_CURRENT_USER, kSettingsSubKey, name);
            }
            LoadSettingsUi();
        });

    FindToggle(L"AppsWhereOpen").Toggled([](const auto&, const auto&) {
        if (!g_settingsUi.loading) {
            SetTaskbarAppsWhereWindowOpen(FindToggle(L"AppsWhereOpen").IsOn());
        }
    });

    // Turned on, it checks right away instead of waiting for the next day.
    FindToggle(L"CheckUpdates").Toggled([](const auto&, const auto&) {
        if (!g_settingsUi.loading && FindToggle(L"CheckUpdates").IsOn() &&
            g_updateCheckEvent) {
            SetEvent(g_updateCheckEvent);
        }
    });

    FindSettingsElement<Controls::Button>(L"UpdateButton")
        .Click([](const auto&, const auto&) {
            HANDLE event = IsUpdateAvailable() ? g_updateInstallEvent
                                               : g_updateCheckEvent;
            if (event) {
                SetEvent(event);
            }
        });

    FindSettingsElement<Controls::HyperlinkButton>(L"AuthorLink")
        .Click([](const auto&, const auto&) {
            ShellExecute(nullptr, L"open", L"https://github.com/Will0wn", nullptr,
                         nullptr, SW_SHOWNORMAL);
        });
}

// The settings glyph as an icon, in a given color, for the title bar and the
// taskbar button.
HICON CreateGlyphIcon(int size, BYTE shade) {
    BITMAPINFO bitmapInfo{
        .bmiHeader =
            {
                .biSize = sizeof(BITMAPINFOHEADER),
                .biWidth = size,
                .biHeight = -size,
                .biPlanes = 1,
                .biBitCount = 32,
                .biCompression = BI_RGB,
            },
    };

    void* bits;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color =
        CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) {
        DeleteDC(dc);
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, color);
    HFONT font = CreateFont(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH, L"Segoe Fluent Icons");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc{0, 0, size, size};
    DrawText(dc, kSettingsGlyph, 1, &rc,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    // White text on black: the brightness is the coverage, used as alpha.
    auto* pixels = static_cast<DWORD*>(bits);
    for (int i = 0; i < size * size; i++) {
        DWORD alpha = pixels[i] & 0xFF;
        DWORD c = shade * alpha / 255;
        pixels[i] = (alpha << 24) | (c << 16) | (c << 8) | c;
    }

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteDC(dc);

    std::vector<BYTE> maskBits(((size + 15) / 16) * 2 * size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO iconInfo{
        .fIcon = TRUE,
        .hbmMask = mask,
        .hbmColor = color,
    };
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void SetSettingsWindowAppId(HWND hWnd, PCWSTR appId) {
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(hWnd, IID_PPV_ARGS(store.put())))) {
        return;
    }

    PROPVARIANT value{};
    if (appId) {
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<PWSTR>(appId);
    }
    store->SetValue(kAppUserModelIdKey, value);
    store->Commit();
}

// Light or dark like the taskbar: the Windows mode, not the app mode.
void ApplySettingsWindowTheme(HWND hWnd) {
    bool light = IsTaskbarLightTheme();

    BOOL dark = !light;
    DwmSetWindowAttribute(hWnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));

    if (g_settingsUi.root) {
        g_settingsUi.root.RequestedTheme(light ? ElementTheme::Light
                                               : ElementTheme::Dark);
    }

    UINT dpi = GetDpiForWindow(hWnd);
    HICON smallIcon = CreateGlyphIcon(
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), light ? 0 : 255);
    HICON bigIcon = CreateGlyphIcon(GetSystemMetricsForDpi(SM_CXICON, dpi),
                                    light ? 0 : 255);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);

    if (g_settingsUi.smallIcon) {
        DestroyIcon(g_settingsUi.smallIcon);
    }
    if (g_settingsUi.bigIcon) {
        DestroyIcon(g_settingsUi.bigIcon);
    }
    g_settingsUi.smallIcon = smallIcon;
    g_settingsUi.bigIcon = bigIcon;
}

LRESULT CALLBACK SettingsWindowProc(HWND hWnd,
                                    UINT msg,
                                    WPARAM wParam,
                                    LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (g_settingsUi.islandWnd) {
                SetWindowPos(g_settingsUi.islandWnd, nullptr, 0, 0,
                             LOWORD(lParam), HIWORD(lParam),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            break;

        case WM_SETFOCUS:
            if (g_settingsUi.islandWnd) {
                SetFocus(g_settingsUi.islandWnd);
            }
            break;

        case WM_SETTINGCHANGE:
            if (lParam && wcscmp((PCWSTR)lParam, L"ImmersiveColorSet") == 0) {
                ApplySettingsWindowTheme(hWnd);
            }
            break;

        case kUpdateChangedMsg:
            if (g_settingsUi.root) {
                UpdateUpdateUi();
            }
            return 0;

        case WM_DPICHANGED: {
            auto* rc = (RECT*)lParam;
            SetWindowPos(hWnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_DESTROY:
            g_settingsWindow = nullptr;
            SetSettingsWindowAppId(hWnd, nullptr);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// Windows.UI.Xaml.IXamlSourceTransparency, documented by Microsoft but not in
// the SDK headers. XAML islands have an opaque background in the app mode's
// color by default; transparent, the window's Mica shows through.
struct IXamlSourceTransparency : ::IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBackgroundTransparent(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBackgroundTransparent(boolean value) = 0;
};

constexpr GUID kIXamlSourceTransparencyIid = {
    0x06636c29, 0x5a17, 0x458d, {0x8e, 0xa2, 0x24, 0x22, 0xd9, 0x97, 0xa9, 0x22}};

// For all the islands of the current thread.
void MakeXamlIslandsTransparent() {
    winrt::com_ptr<IXamlSourceTransparency> transparency;
    HRESULT hr = winrt::get_unknown(Window::Current())
                     ->QueryInterface(kIXamlSourceTransparencyIid,
                                      transparency.put_void());
    if (SUCCEEDED(hr)) {
        hr = transparency->put_IsBackgroundTransparent(true);
    }
    DebugLog(L"Settings window transparent background: %08X", hr);
}

HINSTANCE GetCurrentModuleHandle() {
    HMODULE module = nullptr;
    GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (PCWSTR)&GetCurrentModuleHandle, &module);
    return module;
}

void RunSettingsWindow() {
    HINSTANCE instance = GetCurrentModuleHandle();

    WNDCLASSEX windowClass{
        .cbSize = sizeof(WNDCLASSEX),
        .lpfnWndProc = SettingsWindowProc,
        .hInstance = instance,
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        // With the frame extended over the client area, black is transparent:
        // the Mica backdrop shows through, instead of white.
        .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
        .lpszClassName = kSettingsWindowClass,
    };
    RegisterClassEx(&windowClass);

    g_settingsUi.italian = ReadSettings().language == L"it";

    HWND hWnd = CreateWindowEx(0, kSettingsWindowClass, Text(L"WindowTitle"),
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                               instance, nullptr);
    if (!hWnd) {
        DebugLog(L"Settings window: CreateWindowEx failed: %u", GetLastError());
        UnregisterClass(kSettingsWindowClass, instance);
        return;
    }

    UINT dpi = GetDpiForWindow(hWnd);
    SetWindowPos(hWnd, nullptr, 0, 0, MulDiv(860, dpi, 96), MulDiv(900, dpi, 96),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetSettingsWindowAppId(hWnd, kSettingsWindowAppId);

    // Mica, drawn by Windows behind the whole window.
    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hWnd, &margins);
    DWORD backdrop = kDwmsbtMainWindow;
    DwmSetWindowAttribute(hWnd, kDwmwaSystemBackdropType, &backdrop,
                          sizeof(backdrop));

    Hosting::WindowsXamlManager manager{nullptr};
    Hosting::DesktopWindowXamlSource source{nullptr};
    winrt::com_ptr<IDesktopWindowXamlSourceNative2> native;

    try {
        manager = Hosting::WindowsXamlManager::InitializeForCurrentThread();
        MakeXamlIslandsTransparent();
        source = Hosting::DesktopWindowXamlSource();
        native = source.as<IDesktopWindowXamlSourceNative2>();
        winrt::check_hresult(native->AttachToWindow(hWnd));
        winrt::check_hresult(native->get_WindowHandle(&g_settingsUi.islandWnd));

        g_settingsUi.root =
            Markup::XamlReader::Load(SettingsPageXaml()).as<Controls::Grid>();
        g_settingsWindow = hWnd;
        LoadSettingsUi();
        ConnectSettingsUi();
        ApplySettingsWindowTheme(hWnd);
        source.Content(g_settingsUi.root);

        RECT rc;
        GetClientRect(hWnd, &rc);
        SetWindowPos(g_settingsUi.islandWnd, nullptr, 0, 0, rc.right, rc.bottom,
                     SWP_NOZORDER | SWP_SHOWWINDOW);

        ShowWindow(hWnd, SW_SHOWNORMAL);
        SetForegroundWindow(hWnd);

        DebugLog(L"Settings window opened");
    } catch (const winrt::hresult_error& e) {
        DebugLog(L"Settings window failed: %08X %s", (DWORD)e.code(),
                 e.message().c_str());
        DestroyWindow(hWnd);
    }

    // Keyboard input reaches the XAML controls through PreTranslateMessage.
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        BOOL handled = FALSE;
        if (native) {
            native->PreTranslateMessage(&msg, &handled);
        }
        if (!handled) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    g_settingsUi.root = nullptr;
    g_settingsUi.islandWnd = nullptr;
    native = nullptr;
    if (source) {
        source.Close();
    }
    if (manager) {
        manager.Close();
    }

    for (HICON* icon : {&g_settingsUi.smallIcon, &g_settingsUi.bigIcon}) {
        if (*icon) {
            DestroyIcon(*icon);
            *icon = nullptr;
        }
    }

    UnregisterClass(kSettingsWindowClass, instance);

    DebugLog(L"Settings window closed");
}

DWORD WINAPI SettingsWindowThreadProc(LPVOID) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    RunSettingsWindow();
    winrt::uninit_apartment();
    return 0;
}

// The button opens the window on the taskbar thread, the link on the settings
// watcher thread.
std::mutex g_settingsWindowThreadMutex;

// Brings the window to the front if it's open, or opens it.
void OpenSettingsWindow() {
    std::lock_guard<std::mutex> guard(g_settingsWindowThreadMutex);

    if (HWND hWnd = g_settingsWindow) {
        if (IsIconic(hWnd)) {
            ShowWindow(hWnd, SW_RESTORE);
        }
        SetForegroundWindow(hWnd);
        return;
    }

    if (g_settingsWindowThread) {
        // Still opening: the window shows up on its own.
        if (WaitForSingleObject(g_settingsWindowThread, 0) != WAIT_OBJECT_0) {
            return;
        }
        CloseHandle(g_settingsWindowThread);
    }

    g_settingsWindowThread = CreateThread(nullptr, 0, SettingsWindowThreadProc,
                                          nullptr, 0, nullptr);
}

void CloseSettingsWindow() {
    std::lock_guard<std::mutex> guard(g_settingsWindowThreadMutex);

    if (!g_settingsWindowThread) {
        return;
    }

    // The window may not exist yet while the thread is still opening it.
    while (WaitForSingleObject(g_settingsWindowThread, 100) == WAIT_TIMEOUT) {
        if (HWND hWnd = g_settingsWindow) {
            PostMessage(hWnd, WM_CLOSE, 0, 0);
        }
    }

    CloseHandle(g_settingsWindowThread);
    g_settingsWindowThread = nullptr;
}

////////////////////////////////////////////////////////////////////////////////
// Settings watcher

HANDLE g_settingsWatcherThread;
HANDLE g_settingsWatcherStopEvent;

void ReloadSettings() {
    LoadSettings();

    DebugLog(L"Settings changed");

    {
        std::lock_guard<std::mutex> guard(g_dockWidthsMutex);
        g_dockWidths.clear();
    }

    ApplySettings();
}

// Applies changes made by the settings window as soon as they're written.
DWORD WINAPI SettingsWatcherThreadProc(LPVOID) {
    HKEY key;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, kSettingsSubKey, 0, nullptr, 0,
                       KEY_NOTIFY, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        DebugLog(L"Can't watch the settings key");
        return 0;
    }

    HANDLE changeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HANDLE events[] = {g_settingsWatcherStopEvent, changeEvent};

    while (true) {
        if (RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                                    changeEvent, TRUE) != ERROR_SUCCESS) {
            break;
        }

        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE,
                                   INFINITE) != WAIT_OBJECT_0 + 1) {
            break;
        }

        // The window writes several values at once: wait for the burst to end.
        while (true) {
            if (RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                                        changeEvent, TRUE) != ERROR_SUCCESS) {
                break;
            }

            DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events,
                                                FALSE, 150);
            if (wait == WAIT_OBJECT_0) {
                CloseHandle(changeEvent);
                RegCloseKey(key);
                return 0;
            }

            if (wait == WAIT_TIMEOUT) {
                break;
            }
        }

        // Removing the request is a change too, which reloads the settings.
        if (TakeOpenSettingsRequest()) {
            OpenSettingsWindow();
            continue;
        }

        ReloadSettings();
    }

    CloseHandle(changeEvent);
    RegCloseKey(key);
    return 0;
}

void StartSettingsWatcher() {
    g_settingsWatcherStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_settingsWatcherThread = CreateThread(
        nullptr, 0, SettingsWatcherThreadProc, nullptr, 0, nullptr);
}

void StopSettingsWatcher() {
    if (g_settingsWatcherThread) {
        SetEvent(g_settingsWatcherStopEvent);
        WaitForSingleObject(g_settingsWatcherThread, INFINITE);
        CloseHandle(g_settingsWatcherThread);
        g_settingsWatcherThread = nullptr;
    }

    if (g_settingsWatcherStopEvent) {
        CloseHandle(g_settingsWatcherStopEvent);
        g_settingsWatcherStopEvent = nullptr;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Updates
//
// New versions are GitHub releases: the tag is the version (v0.15) and the
// compiled mod and its source are attached. The check runs at startup and once
// a day when on, or from the settings window. Installing is always up to the
// user: the Windhawk mods folder and registry key are protected, so Windows
// asks for administrator permission.

constexpr WCHAR kLatestReleaseUrl[] =
    L"https://api.github.com/repos/Will0wn/fluent-experience/releases/latest";
constexpr DWORD kFirstUpdateCheckDelay = 30 * 1000;
constexpr DWORD kUpdateCheckInterval = 24 * 60 * 60 * 1000;
constexpr int kHttpTimeout = 15 * 1000;

HANDLE g_updateThread;
HANDLE g_updateStopEvent;

void SetUpdateInfo(const UpdateInfo& info) {
    {
        std::lock_guard<std::mutex> guard(g_updateMutex);
        g_update = info;
    }

    if (HWND hWnd = g_settingsWindow) {
        PostMessage(hWnd, kUpdateChangedMsg, 0, 0);
    }

    // The settings button shows or hides the attention state.
    if (HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd()) {
        PostMessage(hTaskbarWnd, g_applyStyleMsg, 1, 0);
    }
}

void SetUpdateState(UpdateState state) {
    UpdateInfo info = GetUpdateInfo();
    info.state = state;
    SetUpdateInfo(info);
}

// Downloads a URL over HTTPS. WinHTTP follows the redirect of release assets,
// which are served from another host.
bool HttpGet(PCWSTR url, PCWSTR headers, std::string* body) {
    WCHAR host[256];
    WCHAR path[2048];
    WCHAR extra[1024];
    URL_COMPONENTS parts{
        .dwStructSize = sizeof(URL_COMPONENTS),
        .lpszHostName = host,
        .dwHostNameLength = ARRAYSIZE(host),
        .lpszUrlPath = path,
        .dwUrlPathLength = ARRAYSIZE(path),
        .lpszExtraInfo = extra,
        .dwExtraInfoLength = ARRAYSIZE(extra),
    };
    if (!WinHttpCrackUrl(url, 0, 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }

    std::wstring object = std::wstring(path) + extra;
    bool result = false;
    HINTERNET session = WinHttpOpen(L"FluentExperience/" WH_MOD_VERSION,
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    if (session) {
        WinHttpSetTimeouts(session, kHttpTimeout, kHttpTimeout, kHttpTimeout,
                           kHttpTimeout);
        connection = WinHttpConnect(session, host, parts.nPort, 0);
    }

    if (connection) {
        request = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    }

    if (request &&
        WinHttpSendRequest(request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                           (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            body->clear();
            result = true;
            while (true) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    result = false;
                    break;
                }
                if (!available) {
                    break;
                }

                size_t offset = body->size();
                body->resize(offset + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, body->data() + offset, available,
                                     &read)) {
                    result = false;
                    break;
                }
                body->resize(offset + read);
            }
        } else {
            DebugLog(L"Update: %s returned %u", url, status);
        }
    }

    for (HINTERNET handle : {request, connection, session}) {
        if (handle) {
            WinHttpCloseHandle(handle);
        }
    }

    return result;
}

// The string values of a key in a JSON text, in order. Enough for the release
// tag and asset URLs, which have no escaped characters.
std::vector<std::wstring> JsonStrings(const std::string& json, const char* key) {
    std::vector<std::wstring> values;
    std::string quotedKey = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(quotedKey, pos)) != std::string::npos) {
        pos += quotedKey.size();
        size_t colon = json.find_first_not_of(" \t\r\n", pos);
        if (colon == std::string::npos || json[colon] != ':') {
            continue;
        }
        size_t start = json.find_first_not_of(" \t\r\n", colon + 1);
        if (start == std::string::npos || json[start] != '"') {
            continue;
        }
        size_t end = json.find('"', start + 1);
        if (end == std::string::npos) {
            break;
        }

        std::string value = json.substr(start + 1, end - start - 1);
        int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         (int)value.size(), nullptr, 0);
        std::wstring wide(length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(),
                            wide.data(), length);
        values.push_back(wide);
        pos = end + 1;
    }

    return values;
}

// "0.15" or "v0.15" as numbers, for comparing versions.
std::vector<int> ParseVersion(std::wstring version) {
    if (!version.empty() && (version[0] == L'v' || version[0] == L'V')) {
        version.erase(0, 1);
    }

    std::vector<int> parts;
    PCWSTR p = version.c_str();
    while (*p) {
        PWSTR end;
        parts.push_back((int)wcstol(p, &end, 10));
        if (*end != L'.') {
            break;
        }
        p = end + 1;
    }

    return parts;
}

bool EndsWith(const std::wstring& text, PCWSTR suffix) {
    size_t length = wcslen(suffix);
    return text.size() >= length &&
           _wcsicmp(text.c_str() + text.size() - length, suffix) == 0;
}

void CheckForUpdates() {
    SetUpdateState(UpdateState::checking);

    UpdateInfo info;
    std::string json;
    if (HttpGet(kLatestReleaseUrl, L"Accept: application/vnd.github+json",
                &json)) {
        auto tags = JsonStrings(json, "tag_name");
        for (const auto& url : JsonStrings(json, "browser_download_url")) {
            if (EndsWith(url, L".dll")) {
                info.dllUrl = url;
            } else if (EndsWith(url, L".wh.cpp")) {
                info.sourceUrl = url;
            }
        }

        if (!tags.empty()) {
            info.version = tags[0];
            if (!info.version.empty() && (info.version[0] == L'v' ||
                                          info.version[0] == L'V')) {
                info.version.erase(0, 1);
            }

            bool newer = ParseVersion(info.version) > ParseVersion(WH_MOD_VERSION);
            info.state = newer && !info.dllUrl.empty() ? UpdateState::available
                                                       : UpdateState::upToDate;
        } else {
            info.state = UpdateState::checkFailed;
        }
    } else {
        info.state = UpdateState::checkFailed;
    }

    GetLocalTime(&info.lastChecked);
    SetUpdateInfo(info);

    DebugLog(L"Update check: state %d, latest %s", (int)info.state,
             info.version.c_str());
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE file = CreateFile(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    bool result = WriteFile(file, data.data(), (DWORD)data.size(), &written,
                            nullptr) &&
                  written == data.size();
    CloseHandle(file);
    return result;
}

// Run elevated: does what the Windhawk editor does for a local mod. The new
// DLL goes next to the current one under a new name, the mod's registry key
// points to it and Windhawk reloads the mod. The current DLL is deleted once
// it's unloaded, and the downloaded files with it.
constexpr char kInstallScript[] = R"(param($ModId, $Version, $NewDll, $Source, $CurrentDll)
$ErrorActionPreference = 'Stop'
$downloadDir = Split-Path $NewDll -Parent
try {
    $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$ModId"
    if (-not (Test-Path $key)) { exit 2 }
    $modsDir = Split-Path $CurrentDll -Parent
    $name = '{0}_{1}_{2}.dll' -f $ModId, $Version, (Get-Random -Minimum 100000 -Maximum 999999)
    Copy-Item $NewDll (Join-Path $modsDir $name) -Force
    $sourceDir = Join-Path (Split-Path (Split-Path (Split-Path $modsDir -Parent) -Parent) -Parent) 'ModsSource'
    if ((Test-Path $Source) -and (Test-Path $sourceDir)) {
        Copy-Item $Source (Join-Path $sourceDir "$ModId.wh.cpp") -Force
    }
    Set-ItemProperty $key -Name LibraryFileName -Value $name -Type String
    Set-ItemProperty $key -Name Version -Value $Version -Type String
    $changeTime = [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() -band 0x7fffffff)
    Set-ItemProperty $key -Name SettingsChangeTime -Value $changeTime -Type DWord
    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Seconds 2
        try { Remove-Item $CurrentDll -Force; break } catch {}
    }
    exit 0
} catch {
    exit 1
} finally {
    Remove-Item $downloadDir -Recurse -Force -ErrorAction SilentlyContinue
}
)";

void InstallUpdate() {
    UpdateInfo info = GetUpdateInfo();
    if (info.state != UpdateState::available) {
        return;
    }

    SetUpdateState(UpdateState::installing);

    WCHAR tempPath[MAX_PATH];
    GetTempPath(ARRAYSIZE(tempPath), tempPath);
    std::wstring dir = std::wstring(tempPath) + L"FluentExperienceUpdate";
    CreateDirectory(dir.c_str(), nullptr);
    std::wstring dllPath = dir + L"\\fluent-experience.dll";
    std::wstring sourcePath = dir + L"\\fluent-experience.wh.cpp";
    std::wstring scriptPath = dir + L"\\install.ps1";

    std::string dll;
    std::string source;
    bool ok = HttpGet(info.dllUrl.c_str(), nullptr, &dll) &&
              dll.size() > 2 && dll[0] == 'M' && dll[1] == 'Z' &&
              WriteFileBytes(dllPath, dll);
    if (ok && !info.sourceUrl.empty() &&
        HttpGet(info.sourceUrl.c_str(), nullptr, &source)) {
        WriteFileBytes(sourcePath, source);
    }
    ok = ok && WriteFileBytes(scriptPath, kInstallScript);

    WCHAR currentDll[MAX_PATH];
    GetModuleFileName(GetCurrentModuleHandle(), currentDll,
                      ARRAYSIZE(currentDll));

    // Without the install, nothing downloaded stays behind and the update can
    // be tried again.
    auto abandon = [&] {
        DeleteFile(dllPath.c_str());
        DeleteFile(sourcePath.c_str());
        DeleteFile(scriptPath.c_str());
        RemoveDirectory(dir.c_str());
        SetUpdateState(UpdateState::available);
    };

    if (!ok) {
        DebugLog(L"Update: download failed");
        abandon();
        return;
    }

    WCHAR systemDir[MAX_PATH];
    GetSystemDirectory(systemDir, ARRAYSIZE(systemDir));
    std::wstring powershell =
        std::wstring(systemDir) + LR"(\WindowsPowerShell\v1.0\powershell.exe)";
    std::wstring parameters =
        L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
        scriptPath + L"\" -ModId \"" + WH_MOD_ID + L"\" -Version \"" +
        info.version + L"\" -NewDll \"" + dllPath + L"\" -Source \"" +
        sourcePath + L"\" -CurrentDll \"" + currentDll + L"\"";

    SHELLEXECUTEINFO execute{
        .cbSize = sizeof(SHELLEXECUTEINFO),
        .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
        .hwnd = g_settingsWindow,
        .lpVerb = L"runas",
        .lpFile = powershell.c_str(),
        .lpParameters = parameters.c_str(),
        .nShow = SW_HIDE,
    };
    if (!ShellExecuteEx(&execute) || !execute.hProcess) {
        // Also when the permission request is declined.
        DebugLog(L"Update: install not started: %u", GetLastError());
        abandon();
        return;
    }

    // Windhawk unloads the mod while the script runs: the stop event ends the
    // wait then. An exit code is left only when something went wrong.
    HANDLE events[] = {g_updateStopEvent, execute.hProcess};
    DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE,
                                        kHttpTimeout * 4);
    DWORD exitCode = 0;
    if (wait == WAIT_TIMEOUT ||
        (wait == WAIT_OBJECT_0 + 1 &&
         GetExitCodeProcess(execute.hProcess, &exitCode) && exitCode != 0)) {
        DebugLog(L"Update: install failed: %u", exitCode);
        SetUpdateState(UpdateState::available);
    }
    CloseHandle(execute.hProcess);
}

DWORD WINAPI UpdateThreadProc(LPVOID) {
    HANDLE events[] = {g_updateStopEvent, g_updateCheckEvent,
                       g_updateInstallEvent};
    ULONGLONG nextCheck = GetTickCount64() + kFirstUpdateCheckDelay;

    while (true) {
        ULONGLONG now = GetTickCount64();
        DWORD timeout = nextCheck > now ? (DWORD)(nextCheck - now) : 0;
        DWORD wait =
            WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, timeout);

        if (wait == WAIT_OBJECT_0 + 1) {
            CheckForUpdates();
            nextCheck = GetTickCount64() + kUpdateCheckInterval;
        } else if (wait == WAIT_OBJECT_0 + 2) {
            InstallUpdate();
        } else if (wait == WAIT_TIMEOUT) {
            if (g_settings.checkUpdates) {
                CheckForUpdates();
            }
            nextCheck = GetTickCount64() + kUpdateCheckInterval;
        } else {
            break;
        }
    }

    return 0;
}

void StartUpdateThread() {
    g_updateStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_updateCheckEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_updateInstallEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_updateThread =
        CreateThread(nullptr, 0, UpdateThreadProc, nullptr, 0, nullptr);
}

void StopUpdateThread() {
    if (g_updateThread) {
        SetEvent(g_updateStopEvent);
        WaitForSingleObject(g_updateThread, INFINITE);
        CloseHandle(g_updateThread);
        g_updateThread = nullptr;
    }

    for (HANDLE* event :
         {&g_updateStopEvent, &g_updateCheckEvent, &g_updateInstallEvent}) {
        if (*event) {
            CloseHandle(*event);
            *event = nullptr;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

BOOL Wh_ModInit() {
    LoadSettings();
    RemoveSettingsAppLeftovers();
    TakeOpenSettingsRequest();
    RegisterSettingsLink();

    DebugLog(L"Init");

    if (!HookTaskbarDllSymbols()) {
        return FALSE;
    }

    HMODULE systemTrayModule = GetModuleHandle(L"SystemTray.dll");
    if (systemTrayModule) {
        g_systemTrayDllHooked = true;
        if (!HookSystemTrayDllSymbols(systemTrayModule)) {
            DebugLog(L"SystemTray.dll hooks failed");
        }
    }

    HMODULE taskbarViewModule = GetTaskbarViewModuleHandle();
    if (taskbarViewModule) {
        g_taskbarViewDllHooked = true;
        if (!HookTaskbarViewDllSymbols(taskbarViewModule)) {
            DebugLog(L"Taskbar.View.dll hooks failed");
        }
    }

    if (!taskbarViewModule || !systemTrayModule) {
        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        auto pKernelBaseLoadLibraryExW =
            (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule,
                                                      "LoadLibraryExW");
        WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                       LoadLibraryExW_Hook,
                                       &LoadLibraryExW_Original);
    }

    WindhawkUtils::SetFunctionHook(SubtractRect, SubtractRect_Hook,
                                   &SubtractRect_Original);

    if (HMODULE user32Module = GetModuleHandle(L"user32.dll")) {
        pGetWindowCompositionAttribute =
            (GetWindowCompositionAttribute_t)GetProcAddress(
                user32Module, "GetWindowCompositionAttribute");
        if (auto pSetWindowCompositionAttribute =
                (SetWindowCompositionAttribute_t)GetProcAddress(
                    user32Module, "SetWindowCompositionAttribute")) {
            WindhawkUtils::SetFunctionHook(pSetWindowCompositionAttribute,
                                           SetWindowCompositionAttribute_Hook,
                                           &SetWindowCompositionAttribute_Original);
        }
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    DebugLog(L"AfterInit");

    if (!g_taskbarViewDllHooked) {
        if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle()) {
            HandleLoadedModuleIfTaskbarView(taskbarViewModule,
                                            L"Taskbar.View.dll");
        }
    }

    if (!g_systemTrayDllHooked) {
        if (HMODULE systemTrayModule = GetModuleHandle(L"SystemTray.dll")) {
            HandleLoadedModuleIfSystemTray(systemTrayModule, L"SystemTray.dll");
        }
    }

    ApplySettings();

    StartSettingsWatcher();
    StartUpdateThread();
}

void Wh_ModBeforeUninit() {
    DebugLog(L"BeforeUninit");

    // The watcher first: the link can't open the window again while it closes.
    StopSettingsWatcher();
    StopUpdateThread();
    CloseSettingsWindow();
    UnregisterSettingsLink();

    g_unloading = true;

    {
        std::lock_guard<std::mutex> guard(g_dockWidthsMutex);
        g_dockWidths.clear();
    }

    ApplySettings();
}

void Wh_ModUninit() {
    while (g_hookCallCounter > 0) {
        Sleep(100);
    }
}
