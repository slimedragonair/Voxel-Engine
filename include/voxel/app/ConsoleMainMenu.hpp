#pragma once

#include <iosfwd>
#include <string>

#include <voxel/save/WorldRegistry.hpp>

namespace voxel::app {

// Outcome of running the menu. `kind == EnterWorld` carries the chosen
// world; `kind == Quit` means the player picked "exit" (or stdin closed).
struct MenuResult {
    enum class Kind { Quit, EnterWorld };
    Kind kind{Kind::Quit};
    save::WorldEntry world{};
};

// A bootstrap menu that runs *before* the Vulkan window is opened. It lets
// the player pick a save, create a new world, or quit — all over plain
// stdin/stdout. The renderer doesn't have a font system yet, so this is the
// simplest UI that hits the user's "main menu" goal without blocking on a
// proper UI layer.
//
// `run()` accepts the streams explicitly so unit tests can drive the menu
// with std::istringstream/std::ostringstream.
class ConsoleMainMenu {
public:
    explicit ConsoleMainMenu(save::WorldRegistry& registry);

    [[nodiscard]] MenuResult run(std::istream& input, std::ostream& output);

private:
    save::WorldRegistry& registry_;
};

} // namespace voxel::app
