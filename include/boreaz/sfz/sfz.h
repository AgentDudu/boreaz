#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace boreaz::sfz {

// Reading SFZ, in service of one thing: a key and a velocity in, one recording out. The same
// doctrine as the Standard MIDI File reader next door (PLAN.md §4 phase 6) - a declared subset,
// listed below, with anything outside it either warned about once by name or refused with the
// line it was found on. Never "best effort", because an instrument that plays almost right is
// reported by nobody: it just sounds wrong somewhere and gets blamed on the synth.
//
// Understood: `<global>`, `<control>`, `<group>`, `<region>`, `<master>` and `<cite>` headers
// (the last two treated as comments - no master effects, no credits to parse); inheritance of
// opcodes from global to group to region; a value continuing into the next region unless another
// region overrides it, which is how SFZ's own "last is best" defaulting works; comments (`//`,
// `/* */` and one form of `/** **/`); and these opcodes:
//
//   sample             the file, relative to the path in force
//   path default_path  directories in force, joined by hand (no CRT, no shell)
//   lokey hikey key    key range; `key` sets both, as SFZ says
//   lovel hivel        velocity range
//   pitch_keycenter    the key this recording is already in tune with; `pitch_keyroot` is its
//                      ARIA spelling and means the same thing, and the instruments on disk use
//                      that one
//   offset             where to start reading: seconds below 100, samples at or above it, which
//                      is SFZ's rule and not our invention
//   volume tune        dB of gain and cents of pitch, per region
//   amp_veltrack       how much of the velocity curve to apply, in percent
//   group              the engine's own group number (0-500 accepted, others warned and stored)
//   trigger           `note` (default) or `release`; a release region is parsed and then refused
//                     by the store, because a release sample is not a place to start a note
//   loop_mode          `loop_continuous`, `one_shot`, `no_loop`, `sustain`
//   loop_start loop_end    loop points in SAMPLES - the SFZ 1.0 spelling with the underscore,
//                          which is what a real mapping uses (`loop_start=244730` is obviously a
//                          frame index and obviously not 244730 seconds)
//   loopstart loopend      the same two in SECONDS, the SFZ 2.0 spelling. Mixing the two in one
//                          region is refused rather than guessed at.
//
// Seen, understood, and deliberately not honoured, each warned once per name (see the store for
// the audible consequence, which is why these are not silently dropped): `fil_type`, `cutoff`
// (a sampled voice carries no filter - PLAN.md §4 phase 8), `width` (the file's own channels are
// the stereo image), `ampeg_release` (the release time is the engine's parameter, not the file's),
// `ampegain`, `egN_*`, `lfoN_*`, `filN_*`, `eqN_*`, `effect*`, `pan`/`panpos`, `seq_length`,
// `sratch`-anything, and every `*_veltrack` except `amp_veltrack`.
//
// Refused outright, with the line number: `#define` and `#include` (preprocessing is `mkinst`'s
// job, so a file that needs it has not been subset yet), a value that is not
// a number where a number is required, a `<region>` with no `sample`, a key or velocity range
// whose ends are the wrong way round, and a file over `kMaxFileBytes`.

enum class Loop : std::uint8_t {
    kOneShot,    // play to the end and stop
    kContinuous, // loop for as long as the voice lives
    kSustain,    // loop until the key goes up, then play to the end: not honoured, see the store
};

struct Region {
    std::string sample; // path relative to the `path`/`default_path` in force when it was read

    int lo_key = 0;
    int hi_key = 127;
    int lo_velocity = 0;
    int hi_velocity = 127;
    // The key this recording is already in tune with: the voice shifts the read-head by the
    // distance from it. A region that never said `pitch_keycenter` gets its own lowest key, which
    // is what "the key you pressed" means for a one-pcm-per-key mapping and an approximation for a
    // region that covers several keys and would rather not have said nothing.
    int key_center = 60;
    int group = 0;
    bool release_trigger = false;

    double offset = 0.0; // seconds when < 100, samples at or above it (SFZ's own rule)
    float gain_db = 0.0f;
    float tune_cents = 0.0f;
    float velocity_tracking = 100.0f; // percent of the velocity curve

    Loop loop = Loop::kOneShot;
    // Loop points, -1 when the region says nothing. Which unit they are in is spelled by the
    // opcode name and recorded here once, so the store resolves them against the decoded file's
    // frame count and rate without guessing. A loop needs both ends.
    double loop_start = -1.0;
    double loop_end = -1.0;
    bool loop_in_seconds = false;

    // Source order, which is the precedence: of two regions that both match, SFZ plays the later.
    std::int32_t order = 0;
};

struct Instrument {
    // The `<control>` default directory, relative to the `.sfz` itself. Empty means "beside it".
    std::string default_path;
    std::vector<Region> regions; // ascending `order`

    // The span of keys any region covers, so a key outside the instrument can be clamped to the
    // nearest edge instead of falling silent (PLAN.md: those keys are honest, not mute).
    int low_key = 127;
    int high_key = 0;

    // The region to play, or nullptr when the instrument has nothing for it. Both arguments are
    // MIDI numbers, 0-127. Scans backwards and stops at the first match, which is the precedence
    // rule and is also the fast case: a piano's regions are ordered by key, not by luck.
    const Region* find(int key, int velocity) const;

    bool empty() const { return regions.empty(); }
    std::size_t region_count() const { return regions.size(); }
};

struct LoadResult {
    bool ok = false;
    // Empty unless ok. One line, sentence case, no trailing newline: it goes straight to stderr.
    std::string error;
    // One entry per *name* that was seen and ignored, and per region refused - not one per
    // occurrence. A 66-region instrument that uses one unsupported opcode should produce one line
    // of output, because forty lines is how warnings stop being read.
    std::vector<std::string> warnings;
    Instrument instrument;
};

// Parse text already in memory. Pure: no files, no engine, no global state, nothing that throws
// past the buffer it was given.
LoadResult parse(std::string_view text);

// Read the whole file, then parse it. `path` is also the directory every relative `sample=` and
// `default_path=` is resolved against, so the caller passes a real path and not a name; the
// instrument it returns carries paths relative to that directory, not absolute ones, which is
// what lets a directory of samples move with its `.sfz`.
LoadResult load_file(const std::string& path);

// An SFZ is text with a path per region; anything bigger than this is not a mapping.
inline constexpr std::size_t kMaxFileBytes = 8u * 1024u * 1024u;

} // namespace boreaz::sfz
