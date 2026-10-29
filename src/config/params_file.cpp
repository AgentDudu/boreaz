#include "boreaz/config/params_file.h"

#include <cstdio>
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
// two copies of this table is the failure mode the plan warns about.
bool load_params_file(const std::string& path, Params& params, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open the parameter file '" + path + "'";
        return false;
    }

    struct Field {
        const char* name;
        float Params::*real;
        int Params::*integer;
    };
    static constexpr Field kFields[] = {
        {"master_gain_db", &Params::master_gain_db, nullptr},
        {"osc2_level", &Params::osc2_level, nullptr},
        {"osc3_level", &Params::osc3_level, nullptr},
        {"osc_detune_cents", &Params::osc_detune_cents, nullptr},
        {"noise_level", &Params::noise_level, nullptr},
        {"noise_decay_s", &Params::noise_decay_s, nullptr},
        {"filter_cutoff_ratio", &Params::filter_cutoff_ratio, nullptr},
        {"filter_resonance", &Params::filter_resonance, nullptr},
        {"filter_keytrack", &Params::filter_keytrack, nullptr},
        {"amp_attack_s", &Params::amp_attack_s, nullptr},
        {"amp_decay_s", &Params::amp_decay_s, nullptr},
        {"amp_sustain", &Params::amp_sustain, nullptr},
        {"amp_release_s", &Params::amp_release_s, nullptr},
        {"pan_spread", &Params::pan_spread, nullptr},
        {"bend_range_semitones", &Params::bend_range_semitones, nullptr},
        {"transpose_semitones", nullptr, &Params::transpose_semitones},
        {"octave_shift", nullptr, &Params::octave_shift},
    };

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

        const Field* field = nullptr;
        for (const Field& candidate : kFields) {
            if (key == candidate.name) {
                field = &candidate;
                break;
            }
        }
        if (field == nullptr) {
            error = "line " + std::to_string(number + 1) + ": '" + key +
                    "' is not an engine parameter. Known keys are listed in --help";
            return false;
        }
        if (field->real != nullptr) {
            float parsed = 0.0f;
            if (std::sscanf(value.c_str(), "%f", &parsed) != 1) {
                error = "line " + std::to_string(number + 1) + ": '" + key + "' wants a number, got '" +
                        value + "'";
                return false;
            }
            params.*field->real = parsed;
        } else {
            int parsed = 0;
            if (std::sscanf(value.c_str(), "%d", &parsed) != 1) {
                error = "line " + std::to_string(number + 1) + ": '" + key +
                        "' wants a whole number, got '" + value + "'";
                return false;
            }
            params.*field->integer = parsed;
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

} // namespace boreaz::config
