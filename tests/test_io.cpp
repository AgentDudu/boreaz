// The parts of phase 3 that do not need a driver: the callback watchdog's arithmetic, the port
// lookup and unplug decisions, and the render door with no device behind it.
//
// What is deliberately not here: opening a real output or MIDI port. Those are exercised by
// `boreaz --demo --seconds 5 --report` (README, phase 3), which prints the numbers the exit
// criteria ask for. A unit test that needs a speaker is a test that cannot run headless.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <miniaudio.h>

#include "boreaz/engine/engine.h"
#include "boreaz/io/audio_device.h"
#include "boreaz/io/midi_input.h"
#include "engine_fixture.h"

using boreaz::io::AudioDevice;
using boreaz::io::CallbackWatchdog;
using boreaz::io::MidiInput;
using namespace fixture;

namespace {

constexpr std::int64_t kMs = 1'000'000;

// A 512 frame period at 44.1 kHz, in nanoseconds: 11.6 ms.
constexpr std::int64_t kPeriodNs = 11'600'000;

std::uint32_t pack(std::uint8_t status, std::uint8_t one, std::uint8_t two) {
    return static_cast<std::uint32_t>(status) | (static_cast<std::uint32_t>(one) << 8u) |
           (static_cast<std::uint32_t>(two) << 16u);
}

std::vector<MidiInput::Port> ports(std::initializer_list<const char*> names) {
    std::vector<MidiInput::Port> out;
    std::uint32_t index = 0;
    for (const char* name : names)
        out.push_back(MidiInput::Port{index++, name});
    return out;
}

} // namespace

TEST_CASE("callback watchdog: a steady callback stream is never late") {
    CallbackWatchdog watchdog;
    for (int i = 0; i < 100; ++i)
        watchdog.arrive(static_cast<std::int64_t>(i) * kPeriodNs, kPeriodNs);
    CHECK(watchdog.callbacks() == 100);
    CHECK(watchdog.late() == 0);
}

TEST_CASE("callback watchdog: a gap over the margin is one late callback") {
    CallbackWatchdog watchdog;
    watchdog.arrive(0, kPeriodNs);                                        // first: no gap to judge
    watchdog.arrive(kPeriodNs, kPeriodNs);                                 // on time
    watchdog.arrive(kPeriodNs + static_cast<std::int64_t>(2.5 * kPeriodNs), kPeriodNs); // late
    watchdog.arrive(kPeriodNs * 4, kPeriodNs);
    CHECK(watchdog.callbacks() == 4);
    CHECK(watchdog.late() == 1);
    CHECK(watchdog.worst_gap_ns() == doctest::Approx(2.5 * kPeriodNs));
}

TEST_CASE("callback watchdog: jitter inside the margin is not called a glitch") {
    CallbackWatchdog watchdog;
    std::int64_t now = 0;
    for (int i = 0; i < 50; ++i) {
        now += kPeriodNs + (i % 2 == 0 ? 1'000'000 : -1'000'000); // 1 ms of jitter either way
        watchdog.arrive(now, kPeriodNs);
    }
    CHECK(watchdog.late() == 0);
}

TEST_CASE("callback watchdog: with no sample rate nothing is late") {
    // AudioDevice::render is callable before a device exists, and a counter that invents
    // glitches with an unknown period would make every headless run look broken.
    CallbackWatchdog watchdog;
    watchdog.arrive(0, 0);
    watchdog.arrive(10 * kPeriodNs, 0);
    CHECK(watchdog.callbacks() == 2);
    CHECK(watchdog.late() == 0);
    CHECK(watchdog.worst_gap_ns() == 0);
}

TEST_CASE("callback watchdog: a late gap is judged by how much of a buffer it cost") {
    // The count of late callbacks cannot tell jitter from a dropout, and only a dropout is
    // audible, so the split is the number the exit criteria rest on.
    CallbackWatchdog watchdog;
    std::int64_t now = 0;
    const std::int64_t gaps[] = {kPeriodNs,          // on time
                                 kPeriodNs + 100,    // on time
                                 static_cast<std::int64_t>(1.8 * kPeriodNs),
                                 static_cast<std::int64_t>(2.5 * kPeriodNs),
                                 static_cast<std::int64_t>(6.0 * kPeriodNs),
                                 static_cast<std::int64_t>(20.0 * kPeriodNs)};
    for (const std::int64_t gap : gaps) {
        now += gap;
        watchdog.arrive(now, kPeriodNs);
    }

    using Bucket = CallbackWatchdog::Bucket;
    CHECK(watchdog.late() == 4);
    CHECK(watchdog.bucket(Bucket::kBucketOnTime) == 1); // the first callback has no gap
    CHECK(watchdog.bucket(Bucket::kBucketSlight) == 1);
    CHECK(watchdog.bucket(Bucket::kBucketDouble) == 1);
    CHECK(watchdog.bucket(Bucket::kBucketQuad) == 1);
    CHECK(watchdog.bucket(Bucket::kBucketBeyond) == 1);
}

TEST_CASE("midi ports: selecting by nothing, index, or part of a name") {
    const auto available = ports({"Microsoft Bluetooth Enumerator", "Arturia KeyLab 88", "FLDI"});

    CHECK(MidiInput::find(available, "")->index == 0);
    REQUIRE(MidiInput::find(available, "1") != nullptr);
    CHECK(MidiInput::find(available, "1")->name == "Arturia KeyLab 88");
    REQUIRE(MidiInput::find(available, "keylab") != nullptr);
    CHECK(MidiInput::find(available, "keylab")->index == 1);
    CHECK(MidiInput::find(available, "nope") == nullptr);
    CHECK(MidiInput::find(available, "9") == nullptr); // an index that is not there
    CHECK(MidiInput::find({}, "") == nullptr);         // an empty machine has no first port
}

TEST_CASE("midi ports: presence is what the unplug poll looks at") {
    const auto before = ports({"KeyLab 88", "iRig"});
    CHECK(MidiInput::present(before, "KeyLab 88"));
    CHECK_FALSE(MidiInput::present(before, "iRig Bluetooth"));

    auto after = ports({"iRig"}); // the keyboard's cable came out
    CHECK_FALSE(MidiInput::present(after, "KeyLab 88"));
    CHECK(MidiInput::present(after, "iRig"));
}

TEST_CASE("midi unplug: a lost port drops every voice and lets the pedal go") {
    // The exit criterion is "unplug does not crash or leak voices". Voices leak by staying
    // sounding, so the test holds notes, pulls the cable, and demands silence afterwards.
    Engine engine;
    engine.prepare(kRate);
    engine.push_event(Event::control(0, 0, Event::kSustainPedal, 127));
    for (const std::uint8_t note : std::initializer_list<std::uint8_t>{24, 48, 72, 96})
        engine.push_event(Event::note_on(0, 0, note, 100));
    (void)render(engine, 512, 512);
    REQUIRE(engine.active_voices() > 0);
    REQUIRE(engine.sustain_pedal_down());

    MidiInput midi;
    midi.handle_loss(engine);
    CHECK(midi.losses() == 1);

    (void)render(engine, 44100 * 2, 512); // the pedal's release, however long it is
    CHECK(engine.active_voices() == 0);
    CHECK_FALSE(engine.sustain_pedal_down());
    const Audio after = render(engine, 44100, 512);
    CHECK(after.peak < kSilence);
}

TEST_CASE("midi input: a message that does not start with a status byte is counted, not played") {
    Engine engine;
    engine.prepare(kRate);
    MidiInput midi;
    midi.attach(engine);

    midi.on_message(0x007F3C3Cu); // low byte is data, so the driver handed us something odd

    CHECK(midi.dropped_messages() == 1);
    CHECK(midi.events() == 0);
    CHECK(render(engine, 512, 512).peak < kSilence); // nothing was queued behind it
}

TEST_CASE("midi input: a packed note reaches the engine and sounds") {
    // This is the whole receive path minus winmm's thread: driver word in, audible note out.
    Engine engine;
    engine.prepare(kRate);
    MidiInput midi;
    midi.attach(engine);

    midi.on_message(pack(0x90, 0x3C, 0x7F)); // C4, full velocity
    midi.on_message(pack(0xB0, 0x40, 0x7F)); // pedal down, because a real player has a foot

    CHECK(midi.events() == 2);
    const Audio audio = render(engine, 44100, 512);
    CHECK(engine.active_voices() == 1);
    CHECK(engine.sustain_pedal_down());
    CHECK(audio.first_sound() < 512u);
    CHECK(audio.peak > kSilence);
    CHECK(audio.peak <= 1.0f);
    CHECK(audio.finite);
}

TEST_CASE("audio device: a selector that matches nothing says so, and names what was asked for") {
    Engine engine;
    AudioDevice device(engine);

    std::string error;
    CHECK_FALSE(device.open("definitely-not-an-audio-output-zz", 512, error));
    CHECK(error.find("definitely-not-an-audio-output-zz") != std::string::npos);
    CHECK_FALSE(device.is_open());

    device.poll(); // nothing open, so nothing to restart
    CHECK(device.callbacks() == 0);
    CHECK(device.restarts() == 0);
    CHECK(device.cpu_fraction() == 0.0);

    // Listing must not fall over whatever the machine has, and whatever it lists must be
    // nameable, because --device matches on it.
    for (const AudioDevice::Port& port : device.ports())
        CHECK_FALSE(port.name.empty());
}

TEST_CASE("audio render door: 512 frame blocks sound, stay finite, and stay in range") {
    Engine engine;
    engine.prepare(kRate);
    AudioDevice device(engine);
    engine.push_event(Event::note_on(0, 0, 60, 110));

    std::vector<float> block(512 * 2, 0.0f);
    for (int i = 0; i < 20; ++i)
        device.render(block.data(), 512);

    CHECK(device.callbacks() == 20);
    CHECK(device.callback_frames() == 512);
    CHECK(device.late_callbacks() == 0); // no period known, so nothing can be late
    CHECK(device.worst_gap_ns() >= 0);
    CHECK(device.cpu_fraction() >= 0.0);

    float peak = 0.0f;
    for (const float value : block) {
        REQUIRE(std::isfinite(value));
        peak = std::max(peak, std::fabs(value));
    }
    CHECK(peak > kSilence); // the last block, 10 ms in, is a note and not a click
    CHECK(peak <= 1.0f);
}


namespace {

// Real devices deliver their first callback within a few milliseconds of starting. Two seconds of
// patience is a driver doing something slow, not a bug we are papering over.
std::uint64_t wait_for_callbacks(AudioDevice& device, std::uint64_t at_least) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (device.callbacks() >= at_least)
            break;
    }
    return device.callbacks();
}

} // namespace

TEST_CASE("audio device: a device the system stopped is started again") {
    // The scope item is auto-restart on device loss. Nothing here can unplug a speaker, but the
    // state a loss leaves behind - open, and no longer running - is produced honestly by stopping
    // the device the way a driver would. Skipped with a shout on a machine with no output: a test
    // that quietly passes nowhere is worse than one that says it did not run.
    Engine engine;
    AudioDevice device(engine);

    std::string error;
    if (!device.open("", 512, error)) {
        MESSAGE("skipped, no audio output on this machine: " << error);
        return;
    }
    REQUIRE(device.start(error));
    REQUIRE(wait_for_callbacks(device, 3) >= 3);

    device.stop();
    const std::uint64_t while_stopped = device.callbacks();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(device.callbacks() == while_stopped); // stopped means stopped

    device.poll(); // the application thread noticing, which is what main() does 20 times a second
    CHECK(device.restarts() == 1);
    CHECK(device.is_open());
    CHECK(wait_for_callbacks(device, while_stopped + 3) >= while_stopped + 3);

    device.close();
    device.poll(); // a closed device has nothing to restart
    CHECK(device.restarts() == 1);
}
