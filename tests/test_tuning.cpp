#include <cmath>
#include <limits>
// The STL only declares `operator<<(ostream&, string_view)` where <ostream> has been seen, and
// clang-cl instantiates it late - so: doctest stringifies the string_view a CHECK() failed on,
// and without this include that blows up inside __msvc_string_view.hpp instead of here.
#include <ostream>

#include <doctest/doctest.h>

#include "boreaz/dsp/tuning.h"

using doctest::Approx;

using boreaz::dsp::note_name;
using boreaz::dsp::Tuning;

namespace {

// doctest's Approx is relative; these checks care about absolute error.
bool near(double actual, double ideal, double tolerance) noexcept {
    return std::abs(actual - ideal) <= tolerance;
}

constexpr double kCentsPerRatio = 1200.0;

// Error of a produced frequency against the ideal tempered value, in cents.
double cents(double actual, double ideal) noexcept {
    return kCentsPerRatio * std::log2(actual / ideal);
}

} // namespace

TEST_CASE("tuning hits the four reference pitches named in the plan") {
    const Tuning tuning;

    CHECK(near(tuning.frequency(Tuning::kFirstKey), 27.5, 5.0e-4));       // A0
    CHECK(near(tuning.frequency(60), 261.626, 5.0e-4));                   // C4
    CHECK(near(tuning.frequency(Tuning::kReferenceNote), 440.0, 5.0e-4)); // A4
    CHECK(near(tuning.frequency(Tuning::kLastKey), 4186.009, 5.0e-3));    // C8
}

TEST_CASE("tuning is inside half a cent at the bottom, middle, top and beyond") {
    const Tuning tuning;

    // The ideal values are recomputed in double here on purpose: comparing the table against
    // itself would prove nothing.
    const double reference = 440.0;
    for (const int note : {Tuning::kFirstKey, 48, 60, 69, 84, Tuning::kLastKey}) {
        const double ideal = reference * std::pow(2.0, (note - 69) / 12.0);
        const double error = std::abs(cents(tuning.frequency(note), ideal));
        INFO("note " << note << " (" << note_name(note) << ")");
        CHECK(error < 0.5);
    }
}

TEST_CASE("tuning is monotonic over the whole table and one twelfth apart per key") {
    const Tuning tuning;

    const double twelfth = std::pow(2.0, 1.0 / 12.0);
    for (int note = 1; note < Tuning::kNoteCount; ++note) {
        const float previous = tuning.frequency(note - 1);
        const float current = tuning.frequency(note);

        INFO("note " << note);
        CHECK(current > previous); // also covers "monotonic over all 88 keys"
        CHECK(current / previous == Approx(twelfth).epsilon(1.0e-6));
    }
}

TEST_CASE("all 88 keys of the instrument are present") {
    const Tuning tuning;

    CHECK(Tuning::kLastKey - Tuning::kFirstKey + 1 == 88);
    for (int note = Tuning::kFirstKey; note <= Tuning::kLastKey; ++note) {
        const float frequency = tuning.frequency(note);
        CHECK(frequency >= tuning.frequency(Tuning::kFirstKey));
        CHECK(frequency <= tuning.frequency(Tuning::kLastKey));
    }
}

TEST_CASE("note_name spells the convention out loud") {
    CHECK(note_name(Tuning::kFirstKey) == "A0");
    CHECK(note_name(60) == "C4"); // middle C, stated once and tested once
    CHECK(note_name(69) == "A4");
    CHECK(note_name(Tuning::kLastKey) == "C8");

    // Outside the keyboard the engine still accepts these, so they still have names.
    CHECK(note_name(0) == "C-1");
    CHECK(note_name(127) == "G9");

    // Clamped, never out of bounds: MIDI is clamped before it reaches the engine.
    CHECK(note_name(-9) == "C-1");
    CHECK(note_name(999) == "G9");
}

TEST_CASE("the reference pitch is configurable inside the stated range") {
    const Tuning baroque(415.0f);
    const Tuning high(466.0f);

    CHECK(near(baroque.frequency(69), 415.0, 1.0e-3));
    CHECK(near(high.frequency(69), 466.0, 1.0e-3));

    // Transposing the reference moves A0 by the same ratio, not by the same number of hertz.
    const Tuning tuning;
    CHECK(baroque.frequency(21) / tuning.frequency(21) == Approx(415.0 / 440.0).epsilon(1.0e-6));
}

TEST_CASE("an unusable reference pitch falls back to 440 instead of silencing the piano") {
    const float nan = std::numeric_limits<float>::quiet_NaN();

    CHECK(Tuning(0.0f).reference_hz() == 440.0f);
    CHECK(Tuning(-10.0f).reference_hz() == 440.0f);
    CHECK(Tuning(nan).reference_hz() == 440.0f);
    CHECK(Tuning(1000.0f).reference_hz() == 440.0f); // outside 415-466
    CHECK(Tuning(442.0f).reference_hz() == 442.0f);  // inside, and honoured
}
