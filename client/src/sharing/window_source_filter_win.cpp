#include "window_source_filter.h"

#include <qt_windows.h>
#include <dwmapi.h>
#include <string>

namespace tmc {
namespace {
BOOL CALLBACK collectWindow(HWND window, LPARAM data) {
    if (!IsWindowVisible(window)) return TRUE;
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return TRUE;
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, buffer.data(), static_cast<int>(buffer.size()));
    if (copied <= 0) return TRUE;
    const auto title = QString::fromWCharArray(buffer.data(), copied);
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    DWORD cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT bounds{};
    const bool allowed = window != GetShellWindow() && window != GetDesktopWindow()
        && !IsIconic(window) && !cloaked
        && !(style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
        && (!GetWindow(window, GW_OWNER) || (style & WS_EX_APPWINDOW))
        && GetWindowRect(window, &bounds)
        && bounds.right > bounds.left && bounds.bottom > bounds.top;
    auto& titles = *reinterpret_cast<ApplicationWindowTitles*>(data);
    rememberWindowTitle(titles, title, title.trimmed(), allowed);
    return TRUE;
}
} // namespace

ApplicationWindowTitles applicationWindowTitles() {
    ApplicationWindowTitles titles;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&titles));
    return titles;
}
} // namespace tmc
