#include "pipeline_debug.h"

#include "paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace pipeline_debug {
namespace {

std::mutex& log_mutex() {
    static std::mutex mu;
    return mu;
}

std::filesystem::path common_log_path() {
    return get_large_data_root() / "output" / "debug" / "pipeline.common.log";
}

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

}  // namespace

bool enabled() noexcept {
    return is_pipeline_debug_enabled();
}

void log(const std::string& stage, const std::string& message, bool is_error) noexcept {
    if (!enabled()) {
        return;
    }

    try {
        const auto log_path = common_log_path();
        std::lock_guard<std::mutex> guard(log_mutex());
        std::filesystem::create_directories(log_path.parent_path());

        std::ofstream out(log_path, std::ios::app);
        if (!out) {
            return;
        }

        out << '[' << timestamp_now() << "] "
            << '[' << (is_error ? "ERROR" : "INFO") << "] "
            << '[' << stage << "] "
            << message << '\n';
    } catch (...) {
    }
}

}  // namespace pipeline_debug
