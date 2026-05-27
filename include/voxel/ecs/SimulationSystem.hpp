#pragma once

#include <chrono>
#include <cstdint>

#include <voxel/ecs/Components.hpp>

namespace voxel::ecs {

struct SimulationStats {
    std::size_t entitiesTicked{0};
    std::size_t blockEntitiesTicked{0};
    std::size_t beltEntitiesTicked{0};
    std::size_t itemDropsTicked{0};
    double simulationMs{0.0};
    bool budgetSaturated{false};
};

class SimulationSystem {
public:
    void initialize();
    void tick(GameRegistry& registry, float dt);

    [[nodiscard]] const SimulationStats& stats() const noexcept { return stats_; }
    SimulationStats drainStats();

    void setBudgetMs(double budgetMs) noexcept { budgetMs_ = budgetMs; }

private:
    void tickItemDrops(GameRegistry& registry, float dt);
    void tickBelts(GameRegistry& registry, float dt);

    SimulationStats stats_;
    double budgetMs_{3.0};
};

} // namespace voxel::ecs
