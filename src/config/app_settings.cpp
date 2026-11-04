// The machine's setup, in four lines. Where the parameter file describes a sound and refuses to be
// part-read, this one describes a machine, and the two rules that follow from that difference are
// written next to the code that obeys them: an unknown key here is a line to report and skip, not a
// reason the instrument cannot start, and the file is written whole because a hole in it would mean
// "the default this version happens to have", which is not the same file twice.

// Same reason params_file.cpp gives: this asks the environment for a path, and MSVC deprecates the
// standard function for it. The value is copied into a std::string on the line that reads it, so
// the define is the honest answer rather than a per-platform branch around one call. It has to
// precede the includes to mean anything.
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE

#include "boreaz/config/app_settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace boreaz::config {
namespace {

// The header line, for the same reason the patch format has one: a number a future reader can
// compare against, rather than a date and a guess.
constexpr const char* kSettingsHeader = "# boreaz app setup 1";

// The five keys, in the order they are written. A name here is a name in the file, and the list is
// walked by both the reader and the writer so neither of them can know a key the other does not.
struct Field {
    const char* key;
    std::string AppSettings::* value;
};

const Field kFields[] = {
    {"audio_device", &AppSettings::audio_device}, {"midi_port", &AppSettings::midi_port},
    {"instrument", &AppSettings::instrument},     {"patch", &AppSettings::patch},
    {"peak_decay", &AppSettings::peak_decay},
};

std::string trimmed(std::string text) {
    const char* space = " \t\r\n";
    const std::string::size_type from = text.find_first_not_of(space);
    if (from == std::string::npos)
        return {};
    return text.substr(from, text.find_last_not_of(space) - from + 1);
}

} // namespace

std::string settings_path() {
    const char* app_data = std::getenv("APPDATA");
    if (app_data != nullptr && *app_data != '\0')
        return std::string{app_data} + "/Boreaz/settings.txt";
    // No APPDATA: a service session, or a profile with the variable cleared. Next to the working
    // directory beats nowhere, and it is the shape of everything else that falls back this way.
    return "settings.txt";
}

void load_settings(const std::string& path, AppSettings& into, std::string& warning) {
    std::ifstream file(path);
    if (!file)
        return; // a first run, which is the normal case and not worth a word

    // One note per thing the file said that this version cannot use, so the app can print the whole
    // list at once instead of the first one and nothing else.
    auto note = [&](const std::string& what) { warning += (warning.empty() ? "" : "\n") + what; };

    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
        ++number;
        const std::string text = trimmed(line);
        if (text.empty() || text[0] == '#')
            continue;
        const std::string::size_type sign = text.find('=');
        if (sign == std::string::npos) {
            note("line " + std::to_string(number) + ": not a `key = value` line: " + text);
            continue;
        }
        const std::string key = trimmed(text.substr(0, sign));
        const std::string value = trimmed(text.substr(sign + 1));
        const Field* found = nullptr;
        for (const Field& field : kFields)
            if (key == field.key)
                found = &field;
        if (found == nullptr) {
            // Ignored, and said. A setup file written by a newer version of the app should not stop
            // an older one from playing: a machine that cannot start because of a word it does not
            // know is a machine with no way out.
            note("line " + std::to_string(number) + ": not a setting this version knows: " + key);
            continue;
        }
        into.*found->value = value;
    }
}

bool save_settings(const std::string& path, const AppSettings& settings, std::string& error) {
    // Built in memory first, then one write, for the reason the patch writer gives: a file that
    // stops half way is a file that loads and is wrong.
    std::string body = kSettingsHeader;
    for (const Field& field : kFields) {
        body += "\n";
        body += field.key;
        body += " = ";
        body += settings.*field.value;
    }
    body += "\n";

    const std::filesystem::path target{path};
    if (target.has_parent_path()) {
        // The directory is the app's own, under APPDATA; making it is not an intrusion, and writing
        // into a directory that is not there is only a failure with a worse message.
        std::error_code ignored;
        std::filesystem::create_directories(target.parent_path(), ignored);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out << body;
    if (!out) {
        error = "the disk refused " + path;
        return false;
    }
    return true;
}

} // namespace boreaz::config
