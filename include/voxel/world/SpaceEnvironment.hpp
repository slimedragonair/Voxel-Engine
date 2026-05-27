#pragma once

#include <cstdint>
#include <vector>

#include <voxel/core/Math.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::world {

enum class SpaceFeatureType : std::uint8_t {
    Empty,
    AsteroidCluster,
    IceField,
    MetalRichAsteroids,
    CrystalAsteroids,
    DustCloud,
    Comet,
    Planet,
    Moon,
    RingDebris,
    NaturalAnomaly,
    AncientRuin,
    DerelictShip,
    BiologicalSignal,
    ArtifactSite
};

enum class FeatureOrigin : std::uint8_t {
    Natural,
    AncientExtinctCivilization,
    DerelictUnknown,
    Biological,
    PlayerMade,
    Modded
};

enum class SpaceBodyClass : std::uint8_t {
    None,
    DeadRocky,
    Frozen,
    MetalRich,
    Volcanic,
    Toxic,
    Ocean,
    GasGiant,
    LifeBearing,
    Crystal
};

struct SpaceSettings {
    // First implementation target from the design doc: near-space starts well
    // above current terrain while still reachable in block coordinates.
    float atmosphereTopY{10000.0F};
    float nearSpaceStartY{8000.0F};
    float gravityFalloffStartY{512.0F};
    float zeroGravityY{10000.0F};
    int sectorSizeBlocks{2048};
    std::uint64_t seed{0xA37B4F1D6C925E3BULL};
};

struct SpaceEnvironmentState {
    float altitudeY{};
    float atmosphereDensity{1.0F};
    float spaceBlend{};
    float gravityScale{1.0F};
    bool inNearSpace{};
    bool inSpace{};
};

struct SpaceSectorCoord {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};

    [[nodiscard]] bool operator==(const SpaceSectorCoord& other) const noexcept = default;
};

struct SpaceFeature {
    SpaceFeatureType type{SpaceFeatureType::Empty};
    FeatureOrigin origin{FeatureOrigin::Natural};
    SpaceSectorCoord sector{};
    core::Vec3 localOffset{};
    float radius{};
    std::uint32_t resourceSeed{};
    SpaceBodyClass bodyClass{SpaceBodyClass::None};
    float gravityScale{};
    float atmosphereDensity{};
    float surfaceRoughness{};
    float oceanCoverage{};
    float resourceRichness{};
    float lifeSignal{};
    bool landable{};
};

class SpaceEnvironment {
public:
    explicit SpaceEnvironment(SpaceSettings settings = {});

    [[nodiscard]] const SpaceSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] SpaceEnvironmentState evaluate(float worldY) const noexcept;
    [[nodiscard]] SpaceSectorCoord sectorFor(core::Vec3 worldPosition) const noexcept;
    [[nodiscard]] core::Vec3 featureWorldCenter(const SpaceFeature& feature) const noexcept;
    [[nodiscard]] std::vector<SpaceFeature> featuresForSector(SpaceSectorCoord sector) const;

private:
    SpaceSettings settings_{};
};

} // namespace voxel::world
