#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <voxel/core/Math.hpp>
#include <voxel/core/Types.hpp>
#include <voxel/data/Identifier.hpp>
#include <voxel/world/Coordinates.hpp>

namespace voxel::magic {

enum class CastButton : std::uint8_t {
    Primary,
    Secondary
};

struct SpellCastRequest {
    SpellId spellId{};
    CastButton button{CastButton::Primary};
    world::PlanetCoord targetBlock{};
    bool hasTarget{false};
    core::Vec3 castOrigin{};
    core::Vec3 castDirection{};
};

struct SpellCastResult {
    bool success{false};
    float manaCost{0.0F};
    std::string effectName{};
};

struct ManaStore {
    float current{100.0F};
    float maximum{100.0F};
    float regenRate{2.0F};

    [[nodiscard]] bool canAfford(float cost) const noexcept { return current >= cost; }
    bool spend(float cost) noexcept
    {
        if (!canAfford(cost)) {
            return false;
        }
        current -= cost;
        return true;
    }
    void regen(float dt) noexcept
    {
        current = std::min(current + regenRate * dt, maximum);
    }
    [[nodiscard]] float fraction() const noexcept
    {
        return maximum > 0.0F ? current / maximum : 0.0F;
    }
};

struct SpellSlot {
    std::variant<data::Identifier, SpellGraphId> spell{};
    [[nodiscard]] bool empty() const noexcept
    {
        if (const auto* id = std::get_if<data::Identifier>(&spell)) {
            return id->path.empty();
        }
        if (const auto* gid = std::get_if<SpellGraphId>(&spell)) {
            return gid->value == 0;
        }
        return true;
    }
    [[nodiscard]] bool isBuiltIn() const noexcept { return std::holds_alternative<data::Identifier>(spell); }
    [[nodiscard]] bool isCraftedGraph() const noexcept { return std::holds_alternative<SpellGraphId>(spell); }
    [[nodiscard]] const data::Identifier* builtInId() const { return std::get_if<data::Identifier>(&spell); }
    [[nodiscard]] const SpellGraphId* graphId() const { return std::get_if<SpellGraphId>(&spell); }
};

class SpellHotbar {
public:
    static constexpr std::size_t SlotCount = 9;

    [[nodiscard]] const SpellSlot& slot(std::size_t index) const noexcept;
    [[nodiscard]] SpellSlot& slot(std::size_t index) noexcept;
    [[nodiscard]] std::size_t selectedSlot() const noexcept { return selected_; }
    bool select(std::size_t slot) noexcept;
    bool scroll(int direction) noexcept;
    void setSlot(std::size_t index, SpellSlot spell);

private:
    std::array<SpellSlot, SlotCount> slots_{};
    std::size_t selected_{0};
};

struct PlayerMagicState {
    ManaStore mana{};
    SpellHotbar hotbar{};
    bool castingMode{false};

    static PlayerMagicState createDefault();
};

class ISpellEffect {
public:
    virtual ~ISpellEffect() = default;
    virtual SpellCastResult cast(const SpellCastRequest& request, float dt) = 0;
    [[nodiscard]] virtual float manaCost() const noexcept = 0;
    [[nodiscard]] virtual const data::Identifier& id() const noexcept = 0;
};

class SpellRegistry {
public:
    void registerEffect(std::unique_ptr<ISpellEffect> effect);
    [[nodiscard]] ISpellEffect* find(const data::Identifier& id) const;

private:
    std::unordered_map<data::Identifier, std::unique_ptr<ISpellEffect>, data::IdentifierHash> effects_;
};

class SpellExecutor {
public:
    SpellCastResult execute(const SpellCastRequest& request, PlayerMagicState& state, float dt);
    void setSpellRegistry(const SpellRegistry* registry) noexcept { registry_ = registry; }

private:
    const SpellRegistry* registry_{nullptr};
};

class MagicSystem {
public:
    void initialize();
    void tick(Tick tick);

    PlayerMagicState& playerMagic() noexcept { return playerMagic_; }
    const PlayerMagicState& playerMagic() const noexcept { return playerMagic_; }
    SpellRegistry& spellRegistry() noexcept { return spellRegistry_; }
    SpellExecutor& spellExecutor() noexcept { return spellExecutor_; }

private:
    PlayerMagicState playerMagic_{};
    SpellRegistry spellRegistry_{};
    SpellExecutor spellExecutor_{};
};

void registerCoreSpells(SpellRegistry& registry);

} // namespace voxel::magic
