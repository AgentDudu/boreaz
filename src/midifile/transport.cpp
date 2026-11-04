#include "boreaz/midifile/transport.h"

#include <algorithm>

#include "boreaz/engine/engine.h"

namespace boreaz::midifile {

void Transport::load(const Song& song) {
    song_ = &song;
    cursor_ = 0;
    position_ = 0;
    playing_ = false;
    // Not remapped: nothing is playing and nothing is queued, and a load does not owe the engine a
    // flush it does not need. The next play() sets the mapping against the position it finds.
    base_ = engine_position_;
}

void Transport::remap(boreaz::engine::Engine& engine, std::int64_t song_sample) {
    if (song_ == nullptr)
        return;
    // Clamped at the song's end and not past it: past the end there is nothing to feed, and a
    // position out there is a playhead drawn over an empty lane.
    song_sample = std::clamp(song_sample, std::int64_t{0}, song_->end_sample);
    // The cursor goes to the first event *after* the position. Events at or before it have either
    // already been applied or are about to be dropped with the ring, and a cursor that points back
    // at them plays them a second time - which is the stuck-note-shaped bug of every seek
    // implementation that gets this wrong.
    cursor_ = static_cast<std::size_t>(
        std::upper_bound(song_->events.begin(), song_->events.end(), song_sample,
                         [](std::int64_t sample, const boreaz::engine::Event& event) {
                             return sample < event.tick;
                         }) -
        song_->events.begin());
    position_ = song_sample;
    base_ = engine_position_ - position_;
    engine.request_transport_flush();
    change_pending_ = true;
}

void Transport::play(boreaz::engine::Engine& engine) {
    if (song_ == nullptr || playing_)
        return;
    // Stopped at the end means "play it again", which is what a player pressing play there means.
    remap(engine, position_ >= song_->end_sample ? 0 : position_);
    playing_ = true;
}

void Transport::pause(boreaz::engine::Engine& engine) {
    if (!playing_)
        return;
    playing_ = false;
    remap(engine, position_);
}

void Transport::stop(boreaz::engine::Engine& engine) {
    playing_ = false;
    remap(engine, 0);
}

void Transport::toggle(boreaz::engine::Engine& engine) {
    // Two lines, and the interesting half is not here: running becomes paused and paused becomes
    // running, and "paused at the end means from the top" already lives in play(), where a click, a
    // keyboard shortcut and a test all reach it. Saying it again here is how a button and a key end
    // up disagreeing about what one press means.
    if (playing_)
        pause(engine);
    else
        play(engine);
}

void Transport::seek_fraction(boreaz::engine::Engine& engine, double fraction) {
    if (song_ == nullptr)
        return;
    // Clamped twice over on purpose: the sample count has to stay inside the song, and a double
    // that rounds past the end because of its own precision would put the cursor past the last
    // event and stop a song that was only asked to skip to the end.
    if (!(fraction > 0.0))
        fraction = 0.0;
    if (fraction > 1.0)
        fraction = 1.0;
    auto end = static_cast<std::int64_t>(song_->end_sample);
    seek(engine, static_cast<std::int64_t>(fraction * static_cast<double>(end)));
}

void Transport::seek(boreaz::engine::Engine& engine, std::int64_t song_sample) {
    remap(engine, song_sample);
}

FeedResult Transport::feed(boreaz::engine::Engine& engine, std::int64_t engine_position) {
    engine_position_ = engine_position;
    FeedResult out{cursor_, 0u};
    if (song_ == nullptr || !playing_)
        return out;

    position_ = engine_position - base_;

    if (loop_ && position_ >= song_->end_sample && cursor_ > 0u) {
        // The wrap. The mapping moves by the song's length rather than the position being set, so
        // the arithmetic stays exact across repeats: a loop that drifted would be a loop you could
        // hear by the fourth time round. What was held across the bar is cut by the flush, which is
        // what makes the first bar of a repeat a repeat instead of a continuation. The cursor test
        // keeps one crossing from wrapping twice, which would shift the mapping twice and lose a
        // length of the song.
        base_ += song_->end_sample;
        position_ -= song_->end_sample;
        cursor_ = 0u;
        engine.request_transport_flush();
        change_pending_ = true;
    }

    if (change_pending_) {
        // The block whose ring is being dropped: nothing goes out here, or the new mapping's events
        // would be dropped with the old mapping's. See change_pending_.
        change_pending_ = false;
        return out;
    }

    const std::int64_t up_to = position_ + lookahead_;
    while (cursor_ < song_->events.size() && song_->events[cursor_].tick <= up_to) {
        boreaz::engine::Event event = song_->events[cursor_];
        // The song's own timeline to the engine's: an event keeps its distance from the position,
        // and the engine applies it on the sample it names.
        event.tick += base_;
        if (!engine.push_event(event))
            ++out.dropped; // the caller reports this; a note lost in silence is not a note lost
        ++cursor_;
    }

    out.cursor = cursor_;
    return out;
}

} // namespace boreaz::midifile
