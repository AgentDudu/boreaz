#include "boreaz/sfz/sfz.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace boreaz::sfz {
namespace {

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(begin, end - begin));
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// A sample called "My Song.wav" is legal and common, so values may be quoted.
std::string unquote(std::string_view value) {
    std::string text = trim(value);
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front()) {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

// strtod and not std::stod: this library is built with exceptions off, and a number that will
// not parse is an ordinary line in an ordinary file, not an exception.
bool parse_number(std::string_view text, double& out) {
    const std::string value = trim(text);
    if (value.empty())
        return false;
    const char* begin = value.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end != begin + value.size() || !std::isfinite(parsed))
        return false; // "60abc" is not a number, it is a typo
    out = parsed;
    return true;
}

bool parse_int(std::string_view text, int& out, double low, double high) {
    double parsed = 0.0;
    if (!parse_number(text, parsed))
        return false;
    // Rounding, not truncating: `lokey=35.5` is a strange thing to write and 36 is the nearest
    // thing it could have meant.
    const double rounded = std::round(parsed);
    if (rounded < low || rounded > high)
        return false;
    out = static_cast<int>(rounded);
    return true;
}

bool parse_float(std::string_view text, float& out) {
    double parsed = 0.0;
    if (!parse_number(text, parsed) || std::abs(parsed) > 1.0e9)
        return false;
    out = static_cast<float>(parsed);
    return true;
}

bool has_prefix(const std::string& name, std::string_view prefix) {
    return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
}

// Comments become spaces, and newlines stay newlines so line numbers survive: an error that says
// "line 210" of a file whose lines have moved is worse than no error.
std::string strip_comments(std::string_view text) {
    std::string out(text);
    std::size_t at = 0;
    while (at < out.size()) {
        const char c = out[at];
        if (c == '"' || c == '\'') {
            // A comment marker inside a quoted path is part of the path.
            const char quote = c;
            ++at;
            while (at < out.size() && out[at] != quote)
                ++at;
            if (at < out.size())
                ++at;
            continue;
        }
        if (c == '/' && at + 1 < out.size() && out[at + 1] == '/') {
            while (at < out.size() && out[at] != '\n')
                out[at++] = ' ';
            continue;
        }
        if (c == '/' && at + 1 < out.size() && out[at + 1] == '*') {
            // Both /** ... **/ and /* ... */ are in the wild; the long form terminates with two
            // asterisks, so looking for the short one inside it ends the comment early.
            const std::string_view stop =
                (at + 2 < out.size() && out[at + 2] == '*') ? std::string_view("**/")
                                                            : std::string_view("*/");
            std::size_t end = out.find(stop, at + 2);
            if (end == std::string::npos)
                end = out.size();
            for (std::size_t i = at; i < std::min(end + stop.size(), out.size()); ++i)
                if (out[i] != '\n')
                    out[i] = ' ';
            at = end + stop.size();
            continue;
        }
        ++at;
    }
    return out;
}

// A scope is "the values so far" plus "the directory prefix in force". Two things, so `path=a/b`
// followed by five regions does not have to be remembered anywhere else.
struct Scope {
    Region values;
    std::string path;
    // Whether this scope was ever told which key the recording is already in tune with. A region
    // that does not say gets its own lowest key, which is the right answer for the one-pcm-per-key
    // mappings this library exists for (see `Region::key_center`).
    bool centre_given = false;
};

// `a/b/` + `c.wav` = `a/b/c.wav`, once, whatever the trailing slash situation was.
constexpr char kBackslash = 0x5C; // spelled in hex so no quoting layer can eat it

std::string join_path(const std::string& prefix, const std::string& file) {
    if (prefix.empty() || file.empty())
        return prefix + file;
    const char last = prefix.back();
    if (last == '/' || last == kBackslash) // a Windows separator in an SFZ path
        return prefix + file;
    return prefix + "/" + file;
}

struct Parser {
    Instrument instrument;
    std::vector<std::string> warnings;
    std::vector<std::string> seen; // names already warned about, so a warning stays worth reading
    std::string error;

    // One chain, not three scopes: SFZ defaults are "last is best" and cumulative, so everything
    // written outside a region is simply the value in force until something replaces it. `<global>`,
    // `<control>` and `<group>` therefore differ in which opcodes a human puts in them, not in what
    // they inherit - and a parser that invented a reset here would silently drop a group's
    // `hivel=` at the next `<group>` header.
    Scope defaults;
    Scope region_scope;
    bool in_region = false;
    bool ignoring = false; // an unknown section: its opcodes go to a bit bucket, not to global
    int order = 0;

    void warn(const std::string& message) { warnings.push_back(message); }

    bool seen_before(const std::string& name) {
        if (std::find(seen.begin(), seen.end(), name) != seen.end())
            return true;
        seen.push_back(name);
        return false;
    }

    // One line per opcode *name*, not per occurrence: the instrument this was written against has
    // sixty-six regions and one unsupported filter opcode, and sixty-six lines of the same warning
    // is how the two lines that mattered stopped being read.
    void warn_once(const std::string& name, const std::string& consequence) {
        if (ignoring) // the section itself was already named; its contents are not news
            return;
        if (seen_before(name))
            return;
        warnings.push_back("opcode `" + name + "` is not honoured: " + consequence);
    }

    Scope& target() {
        if (ignoring)
            return junk_;
        if (in_region)
            return region_scope;
        return defaults;
    }

    void close_region(int line) {
        if (!in_region)
            return;
        in_region = false;
        Region region = region_scope.values;
        region.sample = join_path(region_scope.path, region.sample);
        region.order = order++;

        if (region.sample.empty()) {
            error = "line " + std::to_string(line) + ": a <region> with no sample";
            return;
        }
        if (region.lo_key > region.hi_key) {
            error = "line " + std::to_string(line) + ": lokey is above hikey in " + region.sample;
            return;
        }
        if (region.lo_velocity > region.hi_velocity) {
            error = "line " + std::to_string(line) + ": lovel is above hivel in " + region.sample;
            return;
        }
        const bool starts = region.loop_start >= 0.0;
        const bool stops = region.loop_end >= 0.0;
        if (region.loop == Loop::kContinuous && (starts != stops)) {
            error = "line " + std::to_string(line) +
                    ": a looping region needs both loop points in " + region.sample;
            return;
        }
        if (!starts) {
            // Only a claimed loop is downgraded: `sustain` has to stay `sustain` so the store can
            // say out loud that the mode is not honoured, instead of the file quietly becoming a
            // one-shot here.
            if (region.loop == Loop::kContinuous) {
                // The claim stays: the *file* may carry the loop points in its `smpl` chunk, and
                // only the store, which has read the file, can tell "plays once" from "loops where
                // the recording says". Downgrading it here would lose that.
                warn("region " + region.sample + " asks to loop and gives no loop points of its own");
            }
            region.loop_start = region.loop_end = -1.0;
        }
        if (!region_scope.centre_given)
            region.key_center = region.lo_key; // SFZ says "the key", and the lowest key of the
                                              // region is the only one both of us can name

        instrument.low_key = std::min(instrument.low_key, region.lo_key);
        instrument.high_key = std::max(instrument.high_key, region.hi_key);
        instrument.regions.push_back(region);
    }

    // One `name=value`, already split. Returns false once the file is refused; the caller stops
    // reading at that point, because every later line of a file we cannot trust is speculation.
    bool opcode(std::string_view raw, int line) {
        const std::size_t equals = raw.find('=');
        if (equals == std::string_view::npos) {
            error = "line " + std::to_string(line) + ": expected name=value";
            return false;
        }
        const std::string name = lower(trim(raw.substr(0, equals)));
        const std::string value = unquote(raw.substr(equals + 1));
        if (name.empty()) {
            error = "line " + std::to_string(line) + ": opcode with no name";
            return false;
        }
        if (name[0] == '#') {
            error = "line " + std::to_string(line) + ": `" + name +
                    "` is preprocessing, which mkinst resolves before a file reaches here";
            return false;
        }
        if (value.empty()) {
            error = "line " + std::to_string(line) + ": `" + name + "` has no value";
            return false;
        }

        Scope& scope = target();
        Region& out = scope.values;

        // Ranges and identity.
        if (name == "sample") {
            out.sample = value;
            return true;
        }
        if (name == "path") {
            scope.path = value;
            return true;
        }
        if (name == "default_path") {
            if (ignoring)
                return true;
            instrument.default_path = value; // <control> only, in practice; last one wins
            return true;
        }
        if (name == "key" || name == "lokey" || name == "hikey") {
            int parsed = 0;
            if (!parse_int(value, parsed, 0, 127)) {
                error = "line " + std::to_string(line) + ": `" + name + "` wants a key 0-127";
                return false;
            }
            if (name != "lokey")
                out.hi_key = parsed;
            if (name != "hikey")
                out.lo_key = parsed;
            return true;
        }
        if (name == "lovel" || name == "hivel") {
            int parsed = 0;
            if (!parse_int(value, parsed, 0, 127)) {
                error = "line " + std::to_string(line) + ": `" + name + "` wants a velocity 0-127";
                return false;
            }
            if (name == "lovel")
                out.lo_velocity = parsed;
            else
                out.hi_velocity = parsed;
            return true;
        }
        if (name == "pitch_keycenter" || name == "pitch_keyroot") {
            int parsed = 0;
            if (!parse_int(value, parsed, 0, 127)) {
                error = "line " + std::to_string(line) + ": `" + name + "` wants a key 0-127";
                return false;
            }
            out.key_center = parsed;
            scope.centre_given = true;
            return true;
        }
        // How it sounds.
        if (name == "offset") {
            double parsed = 0.0;
            if (!parse_number(value, parsed) || parsed < 0.0) {
                error = "line " + std::to_string(line) + ": `offset` wants a non-negative number";
                return false;
            }
            out.offset = parsed; // seconds below 100, samples at or above it: SFZ's rule, its units
            return true;
        }
        if (name == "volume") {
            if (!parse_float(value, out.gain_db)) {
                error = "line " + std::to_string(line) + ": `volume` wants decibels";
                return false;
            }
            return true;
        }
        if (name == "tune") {
            if (!parse_float(value, out.tune_cents)) {
                error = "line " + std::to_string(line) + ": `tune` wants cents";
                return false;
            }
            return true;
        }
        if (name == "amp_veltrack") {
            if (!parse_float(value, out.velocity_tracking)) {
                error = "line " + std::to_string(line) + ": `amp_veltrack` wants a percentage";
                return false;
            }
            return true;
        }
        if (name == "group") {
            int parsed = 0;
            if (!parse_int(value, parsed, 0, 1000)) {
                error = "line " + std::to_string(line) + ": `group` wants a small integer";
                return false;
            }
            out.group = parsed;
            return true;
        }
        if (name == "trigger") {
            const std::string kind = lower(value);
            if (kind == "note") {
                out.release_trigger = false;
                return true;
            }
            if (kind == "release") {
                // Parsed and stored; the store refuses to start a note with it, which is the
                // honest reading of an opcode whose whole point is "not a note start".
                out.release_trigger = true;
                return true;
            }
            error = "line " + std::to_string(line) + ": `trigger` is `note` or `release`";
            return false;
        }

        // Looping. The two spellings are two units, and one region mixing them is a file that
        // means something we would have to invent, so it is refused.
        if (name == "loop_mode") {
            const std::string mode = lower(value);
            if (mode == "loop_continuous" || mode == "continuous") {
                out.loop = Loop::kContinuous;
                return true;
            }
            if (mode == "one_shot" || mode == "one-shot") {
                out.loop = Loop::kOneShot;
                return true;
            }
            if (mode == "no_loop" || mode == "off") {
                out.loop = Loop::kOneShot;
                out.loop_start = out.loop_end = -1.0;
                return true;
            }
            if (mode == "sustain") {
                // Loops until the key goes up and then runs out. There is no voice state for "the
                // key went up but keep playing" yet, so it behaves as a one-shot.
                warn_once(name, "`sustain` behaves as one_shot until a release-following voice exists");
                out.loop = Loop::kSustain;
                return true;
            }
            error = "line " + std::to_string(line) + ": `loop_mode` is not a known mode";
            return false;
        }
        if (name == "loop_start" || name == "loop_end") {
            return loop_point(name, value, line, out, false);
        }
        if (name == "loopstart" || name == "loopend") {
            return loop_point(name, value, line, out, true);
        }
        // Anything else in the file is either a known thing we have chosen not to do, or a thing
        // we have never seen. Both are one warning per name; neither stops the instrument playing.
        // The consequences are written out because "not implemented" teaches nobody anything.
        static const std::pair<const char*, const char*> kIgnored[] = {
            {"fil_type", "a sampled voice has no filter (PLAN.md phase 8: amp and pan only)"},
            {"cutoff", "a sampled voice has no filter, so a softened layer arrives brighter"},
            {"width", "the file's own channels are the stereo image"},
            {"ampeg_release", "the release time is the engine's parameter, not the file's"},
            {"ampegain", "per-region gain comes from `volume`"},
            {"pan", "the key position across the 88 keys is the engine's pan_spread"},
            {"panpos", "as `pan`"},
            {"hires", "the engine's clock is the audio sample counter"},
        };
        for (const auto& ignored : kIgnored) {
            if (name == ignored.first) {
                warn_once(name, ignored.second);
                return true;
            }
        }
        // Families, by prefix, because there are hundreds of them and each would be its own line:
        // envelopes, LFOs, extra filters, EQ, effects, and the per-oscillator velocity trackers.
        static const std::pair<const char*, const char*> kIgnoredFamilies[] = {
            {"eg", "modulation envelopes are a synth feature (phase 8)"},
            {"lfo", "LFOs are a synth feature (phase 8)"},
            {"fil2", "only one filter exists and a sampled voice does not use it"},
            {"fil3", "only one filter exists and a sampled voice does not use it"},
            {"eq", "no EQ stage"},
            {"effect", "no FX sends yet (phase 8)"},
            {"send", "no FX sends yet (phase 8)"},
            {"filt", "modulated filter cutoff is a synth feature (phase 8)"},
            {"offtime", "release samples are refused, so there is nothing to time out"},
            {"delay", "no start delay"},
            {"count", "no sample repetition counting"},
            {"seq_", "no sample sequencing"},
            {"rand_", "no random variant selection yet"},
            {"sratch", "scratch is not a thing a piano does"},
        };
        for (const auto& family : kIgnoredFamilies) {
            if (has_prefix(name, family.first)) {
                warn_once(name, family.second);
                return true;
            }
        }

        warn_once(name, "it is not in the declared subset");
        return true;
    }

    // The loop points, with the unit the opcode name carries. `seconds` is true for the SFZ 2.0
    // spelling.
    bool loop_point(const std::string& name, const std::string& value, int line, Region& out,
                    bool seconds) {
        double parsed = 0.0;
        if (!parse_number(value, parsed) || parsed < 0.0) {
            error = "line " + std::to_string(line) + ": `" + name + "` wants a non-negative number";
            return false;
        }
        const bool sets_start = name == "loop_start" || name == "loopstart";
        const bool anything_set = out.loop_start >= 0.0 || out.loop_end >= 0.0;
        // One point in samples and the other in seconds is two units in one region: refuse it and
        // let whoever wrote the file say what they meant.
        if (anything_set && out.loop_in_seconds != seconds) {
            error = "line " + std::to_string(line) + ": `" + name +
                    "` mixes sample and second loop points in one region";
            return false;
        }
        out.loop_in_seconds = seconds;
        if (sets_start)
            out.loop_start = parsed;
        else
            out.loop_end = parsed;
        return true;
    }

    Scope junk_; // the bit bucket an unknown section writes into

    void section(std::string_view header, int line) {
        std::string name = lower(trim(header));
        if (!name.empty() && name.front() == '<')
            name.erase(0, 1);
        if (!name.empty() && name.back() == '>')
            name.pop_back();
        close_region(line); // a header closes the region above it
        in_region = false;
        ignoring = false;
        if (name == "region") {
            region_scope = defaults;
            in_region = true;
        } else if (name == "global" || name == "group" || name == "control") {
            // Nothing to do but stop being a region: the values written here are the ones in force,
            // which is the whole of SFZ's defaulting model. `<control>` cares about default_path
            // alone, and `opcode` handles that wherever it appears.
        } else {
            // <master>, <cite>, <curve>, an ARIA section, a typo: one branch, because the right
            // answer for every one of them is "skip its contents and say so once".

            warn_once(name, "the section is not implemented and its contents are skipped");
            ignoring = true;
        }
    }
};

} // namespace

const Region* Instrument::find(int key, int velocity) const {
    // Backwards, so the later region wins: that is SFZ's own precedence, and it is also what makes
    // "override the group default for this one key" possible at all.
    for (std::size_t i = regions.size(); i > 0; --i) {
        const Region& region = regions[i - 1];
        if (key >= region.lo_key && key <= region.hi_key && velocity >= region.lo_velocity &&
            velocity <= region.hi_velocity) {
            return &region;
        }
    }
    return nullptr;
}

LoadResult parse(std::string_view text) {
    LoadResult result;
    if (text.size() > kMaxFileBytes) {
        result.error = "mapping is " + std::to_string(text.size()) +
                       " bytes, over the " + std::to_string(kMaxFileBytes) + " byte limit";
        return result;
    }

    const std::string body = strip_comments(text);
    Parser parser;
    std::size_t at = 0;
    int line = 1;

    while (at < body.size() && parser.error.empty()) {
        const char c = body[at];
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (c == '\n')
                ++line;
            ++at;
            continue;
        }
        if (c == '<') {
            const std::size_t begin = at++;
            while (at < body.size() && body[at] != '>' && body[at] != '\n')
                ++at;
            if (at >= body.size() || body[at] == '\n') {
                parser.error = "line " + std::to_string(line) + ": section header with no '>'";
                break;
            }
            ++at;
            parser.section(std::string_view(body).substr(begin, at - begin), line);
            continue;
        }
        if (c == '#') {
            const std::size_t end = body.find('\n', at);
            const std::size_t stop = (end == std::string::npos) ? body.size() : end;
            parser.error = "line " + std::to_string(line) + ": `" + trim(body.substr(at, stop - at)) +
                           "` is preprocessing, which mkinst resolves before a file reaches here";
            at = stop;
            break;
        }

        const std::size_t begin = at;
        while (at < body.size() && body[at] != '\n' && body[at] != '<' && body[at] != '=') {
            if (body[at] == '"' || body[at] == '\'') {
                const char quote = body[at++];
                while (at < body.size() && body[at] != quote) {
                    if (body[at] == '\n')
                        break;
                    ++at;
                }
                if (at < body.size() && body[at] == quote)
                    ++at;
                continue;
            }
            ++at;
        }
        if (at >= body.size() || body[at] != '=') {
            const std::size_t end = body.find('\n', begin);
            const std::size_t stop = (end == std::string::npos) ? body.size() : end;
            parser.error = "line " + std::to_string(line) + ": expected name=value, found `" +
                           trim(body.substr(begin, stop - begin)) + "`";
            at = stop;
            break;
        }
        ++at; // the '='
        // A value ends at whitespace, a newline, or the next section header - except inside quotes,
        // which the branch below eats whole. A value that ran to the end of the line would swallow
        // the rest of the mapping, and the mapping would load with one region and no key range.
        while (at < body.size() && body[at] != '\n' && body[at] != '<' && body[at] != ' ' &&
               body[at] != '\t') {
            if (body[at] == '"' || body[at] == '\'') {
                const char quote = body[at++];
                while (at < body.size() && body[at] != quote && body[at] != '\n')
                    ++at;
                if (at < body.size() && body[at] == quote)
                    ++at;
                continue;
            }
            ++at;
        }
        if (!parser.opcode(std::string_view(body).substr(begin, at - begin), line))
            break;
        // What follows a value is whitespace, a comment, or the next header; the loop top reads
        // all three, so there is nothing to do here.
    }

    if (parser.error.empty())
        parser.close_region(line);

    result.warnings = parser.warnings;
    if (!parser.error.empty()) {
        result.error = parser.error;
        return result;
    }
    if (parser.instrument.regions.empty()) {
        result.error = "no usable region: the mapping has no <region> with a sample";
        return result;
    }
    result.ok = true;
    result.instrument = std::move(parser.instrument);
    return result;
}

LoadResult load_file(const std::string& path) {
    LoadResult result;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        result.error = "cannot open " + path;
        return result;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        result.error = "cannot read " + path;
        return result;
    }
    if (static_cast<std::size_t>(size) > kMaxFileBytes) {
        result.error = path + " is " + std::to_string(size) +
                       " bytes, over the " + std::to_string(kMaxFileBytes) + " byte limit";
        return result;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    file.read(text.data(), size);
    if (!file && size > 0) {
        result.error = "cannot read " + path;
        return result;
    }
    return parse(text);
}

} // namespace boreaz::sfz
