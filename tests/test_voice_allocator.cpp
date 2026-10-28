#include <algorithm>
#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/engine/voice_allocator.h"

using boreaz::engine::NoteRequest;
using boreaz::engine::Params;
using boreaz::engine::VoiceAllocator;

namespace {

constexpr double kRate = 44100.0;

NoteRequest request(int note, float velocity = 0.8f, int channel = 0) {
    NoteRequest value;
    value.note = note;
    value.key = note;
    value.channel = static_cast<std::uint8_t>(channel);
    value.velocity = velocity;
    value.frequency = 440.0f * std::pow(2.0f, static_cast<float>(note - 69) / 12.0f);
    return value;
}

int alloc(VoiceAllocator& pool, int note, std::int64_t tick, float velocity = 0.8f, int channel = 0,
          const Params& params = {}) {
    return pool.allocate(request(note, velocity, channel), params, kRate, tick);
}

bool holds(const VoiceAllocator& pool, int note) {
    for (int i = 0; i < pool.pool_size(); ++i) {
        if (pool.voice(i).active() && pool.voice(i).note() == note)
            return true;
    }
    return false;
}

int index_of(const VoiceAllocator& pool, int note) {
    for (int i = 0; i < pool.pool_size(); ++i) {
        if (pool.voice(i).active() && pool.voice(i).note() == note)
            return i;
    }
    return -1;
}

} // namespace

TEST_CASE("the pool is sixteen, and a free voice is never stolen") {
    VoiceAllocator pool;
    CHECK(pool.pool_size() == VoiceAllocator::kDefaultPoolSize);
    CHECK(pool.active_count() == 0);

    for (int note = 21; note <= 36; ++note) {
        alloc(pool, note, note);
    }
    CHECK(pool.active_count() == 16);
    CHECK(pool.steal_count() == 0);

    alloc(pool, 40, 100);
    CHECK(pool.active_count() == 16);
    CHECK(pool.steal_count() == 1);
}

TEST_CASE("when the pool is full the oldest note is the one that goes") {
    VoiceAllocator pool(3);
    alloc(pool, 30, 0);
    alloc(pool, 31, 100);
    alloc(pool, 32, 200);
    CHECK(pool.active_count() == 3);

    alloc(pool, 33, 300);
    CHECK_FALSE(holds(pool, 30)); // oldest
    CHECK(holds(pool, 31));
    CHECK(holds(pool, 32));
    CHECK(holds(pool, 33));
    CHECK(pool.steal_count() == 1);
}

TEST_CASE("a released voice is stolen before an older held one") {
    VoiceAllocator pool(3);
    alloc(pool, 30, 0);   // oldest, but the key is still down
    alloc(pool, 31, 100); // newer and already released: dying anyway
    alloc(pool, 32, 200);
    Params params;
    pool.note_off(0, 31, params, 250, false);
    REQUIRE(pool.voice(index_of(pool, 31)).released());

    alloc(pool, 33, 300);
    CHECK_FALSE(holds(pool, 31));
    CHECK(holds(pool, 30));
    CHECK(holds(pool, 32));
}

TEST_CASE("a note the pedal is holding goes before a held key, after a released one") {
    VoiceAllocator pool(3);
    alloc(pool, 30, 0);   // held by the pedal
    alloc(pool, 31, 100); // key down
    alloc(pool, 32, 200); // key down
    Params params;
    pool.pedal(true, params, 210);
    pool.note_off(0, 30, params, 220, true);
    REQUIRE(pool.voice(index_of(pool, 30)).sustained());

    alloc(pool, 33, 300);
    CHECK_FALSE(holds(pool, 30));
    CHECK(holds(pool, 31));
    CHECK(holds(pool, 33));
}

TEST_CASE("notes struck on the same sample give up the quietest one") {
    VoiceAllocator pool(2);
    const int loud = alloc(pool, 40, 0, 1.0f);
    const int quiet = alloc(pool, 41, 0, 0.2f);
    REQUIRE(loud != quiet);
    // The envelope of a note that has not rendered a block yet is zero for both, so the rule is
    // about how hard each key was struck.
    CHECK(pool.voice(loud).strike() > pool.voice(quiet).strike());

    alloc(pool, 42, 0, 0.8f);
    CHECK(holds(pool, 40));
    CHECK_FALSE(holds(pool, 41));
    CHECK(holds(pool, 42));
}

TEST_CASE("note off finds its own channel and leaves the same note elsewhere alone") {
    VoiceAllocator pool(4);
    alloc(pool, 60, 0, 0.8f, 0);
    alloc(pool, 60, 1, 0.8f, 1);
    CHECK(pool.active_count() == 2);

    Params params;
    pool.note_off(0, 60, params, 10, false);
    CHECK(pool.voice(0).released());
    CHECK_FALSE(pool.voice(1).released());
    CHECK_FALSE(pool.voice(1).sustained());
}

TEST_CASE("the pedal turns a key up into a held note and does not stop it") {
    VoiceAllocator pool(2);
    alloc(pool, 60, 0);
    Params params;
    pool.pedal(true, params, 1);
    pool.note_off(0, 60, params, 10, true);

    CHECK(pool.voice(0).sustained());
    CHECK(pool.voice(0).active());
    CHECK_FALSE(pool.voice(0).released());

    std::vector<float> left(2048, 0.0f);
    std::vector<float> right(2048, 0.0f);
    pool.render(left.data(), right.data(), 2048);
    float peak = 0.0f;
    for (std::size_t i = 0; i < left.size(); ++i)
        peak = std::max(peak, std::fabs(left[i]));
    CHECK(peak > 1.0e-3f); // still audible on the pedal

    // Pedal up hands back everything it was holding on to.
    pool.pedal(false, params, 2048);
    CHECK(pool.voice(0).released());
    CHECK_FALSE(pool.voice(0).sustained());
}

TEST_CASE("panic clears one channel or everything, and nothing stays stuck") {
    VoiceAllocator pool(6);
    alloc(pool, 60, 0, 0.8f, 0);
    alloc(pool, 62, 0, 0.8f, 1);
    Params params;

    pool.panic(0, 10, params);
    CHECK(pool.voice(0).released());
    CHECK_FALSE(pool.voice(1).released());

    pool.panic(-1, 20, params);
    for (int i = 0; i < pool.pool_size(); ++i) {
        if (i < 2)
            CHECK(pool.voice(i).released());
    }
    // A released voice must fall off on its own: that is what "no stuck notes" means.
    for (int i = 0; i < 200; ++i) {
        std::vector<float> left(256, 0.0f);
        std::vector<float> right(256, 0.0f);
        pool.render(left.data(), right.data(), 256);
    }
    CHECK(pool.active_count() == 0);
}

TEST_CASE("an empty pool leaves the caller's buffer alone, and a full one adds to it") {
    VoiceAllocator pool(2);
    std::vector<float> left(256, 0.5f);
    std::vector<float> right(256, -0.5f);
    pool.render(left.data(), right.data(), 256);
    CHECK(left[0] == 0.5f);
    CHECK(right[0] == -0.5f);

    alloc(pool, 60, 0);
    pool.render(left.data(), right.data(), 256);
    CHECK(left[0] != 0.5f); // voices add, they do not overwrite
}

TEST_CASE("a smaller pool is a test seam, not a way to lose a note you are holding") {
    VoiceAllocator pool(8);
    CHECK(pool.pool_size() == 8);
    alloc(pool, 60, 0);
    CHECK(pool.active_count() == 1);
    pool.reallocate(2);
    CHECK(pool.pool_size() == 2);
    CHECK(pool.active_count() == 0); // documented: resizing drops what no longer fits
    CHECK_FALSE(pool.voice(0).active());
}

TEST_CASE("params changing under a held note keep it finite and inside full scale") {
    VoiceAllocator pool(2);
    alloc(pool, 21, 0, 1.0f);
    Params params;
    params.filter_resonance = 1.0f; // the SVF's own edge, close to self-oscillation
    params.filter_cutoff_ratio = 24.0f;
    pool.refresh(params, false);

    float peak = 0.0f;
    for (int i = 0; i < 20; ++i) {
        std::vector<float> left(256, 0.0f);
        std::vector<float> right(256, 0.0f);
        pool.render(left.data(), right.data(), 256);
        for (std::size_t frame = 0; frame < left.size(); ++frame) {
            REQUIRE(std::isfinite(left[frame]));
            REQUIRE(std::isfinite(right[frame]));
            peak = std::max({peak, std::fabs(left[frame]), std::fabs(right[frame])});
        }
    }
    CHECK(peak <= 1.0f);
    CHECK(peak > 1.0e-3f);
}
