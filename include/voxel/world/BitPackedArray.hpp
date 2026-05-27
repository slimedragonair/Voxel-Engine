#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace voxel::world {

// Fixed-element-count, variable-bit-width packed array of unsigned values.
// Used by Chunk to store palette indices densely.
//
// Indices into `at`/`set` are 0..count-1. Each entry stores up to 32 bits.
// `bitsPerEntry` must be in [1, 32]. The implementation handles entries that
// straddle 64-bit word boundaries.
class BitPackedArray {
public:
    BitPackedArray() = default;
    BitPackedArray(std::size_t count, std::size_t bitsPerEntry);

    [[nodiscard]] std::uint32_t at(std::size_t i) const;
    void set(std::size_t i, std::uint32_t value);

    [[nodiscard]] inline std::uint32_t at_unchecked(std::size_t i) const noexcept
    {
        const std::size_t startBit = i * bitsPerEntry_;
        const std::size_t startWord = startBit >> 6U;
        const std::size_t bitOffset = startBit & 63U;
        const std::uint64_t mask = (bitsPerEntry_ >= 64U) ? ~std::uint64_t{0} : ((std::uint64_t{1} << bitsPerEntry_) - 1U);

        std::uint64_t low = words_[startWord] >> bitOffset;
        if (bitOffset + bitsPerEntry_ <= 64U) {
            return static_cast<std::uint32_t>(low & mask);
        }
        const std::uint64_t high = words_[startWord + 1U] << (64U - bitOffset);
        return static_cast<std::uint32_t>((low | high) & mask);
    }

    inline void set_unchecked(std::size_t i, std::uint32_t value) noexcept
    {
        const std::size_t startBit = i * bitsPerEntry_;
        const std::size_t startWord = startBit >> 6U;
        const std::size_t bitOffset = startBit & 63U;
        const std::uint64_t mask = (bitsPerEntry_ >= 64U) ? ~std::uint64_t{0} : ((std::uint64_t{1} << bitsPerEntry_) - 1U);
        const std::uint64_t v = static_cast<std::uint64_t>(value) & mask;

        words_[startWord] &= ~(mask << bitOffset);
        words_[startWord] |= (v << bitOffset);

        if (bitOffset + bitsPerEntry_ > 64U) {
            const std::size_t lowBits = 64U - bitOffset;
            const std::size_t highBits = bitsPerEntry_ - lowBits;
            const std::uint64_t highMask = (std::uint64_t{1} << highBits) - 1U;
            words_[startWord + 1U] &= ~highMask;
            words_[startWord + 1U] |= (v >> lowBits);
        }
    }

    // Re-pack the buffer to a different bit-width. Existing values are preserved.
    // If `newBitsPerEntry` is smaller, callers must ensure no current entry
    // exceeds `(1 << newBitsPerEntry) - 1`.
    void resize(std::size_t newBitsPerEntry);

    // Bulk read/write helpers for serialization.
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] std::size_t bitsPerEntry() const noexcept { return bitsPerEntry_; }
    [[nodiscard]] const std::vector<std::uint64_t>& words() const noexcept { return words_; }

    void resetWith(std::size_t count, std::size_t bitsPerEntry, std::vector<std::uint64_t> words);

private:
    std::size_t count_{0};
    std::size_t bitsPerEntry_{1};
    std::vector<std::uint64_t> words_;
};

} // namespace voxel::world
