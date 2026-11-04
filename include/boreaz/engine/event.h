#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace boreaz::engine {

// One MIDI-ish message. `tick` is an absolute audio frame count, which is what lets the
// offline renderer, the .mid sequencer, the MIDI callback and a GUI button all feed the same
// queue with the same meaning.
enum class EventType : std::uint8_t {
    kNoteOn = 0,
    kNoteOff,
    kControlChange,
    kPitchBend,
    kAllNotesOff,
};

struct Event {
    static constexpr std::uint8_t kSustainPedal = 64;
    // miniaudio's own centre for a 14-bit bend. 8192, not 8191: with 8191 the resting position
    // would detune every note by 0.07 cents and no test could explain why.
    static constexpr std::int16_t kBendCenter = 8192;

    std::int64_t tick = 0;
    EventType type = EventType::kNoteOn;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;     // note number for on/off, CC number for CC
    std::uint8_t velocity = 0; // 0-127
    std::int16_t value = kBendCenter;

    static Event note_on(std::int64_t tick, std::uint8_t channel, std::uint8_t note,
                         std::uint8_t velocity) {
        return Event{tick, EventType::kNoteOn, channel, note, velocity, kBendCenter};
    }
    static Event note_off(std::int64_t tick, std::uint8_t channel, std::uint8_t note) {
        return Event{tick, EventType::kNoteOff, channel, note, 0, kBendCenter};
    }
    static Event control(std::int64_t tick, std::uint8_t channel, std::uint8_t controller,
                         std::uint8_t value) {
        return Event{tick, EventType::kControlChange, channel, controller, value, kBendCenter};
    }
    static Event bend(std::int64_t tick, std::uint8_t channel, std::int16_t value) {
        return Event{tick, EventType::kPitchBend, channel, 0, 0, value};
    }
    static Event all_notes_off(std::int64_t tick, std::uint8_t channel) {
        return Event{tick, EventType::kAllNotesOff, channel, 0, 0, kBendCenter};
    }

    // MIDI bend is 0..16383 with 8192 at rest. -1..1 costs nothing and keeps the math honest at
    // the bottom of the travel, where a /8192.0 would be 1/8192 too wide.
    float bend_normalized() const {
        return static_cast<float>(static_cast<int>(value) - static_cast<int>(kBendCenter)) /
               static_cast<float>(kBendCenter);
    }
};

// Single producer, single consumer ring, fixed capacity, drop-on-full. The pool, the ring and
// the output are all preallocated at device open (plan 3), so a stalled consumer costs frames,
// never a malloc in the callback.
class EventQueue {
  public:
    static constexpr std::size_t kCapacity = 4096;

    bool push(const Event& event) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1u) % kCapacity;
        if (next == tail_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        slots_[head] = event;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Event& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = slots_[tail];
        tail_.store((tail + 1u) % kCapacity, std::memory_order_release);
        return true;
    }

    // Look without consuming, so the consumer can render up to the next event's tick instead of
    // applying events on block boundaries. Consumer side only.
    bool peek(Event& out) const {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = slots_[tail];
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
    std::size_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    // Consumer side only.
    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_relaxed);
        dropped_.store(0u, std::memory_order_relaxed);
    }

  private:
    Event slots_[kCapacity]{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::size_t> dropped_{0};
};

} // namespace boreaz::engine
