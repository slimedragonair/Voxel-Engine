#include <voxel/world/ChunkStreamer.hpp>

#include <algorithm>
#include <cmath>

namespace voxel::world {

ChunkStreamer::ChunkStreamer(ChunkManager& chunks)
    : chunks_(chunks)
{
}

std::vector<ChunkRequest> ChunkStreamer::planRequests(ChunkCoord center, const StreamingSettings& settings) const
{
    return planRequests(center, settings, {});
}

std::vector<ChunkRequest> ChunkStreamer::planRequests(ChunkCoord center, const StreamingSettings& settings, core::Vec3 forward) const
{
    const int hRadius = settings.renderDistanceChunks;
    const int vRadius = settings.verticalRenderDistanceChunks;
    std::vector<ChunkRequest> requests;
    requests.reserve(static_cast<std::size_t>((hRadius * 2 + 1) * (vRadius * 2 + 1) * (hRadius * 2 + 1)));

    const float forwardLength = std::sqrt((forward.x * forward.x) + (forward.z * forward.z));
    const float forwardX = forwardLength > 0.0F ? forward.x / forwardLength : 0.0F;
    const float forwardZ = forwardLength > 0.0F ? forward.z / forwardLength : 0.0F;

    for (int dz = -hRadius; dz <= hRadius; ++dz) {
        for (int dy = -vRadius; dy <= vRadius; ++dy) {
            for (int dx = -hRadius; dx <= hRadius; ++dx) {
                const float horizontalDistance2 = static_cast<float>((dx * dx) + (dz * dz));
                const float verticalDistance2 = static_cast<float>(dy * dy);
                const float forwardDot = static_cast<float>(dx) * forwardX + static_cast<float>(dz) * forwardZ;
                const float forwardBias = std::max(0.0F, forwardDot) * 0.25F;
                const float priority = horizontalDistance2 + (verticalDistance2 * 4.0F) - forwardBias;
                requests.push_back({{center.x + dx, center.y + dy, center.z + dz}, priority});
            }
        }
    }

    std::sort(requests.begin(), requests.end(), [](const ChunkRequest& lhs, const ChunkRequest& rhs) {
        if (lhs.priority == rhs.priority) {
            return lhs.coord.y < rhs.coord.y;
        }
        return lhs.priority < rhs.priority;
    });

    return requests;
}

void ChunkStreamer::pump(ChunkCoord center, const StreamingSettings& settings)
{
    for (const auto& request : planRequests(center, settings)) {
        auto& chunk = chunks_.createOrGet(request.coord);
        if (chunk.state() == ChunkState::Empty) {
            chunk.setState(ChunkState::Requested);
        }
    }
}

} // namespace voxel::world
