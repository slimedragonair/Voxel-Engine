#include <voxel/ecs/SimulationSystem.hpp>

#include <chrono>

namespace voxel::ecs {

void SimulationSystem::initialize()
{
}

void SimulationSystem::tick(GameRegistry& registry, float dt)
{
    const auto start = std::chrono::steady_clock::now();

    tickItemDrops(registry, dt);
    tickBelts(registry, dt);

    const auto end = std::chrono::steady_clock::now();
    stats_.simulationMs = std::chrono::duration<double, std::milli>(end - start).count();
    stats_.budgetSaturated = stats_.simulationMs >= budgetMs_;
}

SimulationStats SimulationSystem::drainStats()
{
    auto result = stats_;
    stats_ = SimulationStats{};
    return result;
}

void SimulationSystem::tickItemDrops(GameRegistry& registry, float dt)
{
    auto view = registry.view<ItemStackComponent, LifetimeComponent, TransformComponent>();
    for (auto entity : view) {
        auto& lifetime = view.get<LifetimeComponent>(entity);
        if (lifetime.ticksRemaining > 0) {
            --lifetime.ticksRemaining;
        }
        if (lifetime.ticksRemaining == 0) {
            registry.destroy(entity);
            ++stats_.itemDropsTicked;
        }
    }
}

void SimulationSystem::tickBelts(GameRegistry& registry, float dt)
{
    auto view = registry.view<BeltSegmentComponent, TransformComponent>();
    for (auto entity : view) {
        ++stats_.beltEntitiesTicked;
    }
}

} // namespace voxel::ecs
