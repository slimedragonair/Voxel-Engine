#pragma once

#include <array>
#include <cstdint>

namespace voxel::core {

// Deterministic value noise. Used by NoiseTerrainGenerator to produce
// hills + caves without an external dependency.
[[nodiscard]] std::uint32_t hash3D(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed) noexcept;
[[nodiscard]] float valueNoise2D(float x, float z, std::uint32_t seed) noexcept;
[[nodiscard]] float valueNoise3D(float x, float y, float z, std::uint32_t seed) noexcept;
[[nodiscard]] float fbm2D(float x, float z, std::uint32_t seed, int octaves, float lacunarity, float gain) noexcept;
[[nodiscard]] float fbm3D(float x, float y, float z, std::uint32_t seed, int octaves, float lacunarity, float gain) noexcept;

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct DVec3 {
    double x{};
    double y{};
    double z{};
};

struct Mat4 {
    std::array<float, 16> m{};
};

[[nodiscard]] Vec3 operator-(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] Vec3 operator+(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] Vec3 operator*(Vec3 lhs, float rhs) noexcept;
Vec3& operator+=(Vec3& lhs, Vec3 rhs) noexcept;
[[nodiscard]] Vec3 cross(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] float dot(Vec3 lhs, Vec3 rhs) noexcept;
[[nodiscard]] Vec3 normalize(Vec3 value) noexcept;
[[nodiscard]] float length(Vec3 value) noexcept;

[[nodiscard]] DVec3 operator-(DVec3 lhs, DVec3 rhs) noexcept;
[[nodiscard]] DVec3 operator+(DVec3 lhs, DVec3 rhs) noexcept;
[[nodiscard]] DVec3 operator*(DVec3 lhs, double rhs) noexcept;
DVec3& operator+=(DVec3& lhs, DVec3 rhs) noexcept;
[[nodiscard]] DVec3 cross(DVec3 lhs, DVec3 rhs) noexcept;
[[nodiscard]] double dot(DVec3 lhs, DVec3 rhs) noexcept;
[[nodiscard]] DVec3 normalize(DVec3 value) noexcept;
[[nodiscard]] double length(DVec3 value) noexcept;

[[nodiscard]] Mat4 identity() noexcept;
[[nodiscard]] Mat4 multiply(const Mat4& lhs, const Mat4& rhs) noexcept;
[[nodiscard]] Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) noexcept;
[[nodiscard]] Mat4 perspectiveVulkan(float fovYRadians, float aspect, float nearPlane, float farPlane) noexcept;
[[nodiscard]] Mat4 perspectiveVulkanReversedZ(float fovYRadians, float aspect, float nearPlane, float farPlane) noexcept;

// J2a: Gribb–Hartmann frustum plane extraction.
// Each plane is stored as (a, b, c, d) where the inward-pointing normal is
// (a, b, c) and a point P is inside the plane iff `a*P.x + b*P.y + c*P.z + d >= 0`.
struct FrustumPlanes {
    std::array<std::array<float, 4>, 6> planes{}; // left, right, bottom, top, near, far
};

[[nodiscard]] FrustumPlanes extractFrustumPlanes(const Mat4& viewProjection) noexcept;
// True if the AABB intersects the frustum (or is fully inside). Uses the
// "positive vertex" test — one dot-product per plane, no per-corner loop.
[[nodiscard]] bool aabbIntersectsFrustum(const FrustumPlanes& frustum, Vec3 minCorner, Vec3 maxCorner) noexcept;

} // namespace voxel::core
