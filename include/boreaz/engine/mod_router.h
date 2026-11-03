#pragma once

#include <algorithm>
#include <cstdint>

namespace boreaz::engine {

// The modulation router: a fixed source list times a fixed destination list, in eight slots.
//
// PLAN.md phase 8 sets both lists. The point of them being fixed is that nothing here is a
// patch-cable graph: a graph costs a pointer chase per sample to find out what feeds what, while
// this costs one branch per slot per block, evaluated once for the whole block, which is the
// granularity the real-time rules already set for filter coefficients and LFO increments.
//
// A slot is (source, destination, depth, unipolar). Depth is signed and reads as a *musical*
// amount: 1.0 is the full travel of the destination, whose size `mod_amount` states in octaves,
// semitones or the destination's own units. Every slot defaults to off with zero depth, which is
// what lets the goldens prove the router changed nothing before anybody patched it.
enum class ModSource : std::uint8_t {
    kOff = 0,  // nothing routed; a slot with no source is skipped, which is how the defaults cost
               // nothing
    kEnv2,     // the second envelope, 0..1, struck with the note
    kLfo1,     // -1..1
    kLfo2,     // -1..1, slower by default so that two of them are not one with a cable
    kVelocity, // 0..1, the strike this note arrived with
    kKeyTrack, // -1..1 over the 88 keys, C4 = 0
    kWheel,    // 0..1 unshaped, like the controller: CC1 at rest sends 0 and must not pull a
            // destination to the bottom of its depth. Bipolar is the slot's choice, not the wire's.
    kCount
};

enum class ModDest : std::uint8_t {
    // Both ends of a cable use 0 for "nothing", and that is not symmetry for its own sake: a patch
    // in progress has a source with no destination, and a destination that defaults to the filter
    // makes a half-wired patch audible on every voice. Off costs nothing on either end.
    kOff = 0,
    kCutoff,    // full travel = 3 octaves either way, so a full-depth LFO spans 8x each way
    kPitch,     // full travel = 2 semitones either way, which is PLAN.md's "a vibrato of +-2
                // semitones at default depth", read as the depth knob's whole travel
    kOsc2Level, // full travel = the whole level, added to the parameter's own value
    kOsc3Level,
    kNoiseLevel,
    kPan, // full travel = hard left to hard right, added to the key's own position
    kCount
};

// What each end of a cable is called in the window. They live beside the enums for one reason: the
// `.ini` says `mod1_dest = 2` and the panel says "Pitch", and those two have to be the same cable.
// A list kept anywhere else is a list that drifts from the arithmetic, which is the same argument
// the parameter table makes about ranges. Each array is sized by `kCount`, so an enumerator added
// without a word, or a word added without an enumerator, stops compiling.
//
// The words are the labels of the rows they modulate, not a third vocabulary: the destination list
// reads "Cutoff", "Osc 2" and "Hammer noise" because those are the sliders a cable moves, and the
// panel calls the modulation envelope "Mod env" for the same reason - the row above it is
// "Mod env attack".
inline constexpr const char* kModSourceNames[std::size_t(ModSource::kCount)] = {
    "Off", "Mod env", "LFO 1", "LFO 2", "Velocity", "Key track", "Wheel"};

inline constexpr const char* kModDestNames[std::size_t(ModDest::kCount)] = {
    "Off", "Cutoff", "Pitch", "Osc 2", "Osc 3", "Hammer noise", "Pan"};

constexpr int kModSlots = 8;

// The 88-key range PLAN.md names, in MIDI note numbers: A0 is a piano's bottom note, C8 its top.
// The keyboard reaches wider (MIDI is 0..127) and a key outside the range lands on the end of the
// ramp instead of extrapolating: a source that runs past full scale on the last nine keys is a
// defect nobody could dial out.
constexpr int kKeyTrackLow = 21;    // A0
constexpr int kKeyTrackCentre = 60; // C4, which PLAN.md fixes at exactly 0.0
constexpr int kKeyTrackHigh = 108;  // C8

// -1 at A0, +1 at C8, 0 at C4, linear either side of centre. Both ends are exact because PLAN.md
// says so; the two halves are therefore not the same slope (39 keys below centre, 48 above),
// which is the instrument's own layout and what a keyboard-relative patch expects.
inline float keytrack_value(int key) noexcept {
    const float below = static_cast<float>(kKeyTrackCentre - kKeyTrackLow);
    const float above = static_cast<float>(kKeyTrackHigh - kKeyTrackCentre);
    const float offset = static_cast<float>(key - kKeyTrackCentre);
    const float scaled = offset >= 0.0f ? offset / above : offset / below;
    return std::clamp(scaled, -1.0f, 1.0f);
}

// The two families of source. Velocity, Env2 and the wheel are levels that start at nothing — all
// three are controllers that rest at zero, and a source resting at zero has to add nothing, or
// every patch would start with a pedal pushed down. The LFOs and the keyboard are centres that
// swing both ways. The slot's unipolar flag converts between the families, and knowing which one a
// source belongs to is what makes that conversion meaningful rather than a matter of convention:
// centring a resting source is what turns "vibrato" into "the note is flat until you move the
// wheel".
inline bool source_is_unipolar(ModSource source) noexcept {
    return source == ModSource::kVelocity || source == ModSource::kEnv2 ||
           source == ModSource::kWheel;
}

// Bring a source into the shape the slot asked for, always landing in -1..1.
//
// Unipolar means "0 .. 1, like a level", bipolar means "-1 .. 1, like a swing", and a source that
// is already in the asked-for shape passes through untouched. The two conversions are inverses:
// centring a level doubles its travel around zero, and rectifying a swing shifts it up by half so
// its negative half rides on the positive side instead of being clipped away and flattening the
// top of the wave.
inline float shape_source_value(ModSource source, float value, bool unipolar) noexcept {
    // Four cases, and the two that pass through are the ones to read carefully: a bipolar source
    // staying bipolar must be clamped to -1..1 and not to 0..1, because clamping its negative half
    // away is what turns a vibrato into a note that only goes sharp and a keytrack into a ramp
    // instead of a swing.
    if (source_is_unipolar(source))
        return unipolar ? std::clamp(value, 0.0f, 1.0f)
                        : std::clamp(2.0f * value - 1.0f, -1.0f, 1.0f);
    return unipolar ? std::clamp(0.5f * value + 0.5f, 0.0f, 1.0f) : std::clamp(value, -1.0f, 1.0f);
}

// How far the destination moves, in the destination's own units: octaves for the cutoff,
// semitones for the pitch, plain additive for a level or a pan position. The voice turns those
// into multipliers, because a cutoff of -3 octaves is a division and not a subtraction.
//
// The scales are the depth knob's promise, so they are written once here and quoted in the tests.
inline float mod_amount(ModDest dest, float depth, float shaped_source) noexcept {
    const float travel = depth * shaped_source;
    if (dest == ModDest::kCutoff)
        return travel * 3.0f; // octaves
    if (dest == ModDest::kPitch)
        return travel * 2.0f; // semitones
    return travel;            // the level and pan destinations are already in their own units
}

} // namespace boreaz::engine
