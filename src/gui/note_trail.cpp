#include "boreaz/gui/note_trail.h"

#include <cmath>

namespace boreaz::gui {

namespace {

// How far a voice's own age may disagree with an entry's onset before the two are called different
// notes. A voice's age and the transport's clock are both counted from the same sample position, so
// they agree to within a block; a frame's worth of disagreement is a note struck at another moment,
// and letting the window wide here would fold a re-struck note into the bar it re-struck.
constexpr double kMaxOnsetErrorSeconds = 0.05;

} // namespace

void NoteTrail::update(FrameView& view) {
    const double now = view.transport_seconds;
    const double rate = view.device.sample_rate;

    // A clock going backwards is a song starting again, not time running in reverse. An entry that
    // began ahead of the playhead would draw a bar reaching to its right, so the memory is dropped
    // and this frame's snapshot refills it.
    if (now < last_seconds_)
        count_ = 0;
    last_seconds_ = now;

    // Forget what left the strip a while ago. Entries are in onset order, so what is expired is a
    // prefix and the cost of dropping it is one shift.
    int expired = 0;
    while (expired < count_ && !entries_[static_cast<std::size_t>(expired)].note.sounding() &&
           entries_[static_cast<std::size_t>(expired)].note.end_seconds < now - kMemorySeconds)
        ++expired;
    for (int index = expired; index < count_; ++index)
        entries_[index - expired] = entries_[static_cast<std::size_t>(index)];
    count_ -= expired;

    for (int index = 0; index < count_; ++index)
        entries_[static_cast<std::size_t>(index)].seen = false;

    // Match the snapshot against the memory. A note in the snapshot with no entry started between
    // the last frame and this one; an entry the snapshot does not mention stopped.
    for (int slot = 0; slot < view.sounding_count; ++slot) {
        const Sounding& voice = view.sounding[static_cast<std::size_t>(slot)];
        const double onset =
            (rate > 0.0 && voice.age_samples > 0)
                ? now - static_cast<double>(voice.age_samples) / rate
                : now;  // no device, so no rate and no age: the honest onset is now

        // The same key can be sounding twice - with the pedal down, a note struck again while the
        // first is still ringing is two voices on one key, one already released - and they arrive
        // with the same key and channel. The voice's age is what tells them apart, so the entry
        // chosen is the one whose onset agrees with it, and a re-struck note becomes a second bar
        // instead of stretching the first into something neither the hand nor the ear played.
        int match = -1;
        double best = kMaxOnsetErrorSeconds;
        for (int entry = 0; entry < count_; ++entry) {
            const Entry& candidate = entries_[static_cast<std::size_t>(entry)];
            if (!candidate.note.sounding() || candidate.seen || candidate.note.key != voice.key ||
                candidate.note.channel != voice.channel)
                continue;
            const double error = std::fabs(candidate.note.onset_seconds - onset);
            // Within tolerance, closest wins - and "within" has to be tested before "closest", or a
            // re-struck note is folded into the bar it re-struck as soon as that bar is the only
            // candidate, which is the common case.
            if (error <= best) {
                match = entry;
                best = error;
            }
        }

        if (match >= 0) {
            Entry& entry = entries_[static_cast<std::size_t>(match)];
            // The level is re-read, because a note that has decayed should dim on screen the way it
            // does in the room. The touch it started on is not, because that is history.
            entry.note.level = voice.level;
            entry.seen = true;
            continue;
        }

        if (count_ == FrameView::kMaxTrail) {
            // Full. The oldest entry is also the furthest left, so losing it is losing what the
            // player is least likely to still be watching, and shifting keeps the onset order that
            // the expiry above relies on.
            for (int index = 1; index < count_; ++index)
                entries_[index - 1] = entries_[static_cast<std::size_t>(index)];
            --count_;
        }
        Entry& entry = entries_[static_cast<std::size_t>(count_++)];
        entry.note.note = voice.note;
        entry.note.key = voice.key;
        entry.note.channel = voice.channel;
        entry.note.velocity = voice.velocity;
        entry.note.level = voice.level;
        entry.note.onset_seconds = onset;
        entry.note.end_seconds = -1.0;  // sounding, until a frame says otherwise
        entry.seen = true;
    }

    for (int index = 0; index < count_; ++index) {
        Entry& entry = entries_[static_cast<std::size_t>(index)];
        if (!entry.seen && entry.note.sounding())
            entry.note.end_seconds = now;  // stopped between the last frame and this one
    }

    for (int index = 0; index < count_; ++index)
        view.trail[index] = entries_[static_cast<std::size_t>(index)].note;
    view.trail_count = count_;
}

} // namespace boreaz::gui
