#include <voxel/world/SpaceEnvironment.hpp>

#include <algorithm>
#include <cmath>

#include <voxel/world/CoordinateUtils.hpp>

namespace voxel::world {

namespace {

[[nodiscard]] float saturate(float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept
{
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

[[nodiscard]] std::uint64_t sectorHash(SpaceSectorCoord sector, std::uint64_t seed) noexcept
{
    auto h = seed;
    h ^= mix64(static_cast<std::uint64_t>(sector.x) + 0x9e3779b97f4a7c15ULL);
    h = mix64(h);
    h ^= mix64(static_cast<std::uint64_t>(sector.y) + 0xbf58476d1ce4e5b9ULL);
    h = mix64(h);
    h ^= mix64(static_cast<std::uint64_t>(sector.z) + 0x94d049bb133111ebULL);
    return mix64(h);
}

[[nodiscard]] float unitFloat(std::uint64_t& state) noexcept
{
    state = mix64(state + 0x9e3779b97f4a7c15ULL);
    return static_cast<float>((state >> 40U) & 0xFFFFFFU) / static_cast<float>(0xFFFFFFU);
}

[[nodiscard]] SpaceFeatureType naturalFeatureForRoll(float roll) noexcept
{
    if (roll < 0.55F) return SpaceFeatureType::Empty;
    if (roll < 0.70F) return SpaceFeatureType::AsteroidCluster;
    if (roll < 0.77F) return SpaceFeatureType::DustCloud;
    if (roll < 0.83F) return SpaceFeatureType::IceField;
    if (roll < 0.885F) return SpaceFeatureType::MetalRichAsteroids;
    if (roll < 0.925F) return SpaceFeatureType::RingDebris;
    if (roll < 0.95F) return SpaceFeatureType::Comet;
    if (roll < 0.968F) return SpaceFeatureType::CrystalAsteroids;
    if (roll < 0.978F) return SpaceFeatureType::Moon;
    if (roll < 0.984F) return SpaceFeatureType::Planet;
    if (roll < 0.996F) return SpaceFeatureType::NaturalAnomaly;
    return SpaceFeatureType::BiologicalSignal;
}

[[nodiscard]] float radiusForFeature(SpaceFeatureType type, std::uint64_t& rng) noexcept
{
    switch (type) {
    case SpaceFeatureType::Moon:
        return 260.0F + unitFloat(rng) * 520.0F;
    case SpaceFeatureType::Planet:
        return 1400.0F + unitFloat(rng) * 2200.0F;
    case SpaceFeatureType::DustCloud:
    case SpaceFeatureType::NaturalAnomaly:
    case SpaceFeatureType::BiologicalSignal:
        return 220.0F + unitFloat(rng) * 780.0F;
    default:
        return 96.0F + unitFloat(rng) * 420.0F;
    }
}

[[nodiscard]] SpaceBodyClass bodyClassForFeature(SpaceFeatureType type, std::uint64_t& rng) noexcept
{
    const float roll = unitFloat(rng);
    if (type == SpaceFeatureType::Moon) {
        if (roll < 0.55F) return SpaceBodyClass::DeadRocky;
        if (roll < 0.75F) return SpaceBodyClass::Frozen;
        if (roll < 0.90F) return SpaceBodyClass::MetalRich;
        if (roll < 0.97F) return SpaceBodyClass::Volcanic;
        return SpaceBodyClass::Crystal;
    }
    if (type == SpaceFeatureType::Planet) {
        if (roll < 0.30F) return SpaceBodyClass::DeadRocky;
        if (roll < 0.45F) return SpaceBodyClass::Frozen;
        if (roll < 0.56F) return SpaceBodyClass::Ocean;
        if (roll < 0.67F) return SpaceBodyClass::MetalRich;
        if (roll < 0.78F) return SpaceBodyClass::Toxic;
        if (roll < 0.86F) return SpaceBodyClass::Volcanic;
        if (roll < 0.96F) return SpaceBodyClass::GasGiant;
        if (roll < 0.985F) return SpaceBodyClass::LifeBearing;
        return SpaceBodyClass::Crystal;
    }
    return SpaceBodyClass::None;
}

[[nodiscard]] float gravityForBody(SpaceFeatureType type, SpaceBodyClass bodyClass, float radius) noexcept
{
    if (type == SpaceFeatureType::Moon) {
        const float base = 0.08F + std::clamp((radius - 220.0F) / 620.0F, 0.0F, 1.0F) * 0.32F;
        return bodyClass == SpaceBodyClass::MetalRich ? std::min(base + 0.06F, 0.48F) : base;
    }
    if (type == SpaceFeatureType::Planet) {
        if (bodyClass == SpaceBodyClass::GasGiant) {
            return 1.65F;
        }
        const float base = 0.55F + std::clamp((radius - 1400.0F) / 2400.0F, 0.0F, 1.0F) * 0.75F;
        return bodyClass == SpaceBodyClass::MetalRich ? std::min(base + 0.18F, 1.55F) : base;
    }
    return 0.0F;
}

[[nodiscard]] float atmosphereForBody(SpaceFeatureType type, SpaceBodyClass bodyClass) noexcept
{
    if (type == SpaceFeatureType::Moon) {
        switch (bodyClass) {
        case SpaceBodyClass::Frozen: return 0.03F;
        case SpaceBodyClass::Volcanic: return 0.05F;
        case SpaceBodyClass::Toxic: return 0.08F;
        default: return 0.0F;
        }
    }
    if (type == SpaceFeatureType::Planet) {
        switch (bodyClass) {
        case SpaceBodyClass::DeadRocky: return 0.10F;
        case SpaceBodyClass::Frozen: return 0.24F;
        case SpaceBodyClass::MetalRich: return 0.06F;
        case SpaceBodyClass::Volcanic: return 0.36F;
        case SpaceBodyClass::Toxic: return 0.92F;
        case SpaceBodyClass::Ocean: return 0.72F;
        case SpaceBodyClass::GasGiant: return 1.0F;
        case SpaceBodyClass::LifeBearing: return 0.82F;
        case SpaceBodyClass::Crystal: return 0.18F;
        default: return 0.0F;
        }
    }
    return 0.0F;
}

[[nodiscard]] float roughnessForBody(SpaceFeatureType type, SpaceBodyClass bodyClass, std::uint64_t& rng) noexcept
{
    if (type == SpaceFeatureType::Moon) {
        const float variation = unitFloat(rng) * 0.18F;
        switch (bodyClass) {
        case SpaceBodyClass::Frozen: return 0.42F + variation;
        case SpaceBodyClass::MetalRich: return 0.58F + variation;
        case SpaceBodyClass::Volcanic: return 0.70F + variation;
        case SpaceBodyClass::Crystal: return 0.66F + variation;
        default: return 0.54F + variation;
        }
    }
    if (type == SpaceFeatureType::Planet) {
        const float variation = unitFloat(rng) * 0.16F;
        switch (bodyClass) {
        case SpaceBodyClass::Ocean: return 0.18F + variation;
        case SpaceBodyClass::GasGiant: return 0.0F;
        case SpaceBodyClass::Frozen: return 0.36F + variation;
        case SpaceBodyClass::LifeBearing: return 0.34F + variation;
        case SpaceBodyClass::Volcanic: return 0.74F + variation;
        case SpaceBodyClass::MetalRich: return 0.62F + variation;
        case SpaceBodyClass::Crystal: return 0.68F + variation;
        case SpaceBodyClass::Toxic: return 0.48F + variation;
        default: return 0.46F + variation;
        }
    }
    return 0.0F;
}

[[nodiscard]] float oceanCoverageForBody(SpaceFeatureType type, SpaceBodyClass bodyClass, std::uint64_t& rng) noexcept
{
    if (type == SpaceFeatureType::Moon) {
        return bodyClass == SpaceBodyClass::Frozen ? 0.04F + unitFloat(rng) * 0.08F : 0.0F;
    }
    if (type == SpaceFeatureType::Planet) {
        switch (bodyClass) {
        case SpaceBodyClass::Ocean: return 0.72F + unitFloat(rng) * 0.22F;
        case SpaceBodyClass::LifeBearing: return 0.22F + unitFloat(rng) * 0.42F;
        case SpaceBodyClass::Frozen: return 0.08F + unitFloat(rng) * 0.18F;
        case SpaceBodyClass::Toxic: return 0.02F + unitFloat(rng) * 0.12F;
        default: return 0.0F;
        }
    }
    return 0.0F;
}

[[nodiscard]] float resourceRichnessForBody(SpaceFeatureType type, SpaceBodyClass bodyClass, std::uint64_t& rng) noexcept
{
    if (type != SpaceFeatureType::Moon && type != SpaceFeatureType::Planet) {
        return 0.0F;
    }
    const float variation = unitFloat(rng) * 0.18F;
    switch (bodyClass) {
    case SpaceBodyClass::MetalRich: return 0.76F + variation;
    case SpaceBodyClass::Crystal: return 0.68F + variation;
    case SpaceBodyClass::Volcanic: return 0.56F + variation;
    case SpaceBodyClass::DeadRocky: return 0.38F + variation;
    case SpaceBodyClass::Frozen: return 0.32F + variation;
    case SpaceBodyClass::Toxic: return 0.46F + variation;
    case SpaceBodyClass::Ocean: return 0.24F + variation;
    case SpaceBodyClass::LifeBearing: return 0.28F + variation;
    default: return 0.0F;
    }
}

[[nodiscard]] float lifeSignalForBody(SpaceFeatureType type, SpaceBodyClass bodyClass, std::uint64_t& rng) noexcept
{
    if (type != SpaceFeatureType::Planet) {
        return 0.0F;
    }
    if (bodyClass == SpaceBodyClass::LifeBearing) {
        return 0.70F + unitFloat(rng) * 0.25F;
    }
    if (bodyClass == SpaceBodyClass::Ocean) {
        return unitFloat(rng) * 0.18F;
    }
    return 0.0F;
}

[[nodiscard]] bool landableForBody(SpaceFeatureType type, SpaceBodyClass bodyClass) noexcept
{
    if (type == SpaceFeatureType::Moon) {
        return true;
    }
    if (type == SpaceFeatureType::Planet) {
        return bodyClass != SpaceBodyClass::GasGiant;
    }
    return false;
}

[[nodiscard]] SpaceFeature makeFeature(SpaceFeatureType type,
                                        FeatureOrigin origin,
                                        SpaceSectorCoord sector,
                                        core::Vec3 localOffset,
                                        float radius,
                                        std::uint64_t& rng) noexcept
{
    SpaceFeature feature{};
    feature.type = type;
    feature.origin = origin;
    feature.sector = sector;
    feature.localOffset = localOffset;
    feature.radius = radius;
    feature.resourceSeed = static_cast<std::uint32_t>(mix64(rng) & 0xFFFFFFFFU);
    feature.bodyClass = bodyClassForFeature(type, rng);
    feature.gravityScale = gravityForBody(type, feature.bodyClass, radius);
    feature.atmosphereDensity = atmosphereForBody(type, feature.bodyClass);
    feature.surfaceRoughness = roughnessForBody(type, feature.bodyClass, rng);
    feature.oceanCoverage = oceanCoverageForBody(type, feature.bodyClass, rng);
    feature.resourceRichness = resourceRichnessForBody(type, feature.bodyClass, rng);
    feature.lifeSignal = lifeSignalForBody(type, feature.bodyClass, rng);
    feature.landable = landableForBody(type, feature.bodyClass);
    return feature;
}

[[nodiscard]] core::Vec3 randomLocalOffset(std::uint64_t& rng, int sectorSize) noexcept
{
    const float size = static_cast<float>(sectorSize);
    return {
        (unitFloat(rng) - 0.5F) * size,
        (unitFloat(rng) - 0.5F) * size,
        (unitFloat(rng) - 0.5F) * size
    };
}

void appendPlanetCompanions(std::vector<SpaceFeature>& out,
                            SpaceSectorCoord sector,
                            const SpaceFeature& planet,
                            int sectorSize,
                            std::uint64_t& rng)
{
    const float sectorHalf = static_cast<float>(sectorSize) * 0.5F;
    const int moonCount = 1 + static_cast<int>(unitFloat(rng) * 3.0F);
    for (int i = 0; i < moonCount; ++i) {
        const float angle = unitFloat(rng) * 6.28318530718F;
        const float elevation = (unitFloat(rng) - 0.5F) * 0.45F;
        const float orbit = sectorHalf * (0.28F + unitFloat(rng) * 0.34F);
        core::Vec3 offset{
            planet.localOffset.x + std::cos(angle) * orbit,
            planet.localOffset.y + elevation * orbit,
            planet.localOffset.z + std::sin(angle) * orbit
        };
        offset.x = std::clamp(offset.x, -sectorHalf * 0.92F, sectorHalf * 0.92F);
        offset.y = std::clamp(offset.y, -sectorHalf * 0.92F, sectorHalf * 0.92F);
        offset.z = std::clamp(offset.z, -sectorHalf * 0.92F, sectorHalf * 0.92F);
        const float radius = 220.0F + unitFloat(rng) * 360.0F;
        out.push_back(makeFeature(SpaceFeatureType::Moon, FeatureOrigin::Natural, sector, offset, radius, rng));
    }

    if (unitFloat(rng) < 0.55F) {
        core::Vec3 ringOffset{
            std::clamp(planet.localOffset.x + (unitFloat(rng) - 0.5F) * sectorHalf,
                       -sectorHalf * 0.88F, sectorHalf * 0.88F),
            std::clamp(planet.localOffset.y + (unitFloat(rng) - 0.5F) * sectorHalf * 0.18F,
                       -sectorHalf * 0.88F, sectorHalf * 0.88F),
            std::clamp(planet.localOffset.z + (unitFloat(rng) - 0.5F) * sectorHalf,
                       -sectorHalf * 0.88F, sectorHalf * 0.88F)
        };
        out.push_back(makeFeature(
            SpaceFeatureType::RingDebris,
            FeatureOrigin::Natural,
            sector,
            ringOffset,
            220.0F + unitFloat(rng) * 260.0F,
            rng));
    }
}

} // namespace

SpaceEnvironment::SpaceEnvironment(SpaceSettings settings)
    : settings_(settings)
{
    settings_.sectorSizeBlocks = std::max(settings_.sectorSizeBlocks, 128);
    settings_.nearSpaceStartY = std::min(settings_.nearSpaceStartY, settings_.atmosphereTopY);
    settings_.gravityFalloffStartY = std::min(settings_.gravityFalloffStartY, settings_.zeroGravityY);
}

SpaceEnvironmentState SpaceEnvironment::evaluate(float worldY) const noexcept
{
    SpaceEnvironmentState state{};
    state.altitudeY = worldY;

    const float atmosphereRange = std::max(1.0F, settings_.atmosphereTopY);
    state.atmosphereDensity = saturate(1.0F - (worldY / atmosphereRange));

    const float spaceRange = std::max(1.0F, settings_.atmosphereTopY - settings_.nearSpaceStartY);
    state.spaceBlend = saturate((worldY - settings_.nearSpaceStartY) / spaceRange);

    const float gravityRange = std::max(1.0F, settings_.zeroGravityY - settings_.gravityFalloffStartY);
    state.gravityScale = saturate(1.0F - ((worldY - settings_.gravityFalloffStartY) / gravityRange));

    state.inNearSpace = worldY >= settings_.nearSpaceStartY;
    state.inSpace = worldY >= settings_.atmosphereTopY;
    return state;
}

SpaceSectorCoord SpaceEnvironment::sectorFor(core::Vec3 worldPosition) const noexcept
{
    const auto sectorSize = static_cast<std::int64_t>(settings_.sectorSizeBlocks);
    return {
        floorDiv(static_cast<std::int64_t>(std::floor(worldPosition.x)), sectorSize),
        floorDiv(static_cast<std::int64_t>(std::floor(worldPosition.y)), sectorSize),
        floorDiv(static_cast<std::int64_t>(std::floor(worldPosition.z)), sectorSize)
    };
}

core::Vec3 SpaceEnvironment::featureWorldCenter(const SpaceFeature& feature) const noexcept
{
    const float sectorSize = static_cast<float>(settings_.sectorSizeBlocks);
    return {
        static_cast<float>(feature.sector.x) * sectorSize + sectorSize * 0.5F + feature.localOffset.x,
        static_cast<float>(feature.sector.y) * sectorSize + sectorSize * 0.5F + feature.localOffset.y,
        static_cast<float>(feature.sector.z) * sectorSize + sectorSize * 0.5F + feature.localOffset.z
    };
}

std::vector<SpaceFeature> SpaceEnvironment::featuresForSector(SpaceSectorCoord sector) const
{
    std::uint64_t rng = sectorHash(sector, settings_.seed);
    const float roll = unitFloat(rng);
    const auto type = naturalFeatureForRoll(roll);
    if (type == SpaceFeatureType::Empty) {
        return {};
    }

    const auto origin = type == SpaceFeatureType::BiologicalSignal
        ? FeatureOrigin::Biological
        : FeatureOrigin::Natural;
    std::vector<SpaceFeature> result;
    result.reserve(type == SpaceFeatureType::Planet ? 5U : 1U);
    result.push_back(makeFeature(
        type,
        origin,
        sector,
        randomLocalOffset(rng, settings_.sectorSizeBlocks),
        radiusForFeature(type, rng),
        rng));
    if (type == SpaceFeatureType::Planet) {
        appendPlanetCompanions(result, sector, result.front(), settings_.sectorSizeBlocks, rng);
    }
    return result;
}

} // namespace voxel::world
