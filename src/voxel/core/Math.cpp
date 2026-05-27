#include <voxel/core/Math.hpp>

#include <cmath>
#include <cstdint>

namespace voxel::core {

namespace {

// Splitmix-style integer mixer. Stable across compilers and platforms — we want
// world generation to be deterministic for a given seed regardless of host.
constexpr std::uint32_t scrambleHash(std::uint32_t value) noexcept
{
    value ^= value >> 16U;
    value *= 0x85EBCA6BU;
    value ^= value >> 13U;
    value *= 0xC2B2AE35U;
    value ^= value >> 16U;
    return value;
}

float hashToUnit(std::uint32_t h) noexcept
{
    return static_cast<float>(h & 0x00FFFFFFU) / 16777216.0F; // [0, 1)
}

float smoothstep(float t) noexcept
{
    return t * t * (3.0F - 2.0F * t);
}

float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

std::int32_t floorToInt(float v) noexcept
{
    const auto i = static_cast<std::int32_t>(v);
    return (v < static_cast<float>(i)) ? (i - 1) : i;
}

} // namespace

std::uint32_t hash3D(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed) noexcept
{
    std::uint32_t h = seed;
    h = scrambleHash(h ^ static_cast<std::uint32_t>(x) * 0x8DA6B343U);
    h = scrambleHash(h ^ static_cast<std::uint32_t>(y) * 0xD8163841U);
    h = scrambleHash(h ^ static_cast<std::uint32_t>(z) * 0xCB1AB31FU);
    return h;
}

float valueNoise2D(float x, float z, std::uint32_t seed) noexcept
{
    const std::int32_t xi = floorToInt(x);
    const std::int32_t zi = floorToInt(z);
    const float xf = x - static_cast<float>(xi);
    const float zf = z - static_cast<float>(zi);
    const float u = smoothstep(xf);
    const float v = smoothstep(zf);

    const float n00 = hashToUnit(hash3D(xi,     0, zi,     seed));
    const float n10 = hashToUnit(hash3D(xi + 1, 0, zi,     seed));
    const float n01 = hashToUnit(hash3D(xi,     0, zi + 1, seed));
    const float n11 = hashToUnit(hash3D(xi + 1, 0, zi + 1, seed));

    return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
}

float valueNoise3D(float x, float y, float z, std::uint32_t seed) noexcept
{
    const std::int32_t xi = floorToInt(x);
    const std::int32_t yi = floorToInt(y);
    const std::int32_t zi = floorToInt(z);
    const float xf = x - static_cast<float>(xi);
    const float yf = y - static_cast<float>(yi);
    const float zf = z - static_cast<float>(zi);
    const float u = smoothstep(xf);
    const float v = smoothstep(yf);
    const float w = smoothstep(zf);

    const float n000 = hashToUnit(hash3D(xi,     yi,     zi,     seed));
    const float n100 = hashToUnit(hash3D(xi + 1, yi,     zi,     seed));
    const float n010 = hashToUnit(hash3D(xi,     yi + 1, zi,     seed));
    const float n110 = hashToUnit(hash3D(xi + 1, yi + 1, zi,     seed));
    const float n001 = hashToUnit(hash3D(xi,     yi,     zi + 1, seed));
    const float n101 = hashToUnit(hash3D(xi + 1, yi,     zi + 1, seed));
    const float n011 = hashToUnit(hash3D(xi,     yi + 1, zi + 1, seed));
    const float n111 = hashToUnit(hash3D(xi + 1, yi + 1, zi + 1, seed));

    const float nx00 = lerp(n000, n100, u);
    const float nx10 = lerp(n010, n110, u);
    const float nx01 = lerp(n001, n101, u);
    const float nx11 = lerp(n011, n111, u);

    return lerp(lerp(nx00, nx10, v), lerp(nx01, nx11, v), w);
}

float fbm2D(float x, float z, std::uint32_t seed, int octaves, float lacunarity, float gain) noexcept
{
    float amplitude = 1.0F;
    float frequency = 1.0F;
    float sum = 0.0F;
    float norm = 0.0F;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise2D(x * frequency, z * frequency, seed + static_cast<std::uint32_t>(i)) * amplitude;
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return (norm > 0.0F) ? (sum / norm) : 0.0F;
}

float fbm3D(float x, float y, float z, std::uint32_t seed, int octaves, float lacunarity, float gain) noexcept
{
    float amplitude = 1.0F;
    float frequency = 1.0F;
    float sum = 0.0F;
    float norm = 0.0F;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise3D(x * frequency, y * frequency, z * frequency, seed + static_cast<std::uint32_t>(i)) * amplitude;
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return (norm > 0.0F) ? (sum / norm) : 0.0F;
}

Vec3 operator-(Vec3 lhs, Vec3 rhs) noexcept
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3 operator+(Vec3 lhs, Vec3 rhs) noexcept
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3 operator*(Vec3 lhs, float rhs) noexcept
{
    return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs};
}

Vec3& operator+=(Vec3& lhs, Vec3 rhs) noexcept
{
    lhs = lhs + rhs;
    return lhs;
}

Vec3 cross(Vec3 lhs, Vec3 rhs) noexcept
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x
    };
}

float dot(Vec3 lhs, Vec3 rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3 normalize(Vec3 value) noexcept
{
    const float len = std::sqrt(dot(value, value));
    if (len <= 0.00001F) {
        return {};
    }
    return {value.x / len, value.y / len, value.z / len};
}

float length(Vec3 value) noexcept
{
    return std::sqrt(dot(value, value));
}

DVec3 operator-(DVec3 lhs, DVec3 rhs) noexcept
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

DVec3 operator+(DVec3 lhs, DVec3 rhs) noexcept
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

DVec3 operator*(DVec3 lhs, double rhs) noexcept
{
    return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs};
}

DVec3& operator+=(DVec3& lhs, DVec3 rhs) noexcept
{
    lhs = lhs + rhs;
    return lhs;
}

DVec3 cross(DVec3 lhs, DVec3 rhs) noexcept
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x
    };
}

double dot(DVec3 lhs, DVec3 rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

DVec3 normalize(DVec3 value) noexcept
{
    const double len = std::sqrt(dot(value, value));
    if (len <= 0.00000001) {
        return {};
    }
    return {value.x / len, value.y / len, value.z / len};
}

double length(DVec3 value) noexcept
{
    return std::sqrt(dot(value, value));
}

Mat4 identity() noexcept
{
    Mat4 result{};
    result.m[0] = 1.0F;
    result.m[5] = 1.0F;
    result.m[10] = 1.0F;
    result.m[15] = 1.0F;
    return result;
}

Mat4 multiply(const Mat4& lhs, const Mat4& rhs) noexcept
{
    Mat4 result{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                result.m[col * 4 + row] += lhs.m[k * 4 + row] * rhs.m[col * 4 + k];
            }
        }
    }
    return result;
}

Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) noexcept
{
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 result = identity();
    result.m[0] = s.x;
    result.m[4] = s.y;
    result.m[8] = s.z;
    result.m[1] = u.x;
    result.m[5] = u.y;
    result.m[9] = u.z;
    result.m[2] = -f.x;
    result.m[6] = -f.y;
    result.m[10] = -f.z;
    result.m[12] = -dot(s, eye);
    result.m[13] = -dot(u, eye);
    result.m[14] = dot(f, eye);
    return result;
}

Mat4 perspectiveVulkan(float fovYRadians, float aspect, float nearPlane, float farPlane) noexcept
{
    const float f = 1.0F / std::tan(fovYRadians * 0.5F);

    Mat4 result{};
    result.m[0] = f / aspect;
    result.m[5] = -f;
    result.m[10] = farPlane / (nearPlane - farPlane);
    result.m[11] = -1.0F;
    result.m[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

Mat4 perspectiveVulkanReversedZ(float fovYRadians, float aspect, float nearPlane, float farPlane) noexcept
{
    const float f = 1.0F / std::tan(fovYRadians * 0.5F);

    Mat4 result{};
    result.m[0] = f / aspect;
    result.m[5] = -f;
    result.m[10] = nearPlane / (farPlane - nearPlane);
    result.m[11] = -1.0F;
    result.m[14] = (farPlane * nearPlane) / (farPlane - nearPlane);
    return result;
}

namespace {

// Mat4::m is column-major: m[col*4 + row]. Row `r` is {m[r], m[r+4], m[r+8], m[r+12]}.
inline std::array<float, 4> rowOf(const Mat4& m, int r) noexcept
{
    return {m.m[r], m.m[r + 4], m.m[r + 8], m.m[r + 12]};
}

inline std::array<float, 4> addRows(const std::array<float, 4>& a, const std::array<float, 4>& b) noexcept
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
}

inline std::array<float, 4> subRows(const std::array<float, 4>& a, const std::array<float, 4>& b) noexcept
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]};
}

inline std::array<float, 4> normalizePlane(const std::array<float, 4>& p) noexcept
{
    const float length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (length <= 1e-6F) {
        return p;
    }
    return {p[0] / length, p[1] / length, p[2] / length, p[3] / length};
}

} // namespace

FrustumPlanes extractFrustumPlanes(const Mat4& vp) noexcept
{
    const auto r0 = rowOf(vp, 0);
    const auto r1 = rowOf(vp, 1);
    const auto r2 = rowOf(vp, 2);
    const auto r3 = rowOf(vp, 3);

    FrustumPlanes f{};
    // Plane equation: (row3 + row_axis) for "min" side, (row3 - row_axis) for "max" side
    // (Vulkan z range [0, 1] uses row2 as near, row3 - row2 as far.)
    f.planes[0] = normalizePlane(addRows(r3, r0)); // left   (x ≥ -w)
    f.planes[1] = normalizePlane(subRows(r3, r0)); // right  (x ≤  w)
    f.planes[2] = normalizePlane(addRows(r3, r1)); // bottom (y ≥ -w)
    f.planes[3] = normalizePlane(subRows(r3, r1)); // top    (y ≤  w)
    f.planes[4] = normalizePlane(r2);              // near   (z ≥  0)
    f.planes[5] = normalizePlane(subRows(r3, r2)); // far    (z ≤  w)
    return f;
}

bool aabbIntersectsFrustum(const FrustumPlanes& frustum, Vec3 minCorner, Vec3 maxCorner) noexcept
{
    for (const auto& plane : frustum.planes) {
        const float a = plane[0];
        const float b = plane[1];
        const float c = plane[2];
        const float d = plane[3];
        // Positive vertex: the AABB corner that maximises a*x + b*y + c*z.
        const float px = (a >= 0.0F) ? maxCorner.x : minCorner.x;
        const float py = (b >= 0.0F) ? maxCorner.y : minCorner.y;
        const float pz = (c >= 0.0F) ? maxCorner.z : minCorner.z;
        if (a * px + b * py + c * pz + d < 0.0F) {
            // Even the positive vertex is on the outside of this plane:
            // the entire AABB is outside the frustum.
            return false;
        }
    }
    return true;
}

} // namespace voxel::core
