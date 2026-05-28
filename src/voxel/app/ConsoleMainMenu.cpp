#include <voxel/app/ConsoleMainMenu.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace voxel::app {

namespace {

std::string trim(std::string_view value)
{
    // Strip a leading UTF-8 BOM (EF BB BF). PowerShell + several Windows
    // tools quietly prepend it when piping text; without this every first
    // line of stdin would fail the prefix-character check below.
    if (value.size() >= 3
        && static_cast<unsigned char>(value[0]) == 0xEFu
        && static_cast<unsigned char>(value[1]) == 0xBBu
        && static_cast<unsigned char>(value[2]) == 0xBFu) {
        value.remove_prefix(3);
    }
    auto begin = value.find_first_not_of(" \t\r\n");
    auto end = value.find_last_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    return std::string{value.substr(begin, end - begin + 1)};
}

bool parseInt(std::string_view text, int& out)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parseUnsigned64(std::string_view text, std::uint64_t& out)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Format a Unix-ms timestamp as "YYYY-MM-DD HH:MM UTC". Returns "never" for
// the 0 sentinel so the menu doesn't print a 1970 date for freshly minted
// worlds whose lastPlayedAtMs hasn't been bumped yet.
std::string formatTimestamp(std::int64_t unixMs)
{
    if (unixMs <= 0) {
        return "never";
    }
    const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &seconds) != 0) {
        return "?";
    }
#else
    if (gmtime_r(&seconds, &tm) == nullptr) {
        return "?";
    }
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &tm) == 0) {
        return "?";
    }
    return std::string{buffer};
}

void printWorldList(std::ostream& output, const std::vector<save::WorldEntry>& worlds)
{
    output << "\n=== Worlds ===\n";
    if (worlds.empty()) {
        output << "  (no saved worlds yet)\n";
        return;
    }
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        const auto& d = worlds[i].descriptor;
        output << "  [" << (i + 1) << "] " << d.name
               << "  seed=" << d.seed
               << "  last played: " << formatTimestamp(d.lastPlayedAtMs)
               << "\n";
    }
}

MenuResult newWorldFlow(save::WorldRegistry& registry, std::istream& input, std::ostream& output)
{
    output << "\n=== New World ===\n";
    output << "Name (blank = \"New World\"): " << std::flush;
    std::string nameLine;
    std::getline(input, nameLine);
    auto name = trim(nameLine);
    if (name.empty()) {
        name = "New World";
    }

    output << "Seed (blank = random uint64): " << std::flush;
    std::string seedLine;
    std::getline(input, seedLine);
    auto seedText = trim(seedLine);

    std::uint64_t seed = 0;
    if (!seedText.empty() && !parseUnsigned64(seedText, seed)) {
        output << "  (couldn't parse seed; using a random one)\n";
        seed = 0;
    }

    auto entry = registry.createWorld(std::move(name), seed);
    if (!entry.has_value()) {
        output << "  Failed to create world (couldn't write world.json).\n";
        return MenuResult{};
    }
    output << "  Created \"" << entry->descriptor.name << "\" "
           << "(seed=" << entry->descriptor.seed << ", dir=" << entry->root.filename().string() << ")\n";
    return MenuResult{MenuResult::Kind::EnterWorld, std::move(*entry)};
}

// Best-effort confirmation prompt. Returns true when the player explicitly
// types something starting with 'y' or 'Y'. Anything else (including EOF)
// is treated as a cancel.
bool confirmYesNo(std::istream& input, std::ostream& output, std::string_view prompt)
{
    output << prompt << " [y/N]: " << std::flush;
    std::string line;
    if (!std::getline(input, line)) {
        return false;
    }
    const auto trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }
    return trimmed[0] == 'y' || trimmed[0] == 'Y';
}

void renameWorldFlow(save::WorldRegistry& registry,
                     const save::WorldEntry& entry,
                     std::istream& input,
                     std::ostream& output)
{
    output << "\n=== Rename \"" << entry.descriptor.name << "\" ===\n";
    output << "New name (blank to cancel): " << std::flush;
    std::string line;
    if (!std::getline(input, line)) {
        return;
    }
    auto trimmed = trim(line);
    if (trimmed.empty()) {
        output << "  Cancelled.\n";
        return;
    }
    auto updated = registry.renameWorld(entry, std::move(trimmed));
    if (!updated.has_value()) {
        output << "  Failed to rename (couldn't update world.json).\n";
        return;
    }
    output << "  Renamed to \"" << updated->descriptor.name << "\".\n";
}

void deleteWorldFlow(save::WorldRegistry& registry,
                     const save::WorldEntry& entry,
                     std::istream& input,
                     std::ostream& output)
{
    output << "\n=== Delete \"" << entry.descriptor.name << "\" ===\n";
    output << "  Directory: " << entry.root.string() << "\n";
    output << "  Seed: " << entry.descriptor.seed << "\n";
    output << "  WARNING: this is permanent. All chunks, player data, and\n"
           << "           inventory for this world will be removed.\n";
    if (!confirmYesNo(input, output, "Really delete this world?")) {
        output << "  Cancelled.\n";
        return;
    }
    if (!registry.deleteWorld(entry)) {
        output << "  Failed to delete world (filesystem error).\n";
        return;
    }
    output << "  Deleted \"" << entry.descriptor.name << "\".\n";
}

// Parses "<letter> <index>" where letter is one of r/R/d/D and index is
// 1-based. Returns the resolved letter (lowercased) and 1-based index, or
// nullopt if the command didn't parse.
struct LetterIndex {
    char letter{};
    int index{};
};

std::optional<LetterIndex> parseLetterIndex(std::string_view cmd, char expectedLetter)
{
    if (cmd.size() < 2) {
        return std::nullopt;
    }
    const auto first = static_cast<char>(std::tolower(static_cast<unsigned char>(cmd[0])));
    if (first != expectedLetter) {
        return std::nullopt;
    }
    auto rest = cmd.substr(1);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
        rest.remove_prefix(1);
    }
    int idx = 0;
    if (!parseInt(rest, idx)) {
        return std::nullopt;
    }
    return LetterIndex{first, idx};
}

} // namespace

ConsoleMainMenu::ConsoleMainMenu(save::WorldRegistry& registry)
    : registry_(registry)
{
}

MenuResult ConsoleMainMenu::run(std::istream& input, std::ostream& output)
{
    output << "\n=================================\n"
           << " AetherForge: Infinite Creation\n"
           << "=================================\n";

    while (true) {
        const auto worlds = registry_.listWorlds();
        printWorldList(output, worlds);
        output << "\nOptions:\n"
               << "  [1-" << worlds.size() << "] load world\n"
               << "  [n]   new world\n"
               << "  [r N] rename world N\n"
               << "  [d N] delete world N\n"
               << "  [q]   quit\n"
               << "> " << std::flush;

        std::string line;
        if (!std::getline(input, line)) {
            // EOF / stdin closed: treat as quit.
            output << "(stdin closed) quitting.\n";
            return MenuResult{};
        }
        const auto cmd = trim(line);
        if (cmd.empty()) {
            continue;
        }

        // Single-letter shortcuts + numbered index. Rename/delete take a
        // 1-based index, e.g. "r 1" renames world 1, "d 2" deletes world 2.
        if (auto cmdR = parseLetterIndex(cmd, 'r'); cmdR.has_value()
            && cmdR->index >= 1 && static_cast<std::size_t>(cmdR->index) <= worlds.size()) {
            renameWorldFlow(registry_, worlds[cmdR->index - 1], input, output);
            continue;
        }
        if (auto cmdD = parseLetterIndex(cmd, 'd'); cmdD.has_value()
            && cmdD->index >= 1 && static_cast<std::size_t>(cmdD->index) <= worlds.size()) {
            deleteWorldFlow(registry_, worlds[cmdD->index - 1], input, output);
            continue;
        }

        const auto firstChar = static_cast<char>(std::tolower(static_cast<unsigned char>(cmd[0])));
        if (firstChar == 'q') {
            return MenuResult{};
        }
        if (firstChar == 'n') {
            auto result = newWorldFlow(registry_, input, output);
            if (result.kind == MenuResult::Kind::EnterWorld) {
                return result;
            }
            continue;
        }

        int picked = 0;
        if (parseInt(cmd, picked) && picked >= 1 && static_cast<std::size_t>(picked) <= worlds.size()) {
            return MenuResult{MenuResult::Kind::EnterWorld, worlds[picked - 1]};
        }
        output << "  (didn't understand \"" << cmd << "\")\n";
    }
}

} // namespace voxel::app
