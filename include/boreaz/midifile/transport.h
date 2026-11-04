#pragma once

#include <cstddef>
#include <cstdint>

#include "boreaz/midifile/midifile.h"

namespace boreaz::engine {
class Engine;
} // namespace boreaz::engine

namespace boreaz::midifile {

// The mixer's transport over a Song: play, pause, stop, seek, loop.
//
// One clock, and it is the engine's. The transport keeps no counter of its own — it keeps `base`,
// the offset between `Engine::sample_position()` and the position inside the song, so a position is
// a subtraction. Three minutes of playback cannot drift by a sample because there is no second
// clock to drift against, which is the whole reason this is a number like that and not a timer.
//
// It is driven from the pump thread, the thread that owns the buttons, and nothing in it is touched
// by the audio callback — so there is not one atomic in this class. What crosses to the audio
// thread crosses through the engine's own queue and through
// `Engine::request_transport_flush()`.
class Transport {
  public:
    // The default lookahead, and the smallest useful one: one block of the device this runs on.
    static constexpr std::int64_t kLookaheadSamples = 512;

    // How far ahead of the position feed() pushes events. The engine applies an event on the sample
    // it names, so feeding ahead is what makes a note land on time instead of late; the window it
    // is ahead by is this number, and a pump that sleeps longer than its own lookahead feeds late.
    // The same window is what a stop, a pause, a seek or a loop has to throw away, which is why the
    // mixer does not just pick a big number: half a second of lookahead is half a second of notes
    // the flush drops and the cursor feeds back after the resume.

    // Borrowed, not copied: the mixer keeps the Song alive while the transport points at it, and a
    // real file is tens of thousands of events. Loading stops the transport at the top.
    void load(const Song& song);
    const Song* song() const { return song_; }

    void set_lookahead(std::int64_t samples) { lookahead_ = samples < 0 ? 0 : samples; }
    std::int64_t lookahead() const { return lookahead_; }

    // One button that knows what it means: running becomes paused, paused or finished becomes
    // running, and finished means from the top. A control that had to ask the same question in two
    // places is how a pause button ends up starting a song.
    void toggle(boreaz::engine::Engine& engine);

    // Each of these asks the engine to drop what it has queued and silence what is sounding, which
    // is the difference between a pause that is a pause and a wobble you can hear.
    void play(boreaz::engine::Engine& engine);  // from where it stands; at the end, from the top
    void pause(boreaz::engine::Engine& engine); // holds the position
    void stop(boreaz::engine::Engine& engine);  // pause, then back to the top
    void seek(boreaz::engine::Engine& engine, std::int64_t song_sample);

    // The same seek a slider or a click on a progress bar produces: 0.0 is the top of the song,
    // 1.0 is past its last event. Out of range is clamped, because a pointer that leaves the bar
    // is a normal thing to happen and not an error to report.
    void seek_fraction(boreaz::engine::Engine& engine, double fraction);

    // Where the song ends, and where the playhead is, in song samples. A panel draws from these.
    std::int64_t end_sample() const { return song_ ? song_->end_sample : 0; }

    // The song end wraps to the top. Phase 10's only loop shape, so no engine argument: nothing is
    // dropped, the wrap happens in feed() where the position is known.
    void set_loop(bool on) { loop_ = on; }

    bool playing() const { return playing_; }
    bool looping() const { return loop_; }
    // The playhead, in the song's own samples: the value feed() last worked out, so at most one
    // pump interval behind the audio. A playhead one block behind is a playhead nobody can see is
    // behind.
    std::int64_t position() const { return position_; }
    std::int64_t end() const { return song_ == nullptr ? 0 : song_->end_sample; }

    // Push what this transport has reached since the last call. `engine_position` is
    // `Engine::sample_position()`, read by the caller on the thread that owns the number.
    FeedResult feed(boreaz::engine::Engine& engine, std::int64_t engine_position);

  private:
    // Move the mapping, move the cursor to the first event after it, and drop what the old mapping
    // had queued. Every state change goes through here for that reason.
    void remap(boreaz::engine::Engine& engine, std::int64_t song_sample);

    const Song* song_ = nullptr;
    std::size_t cursor_ = 0;
    std::int64_t base_ = 0;            // engine sample position - position inside the song
    std::int64_t position_ = 0;        // song samples
    std::int64_t engine_position_ = 0; // the last number feed() was given
    // Set by a state change, cleared by the feed after it. The engine consumes a flush request at
    // the top of the next block, so events pushed in the same block as the request go out with the
    // old mapping's - which is why the block after a play, a pause, a stop, a seek or a loop wrap
    // feeds nothing. Those events then go out one block late and the engine applies them on
    // arrival, so the price of a pause being a pause is 10.7 ms at the front of the resume.
    bool change_pending_ = false;
    bool playing_ = false;
    bool loop_ = false;
    std::int64_t lookahead_ = kLookaheadSamples;
};

} // namespace boreaz::midifile
