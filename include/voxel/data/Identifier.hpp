#pragma once

#include <string>
#include <utility>

namespace voxel::data {

struct Identifier {
    std::string namespaceId{"core"};
    std::string path;

    Identifier() = default;
    Identifier(std::string namespaceIdIn, std::string pathIn)
        : namespaceId(std::move(namespaceIdIn)), path(std::move(pathIn))
    {
    }

    [[nodiscard]] std::string str() const { return namespaceId + ":" + path; }
    [[nodiscard]] friend bool operator==(const Identifier& lhs, const Identifier& rhs)
    {
        return lhs.namespaceId == rhs.namespaceId && lhs.path == rhs.path;
    }
};

struct IdentifierHash {
    [[nodiscard]] std::size_t operator()(const Identifier& id) const noexcept
    {
        const std::hash<std::string> hash;
        return hash(id.namespaceId) ^ (hash(id.path) << 1U);
    }
};

} // namespace voxel::data

