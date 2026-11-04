// The one class in the engine whose whole job is cross-thread safety, tested as such: a param
// handoff that tears or silently loses a value shows up three phases from now as a click or a
// slider that does not do what it says, with no crash to point at.

#include <thread>

#include <doctest/doctest.h>

#include "boreaz/engine/param_bus.h"

using boreaz::engine::ParamBus;
using boreaz::engine::Params;

namespace {

// Every published block is derived from a single number, so a reader that sees fields from two
// different blocks is detectable rather than merely unlikely-looking.
Params seeded(float seed) {
    Params params;
    params.master_gain_db = seed;
    params.osc2_level = seed * 0.5f;
    params.osc3_level = seed * 0.25f;
    params.filter_resonance = seed * 0.1f;
    params.pan_spread = seed * 0.2f;
    params.transpose_semitones = static_cast<int>(seed * 1000.0f);
    return params;
}

bool whole(const Params& params, float seed) {
    return params.master_gain_db == seed && params.osc2_level == seed * 0.5f &&
           params.osc3_level == seed * 0.25f && params.filter_resonance == seed * 0.1f &&
           params.pan_spread == seed * 0.2f &&
           params.transpose_semitones == static_cast<int>(seed * 1000.0f);
}

} // namespace

TEST_CASE("a published block arrives whole") {
    ParamBus bus;

    // Nothing published yet: defaults, which are a playable instrument, not silence.
    CHECK(bus.latest().master_gain_db == doctest::Approx(-12.0f));

    bus.publish(seeded(0.5f));
    CHECK(whole(bus.latest(), 0.5f));

    // Reading twice in one block is the same block: the audio thread must not see a second
    // handoff appear halfway through rendering.
    CHECK(whole(bus.latest(), 0.5f));
    CHECK(bus.stale_reads() == 0u);
}

TEST_CASE("the last publish wins even when the UI is faster than audio blocks") {
    // A slider drag publishes far more often than a 512-frame block, and the position the user
    // released it at must not be the one that gets lost.
    ParamBus bus;
    for (int i = 1; i <= 500; ++i) {
        bus.publish(seeded(static_cast<float>(i) * 0.001f));
    }
    CHECK(whole(bus.latest(), static_cast<float>(500) * 0.001f)); // the last one, computed the
                                                                  // same way
    CHECK(bus.stale_reads() == 0u);
}

TEST_CASE("writer and reader threads never exchange a half-written block") {
    // No sleeps and no timing assumptions: the writer publishes as fast as it can and the reader
    // checks every block it gets, so this can only fail by tearing or by losing the last value.
    ParamBus bus;
    constexpr int kPublishes = 20000;
    const float kLast = static_cast<float>(kPublishes) * 0.0001f;
    bus.publish(seeded(0.0001f)); // so the reader never has to judge the pre-published defaults

    int torn = 0;
    int reads = 0;
    std::thread writer([&bus]() {
        for (int i = 1; i <= kPublishes; ++i) {
            bus.publish(seeded(static_cast<float>(i) * 0.0001f));
        }
    });
    for (int i = 0; i < kPublishes; ++i) {
        const Params params = bus.latest();
        ++reads;
        // Whole if every field still agrees with the seed its own master gain came from, whatever
        // that seed was.
        if (!whole(params, params.master_gain_db)) {
            ++torn;
        }
    }
    writer.join();

    INFO("reads: " << reads << ", stale reads: " << bus.stale_reads());
    CHECK(torn == 0);
    CHECK(reads == kPublishes);

    // And once the writer is done, the value it ended on is what plays. Losing this is how a
    // released slider stops half way across its range.
    CHECK(whole(bus.latest(), kLast));
}
