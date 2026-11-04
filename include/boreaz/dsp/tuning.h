#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace boreaz::dsp {

// 12-TET note frequencies, computed once in double and stored as float. `pow` never runs in
// the audio path: voices read this table (PLAN.md §1 and §3).
//
// Convention, stated once here because it has wasted real hours: C4 = MIDI 60 = middle C,
// A4 = MIDI 69 = the reference pitch. The instrument is A0 (21) through C8 (108), but the
// engine accepts the full 0-127 range, so the table is 128 entries and never clamped to the
// keyboard.
class Tuning {
  public:
    static constexpr int kNoteCount = 128;
    static constexpr int kReferenceNote = 69; // A4
    static constexpr int kFirstKey = 21;      // A0
    static constexpr int kLastKey = 108;      // C8
    static constexpr float kDefaultReferenceHz = 440.0f;
    static constexpr float kReferenceHzFloor = 415.0f; // configurable range, PLAN.md §1
    static constexpr float kReferenceHzCeil = 466.0f;

    // A reference pitch outside 415-466 Hz, or one that is not a number, falls back to 440.
    explicit Tuning(float reference_a4_hz = kDefaultReferenceHz) noexcept;

    // Out-of-range notes are clamped to 0-127, which is where MIDI is clamped before it
    // reaches the engine.
    float frequency(int midi_note) const noexcept;

    float reference_hz() const noexcept { return reference_hz_; }

  private:
    std::array<float, kNoteCount> frequency_{};
    float reference_hz_ = kDefaultReferenceHz;
};

inline Tuning::Tuning(float reference_a4_hz) noexcept {
    const bool usable = reference_a4_hz >= kReferenceHzFloor && reference_a4_hz <= kReferenceHzCeil;
    reference_hz_ = usable ? reference_a4_hz : kDefaultReferenceHz;

    const double base = static_cast<double>(reference_hz_);
    for (int note = 0; note < kNoteCount; ++note) {
        const double steps = static_cast<double>(note - kReferenceNote) / 12.0;
        frequency_[static_cast<std::size_t>(note)] =
            static_cast<float>(base * std::pow(2.0, steps));
    }
}

inline float Tuning::frequency(int midi_note) const noexcept {
    if (midi_note < 0) {
        midi_note = 0;
    }
    if (midi_note >= kNoteCount) {
        midi_note = kNoteCount - 1;
    }
    return frequency_[static_cast<std::size_t>(midi_note)];
}

// The only place a MIDI note becomes a name. Sharps only: flats need context we do not have,
// and "C#4" next to "Db4" in the same build would be worse than either.
inline std::string_view note_name(int midi_note) noexcept {
    if (midi_note < 0) {
        midi_note = 0;
    }
    if (midi_note >= Tuning::kNoteCount) {
        midi_note = Tuning::kNoteCount - 1;
    }

    static constexpr const char* kPitch[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                               "F#", "G",  "G#", "A",  "A#", "B"};
    static const std::array<std::string, Tuning::kNoteCount> kNames = [] {
        std::array<std::string, Tuning::kNoteCount> names{};
        for (int note = 0; note < Tuning::kNoteCount; ++note) {
            names[static_cast<std::size_t>(note)] =
                std::string(kPitch[note % 12]) + std::to_string(note / 12 - 1);
        }
        return names;
    }();

    return kNames[static_cast<std::size_t>(midi_note)];
}

} // namespace boreaz::dsp
