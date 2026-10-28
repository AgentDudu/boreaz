#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "boreaz/engine/engine.h"

namespace boreaz::io {

// Counts callbacks that arrive late, in nanoseconds, with the clock supplied by the caller so
// the logic can be tested against an invented timeline. A callback that arrives after its
// deadline is the glitch a listener hears, so "no audible glitch at a 512 frame period" needs a
// number rather than an opinion.
//
// What it can see: us overrunning, the scheduler starving us, the device stalling. What it
// cannot see: a glitch introduced below us by the system mixer. Say so in any report quoting it.
class CallbackWatchdog {
  public:
    // A gap 1.5 periods long is late. The margin has to clear the jitter every callback has, or
    // the counter reports a glitch on every block and means nothing.
    static constexpr double kLateMargin = 1.5;

    // Gap size in multiples of the period. This is the question the exit criteria actually turn
    // on: a callback 1.2 periods late is scheduling jitter nobody hears, one 4 periods late is a
    // dropout, and "late: 87" says nothing about which of the two happened.
    enum Bucket {
        kBucketOnTime = 0, // up to 1.5 periods
        kBucketSlight,     // 1.5 to 2: running out of buffer, but not empty
        kBucketDouble,     // 2 to 4: one buffer dropped
        kBucketQuad,       // 4 to 8: a hole a listener hears
        kBucketBeyond,     // 8 and over
        kBucketCount
    };

    // How many gaps landed in each bucket. Empty before the first callback, whose gap is unknown.
    std::uint64_t bucket(Bucket which) const { return buckets_[which]; }

    static Bucket classify(std::int64_t gap_ns, std::int64_t period_ns) {
        if (period_ns <= 0)
            return kBucketOnTime; // no rate, no judgement
        const double periods = static_cast<double>(gap_ns) / static_cast<double>(period_ns);
        if (periods < kLateMargin)
            return kBucketOnTime;
        if (periods < 2.0)
            return kBucketSlight;
        if (periods < 4.0)
            return kBucketDouble;
        if (periods < 8.0)
            return kBucketQuad;
        return kBucketBeyond;
    }

    void arrive(std::int64_t now_ns, std::int64_t period_ns) {
        ++callbacks_;
        // With no period to compare against - an engine that has not been given a sample rate -
        // nothing is late, and a counter that invents glitches is worse than no counter.
        if (previous_ns_ != 0 && period_ns > 0) {
            const std::int64_t gap = now_ns - previous_ns_;
            if (gap > worst_gap_ns_)
                worst_gap_ns_ = gap;
            const Bucket which = classify(gap, period_ns);
            ++buckets_[which];
            if (which != kBucketOnTime)
                ++late_;
        }
        previous_ns_ = now_ns;
    }

    std::uint64_t callbacks() const { return callbacks_; }
    std::uint64_t late() const { return late_; }
    std::int64_t worst_gap_ns() const { return worst_gap_ns_; }

  private:
    std::int64_t previous_ns_ = 0;
    std::int64_t worst_gap_ns_ = 0;
    std::uint64_t callbacks_ = 0;
    std::uint64_t late_ = 0;
    std::uint64_t buckets_[kBucketCount] = {};
};

// A miniaudio playback device driving the engine, plus the counters this phase is judged on.
//
// The data callback disables denormals (miniaudio does that itself unless
// `noDisableDenormals` is set, and we do not set it), renders into the caller's buffer, allocates
// nothing, takes no lock, and prints nothing. Everything else - opening, restarting, listing -
// happens on the application thread.
class AudioDevice {
  public:
    struct Port {
        std::uint32_t index = 0;
        std::string name;
        bool is_default = false;
    };

    explicit AudioDevice(engine::Engine& engine);
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Playback outputs in miniaudio's order, with the system default flagged. The context lives
    // for the life of this object, so listing costs one context init, not one per call.
    std::vector<Port> ports();

    // `which` selects an output: empty for the system default, a decimal index, or a case
    // insensitive name substring. `period_frames` is a request - the driver decides, and
    // callback_frames() reports what it actually gave.
    bool open(const std::string& which, int period_frames, std::string& error);
    bool start(std::string& error);
    // Stop the callback without giving the device back. An idle player wants this (a running
    // device is a thread and a callback), and it is the state a lost device leaves behind - so
    // the restart path below has a door a test can walk through.
    void stop();
    void close();
    bool is_open() const { return open_.load(std::memory_order_relaxed); }

    // Application thread. Reopens the device when the system takes it away: an unplugged
    // output, a new default device, a driver that stopped us. Reopen is by selector rather
    // than by stored device id, because a driver reload changes ids and a name usually not.
    void poll();

    double sample_rate() const { return sample_rate_.load(std::memory_order_relaxed); }
    const std::string& name() const { return name_; }
    int callback_frames() const { return callback_frames_.load(std::memory_order_relaxed); }
    std::uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
    std::uint64_t late_callbacks() const { return late_.load(std::memory_order_relaxed); }
    // Gaps since the last callback, split by size: see CallbackWatchdog::Bucket.
    std::uint64_t callback_gaps(CallbackWatchdog::Bucket which) const {
        return gaps_[static_cast<std::size_t>(which)].load(std::memory_order_relaxed);
    }
    std::int64_t worst_gap_ns() const { return worst_gap_ns_.load(std::memory_order_relaxed); }
    std::uint64_t restarts() const { return restarts_.load(std::memory_order_relaxed); }
    // Engine time over wall time since open, so 0.03 is 3% of one core.
    double cpu_fraction() const;

    // Public because miniaudio calls it through a C function pointer. The unit tests drive the
    // same door, which is how the render path is exercised on a machine with no audio device.
    void render(float* output, int frames);

  private:
    bool open_by_port(const Port* port, std::string& error);

    engine::Engine& engine_;
    // miniaudio's ma_context and ma_device, kept as pointers so this header never includes
    // miniaudio.h.
    void* context_ = nullptr;
    void* device_ = nullptr;
    std::string selector_;
    std::string name_;
    int period_frames_ = 512;

    CallbackWatchdog watchdog_; // callback thread only
    std::int64_t first_ns_ = 0; // callback thread only

    std::atomic<bool> open_{false};
    std::atomic<double> sample_rate_{0.0};
    std::atomic<int> callback_frames_{0};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> late_{0};
    std::atomic<std::int64_t> worst_gap_ns_{0};
    std::array<std::atomic<std::uint64_t>, CallbackWatchdog::kBucketCount> gaps_;
    std::atomic<std::uint64_t> busy_ns_{0};
    std::atomic<std::int64_t> span_ns_{0};
    std::atomic<std::uint64_t> restarts_{0};
};

} // namespace boreaz::io
