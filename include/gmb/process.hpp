#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace gmb::io {
struct ProcessResult {
    int exit_code=0;
    bool timed_out=false, stdout_truncated=false, stderr_truncated=false;
    std::string stdout_tail, stderr_tail;
};
// No shell. Bounded stream capture; terminate the entire job on timeout or
// parent death. The optional MAX adapter is unavailable on non-Windows hosts.
ProcessResult run_max_process(const std::vector<std::string>& arguments,
                             const std::filesystem::path& directory,
                             unsigned timeout_seconds);
}
