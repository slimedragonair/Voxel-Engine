#include <voxel/save/PlayerStateSaveService.hpp>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace voxel::save {

namespace {

constexpr const char* kPlayerSubdir = "player";
constexpr const char* kStateFile = "state.json";

} // namespace

std::filesystem::path PlayerStateSaveService::statePath(const std::filesystem::path& worldRoot) const
{
    return worldRoot / kPlayerSubdir / kStateFile;
}

bool PlayerStateSaveService::save(const std::filesystem::path& worldRoot,
                                   const PlayerStateSnapshot& state) const
{
    const auto path = statePath(worldRoot);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    // Doubles for position so we don't lose precision when the player has
    // wandered far from origin. Yaw/pitch stay as floats — angular precision
    // doesn't compound with distance.
    nlohmann::json doc{
        {"position", {state.position.x, state.position.y, state.position.z}},
        {"yawRadians", state.yawRadians},
        {"pitchRadians", state.pitchRadians},
        {"noclip", state.noclip},
        {"formatVersion", state.formatVersion},
    };

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << doc.dump(2);
    return static_cast<bool>(out);
}

std::optional<PlayerStateSnapshot> PlayerStateSaveService::load(
    const std::filesystem::path& worldRoot) const
{
    const auto path = statePath(worldRoot);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(buffer.str(), nullptr, true, /*ignore_comments*/ true);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!doc.is_object()) {
        return std::nullopt;
    }

    PlayerStateSnapshot snapshot{};
    if (auto it = doc.find("position"); it != doc.end() && it->is_array() && it->size() == 3) {
        // Each component goes through json::value with a default to tolerate
        // partial corruption — a NaN field shouldn't sink the whole load.
        snapshot.position.x = (*it)[0].is_number() ? (*it)[0].get<double>() : 0.0;
        snapshot.position.y = (*it)[1].is_number() ? (*it)[1].get<double>() : 0.0;
        snapshot.position.z = (*it)[2].is_number() ? (*it)[2].get<double>() : 0.0;
    }
    if (auto it = doc.find("yawRadians"); it != doc.end() && it->is_number()) {
        snapshot.yawRadians = it->get<float>();
    }
    if (auto it = doc.find("pitchRadians"); it != doc.end() && it->is_number()) {
        snapshot.pitchRadians = it->get<float>();
    }
    if (auto it = doc.find("noclip"); it != doc.end() && it->is_boolean()) {
        snapshot.noclip = it->get<bool>();
    }
    if (auto it = doc.find("formatVersion"); it != doc.end() && it->is_number_unsigned()) {
        snapshot.formatVersion = it->get<std::uint32_t>();
    }
    return snapshot;
}

} // namespace voxel::save
