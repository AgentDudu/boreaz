// The one place Boreaz asks the environment for a path, and MSVC deprecates `getenv` for it. The
// deprecation is about a returned buffer being overwritten by the next call, and the value here is
// copied into a std::string on the line that reads it, so the define is the honest answer rather
// than a per-platform branch around one call. It has to precede the includes to mean anything.
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE

#include "boreaz/config/params_file.h"

#include "boreaz/engine/param_table.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace boreaz::config {
namespace {

bool exists(const std::string& path) {
    std::error_code ignored;
    return std::filesystem::is_regular_file(path, ignored);
}

// The header line. A number rather than a date so a future reader can tell which fields it may
// expect; nothing reads it yet, and that is the point of writing it down before anything needs it.
constexpr const char* kPatchHeader = "# boreaz patch format 1";

// A name or a path with a newline in it would write a second header line and read back as something
// else, so the newline goes and a space takes its place rather than the two words running together.
std::string one_line(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c == '\n' || c == '\r')
            c = ' ';
    }
    return out;
}

// The shortest text that reads back as the same float. `printf("%g")` gives six digits and loses a
// slider position a tenth of a decibel away; `%.9g` keeps every bit but writes `0.100000001` for a
// value the person set to 0.1. `to_chars` with no precision is the shortest round trip in both
// directions, which is what makes "save, load, save, same bytes" a checkable claim instead of a
// hope.
std::string number_text(const boreaz::engine::ParamSpec& spec, float value) {
    if (boreaz::engine::param_is_integer(spec)) {
        // The reader refuses `octave_shift = 1.5`, and it would refuse `4.0` too: an integer row is
        // written as an integer.
        return std::to_string(static_cast<long long>(value));
    }
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    if (result.ec != std::errc{})
        return "0"; // unreachable for a finite float; the caller refuses a non-finite value first
    return std::string(buffer, result.ptr);
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

} // namespace

using boreaz::engine::Params;

// One key and its text, applied. `where` names the place it came from - "line 12", "--gain" - so
// the message reads like the thing the person typed rather than like the function that parsed it.
//
// Strict in two directions: the whole string has to be a number, and the number has to be inside
// the parameter's own range. A file or a flag that quietly did something near what it says is worse
// than one that stops, and `Params::clamp_all` downstream cannot tell "you meant 1000" from "I
// meant 100".
bool apply_value(const boreaz::engine::ParamSpec& spec, const std::string& key,
                 const std::string& value, Params& params, const std::string& where,
                 std::string& error) {
    const std::string prefix = where + ": '" + key + "'";
    if (!boreaz::engine::param_is_integer(spec)) {
        // strtod plus "and it consumed the whole string": `sscanf("%f")` is happy to read the 2
        // in "2..0" and call the line good, which is a patch that quietly does not say what it
        // says.
        char* end = nullptr;
        const float parsed = static_cast<float>(std::strtod(value.c_str(), &end));
        if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) {
            error = prefix + " wants a number, got '" + value + "'";
            return false;
        }
        // Out of range is refused here rather than quietly clamped later: the person who typed
        // `filter_cutoff_ratio = 1000` wants to know the file did not do what it says.
        if (parsed < spec.minimum || parsed > spec.maximum) {
            char bounds[64];
            std::snprintf(bounds, sizeof bounds, "%g to %g", spec.minimum, spec.maximum);
            error = prefix + " wants " + bounds + ", got '" + value + "'";
            return false;
        }
        boreaz::engine::param_write(spec, parsed, params);
        return true;
    }

    char* end = nullptr;
    const long long whole = std::strtoll(value.c_str(), &end, 10);
    // "1.5" for a transpose is not rounded: it is refused. The file promised a whole number
    // and the person who wrote it means something specific by that.
    if (end == value.c_str() || *end != '\0') {
        error = prefix + " wants a whole number, got '" + value + "'";
        return false;
    }
    const int parsed = static_cast<int>(whole);
    if (static_cast<float>(parsed) < spec.minimum || static_cast<float>(parsed) > spec.maximum) {
        error = prefix + " wants " + std::to_string(static_cast<int>(spec.minimum)) + " to " +
                std::to_string(static_cast<int>(spec.maximum)) + ", got '" + value + "'";
        return false;
    }
    // Through the table, never through `spec.integer`: a row that lives in an array has no member
    // pointer of its own, and the table is what knows which element of which array it is.
    boreaz::engine::param_write(spec, static_cast<float>(parsed), params);
    return true;
}

// `key = value`, one per line, `#` starts a comment. Deliberately the same shape as the patch files
// the preset phase will ship: a render parameter file and a preset are the same thing with a
// different extension. Moved out of the render tool when the app started wanting the same thing -
// two copies of this table is the failure mode the plan warns about, and the names, ranges and
// defaults now come from `engine::param_table` for the third time: the window draws the same rows.
bool load_params_file(const std::string& path, Params& params, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open the parameter file '" + path + "'";
        return false;
    }

    std::string line;
    int number = 0;
    for (; std::getline(file, line); ++number) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue; // blank or a stray line: nothing asked of us

        const auto trim = [](std::string& text) {
            const char* whitespace = " \t\r\n";
            const std::size_t begin = text.find_first_not_of(whitespace);
            if (begin == std::string::npos) {
                text.clear();
                return;
            }
            const std::size_t end = text.find_last_not_of(whitespace);
            text = text.substr(begin, end - begin + 1);
        };

        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        trim(key);
        trim(value);
        if (key.empty())
            continue;

        const boreaz::engine::ParamSpec* spec = boreaz::engine::param_spec(key);
        if (spec == nullptr) {
            error = "line " + std::to_string(number + 1) + ": '" + key +
                    "' is not an engine parameter. Known keys are listed in --help";
            return false;
        }
        if (!apply_value(*spec, key, value, params, "line " + std::to_string(number + 1), error))
            return false;
    }
    return true;
}

std::string defaults_path() {
    for (const char* prefix : {"", "../", "../../", "../../../"}) {
        const std::string candidate = std::string(prefix) + "config/params.ini";
        if (exists(candidate))
            return candidate;
    }
    // Nothing to load. The caller's error message names this path, which is how the player finds
    // out where the program looked instead of being told a file they can see is missing.
    return "config/params.ini";
}

std::string resolved_path(const std::string& path) {
    if (path.empty())
        return path; // the synthesiser, not the folder this session started in
    std::error_code ec;
    const std::filesystem::path absolute =
        std::filesystem::absolute(std::filesystem::path{path}, ec);
    if (ec || absolute.empty())
        return path;
    // `.` and `..` collapsed lexically rather than by `weakly_canonical`, which in this standard
    // library leaves a relative path relative when the file is not there yet - and the file is
    // often one the first save is about to create. A message that shows `../build/../patches` is a
    // message nobody can follow, and a remembered path that says `..` is a path that moves with the
    // folder it was written from.
    return absolute.lexically_normal().string();
}

std::string patches_dir() {
    for (const char* prefix : {"", "./", "../", "../../", "../../../"}) {
        const std::string candidate = std::string(prefix) + "patches";
        std::error_code ignored;
        if (std::filesystem::is_directory(candidate, ignored))
            return resolved_path(candidate);
    }
    // No such directory anywhere near. This is the name the caller creates on the first save, and
    // the name a failure message prints, so the player sees where it was written rather than being
    // told a folder they can see is missing.
    return resolved_path("patches");
}

// The player's own patches. Factory patches are read from `patches_dir()` and never written to:
// a save that could land next to them is a save that can overwrite a voice somebody else named.
std::string user_patches_dir() {
    const char* app_data = std::getenv("APPDATA");
    if (app_data != nullptr && *app_data != '\0')
        return std::string{app_data} + "/Boreaz/patches";
    // No APPDATA - a service session, or a profile with the variable cleared. Next to the working
    // directory beats nowhere, and it is the shape the release zip has, so a portable install keeps
    // its patches beside the executable.
    return "patches-user";
}

std::string known_keys() {
    std::string out;
    std::size_t rows = 0;
    const boreaz::engine::ParamSpec* table = boreaz::engine::param_table(&rows);
    for (std::size_t row = 0; row < rows; ++row) {
        if (row != 0)
            out += ", ";
        out += table[row].key;
    }
    return out;
}

bool set_param_from_text(const std::string& flag, const std::string& key, const std::string& value,
                         Params& params, std::string& error) {
    const boreaz::engine::ParamSpec* spec = boreaz::engine::param_spec(key);
    if (spec == nullptr) {
        error = "'" + key + "' is not an engine parameter. Known keys are listed in --help";
        return false;
    }
    return apply_value(*spec, key, value, params, flag, error);
}

bool save_params_file(const std::string& path, const Params& params, const std::string& name,
                      const std::string& instrument, std::string& error) {
    // Built in memory and written in one go. A patch file that stops half way through its
    // parameters is a file that loads and is wrong, which is the one failure this format has to
    // make impossible.
    std::string body = kPatchHeader;
    body += '\n';
    if (!name.empty()) {
        body += "# name: ";
        body += one_line(name);
        body += '\n';
    }
    if (!instrument.empty()) {
        body += "# instrument: ";
        body += one_line(instrument);
        body += '\n';
    }
    body += '\n';

    std::size_t rows = 0;
    const boreaz::engine::ParamSpec* table = boreaz::engine::param_table(&rows);
    for (std::size_t row = 0; row < rows; ++row) {
        const boreaz::engine::ParamSpec& spec = table[row];
        const float value = boreaz::engine::param_read(spec, params);
        if (!std::isfinite(value)) {
            error = std::string(spec.key) + " is not a number that can be read back";
            return false;
        }
        body += spec.key;
        body += " = ";
        body += number_text(spec, value);
        body += '\n';
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write '" + path + "'";
        return false;
    }
    out << body;
    out.flush();
    if (!out) {
        error = "writing '" + path + "' failed part way through";
        return false;
    }
    return true;
}

bool read_patch_meta(const std::string& path, std::string& name, std::string& instrument,
                     std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }
    name.clear();
    instrument.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // The header is the block of comments above the first parameter. Past that point a `#` is a
        // comment about one parameter, not a field of the patch.
        if (line.empty() || line[0] != '#')
            break;
        const std::string_view text = line;
        if (text.starts_with("# instrument:"))
            instrument = trim(line.substr(13));
        else if (text.starts_with("# name:"))
            name = trim(line.substr(7));
    }
    return true;
}

} // namespace boreaz::config
