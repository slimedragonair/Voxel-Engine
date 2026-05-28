#pragma once

#include <filesystem>
#include <optional>

#include <voxel/core/Math.hpp>

namespace voxel::save {

// Snapshot of the player's transient state that should survive a save → load
// round-trip. Inventory is handled separately by PlayerInventorySaveService.
struct PlayerStateSnapshot {
    static constexpr std::uint32_t kFormatVersion = 1;

    core::DVec3 position{};
    float yawRadians{0.0F};
    float pitchRadians{0.0F};
    bool noclip{false};
    std::uint32_t formatVersion{kFormatVersion};
};

// Reads/writes `<worldRoot>/player/state.json`. Companion to
// PlayerInventorySaveService — they share the same `player/` subdirectory but
// are independent so the inventory can keep its own serialisation cadence.
class PlayerStateSaveService {
public:
    [[nodiscard]] bool save(const std::filesystem::path& worldRoot,
                            const PlayerStateSnapshot& state) const;

    [[nodiscard]] std::optional<PlayerStateSnapshot> load(
        const std::filesystem::path& worldRoot) const;

    [[nodiscard]] std::filesystem::path statePath(const std::filesystem::path& worldRoot) const;
};

} // namespace voxel::save
