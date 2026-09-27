#pragma once
#include "gmb/scene.hpp"

namespace gmb {
struct MaxOptions {
    std::filesystem::path batch_executable;
    int frame=0;
    unsigned timeout_seconds=600;
};
// Optional Windows adapter. The returned scene owns all texture bytes; its
// source remains the original MAX file after the private workspace is removed.
Scene read_max(const std::filesystem::path& source, const ReaderOptions& reader,
               const MaxOptions& options, const std::filesystem::path& worker);
}
