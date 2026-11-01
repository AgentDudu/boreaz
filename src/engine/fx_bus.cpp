#include "boreaz/engine/fx_bus.h"

#include <algorithm>

namespace boreaz::engine {

void FxBus::prepare(double sample_rate) {
    delay_.prepare(sample_rate);
    reverb_.prepare(sample_rate);
}
// Off is zero, exactly, because that is what the bypass in the engine compares against, and a send
// too small for a float is not a thing a slider produces. Anything above zero still reaches the
// effect: at -70 dB it is inaudible, and "inaudible" is not a reason to freeze a knob - the time,
// the feedback and the room size are read on every block that a non-zero mix sends.
bool FxBus::active(const Params& params) {
    return params.fx_distortion_mix > 0.0f || params.fx_delay_mix > 0.0f ||
           params.fx_reverb_mix > 0.0f;
}

void FxBus::reset() {
    distortion_.reset();
    delay_.reset();
    reverb_.reset();
}

void FxBus::process_block(float* left, float* right, int frames, const Params& params) {
    if (frames <= 0 || !active(params))
        return;

    // The table has already clamped these on every path that publishes them; they are clamped again
    // here because both numbers are about to be multiplied into the dry mix on the way to a
    // clipper.
    const float drive = std::clamp(params.fx_distortion_mix, 0.0f, 1.0f);
    const float send = std::clamp(params.fx_delay_mix, 0.0f, 1.0f);
    const float room = std::clamp(params.fx_reverb_mix, 0.0f, 1.0f);

    if (drive > 0.0f) {
        distortion_.set_drive(params.fx_distortion_drive);
        for (int done = 0; done < frames;) {
            const int chunk = std::min(frames - done, kMaxChunkFrames);
            // The stage writes the difference, so this line is a blend rather than a sum: at mix 1
            // the bus carries exactly the driven note, and no amount of drive can lift a peak.
            distortion_.process(left + done, right + done, drive_l_, drive_r_, chunk);
            for (int i = 0; i < chunk; ++i) {
                left[static_cast<std::size_t>(i + done)] +=
                    drive * drive_l_[static_cast<std::size_t>(i)];
                right[static_cast<std::size_t>(i + done)] +=
                    drive * drive_r_[static_cast<std::size_t>(i)];
            }
            done += chunk;
        }
    }

    if (send > 0.0f) {
        // Read every block rather than on a knob move: these are a compare and a multiply, and a
        // delay time that only updates when the UI says so is a delay time out of sync with a
        // patch.
        delay_.set_delay_seconds(static_cast<double>(params.fx_delay_time_ms) * 0.001);
        delay_.set_feedback(params.fx_delay_feedback);
        for (int done = 0; done < frames;) {
            const int chunk = std::min(frames - done, kMaxChunkFrames);
            // The dry sum is the input and the wet buffers are the output, so the send below cannot
            // reach back into the delay's own input and turn an echo into an oscillator.
            delay_.process(left + done, right + done, wet_l_, wet_r_, chunk);
            for (int i = 0; i < chunk; ++i) {
                left[static_cast<std::size_t>(i + done)] +=
                    send * wet_l_[static_cast<std::size_t>(i)];
                right[static_cast<std::size_t>(i + done)] +=
                    send * wet_r_[static_cast<std::size_t>(i)];
            }
            done += chunk;
        }
    }

    if (room > 0.0f) {
        reverb_.set_room_seconds(static_cast<double>(params.fx_reverb_seconds));
        for (int done = 0; done < frames;) {
            const int chunk = std::min(frames - done, kMaxChunkFrames);
            // Same rule as the delay one up: the room is fed the dry sum as it stands (including
            // any echo the delay just added) and writes only its own buffers.
            reverb_.process(left + done, right + done, room_l_, room_r_, chunk);
            for (int i = 0; i < chunk; ++i) {
                left[static_cast<std::size_t>(i + done)] +=
                    room * room_l_[static_cast<std::size_t>(i)];
                right[static_cast<std::size_t>(i + done)] +=
                    room * room_r_[static_cast<std::size_t>(i)];
            }
            done += chunk;
        }
    }
}

} // namespace boreaz::engine
