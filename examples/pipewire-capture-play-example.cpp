#include <cstring>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/names.h>

#include "macros/assert.hpp"
#include "macros/autoptr.hpp"
#include "util/cleaner.hpp"
#include "util/critical.hpp"

namespace {
template <class T>
auto ceil(T a, T b) -> T {
    return (a + b - 1) / b;
}

constexpr auto capture_rate = 48000;
constexpr auto encode_rate  = 8000;
static_assert(capture_rate >= encode_rate);
static_assert(capture_rate % encode_rate == 0);
constexpr auto playback_channels = 1;

declare_autoptr(PWMainLoop, pw_main_loop, pw_main_loop_destroy);
declare_autoptr(PWStream, pw_stream, pw_stream_destroy);
declare_autoptr(PWProperties, pw_properties, pw_properties_clear);

struct Samples {
    std::vector<float> data;
    size_t             cursor;
};

struct Context {
    pw_stream* capture_stream;
    pw_stream* playback_stream;

    spa_audio_info capture_format;

    Critical<Samples> critical_samples;
};

auto capture_on_process(void* const userdata) -> void {
    auto& context = *std::bit_cast<Context*>(userdata);

    const auto pw_buffer = pw_stream_dequeue_buffer(context.capture_stream);
    ensure(pw_buffer != NULL);
    auto cleaner = Cleaner{[&] { pw_stream_queue_buffer(context.capture_stream, pw_buffer); }};

    const auto buffer  = pw_buffer->buffer;
    const auto samples = std::bit_cast<float*>(buffer->datas[0].data);
    ensure(samples != NULL);

    const auto num_channels = context.capture_format.info.raw.channels;
    const auto num_samples  = buffer->datas[0].chunk->size / sizeof(float);

    constexpr auto rate = capture_rate / encode_rate;

    auto resampled = Samples();
    resampled.data.resize(ceil<size_t>(num_samples, rate * num_channels));
    for(auto i = 0uz; i < resampled.data.size(); i += 1) {
        const auto src    = i * rate /*downsample*/ * num_channels /*only use first channel*/;
        resampled.data[i] = samples[src];
    }
    resampled.cursor = 0;

    PRINT("channels={} samples={} buffer={}", num_channels, num_samples, resampled.data.size());

    context.critical_samples.access().second = std::move(resampled);
}

auto capture_on_stream_param_changed(void* const userdata, const uint32_t id, const spa_pod* const param) -> void {
    auto& context = *std::bit_cast<Context*>(userdata);

    if(param == NULL || id != SPA_PARAM_Format) {
        return;
    }

    if(spa_format_parse(param, &context.capture_format.media_type, &context.capture_format.media_subtype) < 0) {
        return;
    }

    if(context.capture_format.media_type != SPA_MEDIA_TYPE_audio || context.capture_format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }

    ensure(spa_format_audio_raw_parse(param, &context.capture_format.info.raw) == 0);
}

auto playback_on_process(void* const userdata) -> void {
    auto& context = *std::bit_cast<Context*>(userdata);

    const auto pw_buffer = pw_stream_dequeue_buffer(context.playback_stream);
    ensure(pw_buffer != NULL);
    auto cleaner = Cleaner{[&] { pw_stream_queue_buffer(context.playback_stream, pw_buffer); }};

    const auto buffer  = pw_buffer->buffer;
    const auto samples = std::bit_cast<float*>(buffer->datas[0].data);
    ensure(samples != NULL);

    auto num_samples = buffer->datas[0].maxsize / sizeof(float);
    if(pw_buffer->requested != 0) {
        num_samples = std::min(pw_buffer->requested * playback_channels, num_samples);
    }

    auto to_copy = 0uz;
    {
        auto [lock, resampled]     = context.critical_samples.access();
        const auto resampled_avail = resampled.data.size() - resampled.cursor;
        to_copy                    = std::min(num_samples, resampled_avail);
        std::memcpy(samples, resampled.data.data(), to_copy * sizeof(float));
        resampled.cursor += to_copy;
    }

    const auto chunk = buffer->datas[0].chunk;
    chunk->offset    = 0;
    chunk->stride    = sizeof(float) * playback_channels;
    chunk->size      = to_copy * chunk->stride;

    PRINT("playback channels={} samples={} copied={}", playback_channels, num_samples, to_copy);
}

const auto capture_stream_events = pw_stream_events{
    .version       = PW_VERSION_STREAM_EVENTS,
    .param_changed = capture_on_stream_param_changed,
    .process       = capture_on_process,
};

const auto playback_stream_events = pw_stream_events{
    .version = PW_VERSION_STREAM_EVENTS,
    .process = playback_on_process,
};
} // namespace

int main(/*const int argc, const char* const* argv*/) {
    pw_init(NULL, NULL);

    auto main_loop = AutoPWMainLoop(pw_main_loop_new(NULL));
    ensure(main_loop.get() != NULL);
    const auto loop = pw_main_loop_get_loop(main_loop.get());

    auto context = Context();

    const auto setup_stream = [loop, &context](const AutoPWProperties props, const char* const name, const pw_stream_events& events, const spa_audio_info_raw format, const spa_direction direction) -> AutoPWStream {
        constexpr auto error_value = nullptr;
        ensure_v(props.get() != NULL);

        auto stream = AutoPWStream(pw_stream_new_simple(loop, name, props.get(), &events, &context));
        ensure_v(stream.get() != NULL);

        // "The POD start is always aligned to 8 bytes."
        alignas(8) auto pod_builder_buffer = std::array<std::byte, 1024>();
        auto            pod_builder        = spa_pod_builder{.data = pod_builder_buffer.data(), .size = pod_builder_buffer.size()};

        const auto params = std::array{
            spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat, &format),
        };

        ensure_v(pw_stream_connect(stream.get(),
                                   direction,
                                   PW_ID_ANY,
                                   pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS),
                                   (const spa_pod**)params.data(), params.size()) == 0);

        return stream;
    };

    auto capture_stream = setup_stream(
        AutoPWProperties(pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Music",
            NULL)),
        "audio-capture",
        capture_stream_events,
        {.format = SPA_AUDIO_FORMAT_F32, .rate = capture_rate},
        SPA_DIRECTION_INPUT);
    ensure(capture_stream.get() != NULL);
    context.capture_stream = capture_stream.get();

    auto playback_stream = setup_stream(
        AutoPWProperties(pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            NULL)),
        "audio-playback",
        playback_stream_events,
        {.format = SPA_AUDIO_FORMAT_F32, .rate = encode_rate, .channels = playback_channels},
        SPA_DIRECTION_OUTPUT);
    ensure(playback_stream.get() != NULL);
    context.playback_stream = playback_stream.get();

    pw_main_loop_run(main_loop.get());

    pw_deinit();

    return 0;
}
