#include <voxel/render/meshing/ClusterMeshDiskCache.hpp>

#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include <voxel/core/JobSystem.hpp>
#include <voxel/core/Logger.hpp>

namespace voxel::render::meshing {

namespace {

constexpr std::uint32_t kMagic   = 0x43444F4Cu; // 'LODC' little-endian
constexpr std::uint32_t kVersion = 1u;

// Sanity caps so a corrupted file doesn't make us allocate gigabytes.
// 64³ supervoxels * 6 faces * 4 verts = 1.5M verts upper bound on
// content; realistic clusters are < 100k. The cap is generous so any
// legitimate mesh fits.
constexpr std::uint32_t kMaxVertices   = 2'000'000u;
constexpr std::uint32_t kMaxIndices    = 6'000'000u;
constexpr std::uint32_t kMaxDrawRanges = 64'000u;

template <typename T>
bool readPod(std::ifstream& s, T& out)
{
    s.read(reinterpret_cast<char*>(&out), sizeof(T));
    return static_cast<bool>(s);
}

template <typename T>
bool writePod(std::ofstream& s, const T& value)
{
    s.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(s);
}

// DrawRange uses an enum with potential padding — serialize as a fixed
// 16-byte layout (1B surface + 3B pad + 4B materialId + 4B offset + 4B count)
// so any future struct-layout changes don't silently break the cache.
struct PackedDrawRange {
    std::uint8_t  surface{};
    std::uint8_t  pad0{};
    std::uint8_t  pad1{};
    std::uint8_t  pad2{};
    std::uint32_t materialId{};
    std::uint32_t indexOffset{};
    std::uint32_t indexCount{};
};
static_assert(sizeof(PackedDrawRange) == 16, "PackedDrawRange must be 16 bytes");

} // namespace

bool ClusterMeshDiskCache::initialize(std::filesystem::path baseDir)
{
    baseDir_ = std::move(baseDir);
    std::error_code ec;
    std::filesystem::create_directories(baseDir_, ec);
    if (ec) {
        Logger::warn("ClusterMeshDiskCache::initialize: failed to create cache dir '"
            + baseDir_.string() + "': " + ec.message());
        initialized_ = false;
        return false;
    }
    initialized_ = true;
    Logger::info("ClusterMeshDiskCache: cache dir ready at " + baseDir_.string());
    return true;
}

std::filesystem::path ClusterMeshDiskCache::pathFor(world::ClusterCoord coord) const
{
    // Flat naming: works fine up to ~10k files per dir on modern FS.
    // If cluster counts get huge, switch to a 2-level hash bucket.
    return baseDir_ / (std::to_string(coord.x) + "_"
        + std::to_string(coord.y) + "_"
        + std::to_string(coord.z) + ".lodc");
}

std::optional<ClusterMesh> ClusterMeshDiskCache::tryLoad(
    world::ClusterCoord coord, std::uint64_t expectedHash) const
{
    if (!initialized_) {
        return std::nullopt;
    }
    const auto path = pathFor(coord);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    // Header.
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t hash = 0;
    std::int64_t cx = 0, cy = 0, cz = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t drawRangeCount = 0;
    if (!readPod(file, magic) || magic != kMagic) return std::nullopt;
    if (!readPod(file, version) || version != kVersion) return std::nullopt;
    if (!readPod(file, hash)) return std::nullopt;
    if (hash != expectedHash) {
        // Stale cache entry — source chunks have changed since this was
        // written. Don't load; the caller will rebuild + overwrite us.
        return std::nullopt;
    }
    if (!readPod(file, cx) || !readPod(file, cy) || !readPod(file, cz)) {
        return std::nullopt;
    }
    if (cx != coord.x || cy != coord.y || cz != coord.z) {
        // Coord mismatch — file was probably renamed/swapped. Treat
        // as miss; safer than loading wrong data.
        return std::nullopt;
    }
    if (!readPod(file, vertexCount) || vertexCount > kMaxVertices) return std::nullopt;
    if (!readPod(file, indexCount)  || indexCount  > kMaxIndices)  return std::nullopt;
    if (!readPod(file, drawRangeCount) || drawRangeCount > kMaxDrawRanges) return std::nullopt;

    // Payload.
    ClusterMesh mesh;
    mesh.coord = coord;
    mesh.sourceRevisionsHash = hash;
    mesh.vertices.resize(vertexCount);
    mesh.indices.resize(indexCount);
    mesh.drawRanges.resize(drawRangeCount);

    if (vertexCount > 0) {
        file.read(reinterpret_cast<char*>(mesh.vertices.data()),
                  static_cast<std::streamsize>(vertexCount) * sizeof(ClusterVertex));
        if (!file) return std::nullopt;
    }
    if (indexCount > 0) {
        file.read(reinterpret_cast<char*>(mesh.indices.data()),
                  static_cast<std::streamsize>(indexCount) * sizeof(std::uint32_t));
        if (!file) return std::nullopt;
    }
    for (std::uint32_t i = 0; i < drawRangeCount; ++i) {
        PackedDrawRange packed{};
        if (!readPod(file, packed)) return std::nullopt;
        mesh.drawRanges[i].surface = static_cast<MeshSurface>(packed.surface);
        mesh.drawRanges[i].materialId = packed.materialId;
        mesh.drawRanges[i].indexOffset = packed.indexOffset;
        mesh.drawRanges[i].indexCount = packed.indexCount;
    }
    return mesh;
}

void ClusterMeshDiskCache::storeAsync(core::JobSystem& jobs,
                                       world::ClusterCoord coord,
                                       const ClusterMesh& mesh) const
{
    if (!initialized_) {
        return;
    }

    // Copy the mesh into a job-owned lambda capture. The mesh data is
    // ~100-300 KB; the copy is cheap compared to the disk write that
    // follows. Avoids any lifetime/sync issues with the source mesh.
    const auto path = pathFor(coord);
    const auto hash = mesh.sourceRevisionsHash;

    struct Snapshot {
        std::vector<ClusterVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<MeshDrawRange> drawRanges;
        std::filesystem::path path;
        std::uint64_t hash;
        world::ClusterCoord coord;
    };
    auto snapshot = std::make_shared<Snapshot>(Snapshot{
        mesh.vertices, mesh.indices, mesh.drawRanges,
        path, hash, coord
    });

    // Fire-and-forget. The Application doesn't track these futures —
    // cluster cache writes are best-effort. If the write fails the
    // worst case is the cluster gets re-classified next session.
    (void)jobs.submit({"cluster.save", core::JobPriority::Low},
        [snapshot]() {
            std::ofstream file(snapshot->path, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                return std::size_t{0};
            }
            writePod(file, kMagic);
            writePod(file, kVersion);
            writePod(file, snapshot->hash);
            writePod(file, snapshot->coord.x);
            writePod(file, snapshot->coord.y);
            writePod(file, snapshot->coord.z);
            const std::uint32_t vc = static_cast<std::uint32_t>(snapshot->vertices.size());
            const std::uint32_t ic = static_cast<std::uint32_t>(snapshot->indices.size());
            const std::uint32_t dc = static_cast<std::uint32_t>(snapshot->drawRanges.size());
            writePod(file, vc);
            writePod(file, ic);
            writePod(file, dc);
            if (vc > 0) {
                file.write(reinterpret_cast<const char*>(snapshot->vertices.data()),
                           static_cast<std::streamsize>(vc) * sizeof(ClusterVertex));
            }
            if (ic > 0) {
                file.write(reinterpret_cast<const char*>(snapshot->indices.data()),
                           static_cast<std::streamsize>(ic) * sizeof(std::uint32_t));
            }
            for (const auto& dr : snapshot->drawRanges) {
                PackedDrawRange packed{};
                packed.surface     = static_cast<std::uint8_t>(dr.surface);
                packed.materialId  = dr.materialId;
                packed.indexOffset = dr.indexOffset;
                packed.indexCount  = dr.indexCount;
                writePod(file, packed);
            }
            return file ? std::size_t{1} : std::size_t{0};
        });
}

} // namespace voxel::render::meshing
