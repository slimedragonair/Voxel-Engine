#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace voxel::render {

class BufferArena {
public:
    struct Slice {
        VkDeviceSize offset{0};
        VkDeviceSize size{0};
        [[nodiscard]] bool valid() const noexcept { return size > 0; }
    };

    struct Relocation {
        VkDeviceSize oldOffset{};
        VkDeviceSize newOffset{};
        VkDeviceSize size{};
    };

    BufferArena() = default;
    ~BufferArena();

    BufferArena(const BufferArena&) = delete;
    BufferArena& operator=(const BufferArena&) = delete;

    void initialize(VkDevice device, VmaAllocator allocator,
                    VkDeviceSize capacity, VkBufferUsageFlags usage);
    void shutdown();

    [[nodiscard]] bool allocate(VkDeviceSize bytes, VkDeviceSize alignment, Slice& out);
    void release(Slice slice);

    [[nodiscard]] VkBuffer buffer() const noexcept { return buffer_; }
    [[nodiscard]] VkDeviceSize capacity() const noexcept { return capacity_; }
    [[nodiscard]] VkDeviceSize used() const noexcept { return capacity_ - freeBytes_; }
    [[nodiscard]] VkDeviceSize freeBytes() const noexcept { return freeBytes_; }
    [[nodiscard]] std::size_t freeRangeCount() const noexcept { return freeRanges_.size(); }

    [[nodiscard]] float fragmentationRatio() const noexcept;

    std::vector<Relocation> defragment(
        VkCommandBuffer cmd,
        const std::vector<Slice>& liveSlices,
        VkDeviceSize alignment);

private:
    struct FreeRange {
        VkDeviceSize offset{};
        VkDeviceSize size{};
    };

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    VkBuffer buffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{VK_NULL_HANDLE};
    VkDeviceSize capacity_{0};
    VkDeviceSize freeBytes_{0};
    std::vector<FreeRange> freeRanges_;
};

} // namespace voxel::render
