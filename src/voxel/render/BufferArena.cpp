#include <voxel/render/BufferArena.hpp>

#include <algorithm>
#include <stdexcept>

namespace voxel::render {

namespace {

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) noexcept
{
    if (alignment <= 1) {
        return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

BufferArena::~BufferArena()
{
    shutdown();
}

void BufferArena::initialize(VkDevice device, VmaAllocator allocator,
                              VkDeviceSize capacity, VkBufferUsageFlags usage)
{
    if (capacity == 0) {
        throw std::invalid_argument("BufferArena::initialize: capacity must be > 0");
    }
    if (allocator == VK_NULL_HANDLE) {
        throw std::invalid_argument("BufferArena::initialize: allocator must be valid");
    }
    device_ = device;
    allocator_ = allocator;
    capacity_ = capacity;
    freeBytes_ = capacity;
    freeRanges_.clear();
    freeRanges_.push_back({0, capacity});

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = capacity;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (vmaCreateBuffer(allocator_, &bufferInfo, &allocInfo, &buffer_, &allocation_, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("BufferArena: failed to allocate buffer");
    }
}

void BufferArena::shutdown()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
    capacity_ = 0;
    freeBytes_ = 0;
    freeRanges_.clear();
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
}

bool BufferArena::allocate(VkDeviceSize bytes, VkDeviceSize alignment, Slice& out)
{
    if (bytes == 0) {
        out = {};
        return true;
    }
    if (alignment == 0) {
        alignment = 1;
    }

    for (auto it = freeRanges_.begin(); it != freeRanges_.end(); ++it) {
        const VkDeviceSize alignedOffset = alignUp(it->offset, alignment);
        const VkDeviceSize padding = alignedOffset - it->offset;
        if (padding + bytes > it->size) {
            continue;
        }

        const VkDeviceSize originalOffset = it->offset;
        const VkDeviceSize originalSize = it->size;
        const VkDeviceSize remaining = originalSize - padding - bytes;

        out.offset = alignedOffset;
        out.size = bytes;

        // The full chosen range leaves the free list. The slice we hand out
        // consumes `bytes`. `padding` + `remaining` go back to the free list.
        freeRanges_.erase(it);
        freeBytes_ -= originalSize;

        if (padding > 0) {
            FreeRange pad{originalOffset, padding};
            const auto insertAt = std::lower_bound(
                freeRanges_.begin(), freeRanges_.end(), pad.offset,
                [](const FreeRange& r, VkDeviceSize off) { return r.offset < off; });
            freeRanges_.insert(insertAt, pad);
            freeBytes_ += padding;
        }
        if (remaining > 0) {
            FreeRange tail{alignedOffset + bytes, remaining};
            const auto insertAt = std::lower_bound(
                freeRanges_.begin(), freeRanges_.end(), tail.offset,
                [](const FreeRange& r, VkDeviceSize off) { return r.offset < off; });
            freeRanges_.insert(insertAt, tail);
            freeBytes_ += remaining;
        }
        return true;
    }
    return false;
}

void BufferArena::release(Slice slice)
{
    if (!slice.valid()) {
        return;
    }
    FreeRange released{slice.offset, slice.size};
    freeBytes_ += slice.size;

    // Insert in sorted order, then coalesce with adjacent free ranges.
    const auto insertAt = std::lower_bound(
        freeRanges_.begin(), freeRanges_.end(), released.offset,
        [](const FreeRange& r, VkDeviceSize off) { return r.offset < off; });
    auto it = freeRanges_.insert(insertAt, released);

    // Merge with successor.
    auto next = std::next(it);
    if (next != freeRanges_.end() && it->offset + it->size == next->offset) {
        it->size += next->size;
        freeRanges_.erase(next);
    }
    // Merge with predecessor.
    if (it != freeRanges_.begin()) {
        auto prev = std::prev(it);
        if (prev->offset + prev->size == it->offset) {
            prev->size += it->size;
            freeRanges_.erase(it);
        }
    }
}

float BufferArena::fragmentationRatio() const noexcept
{
    if (freeBytes_ == 0 || capacity_ == 0) {
        return 0.0F;
    }
    if (freeRanges_.size() <= 1) {
        return 0.0F;
    }
    VkDeviceSize largestFree = 0;
    for (const auto& range : freeRanges_) {
        if (range.size > largestFree) {
            largestFree = range.size;
        }
    }
    const auto fragmented = static_cast<float>(freeBytes_ - largestFree);
    return fragmented / static_cast<float>(freeBytes_);
}

std::vector<BufferArena::Relocation> BufferArena::defragment(
    VkCommandBuffer cmd,
    const std::vector<Slice>& liveSlices,
    VkDeviceSize alignment)
{
    if (cmd == VK_NULL_HANDLE || liveSlices.empty()) {
        return {};
    }
    if (alignment == 0) {
        alignment = 1;
    }

    auto sorted = liveSlices;
    std::sort(sorted.begin(), sorted.end(),
        [](const Slice& a, const Slice& b) { return a.offset < b.offset; });

    std::vector<Relocation> relocations;
    VkDeviceSize writeCursor = 0;

    for (const auto& slice : sorted) {
        const VkDeviceSize alignedCursor = alignUp(writeCursor, alignment);
        if (alignedCursor == slice.offset) {
            writeCursor = alignedCursor + slice.size;
            continue;
        }
        if (alignedCursor < slice.offset) {
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = slice.offset;
            copyRegion.dstOffset = alignedCursor;
            copyRegion.size = slice.size;
            vkCmdCopyBuffer(cmd, buffer_, buffer_, 1, &copyRegion);
            relocations.push_back({slice.offset, alignedCursor, slice.size});
            writeCursor = alignedCursor + slice.size;
        }
    }

    if (relocations.empty()) {
        return {};
    }

    freeBytes_ = capacity_ - writeCursor;
    freeRanges_.clear();
    if (writeCursor < capacity_) {
        freeRanges_.push_back({writeCursor, capacity_ - writeCursor});
    }

    return relocations;
}

} // namespace voxel::render
