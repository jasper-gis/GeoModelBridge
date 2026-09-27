#include "gmb/process.hpp"
#include <stdexcept>
namespace gmb::io {
ProcessResult run_max_process(const std::vector<std::string>&,
                             const std::filesystem::path&,unsigned) {
    throw std::runtime_error("MAX_PLATFORM_UNSUPPORTED: 3ds Max Batch requires Windows. Export FBX on Windows, then use the shared Linux pipeline.");
}
}
