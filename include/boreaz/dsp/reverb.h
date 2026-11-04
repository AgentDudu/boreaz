#pragma once

// A room, for the one instrument in this project that ships without one: a recorded grand played
// dry is a MIDI file, and PLAN 8 says so out loud. Freeverb-shaped - four shared-EQ comb resonators
// per ear feeding two all-passes - because that shape is a dozen delay lines and a multiply each,
// which is the price a live piano can actually pay.
//
// Like `Delay`, this writes the wet signal and nothing else: the send and the dry sum belong to the
// caller, so "off" means "not added" rather than "multiplied by zero", and a bypass stays
// bit-identical to the effect never existing.
#ifndef BOREAZ_DSP_REVERB_H
#define BOREAZ_DSP_REVERB_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace boreaz::dsp {

class Reverb {
  public:
    // The delays are the freeverb primes, which are quoted for 44.1 kHz and scaled from there, so
    // the room does not change pitch when the device does. The right ear is offset by `kSpread`
    // frames: the same room, decorrelated, which is what makes it wide without a pan knob.
    static constexpr double kDefaultRoomSeconds = 1.8;
    static constexpr double kMinRoomSeconds = 0.15;
    static constexpr double kMaxRoomSeconds = 12.0;
    // One-pole loss inside every comb loop, fixed on purpose: a piano needs the high partials gone
    // first and 0.4 is roughly where a hall sits, so the room is convincing before it is tunable.
    // `shortcut`: no damping row, upgrade when someone asks for a bright room against a dark one.
    static constexpr float kDamping = 0.4f;
    // Eight comb outputs summed, each of which re-reads itself every sample: without this the same
    // input that reads as 0 dB out of the delay line comes back ~+12 dB out of here and the master
    // clipper is asked to explain it.
    static constexpr float kInputScale = 0.015f;
    static constexpr float kSilenceFloor = 1e-30f;
    // One-pole high-pass corner inside every comb loop. A 12 s line is a water tank at 20 Hz and a
    // tank is not a room: fed the hardest near-DC wave a parameter file can ask for, the longest
    // room at a full send accumulated to 0.90 of full scale unleaked and 0.14 with this corner,
    // which is the difference between a reverb and one that has to be clipped. What it costs is 1.4
    // dB of send level and about 11 % of tail, both measured in tests/test_fx.cpp.
    static constexpr double kLeakCornerHz = 30.0;

    // Setup path, same rule as `Delay`: nothing here happens in the audio callback, and preparing
    // again at a new rate rebuilds every line from silence because a line measured in samples means
    // nothing at a different rate.
    void prepare(double sample_rate) {
        const double rate = sample_rate > 0.0 ? sample_rate : 44100.0;
        rate_ = rate;
        const double scale = rate / 44100.0;
        for (std::size_t i = 0; i < comb_l_.size(); ++i)
            comb_l_[i].prepare(static_cast<std::size_t>(kCombL[i] * scale) + 1u);
        for (std::size_t i = 0; i < comb_r_.size(); ++i)
            comb_r_[i].prepare(static_cast<std::size_t>(kCombR[i] * scale) + kSpread + 1u);
        for (std::size_t i = 0; i < pass_l_.size(); ++i)
            pass_l_[i].prepare(static_cast<std::size_t>(kPass[i] * scale) + 1u);
        for (std::size_t i = 0; i < pass_r_.size(); ++i)
            pass_r_[i].prepare(static_cast<std::size_t>(kPass[i] * scale) + kSpread + 1u);
        // The decay is a per-sample number, so it is measured once per prepare against the average
        // line length and stored as a ratio.
        feedback_ = feedback_for(room_seconds_);
        leak_ = static_cast<float>(1.0 / (1.0 + 6.283185307179586 * kLeakCornerHz / rate));
    }

    bool ready() const { return !comb_l_[0].empty(); }

    // Test and diagnostics hooks. The longest line is the room's aperture at the current rate; the
    // second says whether every cell reads exact zero, which is how "a rate change leaves no
    // history" and "a dead tail is really dead" get checked from outside.
    std::size_t max_line_frames() const {
        std::size_t biggest = 0;
        for (const Comb& comb : comb_r_)
            biggest = std::max(biggest, comb.line.size());
        return biggest;
    }
    bool all_silent() const {
        for (const Comb& comb : comb_l_)
            for (const float v : comb.line)
                if (v != 0.0f)
                    return false;
        for (const Comb& comb : comb_r_)
            for (const float v : comb.line)
                if (v != 0.0f)
                    return false;
        for (const AllPass& pass : pass_l_)
            for (const float v : pass.line)
                if (v != 0.0f)
                    return false;
        for (const AllPass& pass : pass_r_)
            for (const float v : pass.line)
                if (v != 0.0f)
                    return false;
        return true;
    }

    // RT60 in seconds: how long a room takes to fall 60 dB, converted to a per-lap ratio over the
    // average comb length. The label is what the loop is asked for, not exactly what comes out: the
    // damping and the leak both take a little off every lap, so the measured mid-band decay is 1.6
    // s where 1.8 was asked and 7.6 s where 12 was asked - the ceiling bites twice, because the
    // loop ratio clamps at 0.995 and 12 s of laps of loss is not something a 37 ms line can do.
    // Both numbers are in tests/test_fx.cpp, and what is asserted there is that more seconds means
    // a longer room, which is what the knob is for.
    void set_room_seconds(double seconds) {
        room_seconds_ = seconds < kMinRoomSeconds
                            ? kMinRoomSeconds
                            : (seconds > kMaxRoomSeconds ? kMaxRoomSeconds : seconds);
        feedback_ = feedback_for(room_seconds_);
    }
    double room_seconds() const { return room_seconds_; }
    float feedback() const { return feedback_; }

    void reset() {
        for (Comb& comb : comb_l_)
            comb.reset();
        for (Comb& comb : comb_r_)
            comb.reset();
        for (AllPass& pass : pass_l_)
            pass.reset();
        for (AllPass& pass : pass_r_)
            pass.reset();
    }

    // The combs read one shared mono input (a room is excited by one source), and the two ears
    // diverge from there through the all-passes, so `wet_r` is never a copy of `wet_l`.
    void process(const float* in_l, const float* in_r, float* wet_l, float* wet_r, int frames) {
        if (frames <= 0)
            return;
        if (!ready()) {
            // Silence, not whatever was in the caller's buffer: an unprepared room is bypassed, and
            // a bypass that leaks the previous block's tail is a room that never stops. No local
            // buffer either, because this is the audio callback and it must not allocate.
            std::fill_n(wet_l, frames, 0.0f);
            std::fill_n(wet_r, frames, 0.0f);
            return;
        }
        process_ready(in_l, in_r, wet_l, wet_r, frames);
    }

  private:
    static constexpr std::size_t kSpread = 23;
    static constexpr std::array<double, 4> kCombL{1116.0, 1188.0, 1277.0, 1356.0};
    static constexpr std::array<double, 4> kCombR{1422.0, 1492.0, 1551.0, 1607.0};
    static constexpr std::array<double, 2> kPass{556.0, 441.0};

    // One comb: a delay line with a one-pole in the loop, so the tail loses its top end as it loses
    // its level, which is the whole difference between a room and a slap-back.
    struct Comb {
        std::vector<float> line;
        std::size_t index = 0;
        float eq = 0.0f;
        float leak = 0.0f;    // the high-passed version of `eq`, see the loop below
        float leak_in = 0.0f; // the sample `eq` held a moment ago

        bool empty() const { return line.empty(); }
        void prepare(std::size_t frames) {
            line.assign(frames, 0.0f);
            reset();
        }
        void reset() {
            std::fill(line.begin(), line.end(), 0.0f);
            index = 0;
            eq = 0.0f;
            leak = 0.0f;
            leak_in = 0.0f;
        }
        float tick(float input, float feedback, float leak_coeff) {
            const float out = line[index];
            eq = out * (1.0f - kDamping) + eq * kDamping;
            eq = std::fabs(eq) < kSilenceFloor ? 0.0f : eq;
            // `y = a(y' + x - x')`: the leak, so the loop cannot hold anything near DC. See
            // kLeakCornerHz for the number this is worth.
            leak = leak_coeff * (leak + eq - leak_in);
            leak_in = eq;
            const float written = input + leak * feedback;
            line[index] = std::fabs(written) < kSilenceFloor ? 0.0f : written;
            index = index + 1u >= line.size() ? 0u : index + 1u;
            return out;
        }
    };

    // A Schroeder all-pass: it takes nothing away in level and only smears the transients, which is
    // how a handful of comb resonators become a texture instead of eight metallic echoes.
    struct AllPass {
        std::vector<float> line;
        std::size_t index = 0;

        void prepare(std::size_t frames) {
            line.assign(frames, 0.0f);
            reset();
        }
        void reset() {
            std::fill(line.begin(), line.end(), 0.0f);
            index = 0;
        }
        float tick(float input) {
            const float buffer_out = line[index];
            const float out = buffer_out - input;
            const float written = input + buffer_out * 0.5f;
            line[index] = std::fabs(written) < kSilenceFloor ? 0.0f : written;
            index = index + 1u >= line.size() ? 0u : index + 1u;
            return out;
        }
    };

    float feedback_for(double seconds) const {
        // -60 dB in `seconds`, spread over the average lap: fb = 10^(-3 * lap / seconds).
        double sum = 0.0;
        for (const double frames : kCombL)
            sum += frames;
        for (const double frames : kCombR)
            sum += frames;
        const double lap = (sum / 8.0) / (rate_ > 0.0 ? rate_ : 44100.0);
        const double raw = std::pow(10.0, -3.0 * lap / (seconds > 0.0 ? seconds : kMinRoomSeconds));
        // A ceiling below 1.0, twice over: the arithmetic above already cannot reach it, and a room
        // that rings forever is a stuck note whatever the seconds said.
        return static_cast<float>(raw > 0.995 ? 0.995 : (raw < 0.0 ? 0.0 : raw));
    }

    void process_ready(const float* in_l, const float* in_r, float* wet_l, float* wet_r,
                       int frames) {
        for (int i = 0; i < frames; ++i) {
            const float input = (in_l[i] + in_r[i]) * kInputScale;
            float left = 0.0f;
            float right = 0.0f;
            for (Comb& comb : comb_l_)
                left += comb.tick(input, feedback_, leak_);
            for (Comb& comb : comb_r_)
                right += comb.tick(input, feedback_, leak_);
            for (AllPass& pass : pass_l_)
                left = pass.tick(left);
            for (AllPass& pass : pass_r_)
                right = pass.tick(right);
            wet_l[i] = left;
            wet_r[i] = right;
        }
    }

    std::array<Comb, 4> comb_l_{};
    std::array<Comb, 4> comb_r_{};
    std::array<AllPass, 2> pass_l_{};
    std::array<AllPass, 2> pass_r_{};
    double rate_ = 44100.0;
    double room_seconds_ = kDefaultRoomSeconds;
    float feedback_ = 0.0f;
    float leak_ = 0.996f; // 30 Hz at 48 kHz; prepare() recomputes it for the rate in use
};

} // namespace boreaz::dsp

#endif // BOREAZ_DSP_REVERB_H
