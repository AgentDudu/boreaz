#include "boreaz/engine/voice_allocator.h"

#include "boreaz/engine/sampled_voice.h"
#include "boreaz/engine/synth_voice.h"

namespace boreaz::engine {

VoiceAllocator::VoiceAllocator(int pool_size, const Instrument* instrument) {
    reallocate(pool_size, instrument);
}

void VoiceAllocator::reallocate(int pool_size, const Instrument* instrument) {
    const int count = pool_size < 1 ? 1 : pool_size;
    // Omitting the instrument keeps whatever kind this pool already was, so `reallocate(4)` in a
    // test resizes a sampled pool instead of quietly turning it into a synth.
    if (instrument != nullptr)
        instrument_ = instrument;
    voices_.clear();
    voices_.reserve(static_cast<std::size_t>(count));
    // Every voice in a pool is the same kind, and it is decided here, once, on the setup path: a
    // sound source that changed under a held note would be a bug nobody could name.
    for (int i = 0; i < count; ++i)
        voices_.push_back(instrument_ == nullptr
                              ? std::unique_ptr<Voice>(std::make_unique<SynthVoice>())
                              : std::unique_ptr<Voice>(std::make_unique<SampledVoice>(instrument_)));
}

int VoiceAllocator::allocate(const NoteRequest& request, const Params& params, double sample_rate,
                             std::int64_t tick) {
    for (std::size_t i = 0; i < voices_.size(); ++i) {
        if (!voices_[i]->active()) {
            voices_[i]->start(request, params, sample_rate, tick);
            return static_cast<int>(i);
        }
    }

    std::size_t victim = 0;
    for (std::size_t i = 1; i < voices_.size(); ++i) {
        const Voice& candidate = *voices_[i];
        const Voice& current = *voices_[victim];
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
    voices_[victim]->start(request, params, sample_rate, tick);
    ++steal_count_;
    return static_cast<int>(victim);
}

void VoiceAllocator::note_off(std::uint8_t channel, int note, const Params& params,
                              std::int64_t tick, bool pedal_down) {
    for (const std::unique_ptr<Voice>& slot : voices_) {
        Voice& voice = *slot;
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
    for (const std::unique_ptr<Voice>& slot : voices_) {
        if (slot->sustained())
            slot->release(params, tick);
    }
}

void VoiceAllocator::panic(int channel, std::int64_t tick, const Params& params) {
    for (const std::unique_ptr<Voice>& slot : voices_) {
        if (!slot->active() || (channel >= 0 && slot->channel() != channel))
            continue;
        slot->release(params, tick);
    }
}

void VoiceAllocator::refresh(const Params& params, bool force) {
    for (const std::unique_ptr<Voice>& slot : voices_)
        slot->refresh(params, force);
}

void VoiceAllocator::render(float* left, float* right, int frames) {
    for (const std::unique_ptr<Voice>& slot : voices_)
        slot->render(left, right, frames);
}

int VoiceAllocator::active_count() const {
    int count = 0;
    for (const std::unique_ptr<Voice>& slot : voices_) {
        if (slot->active())
            ++count;
    }
    return count;
}

} // namespace boreaz::engine
