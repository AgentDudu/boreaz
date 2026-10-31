#include "boreaz/engine/fx_bus.h"

#include <algorithm>

namespace boreaz::engine {

void FxBus::prepare(double sample_rate) { delay_.prepare(sample_rate); }

// Off is zero, exactly, because that is what the bypass in the engine compares against, and a send
// too small for a float is not a thing a slider produces. Anything above zero still reaches the
// delay: at -70 dB it is inaudible, and "inaudible" is not a reason to freeze a knob - the time and
// the feedback are read on every block that a non-zero mix sends.
bool FxBus::active(const Params& params) { return params.fx_delay_mix > 0.0f; }

void FxBus::reset() { delay_.reset(); }

void FxBus::process_block(float* left, float* right, int frames, const Params& params) {
    if (frames <= 0 || !active(params))
        return;

    // Every block, not every knob move: these are a compare and a multiply, and reading them here
    // is what keeps a parameter file, a slider and a preset all arriving the same way.
    delay_.set_delay_seconds(static_cast<double>(params.fx_delay_time_ms) * 0.001);
    delay_.set_feedback(params.fx_delay_feedback);
    // The table has already clamped this on every path that publishes it; it is clamped again here
    // because this number is about to be multiplied into the dry mix on the way to a clipper.
    const float send = std::clamp(params.fx_delay_mix, 0.0f, 1.0f);

    for (int done = 0; done < frames;) {
        const int chunk = std::min(frames - done, kMaxChunkFrames);
        // The dry sum is the input and the wet buffers are the output, so the send below cannot
        // reach back into the delay's own input and turn an echo into an oscillator.
        delay_.process(left + done, right + done, wet_l_, wet_r_, chunk);
        for (int i = 0; i < chunk; ++i) {
            left[static_cast<std::size_t>(i + done)] += send * wet_l_[static_cast<std::size_t>(i)];
            right[static_cast<std::size_t>(i + done)] +=
                send * wet_r_[static_cast<std::size_t>(i)];
        }
        done += chunk;
    }
}

} // namespace boreaz::engine
