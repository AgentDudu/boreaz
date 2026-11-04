// The factory patches in `patches/` are the instrument's own voices, and the app loads them with
// the same strict reader that refuses a player's typo. So they are checked the way that reader
// checks them: a shipped patch that names a key twice, misses one, or holds a number outside its
// range is a shipped patch that does not sound the way its name says.
//
// Completeness is the rule worth enforcing. `adopt_patch` layers a file onto the parameters that
// are already playing, so a file with a key left out is not "that key unchanged" - it is that key
// whatever the last patch happened to leave. Every factory patch therefore carries all of them,
// which is also what `save_params_file` writes, and `tools/factory_patches.py` is what expands the
// few dozen keys each patch actually changes into the whole set.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "boreaz/config/params_file.h"
#include "boreaz/engine/param_table.h"
#include "boreaz/engine/params.h"

#ifndef BOREAZ_SOURCE_DIR
#error "BOREAZ_SOURCE_DIR must be defined for the factory patch test"
#endif

namespace {

const std::filesystem::path& patch_dir() {
    static const std::filesystem::path dir{std::string{BOREAZ_SOURCE_DIR} + "/patches"};
    return dir;
}

std::vector<std::filesystem::path> patch_files() {
    std::error_code ignored;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(patch_dir(), ignored)) {
        if (entry.path().extension() == ".txt")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The next `key = value` line, comments and blanks aside. Both a factory patch and the file the app
// writes hold their parameters in the table's order, so pairing these up compares the numbers.
bool next_assignment(std::istringstream& in, std::string& out) {
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line[0] != '#' && line.find(" = ") != std::string::npos) {
            out = line;
            return true;
        }
    }
    return false;
}

// How many times a key is set by hand in this file, counting only the assignment lines and not the
// comments, which is what the reader counts too.
int times_set(const std::string& text, const std::string& key) {
    int hits = 0;
    std::istringstream lines{text};
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(key + " = ", 0) == 0)
            ++hits;
    }
    return hits;
}

} // namespace

TEST_CASE("the factory patches exist and are named by the app's own header") {
    const std::vector<std::filesystem::path> files = patch_files();
    REQUIRE_FALSE(files.empty());

    for (const auto& file : files) {
        INFO(file.string());
        std::string name;
        std::string instrument;
        std::string error;
        REQUIRE(boreaz::config::read_patch_meta(file.string(), name, instrument, error));
        // The browser falls back to the file name, but a patch that does not name itself has a
        // file name as its label, and the file name is the one with `.txt` on the end.
        CHECK_FALSE(name.empty());
        CHECK(name.size() < 96); // the panel's name box, so a factory row fits the box a save uses
        CHECK(name.find_first_of("/\\:.") == std::string::npos); // the name becomes a file name
        // Nothing machine-local or binary is in git, so a factory patch cannot name a library: it
        // keeps whatever library is playing, which is the loader's stated rule for an empty one.
        CHECK(instrument.empty());
    }
}

TEST_CASE("every factory patch sets every parameter exactly once, inside its range") {
    const std::vector<std::filesystem::path> files = patch_files();
    REQUIRE_FALSE(files.empty());

    std::size_t rows = 0;
    const boreaz::engine::ParamSpec* table = boreaz::engine::param_table(&rows);
    REQUIRE(rows > 0);

    for (const auto& file : files) {
        INFO(file.string());
        const std::string text = read_text(file);

        int set = 0;
        for (std::size_t row = 0; row < rows; ++row) {
            const int hits = times_set(text, table[row].key);
            CHECK_MESSAGE(hits == 1,
                          table[row].key << " is set " << hits << " times in " << file.filename());
            set += hits == 1 ? 1 : 0;
        }
        CHECK(set == static_cast<int>(rows));

        // The app's own reader, with the app's own strictness: an unknown key, a value that is not
        // a number, and a number outside a range are all refusals.
        boreaz::engine::Params params;
        std::string error;
        REQUIRE_MESSAGE(boreaz::config::load_params_file(file.string(), params, error), error);
    }
}

TEST_CASE("a factory patch re-saved by the app is the same numbers") {
    // The header and the comments are ours, so they are not compared. The numbers are compared,
    // because `# 0.012` and `# 0.0120` reading back as one number is the difference between a
    // patch set that drifts on every save and one that does not.
    const auto dir = std::filesystem::temp_directory_path() / "boreaz_factory_patches";
    std::filesystem::create_directories(dir);
    std::size_t rows = 0;
    REQUIRE(boreaz::engine::param_table(&rows) != nullptr);
    REQUIRE(rows > 0);

    for (const auto& file : patch_files()) {
        INFO(file.string());
        boreaz::engine::Params params;
        std::string name;
        std::string instrument;
        std::string error;
        REQUIRE(boreaz::config::read_patch_meta(file.string(), name, instrument, error));
        REQUIRE_MESSAGE(boreaz::config::load_params_file(file.string(), params, error), error);

        const auto again = dir / file.filename();
        REQUIRE_MESSAGE(
            boreaz::config::save_params_file(again.string(), params, name, instrument, error),
            error);

        std::istringstream original{read_text(file)};
        std::istringstream written{read_text(again)};
        std::size_t compared = 0;
        std::string from_file;
        std::string from_app;
        while (next_assignment(original, from_file) && next_assignment(written, from_app)) {
            INFO(from_file << " vs " << from_app);
            CHECK(from_file == from_app);
            ++compared;
        }
        CHECK(compared == rows); // neither file has a parameter the other does not
        std::filesystem::remove(again);
    }
    std::filesystem::remove(dir);
}

TEST_CASE("the folder the app writes patches into is not the folder it ships them in") {
    // The factory patches arrived, which made an overwrite possible: a save called "Bass" used to
    // be able to land on the shipped voice of the same name. It cannot, because the two directories
    // are different by construction and only one of them is ever written to.
    const std::string factory = boreaz::config::patches_dir();
    const std::string mine = boreaz::config::user_patches_dir();
    CHECK(mine != factory);
    // `%APPDATA%/Boreaz/patches`, or `patches-user` beside the app on a machine with no APPDATA.
    const bool a_folder_of_our_own = mine == "patches-user" || mine.ends_with("/Boreaz/patches");
    CHECK(a_folder_of_our_own);
    for (const auto& file : patch_files())
        CHECK(file.string().find(mine) == std::string::npos);
}

TEST_CASE("the presets ship a room, and the registers do not") {
    int rooms = 0;
    for (const auto& file : patch_files()) {
        boreaz::engine::Params params;
        std::string error;
        REQUIRE_MESSAGE(boreaz::config::load_params_file(file.string(), params, error), error);
        if (params.fx_reverb_mix > 0.0f)
            ++rooms;
    }
    CHECK(rooms >= 3); // the three dynamics presets, so the room is not a slider nobody finds
}
