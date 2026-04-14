#pragma once

#include <string>

namespace pipeline_debug {

bool enabled() noexcept;
void log(const std::string& stage, const std::string& message, bool is_error = false) noexcept;

}  // namespace pipeline_debug
