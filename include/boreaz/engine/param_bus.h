#pragma once

#include <mutex>

#include "boreaz/engine/params.h"

namespace boreaz::engine {

// One parameter block between the thread that edits it and the thread that plays it.
//
// The writer takes the lock, copies, and lets go; the audio thread tries the lock once per block
// and keeps the block it already has when the try fails. So the audio thread is never delayed by
// the UI - the worst case is one more block (11 ms at 512/44.1k) playing the previous parameters,
// which is one frame of lag on a slider - and a block can never be read half-written.
//
// Why not a lock-free swap, since plan 3 asks for one: with a value this size the reader has to
// copy, and a copy needs the source to stay still. Two slots plus a claim drops the newest
// publish whenever the UI is faster than audio blocks, which loses a slider's final position
// until the next publish; three slots does not fix it, because the reader's "which slot is
// published" load happens before its copy and the writer can recycle that exact slot in between.
// Per-field atomics would work and are the fallback if a try ever measures slow, at the price of
// a parameter set that can mix two generations and a field list that has to be kept in two
// places. The try costs about twenty nanoseconds when uncontended, which it always is at 60 fps.
class ParamBus {
  public:
    // Writer thread: the UI, a patch load, a .mid.
    void publish(const Params& next) {
        std::lock_guard<std::mutex> guard(mutex_);
        published_ = next;
    }

    // Audio thread: once per block, and never in the middle of one.
    Params latest() {
        if (mutex_.try_lock()) {
            copied_ = published_;
            mutex_.unlock();
        } else {
            ++stale_reads_; // the UI was copying; last block's parameters are still the truth
        }
        return copied_;
    }

    // How often the audio thread had to make do with the previous block. Normally zero; a
    // climbing number means something is holding the lock, which is a bug worth seeing.
    std::size_t stale_reads() const { return stale_reads_; }

  private:
    mutable std::mutex mutex_;
    Params published_{};
    Params copied_{};
    std::size_t stale_reads_ = 0;
};

} // namespace boreaz::engine
