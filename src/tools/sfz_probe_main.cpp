// boreaz-sfz-probe: what a mapping means, printed before you play it.
//
//   boreaz-sfz-probe path/to/instrument.sfz
//
// One region per line with the opcodes that decide what you will hear, then what the reader
// recognised, what it ignored, and what it cost. A separate tool rather than a flag on the app,
// because a bad mapping is usually the mapping's fault and the thing worth knowing is what the
// reader saw - not what the synth made of it.
//
// The RAM figure comes from loading the WAVs for real, which is the only number worth printing: an
// estimate from file sizes has to guess at bit depth and channel count, and the reader's arithmetic
// is the thing under test anyway.

#include <cstdint>
#include <iostream>
#include <string>

#include "boreaz/samples/sample_store.h"
#include "boreaz/sfz/sfz.h"

namespace {

const char* loop_name(boreaz::sfz::Loop loop) {
    switch (loop) {
        case boreaz::sfz::Loop::kContinuous:
            return "loop";
        case boreaz::sfz::Loop::kSustain:
            return "sustain-loop";
        case boreaz::sfz::Loop::kOneShot:
            break;
    }
    return "once";
}

void describe(const boreaz::sfz::Region& region, int index) {
    std::cout << "  " << index << "  " << region.sample << "  key " << region.lo_key << "-"
              << region.hi_key << "  vel " << region.lo_velocity << "-" << region.hi_velocity
              << "  centre " << region.key_center << "  " << loop_name(region.loop);
    // Only the things that are not the default, so an 88-line listing can be read down the side
    // for the one region that does something different.
    if (region.lo_key != region.hi_key)
        std::cout << "  [shifted]"; // one file over several keys: it will be pitch-shifted
    if (region.offset != 0.0)
        std::cout << "  offset " << region.offset;
    if (region.gain_db != 0.0f)
        std::cout << "  " << region.gain_db << " dB";
    if (region.tune_cents != 0.0f)
        std::cout << "  " << region.tune_cents << " ct";
    if (region.velocity_tracking != 100.0f)
        std::cout << "  veltrack " << region.velocity_tracking << "%";
    if (region.release_trigger)
        std::cout << "  [refused: release]";
    std::cout << "\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: boreaz-sfz-probe path/to/instrument.sfz\n";
        return 1;
    }
    const std::string path = argv[1];

    const boreaz::sfz::LoadResult parsed = boreaz::sfz::load_file(path);
    if (!parsed.ok) {
        std::cerr << "boreaz-sfz-probe: " << parsed.error << "\n";
        return 1;
    }

    std::cout << path << "\n  " << parsed.instrument.region_count() << " regions, key span "
              << parsed.instrument.low_key << "-" << parsed.instrument.high_key;
    if (!parsed.instrument.default_path.empty())
        std::cout << ", default path " << parsed.instrument.default_path;
    std::cout << "\n";
    for (std::size_t index = 0; index < parsed.instrument.regions.size(); ++index)
        describe(parsed.instrument.regions[index], static_cast<int>(index));
    for (const std::string& warning : parsed.warnings)
        std::cout << "  reader: " << warning << "\n";

    // Loading is what turns "the mapping mentions 28 files" into "this is 41 MB of float".
    boreaz::samples::SampleStore store;
    const boreaz::samples::LoadResult loaded = store.load_mapping(path);
    if (!loaded.ok) {
        std::cout << "load: " << loaded.error << "\n";
        return 1;
    }
    std::cout << "load: " << loaded.files << " files, " << loaded.regions << " regions playable, "
              << loaded.bytes / (1024u * 1024u) << " MB resident, " << loaded.refused
              << " refused, " << loaded.load_ms << " ms\n";
    for (const std::string& warning : loaded.warnings)
        std::cout << "  loader: " << warning << "\n";
    return 0;
}
