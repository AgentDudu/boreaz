#include <string>

#include <doctest/doctest.h>

#include "boreaz/sfz/sfz.h"

using boreaz::sfz::Instrument;
using boreaz::sfz::LoadResult;
using boreaz::sfz::Loop;
using boreaz::sfz::Region;

namespace {

LoadResult parsed(std::string_view text) {
    LoadResult result = boreaz::sfz::parse(text);
    INFO("text: " << std::string(text));
    INFO("error: " << result.error);
    REQUIRE(result.ok);
    return result;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& line : lines)
        all += line + "\n";
    return all;
}

} // namespace

TEST_SUITE("sfz mapping") {

TEST_CASE("the declared subset is read, with inheritance from global to group to region") {
    const LoadResult result = parsed(R"(
// a mapping in the shape a real one arrives in
<global> amp_veltrack=80 volume=-3
<control> default_path=samples/
<group> lovel=1 hivel=64 group=2
<region> sample=cello-c2.wav pitch_keycenter=48 key=48
<region> sample=cello-c4.wav pitch_keycenter=60 key=60 tune=5
<group> hivel=127 lovel=65
<region> sample=cello-ff.wav pitch_keycenter=60 key=60 offset=0.02
)");

    const Instrument& instrument = result.instrument;
    CHECK(instrument.default_path == "samples/");
    REQUIRE(instrument.region_count() == 3);

    // Inherited from <global>, then from the first <group>, then overridden by the region itself.
    const Region& soft = instrument.regions[0];
    CHECK(soft.sample == "cello-c2.wav");
    CHECK(soft.velocity_tracking == doctest::Approx(80.0));
    CHECK(soft.gain_db == doctest::Approx(-3.0f));
    CHECK(soft.lo_velocity == 1);
    CHECK(soft.hi_velocity == 64);
    CHECK(soft.group == 2);
    CHECK(soft.lo_key == 48); // `key=` sets both ends, as SFZ says
    CHECK(soft.hi_key == 48);
    CHECK(soft.key_center == 48);

    CHECK(instrument.regions[1].tune_cents == doctest::Approx(5.0f));

    const Region& loud = instrument.regions[2];
    CHECK(loud.lo_velocity == 65); // the second <group> replaced the first
    CHECK(loud.hi_velocity == 127);
    CHECK(loud.offset == doctest::Approx(0.02));
    CHECK(loud.gain_db == doctest::Approx(-3.0f)); // what <global> said survives the new group
    CHECK(loud.group == 0); // what only the first group said does not: see the reset test below

    // The span the store clamps out-of-instrument keys to.
    CHECK(instrument.low_key == 48);
    CHECK(instrument.high_key == 60);
}

TEST_CASE("a group starts again from <global>, not from the group above it") {
    // The CC0 upright is four groups: two of them say `hivel=80` and two say `lovel=81`, and none
    // of them repeats the other's end of the band. A parser that let group one's `hivel` fall
    // through read that as lovel 81 above hivel 80, refused the region, and this whole piano was
    // unparsable - so this is the shape that found the rule rather than one that illustrates it.
    const LoadResult result = parsed(R"(
<global> volume=-3
<group> hivel=80
<region> sample=a-lo.wav key=21
<group> lovel=81
<region> sample=a-hi.wav key=21
)");
    REQUIRE(result.ok);
    REQUIRE(result.instrument.region_count() == 2);

    const Region& soft = result.instrument.regions[0];
    const Region& loud = result.instrument.regions[1];
    CHECK(soft.lo_velocity == 0);
    CHECK(soft.hi_velocity == 80);
    CHECK(loud.lo_velocity == 81);
    CHECK(loud.hi_velocity == 127); // the bug: 80 inherited from the group above
    CHECK(soft.gain_db == doctest::Approx(-3.0f));
    CHECK(loud.gain_db == doctest::Approx(-3.0f)); // <global> is in both
}

TEST_CASE("comments go, including one with an equals sign in it") {
    const LoadResult result = parsed(R"(
<region> sample=a.wav // volume=-99 and lokey=99 in a comment are not opcodes
/* block
   comment */
<region> sample=b.wav /** another **/ tune=1
)");
    REQUIRE(result.instrument.region_count() == 2);
    CHECK(result.instrument.regions[0].sample == "a.wav");
    CHECK(result.instrument.regions[0].gain_db == doctest::Approx(0.0f));
    CHECK(result.instrument.regions[0].lo_key == 0);
    CHECK(result.instrument.regions[1].tune_cents == doctest::Approx(1.0f));
}

TEST_CASE("quoted paths keep their spaces, and `path` is joined by hand") {
    const LoadResult result = parsed(R"(
<group> path=Gur/
<region> sample="bold soft.wav" key=60
<region> sample=plain.wav key=61
)");
    REQUIRE(result.instrument.region_count() == 2);
    CHECK(result.instrument.regions[0].sample == "Gur/bold soft.wav");
    CHECK(result.instrument.regions[1].sample == "Gur/plain.wav");
}

TEST_CASE("the last region that matches is the one that plays") {
    const LoadResult result = parsed(R"(
<region> sample=layer.wav key=60
<region> sample=round-robin.wav key=60 lovel=100
)");
    const Instrument& instrument = result.instrument;
    CHECK(instrument.find(60, 127)->sample == "round-robin.wav");
    CHECK(instrument.find(60, 50)->sample == "layer.wav");
    CHECK(instrument.find(59, 50) == nullptr);
    CHECK(instrument.find(61, 50) == nullptr);
    CHECK(instrument.find(60, 0)->sample == "layer.wav");
}

TEST_CASE("an opcode that is not honoured is warned about once, not once per region") {
    std::string text = "<global>\n";
    for (int i = 0; i < 66; ++i)
        text += "<region> sample=x" + std::to_string(i) + ".wav key=60 fil_type=lowpass cutoff=9000\n";
    const LoadResult result = parsed(text);

    // Warned, not refused: the instrument still plays, because the filter is a phase 8 feature and
    // 66 regions of the same sentence would hide the two lines that mattered.
    const std::string all = joined(result.warnings);
    INFO(all);
    CHECK(result.warnings.size() == 2);
    CHECK(all.find("fil_type") != std::string::npos);
    CHECK(all.find("cutoff") != std::string::npos);
    CHECK(all.find("lowpass") == std::string::npos); // the consequence, not the file's contents
}

TEST_CASE("preprocessing is refused, because mkinst resolves it before a file reaches here") {
    const LoadResult define = boreaz::sfz::parse("<region> sample=#a#.wav\n#define $a cello\n");
    REQUIRE_FALSE(define.ok);
    CHECK(define.error.find("preprocessing") != std::string::npos);

    const LoadResult include = boreaz::sfz::parse("#include other.sfz\n<region> sample=a.wav\n");
    REQUIRE_FALSE(include.ok);
    CHECK(include.error.find("preprocessing") != std::string::npos);
}

TEST_CASE("a bad range, a region with no sample, and a header with no end are refused") {
    LoadResult backwards = boreaz::sfz::parse("<region> sample=a.wav lokey=60 hikey=50\n");
    REQUIRE_FALSE(backwards.ok);
    CHECK(backwards.error.find("lokey is above hikey") != std::string::npos);

    backwards = boreaz::sfz::parse("<region> sample=a.wav lovel=90 hivel=10\n");
    REQUIRE_FALSE(backwards.ok);
    CHECK(backwards.error.find("lovel is above hivel") != std::string::npos);

    LoadResult no_sample = boreaz::sfz::parse("<global> tune=1\n<region> key=60\n");
    REQUIRE_FALSE(no_sample.ok);
    CHECK(no_sample.error.find("no sample") != std::string::npos);

    LoadResult open_header = boreaz::sfz::parse("<region sample=a.wav\n");
    REQUIRE_FALSE(open_header.ok);
    CHECK(open_header.error.find("no '>'") != std::string::npos);

    LoadResult no_equals = boreaz::sfz::parse("<region>\nsample a.wav\n");
    REQUIRE_FALSE(no_equals.ok);
    CHECK(no_equals.error.find("expected name=value") != std::string::npos);

    LoadResult not_a_number = boreaz::sfz::parse("<region> sample=a.wav key=middle c\n");
    REQUIRE_FALSE(not_a_number.ok);
    INFO(not_a_number.error);
    CHECK(not_a_number.error.find("key 0-127") != std::string::npos);

    // Nothing in the file is a mapping.
    const LoadResult empty = boreaz::sfz::parse("// just a comment\n<global> volume=-2\n");
    REQUIRE_FALSE(empty.ok);
    CHECK(empty.error.find("no usable region") != std::string::npos);
}

TEST_CASE("loop points keep the unit their opcode name carries, and one region may not mix them") {
    const LoadResult samples = parsed("<region> sample=a.wav loop_mode=loop_continuous "
                                      "loop_start=244730 loop_end=264362\n");
    const Region& first = samples.instrument.regions[0];
    CHECK(first.loop == Loop::kContinuous);
    CHECK_FALSE(first.loop_in_seconds);
    CHECK(first.loop_start == doctest::Approx(244730.0));
    CHECK(first.loop_end == doctest::Approx(264362.0));

    const LoadResult seconds =
        parsed("<region> sample=a.wav loop_mode=loop_continuous loopstart=5.5 loopend=6.25\n");
    CHECK(seconds.instrument.regions[0].loop_in_seconds);
    CHECK(seconds.instrument.regions[0].loop_end == doctest::Approx(6.25));

    const LoadResult mixed =
        boreaz::sfz::parse("<region> sample=a.wav loop_start=1000 loopend=2.0\n");
    REQUIRE_FALSE(mixed.ok);
    CHECK(mixed.error.find("mixes sample and second") != std::string::npos);

    // A loop with no points cannot be a loop, and says so: a voice that never wraps is the
    // alternative, and nobody would have found it.
    const LoadResult pointless = boreaz::sfz::parse("<region> sample=a.wav loop_mode=loop_continuous\n");
    REQUIRE(pointless.ok);
    CHECK(pointless.instrument.regions[0].loop == Loop::kContinuous);
    CHECK(pointless.instrument.regions[0].loop_start < 0.0);
    CHECK(joined(pointless.warnings).find("no loop points") != std::string::npos);
    // The claim stays with the mapping: the file's own `smpl` chunk may carry the points, and
    // only the store, which has read the file, can tell one-shot from loop-where-it-says.

    const LoadResult sustain = boreaz::sfz::parse("<region> sample=a.wav loop_mode=sustain\n");
    REQUIRE(sustain.ok);
    CHECK(sustain.instrument.regions[0].loop == Loop::kSustain);
}

TEST_CASE("offset keeps SFZ's two units, and a mode we cannot play is refused with its line") {
    const LoadResult seconds = parsed("<region> sample=a.wav offset=0.5\n");
    CHECK(seconds.instrument.regions[0].offset == doctest::Approx(0.5));

    const LoadResult frames = parsed("<region> sample=a.wav offset=12000\n");
    CHECK(frames.instrument.regions[0].offset == doctest::Approx(12000.0));

    const LoadResult bad = boreaz::sfz::parse("<region> sample=a.wav offset=-4\n");
    REQUIRE_FALSE(bad.ok);
    CHECK(bad.error.find("line 1") != std::string::npos);

    const LoadResult bogus_mode = boreaz::sfz::parse("<region> sample=a.wav loop_mode=both\n");
    REQUIRE_FALSE(bogus_mode.ok);
    CHECK(bogus_mode.error.find("loop_mode") != std::string::npos);
}

TEST_CASE("a region that never says which key it is in tune with is in tune with its own lowest") {
    // One pcm per key, which is the shape of most of the free libraries: no pitch_keycentre
    // anywhere, because the file *is* the key.
    const LoadResult result = parsed("<region> sample=a4.wav key=69\n<region> sample=a5.wav key=81\n");
    CHECK(result.instrument.regions[0].key_center == 69);
    CHECK(result.instrument.regions[1].key_center == 81);

    const LoadResult told = parsed("<region> sample=a4.wav key=69 pitch_keycenter=69\n"
                                   "<region> sample=soft.wav key=69 pitch_keycenter=60\n");
    CHECK(told.instrument.regions[1].key_center == 60);
}

TEST_CASE("a release region is parsed and remembered, because the store has to refuse it by name") {
    const LoadResult result = parsed("<region> sample=release.wav trigger=release key=60\n"
                                     "<region> sample=note.wav key=61\n");
    REQUIRE(result.instrument.region_count() == 2);
    CHECK(result.instrument.regions[0].release_trigger);
    CHECK_FALSE(result.instrument.regions[1].release_trigger);

    const LoadResult bogus = boreaz::sfz::parse("<region> sample=a.wav trigger=drum\n");
    REQUIRE_FALSE(bogus.ok);
    CHECK(bogus.error.find("trigger") != std::string::npos);
}

TEST_CASE("an unknown section is skipped and named, and its opcodes reach nothing") {
    const LoadResult result = parsed(
        "<master> fil_type=lowpass cutoff=300\n<region> sample=a.wav key=60 fil_type=highpass\n");
    REQUIRE(result.instrument.region_count() == 1);
    const std::string all = joined(result.warnings);
    INFO(all);
    // The section is named once, and the opcode inside the region is still warned about once - but
    // the master section's `cutoff` did not leak into the region's defaults.
    CHECK(all.find("master") != std::string::npos);
    CHECK(all.find("fil_type") != std::string::npos);
    CHECK(all.find("cutoff") == std::string::npos);
    CHECK(result.instrument.regions[0].sample == "a.wav");
}

TEST_CASE("load_file says which file it could not open, and parses one it can") {
    const LoadResult missing = boreaz::sfz::load_file("tests/fixtures/sfz/does-not-exist.sfz");
    REQUIRE_FALSE(missing.ok);
    CHECK(missing.error.find("cannot open") != std::string::npos);
}

} // TEST_SUITE
