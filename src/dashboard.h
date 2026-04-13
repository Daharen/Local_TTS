#pragma once

#ifdef _WIN32
#include <windows.h>
#else
using HWND = void*;
#endif

namespace dashboard {

#ifndef LOCAL_TTS_ENABLE_DASHBOARD
#define LOCAL_TTS_ENABLE_DASHBOARD 1
#endif

#if LOCAL_TTS_ENABLE_DASHBOARD
bool show_dashboard_window(HWND owner, bool debug_console = false) noexcept;
void close_dashboard_window() noexcept;
#else
inline bool show_dashboard_window(HWND, bool = false) noexcept { return false; }
inline void close_dashboard_window() noexcept {}
#endif

}  // namespace dashboard
