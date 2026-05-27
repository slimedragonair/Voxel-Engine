#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace voxel::world {

// Computes the minimum bits required to index `count` distinct values.
// Always returns at least 1 so callers can safely allocate a bit-packed
// array even when the palette only contains a single entry. (We don't
// allow zero bits per index — that would collapse all reads/writes.)
[[nodiscard]] constexpr std::size_t bitsRequiredForPaletteSize(std::size_t count) noexcept
{
    if (count <= 1) {
        return 1;
    }
    std::size_t bits = 0;
    std::size_t capacity = 1;
    while (capacity < count) {
        capacity <<= 1U;
        ++bits;
    }
    return bits;
}

// Small palette mapping arbitrary values to dense 16-bit indices.
// The reverse map is rebuilt lazily if a clear()/seed pattern is needed,
// but for our use (Chunk block palette) we only grow.
template <typename T, typename Hasher>
class Palette {
public:
    Palette() = default;

    // Inserts the value if not present. Returns the dense index.
    std::uint16_t getOrInsert(const T& value)
    {
        if (auto it = reverse_.find(value); it != reverse_.end()) {
            return it->second;
        }
        const auto idx = static_cast<std::uint16_t>(entries_.size());
        entries_.push_back(value);
        reverse_.emplace(value, idx);
        return idx;
    }

    [[nodiscard]] const T& at(std::uint16_t index) const { return entries_.at(index); }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] std::size_t bitsRequired() const noexcept
    {
        return bitsRequiredForPaletteSize(entries_.size());
    }

    [[nodiscard]] const std::vector<T>& entries() const noexcept { return entries_; }

    // Direct seeding for deserialization. Caller is responsible for ensuring
    // values are unique; reverse map is rebuilt.
    void resetWith(std::vector<T> entries)
    {
        entries_ = std::move(entries);
        reverse_.clear();
        reverse_.reserve(entries_.size());
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            reverse_.emplace(entries_[i], static_cast<std::uint16_t>(i));
        }
    }

private:
    std::vector<T> entries_;
    std::unordered_map<T, std::uint16_t, Hasher> reverse_;
};

} // namespace voxel::world
