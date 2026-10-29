#include "boreaz/gui/frame_view.h"

#include <algorithm>
#include <utility>

#include "boreaz/gui/keyboard_layout.h"

namespace boreaz::gui {

FrameView make_frame_view(const boreaz::engine::Engine& engine, DeviceStats device,
                          MidiStats midi) {
    FrameView view;
    view.device = std::move(device);
    view.midi = std::move(midi);

    // Every one of these is a relaxed scalar the engine says the display may read. They are not a
    // consistent picture of one instant - the audio thread is between two of them - and a status
    // strip that showed "17 voices of 16" once because a counter was written first is not a bug
    // worth a mutex in the callback.
    view.sample_position = engine.sample_position();
    view.drive_peak = engine.drive_peak();
    view.active_voices = engine.active_voices();
    view.pool_size = engine.pool_size();
    view.steals = static_cast<std::uint64_t>(engine.steal_count());
    view.dropped_events = engine.dropped_events();
    view.sustain = engine.sustain_pedal_down();

    // The voice snapshot. `voices()` is the const view the engine documents as the telemetry read
    // path, and copying it once per frame into PODs is what keeps the rest of the frame away from
    // a live allocator.
    const int pool = std::min(engine.pool_size(), FrameView::kMaxVoices);
    for (int index = 0; index < pool; ++index) {
        const boreaz::engine::Voice& voice = engine.voices().voice(index);
        if (!voice.active())
            continue;
        // A note below A0 or above C8 is counted and left out of the list, never folded onto the
        // nearest key: the engine plays those notes, and a keyboard that lit A0 when a C0 sounded
        // would tell the player their ears are wrong. The count says what the keys cannot.
        if (!KeyboardLayout::contains(voice.note())) {
            ++view.ext_voices;
            continue;
        }
        if (view.sounding_count >= FrameView::kMaxVoices)
            continue;
        Sounding& entry = view.sounding[static_cast<std::size_t>(view.sounding_count++)];
        entry.note = static_cast<std::uint8_t>(voice.note());
        entry.key = static_cast<std::uint8_t>(voice.key());
        entry.channel = static_cast<std::uint8_t>(voice.channel());
        entry.velocity = static_cast<std::uint8_t>(
            std::clamp(voice.velocity(), 0.0f, 1.0f) * 127.0f);
        // Held means the key is still down. A note the sustain pedal is keeping alive is sounding
        // without being held, and unless the display tells those apart the keyboard lights keys
        // nobody is touching.
        entry.held = !voice.released() && !voice.sustained();
        entry.level = std::clamp(voice.amplitude(), 0.0f, 1.0f);
        // Samples, not seconds: the rate is the device's business, and a panel that divides by a
        // sample rate of zero (a frame built before any device opened, which tests do constantly)
        // would rather draw nothing than a NaN-wide bar.
        entry.age_samples = voice.age(engine.sample_position());
    }
    return view;
}

} // namespace boreaz::gui
