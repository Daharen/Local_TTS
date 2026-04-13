#pragma once

#ifdef _WIN32
#include <windows.h>
#else
using HWND = void*;
#endif

namespace dashboard {

bool show_dashboard_window(HWND owner, bool debug_console = false) noexcept;
void close_dashboard_window() noexcept;

}  // namespace dashboard
