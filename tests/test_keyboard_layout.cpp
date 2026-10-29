#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "boreaz/engine/engine.h"
#include "boreaz/gui/keyboard_layout.h"

using boreaz::gui::KeyboardLayout;
using boreaz::gui::Rect;

namespace {
constexpr float kWidth = 1408.0f;  // 16 px per white key, a window nobody has to maximise for
constexpr float kHeight = 220.0f;
KeyboardLayout strip() {
    return KeyboardLayout{0.0f, 0.0f, kWidth, kHeight};
}
} // namespace

TEST_CASE("keyboard layout: the strip is 88 keys, and each is counted exactly once") {
    const KeyboardLayout layout = strip();
    CHECK(KeyboardLayout::kKeyCount == 88);
    CHECK(KeyboardLayout::kWhiteCount == 52);
    CHECK(layout.white_index_of(KeyboardLayout::kFirstNote) == 0);
    CHECK(layout.white_index_of(KeyboardLayout::kLastNote) == KeyboardLayout::kWhiteCount - 1);
    CHECK(layout.white_index_of(60) == 23); // C4 is the 24th white key of this compass, A0 is the first

    int whites = 0;
    int blacks = 0;
    std::vector<int> seen;
    for (int note = KeyboardLayout::kFirstNote; note <= KeyboardLayout::kLastNote; ++note) {
        REQUIRE(KeyboardLayout::contains(note));
        (KeyboardLayout::is_black(note) ? blacks : whites)++;
        seen.push_back(note);
    }
    CHECK(whites == KeyboardLayout::kWhiteCount);
    CHECK(blacks == KeyboardLayout::kKeyCount - KeyboardLayout::kWhiteCount);
    CHECK(seen.size() == 88);

    // Walking the whites left to right has to produce the naturals of this compass in order, and
    // `note_at_white_index` has to agree with `white_index_of` for every one of them: the view
    // draws in one direction and the hit test answers in the other, and those two disagreeing is
    // a key that lights the wrong neighbour.
    for (int index = 0; index < KeyboardLayout::kWhiteCount; ++index) {
        const int note = KeyboardLayout::note_at_white_index(index);
        CHECK_FALSE(KeyboardLayout::is_black(note));
        CHECK(KeyboardLayout::white_index_of(note) == index);
        CHECK(KeyboardLayout::contains(note));
    }
}

TEST_CASE("keyboard layout: all 88 keys are individually clickable") {
    const KeyboardLayout layout = strip();
    // The exit criterion this file exists for: a click lands on the key it was aimed at, for
    // every key, at the size the panel is drawn, with nothing scrolled or paged away.
    float previous_center = -1.0f;
    for (int note = KeyboardLayout::kFirstNote; note <= KeyboardLayout::kLastNote; ++note) {
        const Rect key = layout.rect_for(note);
        CAPTURE(note);
        CHECK(key.width > 0.0f);
        CHECK(key.height > 0.0f);
        // Inside the panel: no key lives outside the window.
        CHECK(key.x >= layout.x() - 0.001f);
        CHECK(key.right() <= layout.right() + 0.001f);
        CHECK(key.y >= layout.y() - 0.001f);
        CHECK(key.bottom() <= layout.bottom() + 0.001f);
        // Centres go strictly left to right in note order, so the strip reads as a keyboard.
        CHECK(key.center_x() > previous_center);
        previous_center = key.center_x();
        // And the middle of that key answers with that key.
        CHECK(layout.note_at(key.center_x(), key.y + key.height * 0.5f) == note);
    }
}

TEST_CASE("keyboard layout: black keys sit on the seam and win where they overlap") {
    const KeyboardLayout layout = strip();
    const Rect c4 = layout.white_rect(60);
    const Rect d4 = layout.white_rect(62);
    const Rect c_sharp = layout.black_rect(61);
    // C# belongs to the C/D seam: centred on C4's right edge, narrower and shorter than a white.
    CHECK(c_sharp.center_x() == doctest::Approx(c4.right()).epsilon(0.0001f));
    CHECK(c_sharp.width < c4.width);
    CHECK(c_sharp.height < c4.height);
    CHECK(c_sharp.x > c4.x);
    CHECK(c_sharp.right() < d4.right());

    // The top of the strip over the seam is the black key; below the black key's bottom, the same
    // x belongs to whichever white is under it. That is the whole reason black keys are tested
    // first: they are what the player sees, and a hit test that disagrees with the picture is a
    // wrong note on every glissando.
    // Points aimed at, not at knife edges: a real click is inside one of these boxes, and the
    // only thing that matters is that the box the player sees is the box that answers.
    CHECK(layout.note_at(c_sharp.center_x() - 2.0f, c_sharp.y + 4.0f) == 61);
    CHECK(layout.note_at(c_sharp.center_x() + 2.0f, c_sharp.y + 4.0f) == 61);
    CHECK(layout.note_at(c4.center_x(), c4.y + 4.0f) == 60);
    CHECK(layout.note_at(d4.center_x(), d4.y + 4.0f) == 62);
    // Below the black key, the same x belongs to the white under it: the black key is drawn on
    // top of the seam and nowhere else.
    CHECK(layout.note_at(d4.center_x(), c_sharp.bottom() + 4.0f) == 62);
    CHECK(layout.note_at(c4.center_x(), c_sharp.bottom() + 4.0f) == 60);

    // Every black key's bottom edge is the same line, so the view can draw one shadow.
    for (int note = KeyboardLayout::kFirstNote; note <= KeyboardLayout::kLastNote; ++note) {
        if (!KeyboardLayout::is_black(note))
            continue;
        CHECK(layout.black_rect(note).bottom() == doctest::Approx(layout.black_rect(61).bottom()));
    }
}

TEST_CASE("keyboard layout: the ends of the strip clamp instead of running off") {
    const KeyboardLayout layout = strip();
    CHECK(layout.note_at(0.0f, kHeight * 0.9f) == KeyboardLayout::kFirstNote);
    CHECK(layout.note_at(kWidth - 1.0f, kHeight * 0.9f) == KeyboardLayout::kLastNote);
    CHECK(layout.note_at(kWidth * 2.0f, kHeight * 0.9f) == -1);
    CHECK(layout.note_at(-5.0f, 10.0f) == -1);
    CHECK(layout.note_at(kWidth * 0.5f, -1.0f) == -1);
    CHECK(layout.note_at(kWidth * 0.5f, kHeight) == -1);

    // Nothing the hit test says is outside the compass, whatever the pointer does.
    for (float px = 0.0f; px < kWidth; px += 3.0f) {
        for (float py = 0.0f; py < kHeight; py += 7.0f) {
            const int note = layout.note_at(px, py);
            const bool inside_or_nothing = note == -1 || KeyboardLayout::contains(note);
            CHECK(inside_or_nothing);
        }
    }
}

TEST_CASE("keyboard layout: where the key is struck is how hard it was struck") {
    CHECK(KeyboardLayout::velocity_at(0.0f, 0.0f, 200.0f) == 127);
    CHECK(KeyboardLayout::velocity_at(199.0f, 0.0f, 200.0f) < 10);
    CHECK(KeyboardLayout::velocity_at(100.0f, 0.0f, 200.0f) == 64); // the exact middle
    // Monotonic down the key, and never 0: MIDI reads a note-on at velocity 0 as a note-off, so a
    // click at the bottom of a key would otherwise be a silent misfire.
    int previous = 128;
    for (float py = 0.0f; py <= 200.0f; py += 4.0f) {
        const int velocity = KeyboardLayout::velocity_at(py, 0.0f, 200.0f);
        CHECK(velocity >= 1);
        CHECK(velocity <= 127);
        CHECK(velocity <= previous);
        previous = velocity;
    }
    // A panel with no height is a panel that cannot be aimed at; the honest answer is the top of
    // the range, not zero.
    CHECK(KeyboardLayout::velocity_at(0.0f, 0.0f, 0.0f) == 127);
    CHECK(KeyboardLayout::velocity_at(-50.0f, 0.0f, 200.0f) == 127);
    CHECK(KeyboardLayout::velocity_at(500.0f, 0.0f, 200.0f) == 1);
}

TEST_CASE("keyboard layout: a strip too narrow to play says so") {
    CHECK(strip().playable());
    CHECK_FALSE(KeyboardLayout{0.0f, 0.0f, 87.0f, kHeight}.playable());
    CHECK(KeyboardLayout{0.0f, 0.0f, 88.0f, kHeight}.playable());
    CHECK_FALSE(KeyboardLayout{0.0f, 0.0f, kWidth, 0.0f}.playable());
    // Degenerate geometry must not divide by zero or answer with a note that is not there.
    const KeyboardLayout nothing;
    CHECK(nothing.note_at(10.0f, 10.0f) == -1);
    CHECK_FALSE(nothing.playable());
}

TEST_CASE("keyboard layout: a click is the same event a cable sends") {
    // The claim worth testing is not that ImGui draws something, it is that the mouse goes
    // through the one Event factory the MIDI parser and the sequencer use, so a click cannot
    // disagree with a keyboard about what a note is.
    const KeyboardLayout layout = strip();
    boreaz::engine::Engine engine;
    engine.prepare(44100.0);

    const int note = layout.note_at(layout.rect_for(72).center_x(), kHeight * 0.25f); // top of C5
    const int velocity = layout.velocity_at(kHeight * 0.25f);
    REQUIRE(velocity > 0);

    CHECK(engine.push_event(
        boreaz::engine::Event::note_on(engine.sample_position(), 0,
                                       static_cast<std::uint8_t>(note),
                                       static_cast<std::uint8_t>(velocity))));
    std::vector<float> block(512 * 2, 0.0f);
    engine.process(block.data(), 512);
    CHECK(engine.active_voices() == 1);

    float peak = 0.0f;
    for (const float sample : block)
        peak = peak > std::abs(sample) ? peak : std::abs(sample);
    CHECK(peak > 0.0f);

    // And the note the strip named is the note the engine is holding.
    bool sounding = false;
    for (int index = 0; index < engine.pool_size(); ++index) {
        const auto& voice = engine.voices().voice(index);
        if (voice.active() && voice.note() == note)
            sounding = true;
    }
    CHECK(sounding);
}

TEST_CASE("keyboard layout: middle C is marked and the compass outside it is not folded in") {
    CHECK(KeyboardLayout::is_middle_c(60));
    CHECK_FALSE(KeyboardLayout::is_middle_c(48));
    CHECK_FALSE(KeyboardLayout::is_middle_c(72));
    CHECK_FALSE(KeyboardLayout::contains(20));  // the note below A0
    CHECK_FALSE(KeyboardLayout::contains(109)); // and above C8
    // The engine and the view agree about which notes this keyboard cannot show, because there is
    // one rule and both ask it.
    for (int note = 0; note < 128; ++note)
        CHECK(KeyboardLayout::contains(note) ==
              !boreaz::engine::Engine::is_extended_key(note));
}
