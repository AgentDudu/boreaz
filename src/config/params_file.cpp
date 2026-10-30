#include "boreaz/config/params_file.h"

#include "boreaz/engine/param_table.h"

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

} // namespace

using boreaz::engine::Params;

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
        if (spec->real != nullptr) {
            // strtod plus "and it consumed the whole string": `sscanf("%f")` is happy to read the 2
            // in "2..0" and call the line good, which is a patch that quietly does not say what it
            // says.
            char* end = nullptr;
            const float parsed = static_cast<float>(std::strtod(value.c_str(), &end));
            if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) {
                error = "line " + std::to_string(number + 1) + ": '" + key +
                        "' wants a number, got '" + value + "'";
                return false;
            }
            // Out of range is refused here rather than quietly clamped later: the person who typed
            // `filter_cutoff_ratio = 1000` wants to know the file did not do what it says.
            if (parsed < spec->minimum || parsed > spec->maximum) {
                char bounds[64];
                std::snprintf(bounds, sizeof bounds, "%g to %g", spec->minimum, spec->maximum);
                error = "line " + std::to_string(number + 1) + ": '" + key + "' wants " + bounds +
                        ", got '" + value + "'";
                return false;
            }
            boreaz::engine::param_write(*spec, parsed, params);
        } else {
            char* end = nullptr;
            const long long whole = std::strtoll(value.c_str(), &end, 10);
            // "1.5" for a transpose is not rounded: it is refused. The file promised a whole number
            // and the person who wrote it means something specific by that.
            if (end == value.c_str() || *end != '\0') {
                error = "line " + std::to_string(number + 1) + ": '" + key +
                        "' wants a whole number, got '" + value + "'";
                return false;
            }
            const int parsed = static_cast<int>(whole);
            if (static_cast<float>(parsed) < spec->minimum ||
                static_cast<float>(parsed) > spec->maximum) {
                error = "line " + std::to_string(number + 1) + ": '" + key + "' wants " +
                        std::to_string(static_cast<int>(spec->minimum)) + " to " +
                        std::to_string(static_cast<int>(spec->maximum)) + ", got '" + value + "'";
                return false;
            }
            params.*(spec->integer) = parsed;
        }
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

} // namespace boreaz::config
