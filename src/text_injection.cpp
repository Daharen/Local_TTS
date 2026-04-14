#include "text_injection.h"
#include "pipeline_debug.h"

#ifdef _WIN32

#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
    return out;
}

void send_ctrl_v() {
    INPUT inputs[4] = {};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(4, inputs, sizeof(INPUT));
}

void release_modifiers() {
    constexpr std::array<WORD, 11> kModifierKeys = {
        VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU,  VK_LMENU, VK_RMENU,
        VK_SHIFT,   VK_LSHIFT,   VK_RSHIFT,   VK_LWIN,  VK_RWIN,
    };

    std::vector<INPUT> inputs;
    inputs.reserve(kModifierKeys.size());
    for (WORD key : kModifierKeys) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

}  // namespace

bool inject_text_via_clipboard_paste(HWND target_window, const std::string& utf8_text, std::string& error_out) {
    error_out.clear();
    pipeline_debug::log("text_injection", "[INJECT_BEGIN] utf8_chars=" + std::to_string(utf8_text.size()));
    const std::wstring wide = utf8_to_wide(utf8_text);
    if (wide.empty()) {
        error_out = "Transcript is empty or UTF-8 conversion failed.";
        pipeline_debug::log("text_injection", "[INJECT_END] ok=false reason=" + error_out, true);
        return false;
    }

    if (!target_window || !IsWindow(target_window) || !IsWindowVisible(target_window)) {
        error_out = "Target window is invalid.";
        pipeline_debug::log("text_injection", "[INJECT_END] ok=false reason=" + error_out, true);
        return false;
    }

    if (GetForegroundWindow() != target_window) {
        error_out = "Target window is not foreground; skipping non-disruptive paste.";
        pipeline_debug::log("text_injection", "[INJECT_END] ok=false reason=" + error_out, true);
        return false;
    }

    if (!OpenClipboard(nullptr)) {
        error_out = "Failed to open clipboard.";
        pipeline_debug::log("text_injection", "[INJECT_END] ok=false reason=" + error_out, true);
        return false;
    }

    if (!EmptyClipboard()) {
        CloseClipboard();
        error_out = "Failed to clear clipboard.";
        pipeline_debug::log("text_injection", "[INJECT_END] ok=false reason=" + error_out, true);
        return false;
    }

    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        error_out = "Failed to allocate clipboard memory.";
        pipeline_debug::log("text_injection", "[INJECT_END] ok=false reason=" + error_out, true);
        return false;
    }

    void* ptr = GlobalLock(mem);
    std::memcpy(ptr, wide.c_str(), bytes);
    GlobalUnlock(mem);

    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        error_out = "Failed to set clipboard data.";
        pipeline_debug::log("text_injection", "[INJECT_END] ok=false reason=" + error_out, true);
        return false;
    }

    CloseClipboard();
    release_modifiers();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    send_ctrl_v();
    pipeline_debug::log("text_injection",
                        "[INJECT_END] ok=true utf16_chars=" + std::to_string(wide.size()) + " bytes=" + std::to_string(bytes));
    return true;
}

#else

bool inject_text_via_clipboard_paste(void*, const std::string&, std::string& error_out) {
    error_out = "Text injection is only supported on Windows.";
    return false;
}

#endif
