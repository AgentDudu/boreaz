// The folder listing a song picker is built from. What is asserted is the three rules that decide
// whether a player finds the file they are looking for: folders come first, only the extensions
// this app can actually read are offered, and the order is the one a person expects rather than the
// one the filesystem happened to store. What is also asserted is the count that keeps a 30 000-file
// folder from stalling the thread that pumps the window: a truncated listing has to say it
// truncated.

#include <doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "boreaz/io/directory.h"

namespace {

// A folder that exists for the length of one test case and is gone afterwards, because a listing
// asserted against a folder the developer happened to have is an assertion that fails on every
// other machine.
struct Scratch {
    std::filesystem::path root;
    explicit Scratch(const std::string& name)
        : root(std::filesystem::temp_directory_path() / name) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root, ignored);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::vector<std::string> names(const std::vector<boreaz::io::FolderEntry>& entries) {
    std::vector<std::string> out;
    for (const boreaz::io::FolderEntry& entry : entries)
        out.push_back(entry.name);
    return out;
}

} // namespace

TEST_CASE("reads_as_song takes the two extensions the parser takes, at any case") {
    CHECK(boreaz::io::reads_as_song("sonata.mid"));
    CHECK(boreaz::io::reads_as_song("SONATA.MIDI"));
    CHECK_FALSE(boreaz::io::reads_as_song("sonata.txt"));
    CHECK_FALSE(boreaz::io::reads_as_song("mid")); // a folder named `mid` is a folder
    CHECK_FALSE(boreaz::io::reads_as_song("samples.mp3"));
    CHECK_FALSE(boreaz::io::reads_as_song(""));
}

TEST_CASE("a folder lists its folders first and only the song files it can open") {
    Scratch scratch("boreaz_dir_basic");
    std::error_code ignored;
    std::filesystem::create_directory(scratch.root / "Beethoven", ignored);
    std::filesystem::create_directory(scratch.root / "apollo", ignored);
    for (const char* file : {"alpha.mid", "BETA.MIDI", "gamma.txt", "delta.flac", "no_extension"})
        std::ofstream(scratch.root / file).close();

    std::vector<boreaz::io::FolderEntry> entries;
    std::size_t more = 0;
    std::string error;
    REQUIRE(boreaz::io::list_folder(scratch.root.string(), entries, 64, more, error));
    CHECK(error.empty());
    CHECK(more == 0);
    CHECK(names(entries) ==
          std::vector<std::string>{"apollo", "Beethoven", "alpha.mid", "BETA.MIDI"});

    // Every entry carries its own joined path, because a name is not a path and the window that
    // draws this list must not be the thing that assembles one.
    for (const boreaz::io::FolderEntry& entry : entries)
        CHECK(entry.path == (scratch.root / entry.name).string());
    CHECK(entries[0].folder);
    CHECK_FALSE(entries[2].folder);
}

TEST_CASE("the sort ignores case, because a player reads names and the filesystem does not") {
    Scratch scratch("boreaz_dir_sort");
    for (const char* file : {"zeta.mid", "Alpha.mid", "BETA.mid"})
        std::ofstream(scratch.root / file).close();

    std::vector<boreaz::io::FolderEntry> entries;
    std::size_t more = 0;
    std::string error;
    REQUIRE(boreaz::io::list_folder(scratch.root.string(), entries, 64, more, error));
    CHECK(names(entries) == std::vector<std::string>{"Alpha.mid", "BETA.mid", "zeta.mid"});
}

TEST_CASE("a folder too big to walk on the frame thread says how big it is") {
    Scratch scratch("boreaz_dir_limit");
    for (int i = 0; i < 20; ++i)
        std::ofstream(scratch.root / ("song" + std::to_string(i) + ".mid")).close();

    std::vector<boreaz::io::FolderEntry> entries;
    std::size_t more = 0;
    std::string error;
    REQUIRE(boreaz::io::list_folder(scratch.root.string(), entries, 8, more, error));
    CHECK(entries.size() == 8);
    CHECK(more == 12);

    // And the cut is the same cut every time: the whole folder is sorted before it is truncated, so
    // the names on show are the first eight of the order a player expects and not eight the
    // directory entry order felt like giving.
    CHECK(names(entries) == std::vector<std::string>{"song0.mid", "song1.mid", "song10.mid",
                                                     "song11.mid", "song12.mid", "song13.mid",
                                                     "song14.mid", "song15.mid"});
}

TEST_CASE("a folder that is not there says so instead of returning an empty list") {
    std::vector<boreaz::io::FolderEntry> entries;
    std::size_t more = 0;
    std::string error;
    const std::string missing =
        (std::filesystem::temp_directory_path() / "boreaz_dir_missing_dont_create").string();
    CHECK_FALSE(boreaz::io::list_folder(missing, entries, 64, more, error));
    CHECK(entries.empty());
    CHECK(more == 0);
    CHECK_FALSE(error.empty());

    // A file is not a folder either, and the picker needs to be able to tell the player which.
    Scratch scratch("boreaz_dir_notafolder");
    std::ofstream(scratch.root / "song.mid").close();
    CHECK_FALSE(
        boreaz::io::list_folder((scratch.root / "song.mid").string(), entries, 64, more, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("parent_folder answers where the app is standing for a bare name") {
    CHECK(boreaz::io::parent_folder("C:/songs/sonata.mid") == "C:/songs");
    CHECK(boreaz::io::parent_folder("song.mid") == ".");
    CHECK(boreaz::io::parent_folder("") == ".");
}

TEST_CASE("current_folder names the folder the app is standing in") {
    // The picker's starting point, and it has to be absolute: "." lists the same folder but
    // parent_folder(".") is ".", so a walk up from a dot stops where it began.
    const std::string here = boreaz::io::current_folder();
    CHECK_FALSE(here.empty());
    CHECK(boreaz::io::parent_folder(here + "/song.mid") != ".");
    std::vector<boreaz::io::FolderEntry> entries;
    std::size_t more = 0;
    std::string note;
    CHECK(boreaz::io::list_folder(here, entries, 50, more, note));
    CHECK(note.empty());
}

TEST_CASE("absolute_folder turns an address bar's .. into a folder") {
    // The bar shows what this returns, so it has to be something a player reads and pastes back.
    const std::string here = boreaz::io::absolute_folder(".");
    CHECK_FALSE(here.empty());
    CHECK(here[0] != '.');
    CHECK(boreaz::io::absolute_folder(here) == here);
#if defined(_WIN32)
    CHECK(boreaz::io::absolute_folder("C:/a/b/../c") == "C:/a/c"); // folded, and one separator
    CHECK(boreaz::io::absolute_folder("C:").rfind("C:", 0) == 0);
#endif
}

TEST_CASE("name_contains narrows a listing by the same folding that sorts it") {
    CHECK(boreaz::io::name_contains("Glissando-88.MID", "gli"));
    CHECK(boreaz::io::name_contains("glissando-88.mid", "SSAND"));
    CHECK_FALSE(boreaz::io::name_contains("lead-simple.mid", "glis"));
    CHECK(boreaz::io::name_contains("anything", "")); // an empty filter narrows nothing
}

#if defined(_WIN32)
TEST_CASE("drives answers the roots a player can jump to") {
    // The row of buttons that replaces climbing out of one drive a folder at a time.
    const std::vector<std::string> roots = boreaz::io::drives();
    CHECK_FALSE(roots.empty());
    for (const std::string& root : roots) {
        CHECK(root.size() == 3);
        CHECK(root[1] == ':');
        CHECK(root[2] == '/');
    }
}
#endif

#if defined(_WIN32)
TEST_CASE("a walk up to a drive root still lists the root") {
    // The edge the picker reaches by pressing Left out of C:/songs: parent_folder answers "C:",
    // which the filesystem reads as "the current directory on drive C:" and is_directory refuses. A
    // folder that plainly exists is not a reason to stop the walk.
    std::vector<boreaz::io::FolderEntry> entries;
    std::size_t more = 0;
    std::string note;
    CHECK(boreaz::io::list_folder("C:", entries, 50, more, note));
    CHECK(note.empty());
}
#endif
