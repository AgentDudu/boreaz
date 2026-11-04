// What the machine was set up with is a different file from what the instrument sounds like, and it
// is read under a different rule. A parameter file with a typo in it is refused whole, because a
// half-read sound is a sound nobody wrote and nobody can name. A setup file with a word this
// version does not know has to be skipped, reported, and the rest of it obeyed: a program that will
// not start over a line nobody can see is a program with no way out. Four things to hold that line,
// and one to keep the two files from being confused for each other.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "boreaz/config/app_settings.h"
#include "boreaz/config/params_file.h"
#include "boreaz/engine/param_table.h"

namespace {

std::filesystem::path scratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "boreaz_app_settings";
    std::filesystem::create_directories(dir);
    return dir / name;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("the setup the app wrote is the setup the app reads") {
    // Names with spaces, brackets and backslashes in them, because that is what a Windows port and
    // a sample library are actually called. The value is everything after the first `=`, so none of
    // it needs quoting - and a setup file that needed quoting would be a file a player cannot fix
    // with Notepad.
    boreaz::config::AppSettings written;
    written.audio_device = "CABLE In 16 Ch (VB-Audio Virtual Cable)";
    written.midi_port = "Auto Input Port (01-Desktop)";
    written.instrument = "D:\\instruments\\grand upright\\library.txt";
    written.patch.clear(); // nothing remembered, which is a state and not an absence

    std::string error;
    const std::filesystem::path file = scratch("roundtrip.txt");
    REQUIRE_MESSAGE(boreaz::config::save_settings(file.string(), written, error), error);

    boreaz::config::AppSettings read;
    std::string warning;
    boreaz::config::load_settings(file.string(), read, warning);
    CHECK(warning.empty());
    CHECK(read.audio_device == written.audio_device);
    CHECK(read.midi_port == written.midi_port);
    CHECK(read.instrument == written.instrument);
    CHECK(read.patch == written.patch);

    // Save it again out of what was read back and the bytes have to be the same. A file that drifts
    // a little on every close is a file whose history nobody asked for, and the same claim the
    // patch writer makes.
    const std::string first = read_text(file);
    REQUIRE_MESSAGE(boreaz::config::save_settings(file.string(), read, error), error);
    CHECK(read_text(file) == first);
}

TEST_CASE("a setting this version does not know is skipped and said") {
    // Written by a newer build of the app. The older one plays: the words it knows are obeyed and
    // the word it does not is named, because a startup that stops on an invisible line is a startup
    // with no way out - which is the opposite of the rule a parameter file runs under, on purpose.
    const std::filesystem::path file = scratch("newer.txt");
    write_text(file, "# boreaz app setup 1\n"
                     "audio_device = Speakers (Realtek)\n"
                     "quantize = 0.5\n"
                     "midi_port = auto\n");

    boreaz::config::AppSettings read;
    std::string warning;
    boreaz::config::load_settings(file.string(), read, warning);
    CHECK(read.audio_device == "Speakers (Realtek)");
    CHECK(read.midi_port == "auto");
    CHECK(read.instrument.empty());
    MESSAGE(warning);
    CHECK(warning.find("quantize") != std::string::npos);
    CHECK(warning.find("3") != std::string::npos);
}

TEST_CASE("no setup file is a first run, and a broken line is a line and not a crash") {
    boreaz::config::AppSettings fresh;
    std::string warning;
    boreaz::config::load_settings((scratch("never_written.txt")).string(), fresh, warning);
    CHECK(fresh.audio_device.empty());
    CHECK(fresh.patch.empty());
    CHECK(
        warning
            .empty()); // a first run is normal, and saying something about it every start is noise

    // A line that is not an assignment at all - a stray word, a hand edit, a note to self left in
    // the file. It is reported with the number of the line it is on, and the keys around it work.
    const std::filesystem::path file = scratch("broken.txt");
    write_text(file, "# boreaz app setup 1\n"
                     "instrument = D:\\piano\\library.txt\n"
                     "remember the cable here, not the speaker\n"
                     "patch = D:\\Boreaz\\patches\\bass.txt\n");
    boreaz::config::AppSettings read;
    warning.clear();
    boreaz::config::load_settings(file.string(), read, warning);
    CHECK(read.instrument == "D:\\piano\\library.txt");
    CHECK(read.patch == "D:\\Boreaz\\patches\\bass.txt");
    CHECK(warning.find("3") != std::string::npos);

    // And a value that holds an `=` itself: the split is at the first one, so a path like this one
    // survives instead of becoming a key nobody wrote.
    write_text(file, "patch = C:\\a=b\\p.txt\n");
    boreaz::config::AppSettings edge;
    warning.clear();
    boreaz::config::load_settings(file.string(), edge, warning);
    CHECK(edge.patch == "C:\\a=b\\p.txt");
    CHECK(warning.empty());
}

TEST_CASE("the setup file is the machine's, and it is not a patch") {
    // Two files, two rules, and the way to keep them from drifting into each other is to say where
    // each one lives and to prove the engine has no parameter with any of the other's names.
    const std::string settings = boreaz::config::settings_path();
    CHECK(settings != boreaz::config::defaults_path());
    // One folder for everything the machine writes: the patches a player saves and the setup the
    // app remembers. Not the repository's, which is the other half of the rule - and it holds on a
    // machine with no `%APPDATA%` too, where both names land beside the app.
    CHECK(std::filesystem::path(settings).parent_path().string() ==
          std::filesystem::path(boreaz::config::user_patches_dir()).parent_path().string());

    for (const char* key : {"audio_device", "midi_port", "instrument", "patch"})
        CHECK_MESSAGE(boreaz::engine::param_spec(key) == nullptr,
                      key << " is a setting, and must not also be an engine parameter");

    // The strict parameter reader refuses this file, which is the same fact from the other side: a
    // setup file cannot be loaded as a patch, and a patch cannot be loaded as a setup file.
    const std::filesystem::path file = scratch("as_patch.txt");
    boreaz::config::AppSettings setup;
    setup.audio_device = "Speakers";
    setup.midi_port = "auto";
    std::string error;
    REQUIRE_MESSAGE(boreaz::config::save_settings(file.string(), setup, error), error);

    boreaz::engine::Params params;
    std::string reason;
    CHECK_FALSE(boreaz::config::load_params_file(file.string(), params, reason));
    CHECK(reason.find("audio_device") != std::string::npos);
}
