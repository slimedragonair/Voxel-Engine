#pragma once

#include <cstdint>

#include <entt/entt.hpp>

#include <voxel/core/Types.hpp>

namespace voxel::ecs {

struct TransformComponent {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
    float yaw{0.0F};
    float pitch{0.0F};
};

struct VelocityComponent {
    float vx{0.0F};
    float vy{0.0F};
    float vz{0.0F};
};

struct ItemStackComponent {
    ItemTypeId itemId{0};
    std::uint16_t count{0};
    std::uint16_t durability{0};
};

struct InventoryReferenceComponent {
    enum class OwnerType : std::uint8_t {
        Player,
        BlockEntity,
        Storage
    };
    OwnerType owner{OwnerType::Storage};
    std::uint64_t ownerKey{0};
};

struct KineticNodeComponent {
    std::uint64_t networkId{0};
    float rpm{0.0F};
    float stressCapacity{0.0F};
    float stressDemand{0.0F};
    std::uint8_t axis{0};
    bool overloaded{false};
};

struct BeltSegmentComponent {
    std::uint8_t direction{0};
    float speed{0.0F};
    entt::entity nextBelt{entt::null};
    entt::entity prevBelt{entt::null};
};

struct BlockEntityRefComponent {
    std::int64_t chunkX{0};
    std::int64_t chunkY{0};
    std::int64_t chunkZ{0};
    std::uint8_t localX{0};
    std::uint8_t localY{0};
    std::uint8_t localZ{0};
};

struct TickBudgetComponent {
    double maxMicroseconds{100.0};
    double accumulator{0.0};
    std::uint32_t tickInterval{1};
    std::uint32_t tickCounter{0};
};

struct LifetimeComponent {
    std::uint32_t ticksRemaining{0};
};

class GameRegistry {
public:
    entt::registry& registry() noexcept { return registry_; }
    const entt::registry& registry() const noexcept { return registry_; }

    entt::entity create() { return registry_.create(); }
    void destroy(entt::entity entity) { registry_.destroy(entity); }

    template <typename Component, typename... Args>
    decltype(auto) emplace(entt::entity entity, Args&&... args)
    {
        return registry_.emplace<Component>(entity, std::forward<Args>(args)...);
    }

    template <typename... Components>
    decltype(auto) view()
    {
        return registry_.view<Components...>();
    }

    template <typename... Components>
    decltype(auto) view() const
    {
        return registry_.view<Components...>();
    }

    void clear() { registry_.clear(); }
    std::size_t size() const noexcept { return registry_.storage<entt::entity>()->size() - registry_.storage<entt::entity>()->free_list(); }

private:
    entt::registry registry_;
};

} // namespace voxel::ecs
