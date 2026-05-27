#include <voxel/world/BitPackedArray.hpp>

#include <cassert>
#include <stdexcept>
#include <utility>

namespace voxel::world {

namespace {

std::size_t wordsForBits(std::size_t count, std::size_t bitsPerEntry)
{
    const std::size_t totalBits = count * bitsPerEntry;
    return (totalBits + 63U) / 64U;
}

std::uint64_t maskForBits(std::size_t bits)
{
    // bits is guaranteed [1, 32] in this file.
    return (bits >= 64U) ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1U);
}

} // namespace

BitPackedArray::BitPackedArray(std::size_t count, std::size_t bitsPerEntry)
    : count_(count),
      bitsPerEntry_(bitsPerEntry),
      words_(wordsForBits(count, bitsPerEntry), 0U)
{
    if (bitsPerEntry == 0U || bitsPerEntry > 32U) {
        throw std::invalid_argument("BitPackedArray: bitsPerEntry must be in [1, 32]");
    }
}

std::uint32_t BitPackedArray::at(std::size_t i) const
{
    if (i >= count_) {
        throw std::out_of_range("BitPackedArray::at index out of range");
    }
    const std::size_t startBit = i * bitsPerEntry_;
    const std::size_t startWord = startBit >> 6U;
    const std::size_t bitOffset = startBit & 63U;
    const std::uint64_t mask = maskForBits(bitsPerEntry_);

    std::uint64_t low = words_[startWord] >> bitOffset;
    if (bitOffset + bitsPerEntry_ <= 64U) {
        return static_cast<std::uint32_t>(low & mask);
    }
    const std::uint64_t high = words_[startWord + 1U] << (64U - bitOffset);
    return static_cast<std::uint32_t>((low | high) & mask);
}

void BitPackedArray::set(std::size_t i, std::uint32_t value)
{
    if (i >= count_) {
        throw std::out_of_range("BitPackedArray::set index out of range");
    }
    const std::size_t startBit = i * bitsPerEntry_;
    const std::size_t startWord = startBit >> 6U;
    const std::size_t bitOffset = startBit & 63U;
    const std::uint64_t mask = maskForBits(bitsPerEntry_);
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

void BitPackedArray::resize(std::size_t newBitsPerEntry)
{
    if (newBitsPerEntry == bitsPerEntry_) {
        return;
    }
    if (newBitsPerEntry == 0U || newBitsPerEntry > 32U) {
        throw std::invalid_argument("BitPackedArray::resize: bitsPerEntry must be in [1, 32]");
    }

    BitPackedArray next(count_, newBitsPerEntry);
    for (std::size_t i = 0; i < count_; ++i) {
        next.set(i, at(i));
    }
    *this = std::move(next);
}

void BitPackedArray::resetWith(std::size_t count, std::size_t bitsPerEntry, std::vector<std::uint64_t> words)
{
    if (bitsPerEntry == 0U || bitsPerEntry > 32U) {
        throw std::invalid_argument("BitPackedArray::resetWith: bitsPerEntry must be in [1, 32]");
    }
    const auto expected = wordsForBits(count, bitsPerEntry);
    if (words.size() != expected) {
        throw std::invalid_argument("BitPackedArray::resetWith: wrong word count for count/bitsPerEntry");
    }
    count_ = count;
    bitsPerEntry_ = bitsPerEntry;
    words_ = std::move(words);
}

} // namespace voxel::world
