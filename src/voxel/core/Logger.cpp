#include <voxel/core/Logger.hpp>

#include <iostream>

namespace voxel {

// std::endl forces a flush on every log line. The few percent perf cost vs
// '\n' is well worth it: when the engine crashes, every log line up to the
// fault is guaranteed visible on disk / in the console (vs being lost in a
// buffer that never flushes). All three streams are used for diagnostics
// only, never for hot per-frame paths.
void Logger::info(std::string_view message)
{
    std::cout << "[info] " << message << std::endl;
}

void Logger::warn(std::string_view message)
{
    std::cout << "[warn] " << message << std::endl;
}

void Logger::error(std::string_view message)
{
    std::cerr << "[error] " << message << std::endl;
}

} // namespace voxel

