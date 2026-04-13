#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

bool inject_text_via_clipboard_paste(
#ifdef _WIN32
    HWND target_window,
#else
    void* target_window,
#endif
    const std::string& utf8_text,
    std::string& error_out);
