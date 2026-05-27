#pragma once

#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <voxel/data/Identifier.hpp>

namespace voxel::data {

template <typename T>
class Registry {
public:
    using RuntimeId = std::uint32_t;

    RuntimeId add(T value)
    {
        const auto key = value.id;
        if (ids_.contains(key)) {
            throw std::runtime_error("Duplicate registry id: " + key.str());
        }

        const RuntimeId runtimeId = static_cast<RuntimeId>(entries_.size() + 1U);
        entries_.push_back(std::move(value));
        ids_.emplace(key, runtimeId);
        return runtimeId;
    }

    [[nodiscard]] const T* find(const Identifier& id) const
    {
        const auto found = ids_.find(id);
        if (found == ids_.end()) {
            return nullptr;
        }
        return byRuntimeId(found->second);
    }

    [[nodiscard]] const T* byRuntimeId(RuntimeId id) const
    {
        if (id == 0 || id > entries_.size()) {
            return nullptr;
        }
        return &entries_[id - 1U];
    }

    [[nodiscard]] RuntimeId runtimeId(const Identifier& id) const
    {
        const auto found = ids_.find(id);
        if (found == ids_.end()) {
            return 0;
        }
        return found->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::vector<T>& entries() const noexcept { return entries_; }

private:
    std::vector<T> entries_;
    std::unordered_map<Identifier, RuntimeId, IdentifierHash> ids_;
};

} // namespace voxel::data

