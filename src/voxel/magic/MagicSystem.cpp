#include <voxel/magic/MagicSystem.hpp>

#include <memory>

#include <voxel/core/Logger.hpp>

namespace voxel::magic {

const SpellSlot& SpellHotbar::slot(std::size_t index) const noexcept
{
    return slots_[index];
}

SpellSlot& SpellHotbar::slot(std::size_t index) noexcept
{
    return slots_[index];
}

bool SpellHotbar::select(std::size_t slot) noexcept
{
    if (slot >= SlotCount) {
        return false;
    }
    selected_ = slot;
    return true;
}

bool SpellHotbar::scroll(int direction) noexcept
{
    if (direction > 0 && selected_ > 0) {
        --selected_;
        return true;
    }
    if (direction < 0 && selected_ + 1 < SlotCount) {
        ++selected_;
        return true;
    }
    return false;
}

void SpellHotbar::setSlot(std::size_t index, SpellSlot spell)
{
    if (index < SlotCount) {
        slots_[index] = std::move(spell);
    }
}

PlayerMagicState PlayerMagicState::createDefault()
{
    PlayerMagicState state;
    state.mana = ManaStore{100.0F, 100.0F, 2.0F};
    state.hotbar.setSlot(0, SpellSlot{data::Identifier{"core", "arcane_break"}});
    state.hotbar.setSlot(1, SpellSlot{data::Identifier{"core", "arcane_place"}});
    state.hotbar.setSlot(2, SpellSlot{data::Identifier{"core", "mage_light"}});
    state.hotbar.setSlot(3, SpellSlot{data::Identifier{"core", "push_pulse"}});
    state.hotbar.setSlot(4, SpellSlot{data::Identifier{"core", "transmute_debug"}});
    return state;
}

void SpellRegistry::registerEffect(std::unique_ptr<ISpellEffect> effect)
{
    if (effect == nullptr) {
        return;
    }
    auto id = effect->id();
    effects_.emplace(id, std::move(effect));
}

ISpellEffect* SpellRegistry::find(const data::Identifier& id) const
{
    const auto it = effects_.find(id);
    return it != effects_.end() ? it->second.get() : nullptr;
}

SpellCastResult SpellExecutor::execute(const SpellCastRequest& request, PlayerMagicState& state, float dt)
{
    (void)dt;
    if (registry_ == nullptr) {
        return {};
    }

    if (request.spellId.value == 0) {
        return {};
    }

    const auto* slot = state.hotbar.slot(state.hotbar.selectedSlot()).builtInId();
    if (slot == nullptr) {
        return {};
    }

    auto* effect = registry_->find(*slot);
    if (effect == nullptr) {
        Logger::warn("Spell not found: " + slot->str());
        return {};
    }

    if (!state.mana.canAfford(effect->manaCost())) {
        Logger::info("Spell cast failed: insufficient mana (" + std::to_string(state.mana.current) + "/" + std::to_string(effect->manaCost()) + ")");
        return {};
    }

    auto result = effect->cast(request, dt);
    if (result.success) {
        state.mana.spend(effect->manaCost());
        Logger::info("Spell cast: " + result.effectName + " mana=" + std::to_string(state.mana.current) + "/" + std::to_string(state.mana.maximum));
    }
    return result;
}

namespace {

class ArcaneBreakSpell : public ISpellEffect {
public:
    SpellCastResult cast(const SpellCastRequest& request, float /*dt*/) override
    {
        SpellCastResult result;
        result.success = request.hasTarget;
        result.manaCost = manaCost();
        result.effectName = "Arcane Break";
        return result;
    }
    [[nodiscard]] float manaCost() const noexcept override { return 5.0F; }
    [[nodiscard]] const data::Identifier& id() const noexcept override { return id_; }

private:
    data::Identifier id_{"core", "arcane_break"};
};

class ArcanePlaceSpell : public ISpellEffect {
public:
    SpellCastResult cast(const SpellCastRequest& request, float /*dt*/) override
    {
        SpellCastResult result;
        result.success = request.hasTarget;
        result.manaCost = manaCost();
        result.effectName = "Arcane Place";
        return result;
    }
    [[nodiscard]] float manaCost() const noexcept override { return 8.0F; }
    [[nodiscard]] const data::Identifier& id() const noexcept override { return id_; }

private:
    data::Identifier id_{"core", "arcane_place"};
};

class MageLightSpell : public ISpellEffect {
public:
    SpellCastResult cast(const SpellCastRequest& /*request*/, float /*dt*/) override
    {
        SpellCastResult result;
        result.success = true;
        result.manaCost = manaCost();
        result.effectName = "Mage Light";
        return result;
    }
    [[nodiscard]] float manaCost() const noexcept override { return 3.0F; }
    [[nodiscard]] const data::Identifier& id() const noexcept override { return id_; }

private:
    data::Identifier id_{"core", "mage_light"};
};

class PushPulseSpell : public ISpellEffect {
public:
    SpellCastResult cast(const SpellCastRequest& /*request*/, float /*dt*/) override
    {
        SpellCastResult result;
        result.success = true;
        result.manaCost = manaCost();
        result.effectName = "Push Pulse";
        return result;
    }
    [[nodiscard]] float manaCost() const noexcept override { return 10.0F; }
    [[nodiscard]] const data::Identifier& id() const noexcept override { return id_; }

private:
    data::Identifier id_{"core", "push_pulse"};
};

class TransmuteDebugSpell : public ISpellEffect {
public:
    SpellCastResult cast(const SpellCastRequest& request, float /*dt*/) override
    {
        SpellCastResult result;
        result.success = request.hasTarget;
        result.manaCost = manaCost();
        result.effectName = "Transmute Debug";
        return result;
    }
    [[nodiscard]] float manaCost() const noexcept override { return 15.0F; }
    [[nodiscard]] const data::Identifier& id() const noexcept override { return id_; }

private:
    data::Identifier id_{"core", "transmute_debug"};
};

} // namespace

void registerCoreSpells(SpellRegistry& registry)
{
    registry.registerEffect(std::make_unique<ArcaneBreakSpell>());
    registry.registerEffect(std::make_unique<ArcanePlaceSpell>());
    registry.registerEffect(std::make_unique<MageLightSpell>());
    registry.registerEffect(std::make_unique<PushPulseSpell>());
    registry.registerEffect(std::make_unique<TransmuteDebugSpell>());
}

void MagicSystem::initialize()
{
    playerMagic_ = PlayerMagicState::createDefault();
    registerCoreSpells(spellRegistry_);
    spellExecutor_.setSpellRegistry(&spellRegistry_);
}

void MagicSystem::tick(Tick /*tick*/)
{
    playerMagic_.mana.regen(1.0F / 60.0F);
}

} // namespace voxel::magic
