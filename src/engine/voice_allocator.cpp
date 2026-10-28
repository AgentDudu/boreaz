#include "boreaz/engine/voice_allocator.h"

namespace boreaz::engine {

int VoiceAllocator::allocate(const NoteRequest& request, const Params& params, double sample_rate,
                             std::int64_t tick) {
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        if (!voices_[i].active()) {
            voices_[i].start(request, params, sample_rate, tick);
            return static_cast<int>(i);
        }
    }

    std::size_t victim = 0;
    for (std::size_t i = 1; i < voices_.size(); ++i) {
        const Voice& candidate = voices_[i];
        const Voice& current = voices_[victim];
        const int candidate_rank = rank(candidate);
        const int current_rank = rank(current);
        const bool older = candidate.start_tick() < current.start_tick();
        const bool same_age = candidate.start_tick() == current.start_tick();
        const bool quieter = candidate.strike() < current.strike();
        if (candidate_rank < current_rank ||
            (candidate_rank == current_rank && (older || (same_age && quieter)))) {
            victim = i;
        }
    }
    voices_[victim].start(request, params, sample_rate, tick);
    ++steal_count_;
    return static_cast<int>(victim);
}

void VoiceAllocator::note_off(std::uint8_t channel, int note, const Params& params,
                              std::int64_t tick, bool pedal_down) {
    for (Voice& voice : voices_) {
        if (!voice.active() || voice.channel() != channel || voice.note() != note)
            continue;
        if (pedal_down) {
            voice.silence(tick);
        } else {
            voice.release(params, tick);
        }
    }
}

void VoiceAllocator::pedal(bool down, const Params& params, std::int64_t tick) {
    if (down)
        return;
    for (Voice& voice : voices_) {
        if (voice.sustained())
            voice.release(params, tick);
    }
}

void VoiceAllocator::panic(int channel, std::int64_t tick, const Params& params) {
    for (Voice& voice : voices_) {
        if (!voice.active() || (channel >= 0 && voice.channel() != channel))
            continue;
        voice.release(params, tick);
    }
}

void VoiceAllocator::refresh(const Params& params, bool force) {
    for (Voice& voice : voices_)
        voice.refresh(params, force);
}

void VoiceAllocator::render(float* left, float* right, int frames) {
    for (Voice& voice : voices_)
        voice.render(left, right, frames);
}

int VoiceAllocator::active_count() const {
    int count = 0;
    for (const Voice& voice : voices_) {
        if (voice.active())
            ++count;
    }
    return count;
}

} // namespace boreaz::engine
