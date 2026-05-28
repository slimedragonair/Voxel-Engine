#pragma once

#include <cstdint>
#include <string>

namespace voxel::world {

// On-disk identity of a single save world. Lives in `<worldRoot>/world.json`.
// Created when a new world is requested from the main menu; updated on
// shutdown to bump `lastPlayedAtMs`.
struct WorldDescriptor {
    // Increment when the on-disk format changes incompatibly. Loaders should
    // refuse to read unknown future versions and migrate older ones.
    static constexpr std::uint32_t kFormatVersion = 1;

    std::string  name;             // Human-readable + on-disk directory name.
    std::uint64_t seed{0};         // Drives every deterministic generator.
    std::int64_t  createdAtMs{0};  // Unix ms.
    std::int64_t  lastPlayedAtMs{0};
    std::uint32_t formatVersion{kFormatVersion};
};

} // namespace voxel::world
