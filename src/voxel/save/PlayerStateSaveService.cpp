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
        doc = nlohmann::json::parse(buffer.str(), nullptr, true, true);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!doc.is_object()) {
        return std::nullopt;
    }
    PlayerStateSnapshot s{};
    if (auto it = doc.find("position"); it != doc.end() && it->is_array() && it->size() == 3) {
        s.position.x = (*it)[0].is_number() ? (*it)[0].get<double>() : 0.0;
        s.position.y = (*it)[1].is_number() ? (*it)[1].get<double>() : 0.0;
        s.position.z = (*it)[2].is_number() ? (*it)[2].get<double>() : 0.0;
    }
    if (auto it = doc.find("yawRadians"); it != doc.end() && it->is_number()) {
        s.yawRadians = it->get<float>();
    }
    if (auto it = doc.find("pitchRadians"); it != doc.end() && it->is_number()) {
        s.pitchRadians = it->get<float>();
    }
    if (auto it = doc.find("noclip"); it != doc.end() && it->is_boolean()) {
        s.noclip = it->get<bool>();
    }
    if (auto it = doc.find("formatVersion"); it != doc.end() && it->is_number_unsigned()) {
        s.formatVersion = it->get<std::uint32_t>();
    }
    return s;
}

} // namespace voxel::save
