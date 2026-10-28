#include "boreaz/io/audio_device.h"

// windows.h for the performance counter, which is what the CPU and lateness numbers are measured
// with. miniaudio pulls its own Windows declarations in privately, so this TU says it directly.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <new>

namespace boreaz::io {
namespace {

// The performance counter ticks at 1-32 MHz, so `ticks * 1e9` would overflow inside a weekend.
// Scaling through a double keeps nanosecond resolution for years, and every number this file
// reports is a difference between two readings rather than an absolute time.
double ticks_to_ns(std::int64_t ticks) {
    static const double scale = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return 1e9 / static_cast<double>(frequency.QuadPart);
    }();
    return static_cast<double>(ticks) * scale;
}

std::int64_t now_ticks() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::int64_t>(now.QuadPart);
}

// miniaudio's own thread. Everything it does is in AudioDevice::render.
void CALLBACK render_through_engine(ma_device* device, void* output, const void*, ma_uint32 frames) {
    auto* self = static_cast<AudioDevice*>(device->pUserData);
    if (self != nullptr)
        self->render(static_cast<float*>(output), static_cast<int>(frames));
}

bool contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != haystack.end();
}

std::string describe(ma_result result) {
    return std::string(ma_result_description(result)) + " (" + std::to_string(static_cast<int>(result)) +
           ")";
}

struct Selection {
    bool found = false;
    ma_device_id id{};
    std::string name;
    bool is_default = false;
};

// One pass over miniaudio's playback list, used for both listing and selecting. The list belongs
// to the context and does not outlive the next enumeration, so the id is copied out here and
// only the copy is ever handed to ma_device_init.
Selection select_port(ma_context* context, const std::string& which) {
    Selection selection;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context, &infos, &count, nullptr, nullptr) != MA_SUCCESS)
        return selection;

    std::uint32_t wanted_index = 0;
    bool by_index = !which.empty() &&
                    std::all_of(which.begin(), which.end(),
                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    if (by_index)
        wanted_index = static_cast<std::uint32_t>(std::stoul(which));

    for (ma_uint32 i = 0; i < count; ++i) {
        const std::string name = infos[i].name;
        const bool match = [&] {
            if (which.empty())
                return infos[i].isDefault == MA_TRUE;
            if (by_index)
                return i == wanted_index;
            return contains(name, which);
        }();
        if (!match)
            continue;
        selection.found = true;
        selection.id = infos[i].id;
        selection.name = name;
        selection.is_default = infos[i].isDefault == MA_TRUE;
        return selection;
    }
    // "Default" on a machine where nothing is flagged default still means the first output, which
    // is what every other program on the box does.
    if (which.empty() && count > 0) {
        selection.found = true;
        selection.id = infos[0].id;
        selection.name = infos[0].name;
    }
    return selection;
}

} // namespace

AudioDevice::AudioDevice(engine::Engine& engine) : engine_(engine) {
    auto* context = new (std::nothrow) ma_context{};
    if (context == nullptr)
        return;
    if (ma_context_init(nullptr, 0, nullptr, context) == MA_SUCCESS) {
        context_ = context;
        return;
    }
    delete context;
}

AudioDevice::~AudioDevice() {
    close();
    if (context_ != nullptr) {
        auto* context = static_cast<ma_context*>(context_);
        ma_context_uninit(context);
        delete context;
        context_ = nullptr;
    }
}

std::vector<AudioDevice::Port> AudioDevice::ports() {
    std::vector<Port> out;
    auto* context = static_cast<ma_context*>(context_);
    if (context == nullptr)
        return out;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context, &infos, &count, nullptr, nullptr) != MA_SUCCESS)
        return out;
    out.reserve(count);
    for (ma_uint32 i = 0; i < count; ++i)
        out.push_back(Port{i, infos[i].name, infos[i].isDefault == MA_TRUE});
    return out;
}

bool AudioDevice::open(const std::string& which, int period_frames, std::string& error) {
    selector_ = which;
    period_frames_ = period_frames > 0 ? period_frames : 512;

    auto* context = static_cast<ma_context*>(context_);
    if (context == nullptr) {
        error = "no audio context: the system audio subsystem could not be initialised";
        return false;
    }
    const Selection selection = select_port(context, which);
    if (!selection.found) {
        error = which.empty() ? std::string("no audio output device found")
                              : "no audio output device matches '" + which + "'";
        return false;
    }
    if (device_ != nullptr)
        close();

    auto* device = new (std::nothrow) ma_device{};
    if (device == nullptr) {
        error = "out of memory";
        return false;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    // 48 kHz, not the endpoint's own rate. This box's default output is a 192 kHz endpoint, and
    // accepting that makes a 512 frame callback 2.7 ms of work and 2.7 ms of deadline instead of
    // 10.7 ms of each, for four times the CPU to play the same piano. miniaudio resamples to
    // whatever the system is mixing at, so asking is always allowed.
    config.sampleRate = 48000;
    config.periodSizeInFrames = static_cast<ma_uint32>(period_frames_);
    config.periods = 2;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = &render_through_engine;
    config.pUserData = this;
    config.playback.pDeviceID = &selection.id;

    if (const ma_result result = ma_device_init(context, &config, device); result != MA_SUCCESS) {
        delete device;
        error = "opening '" + selection.name + "': " + describe(result);
        return false;
    }

    device_ = device;
    name_ = selection.name;
    const double rate = static_cast<double>(device->sampleRate);
    sample_rate_.store(rate, std::memory_order_relaxed);
    // The engine needs the real rate before it renders, and the device is not running yet, so
    // this is the one moment where setting it costs nothing.
    engine_.prepare(rate);
    open_.store(true, std::memory_order_relaxed);
    return true;
}

bool AudioDevice::start(std::string& error) {
    auto* device = static_cast<ma_device*>(device_);
    if (device == nullptr) {
        error = "no audio device is open";
        return false;
    }
    if (const ma_result result = ma_device_start(device); result != MA_SUCCESS) {
        error = "ma_device_start: " + describe(result);
        return false;
    }
    return true;
}

void AudioDevice::stop() {
    if (device_ != nullptr)
        ma_device_stop(static_cast<ma_device*>(device_)); // joins the callback thread
}

void AudioDevice::close() {
    open_.store(false, std::memory_order_relaxed);
    if (device_ == nullptr)
        return;
    auto* device = static_cast<ma_device*>(device_);
    // ma_device_uninit stops the device and joins its thread, which is what makes deleting the
    // object below safe: nothing can be rendering.
    ma_device_uninit(device);
    delete device;
    device_ = nullptr;
}

void AudioDevice::poll() {
    if (!is_open() || device_ == nullptr)
        return;
    if (ma_device_get_state(static_cast<ma_device*>(device_)) == ma_device_state_started)
        return;

    // The system has taken the device: an unplug, a new default output, a driver reset. Reopen
    // what the caller asked for rather than the id we last had, because a driver reload changes
    // ids and usually keeps names.
    restarts_.fetch_add(1, std::memory_order_relaxed);
    const std::string wanted = selector_;
    close();
    std::string error;
    if (open(wanted, period_frames_, error))
        start(error);
}

void AudioDevice::render(float* output, int frames) {
    // Two performance counter reads per block: they are what turns "CPU under 3% of one core"
    // into a measured number, and they cost tens of nanoseconds against a millisecond budget.
    const std::int64_t begin_ns = static_cast<std::int64_t>(ticks_to_ns(now_ticks()));

    callback_frames_.store(frames, std::memory_order_relaxed);
    engine_.process(output, frames);

    const std::int64_t end_ns = static_cast<std::int64_t>(ticks_to_ns(now_ticks()));
    busy_ns_.fetch_add(static_cast<std::uint64_t>(end_ns - begin_ns), std::memory_order_relaxed);

    const double rate = sample_rate_.load(std::memory_order_relaxed);
    const std::int64_t period_ns =
        rate > 0.0 ? static_cast<std::int64_t>(static_cast<double>(frames) * (1e9 / rate)) : 0;
    if (first_ns_ == 0)
        first_ns_ = end_ns;
    watchdog_.arrive(end_ns, period_ns);

    span_ns_.store(end_ns - first_ns_, std::memory_order_relaxed);
    callbacks_.store(watchdog_.callbacks(), std::memory_order_relaxed);
    late_.store(watchdog_.late(), std::memory_order_relaxed);
    worst_gap_ns_.store(watchdog_.worst_gap_ns(), std::memory_order_relaxed);
    for (std::size_t i = 0; i < gaps_.size(); ++i)
        gaps_[i].store(watchdog_.bucket(static_cast<CallbackWatchdog::Bucket>(i)),
                       std::memory_order_relaxed);
}

double AudioDevice::cpu_fraction() const {
    const std::int64_t span = span_ns_.load(std::memory_order_relaxed);
    if (span <= 0)
        return 0.0;
    return static_cast<double>(busy_ns_.load(std::memory_order_relaxed)) /
           static_cast<double>(span);
}

} // namespace boreaz::io
