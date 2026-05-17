#include <voxel/core/Logger.hpp>

#include <iostream>

namespace voxel {

void Logger::info(std::string_view message)
{
    std::cout << "[info] " << message << '\n';
}

void Logger::warn(std::string_view message)
{
    std::cout << "[warn] " << message << '\n';
}

void Logger::error(std::string_view message)
{
    std::cerr << "[error] " << message << '\n';
}

} // namespace voxel

