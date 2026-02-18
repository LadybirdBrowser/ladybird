/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AudioServer/OutputDriver.h>
#include <AudioServer/Server.h>
#include <LibCore/ThreadedPromise.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <pulse/pulseaudio.h>

namespace AudioServer {

using Audio::Channel;
using Audio::ChannelMap;
using Audio::SampleSpecification;

static StringView pulse_audio_error_to_string(int code)
{
    if (code < 0)
        return "Unknown error code"sv;
    char const* string = pa_strerror(code);
    return StringView { string, __builtin_strlen(string) };
}

#define ENUMERATE_CHANNEL_POSITIONS(C)                                        \
    C(Channel::FrontLeft, PA_CHANNEL_POSITION_FRONT_LEFT)                     \
    C(Channel::FrontRight, PA_CHANNEL_POSITION_FRONT_RIGHT)                   \
    C(Channel::FrontCenter, PA_CHANNEL_POSITION_FRONT_CENTER)                 \
    C(Channel::LowFrequency, PA_CHANNEL_POSITION_LFE)                         \
    C(Channel::BackLeft, PA_CHANNEL_POSITION_REAR_LEFT)                       \
    C(Channel::BackRight, PA_CHANNEL_POSITION_REAR_RIGHT)                     \
    C(Channel::FrontLeftOfCenter, PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER)   \
    C(Channel::FrontRightOfCenter, PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER) \
    C(Channel::BackCenter, PA_CHANNEL_POSITION_REAR_CENTER)                   \
    C(Channel::SideLeft, PA_CHANNEL_POSITION_SIDE_LEFT)                       \
    C(Channel::SideRight, PA_CHANNEL_POSITION_SIDE_RIGHT)                     \
    C(Channel::TopCenter, PA_CHANNEL_POSITION_TOP_CENTER)                     \
    C(Channel::TopFrontLeft, PA_CHANNEL_POSITION_TOP_FRONT_LEFT)              \
    C(Channel::TopFrontCenter, PA_CHANNEL_POSITION_TOP_FRONT_CENTER)          \
    C(Channel::TopFrontRight, PA_CHANNEL_POSITION_TOP_FRONT_RIGHT)            \
    C(Channel::TopBackLeft, PA_CHANNEL_POSITION_TOP_REAR_LEFT)                \
    C(Channel::TopBackCenter, PA_CHANNEL_POSITION_TOP_REAR_CENTER)            \
    C(Channel::TopBackRight, PA_CHANNEL_POSITION_TOP_REAR_RIGHT)

static ErrorOr<ChannelMap> pulse_audio_channel_map_to_channel_map(pa_channel_map const& channel_map)
{
    if (channel_map.channels <= 0)
        return Error::from_string_literal("PulseAudio channel map had no channels");
    if (static_cast<size_t>(channel_map.channels) > ChannelMap::capacity())
        return Error::from_string_literal("PulseAudio channel map had too many channels");

    Vector<Channel, ChannelMap::capacity()> channels;
    channels.resize(channel_map.channels);

    static auto pulse_audio_channel_position_to_channel = [](pa_channel_position_t position) {
#define PA_CHANNEL_POSITION_TO_AUDIO_CHANNEL(audio_channel, pa_channel_position) \
    case pa_channel_position:                                                    \
        return audio_channel;
        switch (position) {
            ENUMERATE_CHANNEL_POSITIONS(PA_CHANNEL_POSITION_TO_AUDIO_CHANNEL);
        default:
            return Channel::Unknown;
        }
    };

    for (int index = 0; index < channel_map.channels; index++) {
        channels[index] = pulse_audio_channel_position_to_channel(channel_map.map[index]);
    }

    return ChannelMap(channels);
}

static ErrorOr<pa_channel_map> channel_map_to_pulse_audio_channel_map(ChannelMap const& channel_map)
{
    static_assert(sizeof(pa_channel_map::map) >= PA_CHANNELS_MAX * sizeof(*pa_channel_map::map));
    if (static_cast<size_t>(channel_map.channel_count()) > PA_CHANNELS_MAX)
        return Error::from_string_literal("PulseAudio channel map had too many channels");

    pa_channel_map map;
    map.channels = channel_map.channel_count();

    static auto channel_to_pulse_audio_channel_position = [](Channel channel) {
#define AUDIO_CHANNEL_TO_PA_CHANNEL_POSITION(audio_channel, pa_channel_position) \
    case audio_channel:                                                          \
        return pa_channel_position;
        switch (channel) {
            ENUMERATE_CHANNEL_POSITIONS(AUDIO_CHANNEL_TO_PA_CHANNEL_POSITION);
        default:
            return PA_CHANNEL_POSITION_INVALID;
        }
    };

    u32 index = 0;
    while (index < channel_map.channel_count()) {

        map.map[index] = channel_to_pulse_audio_channel_position(channel_map.channel_at(index));
        index++;
    }

    while (index < PA_CHANNELS_MAX) {
        map.map[index] = PA_CHANNEL_POSITION_INVALID;
        index++;
    }
    return map;
}

class PulseAudioOutputDriver final : public OutputDriver {
public:
    using SampleSpecificationCallback = OutputDriver::SampleSpecificationCallback;
    using AudioDataRequestCallback = OutputDriver::AudioDataRequestCallback;

    static ErrorOr<NonnullOwnPtr<OutputDriver>> create(DeviceHandle, OutputState, u32 target_latency_ms, SampleSpecificationCallback&&, AudioDataRequestCallback&&);

    virtual void set_underrun_callback(Function<void()>) override;
    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;
    virtual AK::Duration device_time_played() const override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;
    virtual ~PulseAudioOutputDriver() override;

private:
    struct MainLoopUnlocker {
        PulseAudioOutputDriver const& driver;

        void operator()()
        {
            driver.unlock_main_loop();
        }
    };

    explicit PulseAudioOutputDriver() = default;

    ErrorOr<void> initialize(DeviceHandle, OutputState, u32 target_latency_ms, SampleSpecificationCallback&&, AudioDataRequestCallback&&);

    ErrorOr<void> ensure_stream_is_initialized() const;
    ErrorOr<void> resume_stream();
    ErrorOr<void> drain_and_suspend_stream();
    ErrorOr<void> flush_and_suspend_stream();
    ErrorOr<void> set_stream_volume(double);

    ErrorOr<void> setup_context();
    ErrorOr<void> wait_for_context_ready();
    ErrorOr<void> resolve_selected_sink_name(DeviceHandle);
    ErrorOr<void> request_device_sample_specification();
    ErrorOr<void> create_stream(OutputState, u32 target_latency_ms);
    ErrorOr<void> wait_for_operation(pa_operation*, StringView error_message);

    bool current_thread_is_main_loop_thread() const;
    void lock_main_loop() const;
    void unlock_main_loop() const;
    [[nodiscard]] ScopeGuard<MainLoopUnlocker> main_loop_locker() const
    {
        lock_main_loop();
        return ScopeGuard(MainLoopUnlocker { *this });
    }
    void wait_for_signal() const;
    void signal_to_wake() const;

    void on_write_requested(size_t bytes_to_write);
    void shutdown();

    pa_threaded_mainloop* m_main_loop { nullptr };
    pa_context* m_context { nullptr };
    pa_stream* m_stream { nullptr };

    SampleSpecification m_sample_specification;
    AudioDataRequestCallback m_data_request_callback { nullptr };
    Function<void()> m_underrun_callback;
    bool m_suspended { true };
    bool m_started_playback { false };
    Optional<ByteString> m_selected_sink_name;
};

ErrorOr<NonnullOwnPtr<OutputDriver>> create_platform_output_driver(DeviceHandle device_handle, OutputState initial_output_state, u32 target_latency_ms, OutputDriver::SampleSpecificationCallback&& sample_specification_callback, OutputDriver::AudioDataRequestCallback&& data_request_callback)
{
    DeviceHandle raw_handle = (device_handle == 0) ? 0 : static_cast<DeviceHandle>(Server::device_handle_to_os_device_id(device_handle));
    return PulseAudioOutputDriver::create(raw_handle, initial_output_state, target_latency_ms, move(sample_specification_callback), move(data_request_callback));
}

ErrorOr<NonnullOwnPtr<OutputDriver>> PulseAudioOutputDriver::create(DeviceHandle device_handle, OutputState initial_state, u32 target_latency_ms, SampleSpecificationCallback&& sample_specification_selected_callback, AudioDataRequestCallback&& data_request_callback)
{
    VERIFY(data_request_callback);

    auto output_driver = TRY(adopt_nonnull_own_or_enomem(new (nothrow) PulseAudioOutputDriver()));
    TRY(output_driver->initialize(device_handle, initial_state, target_latency_ms, move(sample_specification_selected_callback), move(data_request_callback)));
    return output_driver;
}

PulseAudioOutputDriver::~PulseAudioOutputDriver()
{
    shutdown();
}

#define TRY_OR_REJECT(expression, ...)                           \
    ({                                                           \
        auto&& __temporary_result = (expression);                \
        if (__temporary_result.is_error()) [[unlikely]] {        \
            promise->reject(__temporary_result.release_error()); \
            return __VA_ARGS__;                                  \
        }                                                        \
        __temporary_result.release_value();                      \
    })

void PulseAudioOutputDriver::set_underrun_callback(Function<void()> callback)
{
    auto locker = main_loop_locker();
    m_underrun_callback = move(callback);
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> PulseAudioOutputDriver::resume()
{
    auto promise = Core::ThreadedPromise<AK::Duration>::create();
    TRY_OR_REJECT(ensure_stream_is_initialized(), promise);
    TRY_OR_REJECT(resume_stream(), promise);
    promise->resolve(device_time_played());
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PulseAudioOutputDriver::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(ensure_stream_is_initialized(), promise);
    TRY_OR_REJECT(drain_and_suspend_stream(), promise);
    promise->resolve();
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PulseAudioOutputDriver::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(ensure_stream_is_initialized(), promise);
    TRY_OR_REJECT(flush_and_suspend_stream(), promise);
    promise->resolve();
    return promise;
}

AK::Duration PulseAudioOutputDriver::device_time_played() const
{
    auto locker = main_loop_locker();

    if (m_stream == nullptr || !m_started_playback)
        return AK::Duration::zero();

    pa_usec_t time = 0;
    auto error = pa_stream_get_time(m_stream, &time);
    if (error)
        return AK::Duration::zero();

    if (time > NumericLimits<i64>::max()) {
        warnln("WARNING: Audio time is too large!");
        time -= NumericLimits<i64>::max();
    }

    return AK::Duration::from_microseconds(static_cast<i64>(time));
}

NonnullRefPtr<Core::ThreadedPromise<void>> PulseAudioOutputDriver::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(ensure_stream_is_initialized(), promise);
    TRY_OR_REJECT(set_stream_volume(volume), promise);
    promise->resolve();
    return promise;
}

ErrorOr<void> PulseAudioOutputDriver::initialize(DeviceHandle device_handle, OutputState initial_state, u32 target_latency_ms, SampleSpecificationCallback&& sample_specification_selected_callback, AudioDataRequestCallback&& data_request_callback)
{
    m_data_request_callback = move(data_request_callback);

    TRY(setup_context());
    TRY(wait_for_context_ready());
    TRY(resolve_selected_sink_name(device_handle));
    TRY(request_device_sample_specification());

    sample_specification_selected_callback(m_sample_specification);

    TRY(create_stream(initial_state, target_latency_ms));

    TRY(set_stream_volume(1.0));
    return {};
}

ErrorOr<void> PulseAudioOutputDriver::ensure_stream_is_initialized() const
{
    if (m_stream == nullptr)
        return Error::from_string_literal("PulseAudio stream is not initialized");
    return {};
}

ErrorOr<void> PulseAudioOutputDriver::resume_stream()
{
    auto locker = main_loop_locker();

    if (!m_suspended)
        return {};
    m_suspended = false;

    TRY(wait_for_operation(pa_stream_cork(m_stream, 0, [](pa_stream*, int, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this), "Uncorking PulseAudio stream failed"sv));

    size_t writable_size = pa_stream_writable_size(m_stream);
    if (writable_size != static_cast<size_t>(-1))
        on_write_requested(writable_size);

    return {};
}

ErrorOr<void> PulseAudioOutputDriver::drain_and_suspend_stream()
{
    auto locker = main_loop_locker();

    if (m_suspended)
        return {};
    m_suspended = true;

    if (pa_stream_is_corked(m_stream) > 0)
        return {};

    TRY(wait_for_operation(pa_stream_drain(m_stream, [](pa_stream*, int, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this), "Draining PulseAudio stream failed"sv));

    TRY(wait_for_operation(pa_stream_cork(m_stream, 1, [](pa_stream*, int, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this), "Corking PulseAudio stream after drain failed"sv));

    return {};
}

ErrorOr<void> PulseAudioOutputDriver::flush_and_suspend_stream()
{
    auto locker = main_loop_locker();

    if (m_suspended)
        return {};
    m_suspended = true;

    if (pa_stream_is_corked(m_stream) > 0)
        return {};

    TRY(wait_for_operation(pa_stream_flush(m_stream, [](pa_stream*, int, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this), "Flushing PulseAudio stream failed"sv));

    TRY(wait_for_operation(pa_stream_cork(m_stream, 1, [](pa_stream*, int, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this), "Corking PulseAudio stream after flush failed"sv));

    return {};
}

ErrorOr<void> PulseAudioOutputDriver::set_stream_volume(double volume)
{
    auto locker = main_loop_locker();

    if (m_stream == nullptr)
        return Error::from_string_literal("PulseAudio stream is not initialized");

    auto index = pa_stream_get_index(m_stream);
    if (index == PA_INVALID_INDEX)
        return Error::from_string_literal("Failed to get PulseAudio stream index while setting volume");

    auto pulse_volume = pa_sw_volume_from_linear(volume);
    pa_cvolume per_channel_volumes;
    pa_cvolume_set(&per_channel_volumes, pa_stream_get_sample_spec(m_stream)->channels, pulse_volume);

    auto* operation = pa_context_set_sink_input_volume(m_context, index, &per_channel_volumes, [](pa_context*, int, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this);

    return wait_for_operation(operation, "Failed to set PulseAudio stream volume"sv);
}

ErrorOr<void> PulseAudioOutputDriver::setup_context()
{
    m_main_loop = pa_threaded_mainloop_new();
    if (m_main_loop == nullptr)
        return Error::from_string_literal("Failed to create PulseAudio main loop");

    auto* api = pa_threaded_mainloop_get_api(m_main_loop);
    if (api == nullptr)
        return Error::from_string_literal("Failed to get PulseAudio API");

    m_context = pa_context_new(api, "Ladybird AudioServer");
    if (m_context == nullptr)
        return Error::from_string_literal("Failed to create PulseAudio context");

    pa_context_set_state_callback(m_context, [](pa_context*, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this);

    if (auto error = pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr); error < 0)
        return Error::from_string_literal("Error while starting PulseAudio daemon connection");

    if (auto error = pa_threaded_mainloop_start(m_main_loop); error < 0)
        return Error::from_string_literal("Failed to start PulseAudio main loop");

    return {};
}

ErrorOr<void> PulseAudioOutputDriver::wait_for_context_ready()
{
    auto locker = main_loop_locker();

    while (true) {
        auto state = pa_context_get_state(m_context);
        if (state == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(state))
            return Error::from_string_literal("Failed to connect to PulseAudio server");
        wait_for_signal();
    }

    pa_context_set_state_callback(m_context, nullptr, nullptr);
    return {};
}

ErrorOr<void> PulseAudioOutputDriver::resolve_selected_sink_name(DeviceHandle device_handle)
{
    auto locker = main_loop_locker();

    m_selected_sink_name.clear();
    if (device_handle == 0)
        return {};

    struct SelectionResult {
        PulseAudioOutputDriver* state { nullptr };
        Optional<ByteString> sink_name;
    };
    SelectionResult selection {
        .state = this,
        .sink_name = {},
    };

    auto* operation = pa_context_get_sink_info_by_index(
        m_context,
        static_cast<u32>(device_handle),
        [](pa_context*, pa_sink_info const* info, int status, void* user_data) {
            auto& selection = *static_cast<SelectionResult*>(user_data);
            selection.state->signal_to_wake();

            if (status != 0)
                return;

            if (info && info->name)
                selection.sink_name = ByteString(info->name);
        },
        &selection);

    if (operation == nullptr)
        return Error::from_string_literal("Failed to resolve PulseAudio sink for output device handle");

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
        wait_for_signal();

    pa_operation_unref(operation);

    if (!selection.sink_name.has_value())
        return Error::from_string_literal("Unknown PulseAudio output device handle");

    m_selected_sink_name = move(selection.sink_name);
    return {};
}

ErrorOr<void> PulseAudioOutputDriver::request_device_sample_specification()
{
    auto locker = main_loop_locker();

    static constexpr auto set_default_sample_specification = [](PulseAudioOutputDriver& state) {
        state.m_sample_specification = SampleSpecification(44100, ChannelMap::stereo());
        state.signal_to_wake();
    };

    m_sample_specification = {};

    auto* operation = pa_context_get_server_info(
        m_context, [](pa_context*, pa_server_info const* info, void* user_data) {
            auto& state = *static_cast<PulseAudioOutputDriver*>(user_data);

            if (info->default_sink_name == nullptr) {
                set_default_sample_specification(state);
                return;
            }

            auto* operation = pa_context_get_sink_info_by_name(
                state.m_context, info->default_sink_name, [](pa_context*, pa_sink_info const* sink_info, int status, void* user_data) {
                    auto& state = *static_cast<PulseAudioOutputDriver*>(user_data);

                    if (status != 0) {
                        if (!state.m_sample_specification.is_valid())
                            set_default_sample_specification(state);
                        return;
                    }

                    auto channel_map_result = pulse_audio_channel_map_to_channel_map(sink_info->channel_map);
                    if (channel_map_result.is_error()) {
                        set_default_sample_specification(state);
                        return;
                    }

                    state.m_sample_specification = SampleSpecification(sink_info->sample_spec.rate, channel_map_result.release_value());
                    state.signal_to_wake();
                },
                &state);

            if (operation != nullptr)
                pa_operation_unref(operation);
        },
        this);

    if (operation == nullptr)
        return Error::from_string_literal("Failed to query PulseAudio server info");
    pa_operation_unref(operation);

    while (!m_sample_specification.is_valid() && PA_CONTEXT_IS_GOOD(pa_context_get_state(m_context)))
        wait_for_signal();

    if (!m_sample_specification.is_valid())
        return Error::from_string_literal("Failed to determine PulseAudio sample specification");

    return {};
}

ErrorOr<void> PulseAudioOutputDriver::create_stream(OutputState initial_state, u32 target_latency_ms)
{
    auto locker = main_loop_locker();

    pa_sample_spec sample_specification {
        PA_SAMPLE_FLOAT32LE,
        m_sample_specification.sample_rate(),
        m_sample_specification.channel_map().channel_count(),
    };

    if (pa_sample_spec_valid(&sample_specification) == 0)
        return Error::from_string_literal("PulseAudio sample specification is invalid");

    pa_channel_map pa_channel_map = TRY(channel_map_to_pulse_audio_channel_map(m_sample_specification.channel_map()));
    if (!pa_channel_map_valid(&pa_channel_map))
        return Error::from_string_literal("Channel map is incompatible with PulseAudio");

    m_stream = pa_stream_new_with_proplist(m_context, "Audio Stream", &sample_specification, &pa_channel_map, nullptr);
    if (m_stream == nullptr)
        return Error::from_string_literal("Failed to create PulseAudio stream");

    pa_stream_set_state_callback(m_stream, [](pa_stream*, void* user_data) { static_cast<PulseAudioOutputDriver*>(user_data)->signal_to_wake(); }, this);

    pa_stream_set_write_callback(m_stream, [](pa_stream* stream, size_t bytes_to_write, void* user_data) {
        auto& state = *static_cast<PulseAudioOutputDriver*>(user_data);
        VERIFY(state.m_stream == stream);
        state.on_write_requested(bytes_to_write); }, this);

    pa_stream_set_started_callback(m_stream, [](pa_stream* stream, void* user_data) {
        auto& state = *static_cast<PulseAudioOutputDriver*>(user_data);
        state.m_started_playback = true;
        pa_stream_set_started_callback(stream, nullptr, nullptr); }, this);

    pa_stream_set_underflow_callback(m_stream, [](pa_stream*, void* user_data) {
        auto& state = *static_cast<PulseAudioOutputDriver*>(user_data);
        if (state.m_underrun_callback)
            state.m_underrun_callback(); }, this);

    pa_buffer_attr buffer_attributes;
    buffer_attributes.maxlength = -1;
    buffer_attributes.prebuf = -1;
    u64 const target_latency_frames = target_latency_ms * sample_specification.rate / 1000u;
    u64 const target_latency_bytes = target_latency_frames * pa_frame_size(&sample_specification);
    buffer_attributes.tlength = static_cast<u32>(min<u64>(target_latency_bytes, NumericLimits<u32>::max()));
    buffer_attributes.minreq = buffer_attributes.tlength / 4;
    buffer_attributes.fragsize = buffer_attributes.minreq;

    auto flags = static_cast<pa_stream_flags>(PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY | PA_STREAM_RELATIVE_VOLUME);

    m_suspended = initial_state == OutputState::Suspended;
    if (m_suspended)
        flags = static_cast<pa_stream_flags>(static_cast<u32>(flags) | PA_STREAM_START_CORKED);

    auto const* sink_name = m_selected_sink_name.has_value() ? m_selected_sink_name->characters() : nullptr;
    if (auto error = pa_stream_connect_playback(m_stream, sink_name, &buffer_attributes, flags, nullptr, nullptr); error != 0)
        return Error::from_string_literal("Error while connecting the PulseAudio stream");

    while (true) {
        auto state = pa_stream_get_state(m_stream);
        if (state == PA_STREAM_READY)
            break;
        if (!PA_STREAM_IS_GOOD(state))
            return Error::from_string_literal("Failed to connect to PulseAudio stream");
        wait_for_signal();
    }

    pa_stream_set_state_callback(m_stream, nullptr, nullptr);
    return {};
}

ErrorOr<void> PulseAudioOutputDriver::wait_for_operation(pa_operation* operation, StringView error_message)
{
    if (operation == nullptr)
        return Error::from_string_view(error_message);

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
        wait_for_signal();

    bool context_good = PA_CONTEXT_IS_GOOD(pa_context_get_state(m_context));
    bool stream_good = m_stream != nullptr && PA_STREAM_IS_GOOD(pa_stream_get_state(m_stream));
    if (!context_good || !stream_good) {
        int code = pa_context_errno(m_context);
        warnln("Encountered stream error: {}", pulse_audio_error_to_string(code));
        pa_operation_unref(operation);
        return Error::from_string_view(error_message);
    }

    pa_operation_unref(operation);
    return {};
}

bool PulseAudioOutputDriver::current_thread_is_main_loop_thread() const
{
    if (m_main_loop == nullptr)
        return false;
    return static_cast<bool>(pa_threaded_mainloop_in_thread(m_main_loop));
}

void PulseAudioOutputDriver::lock_main_loop() const
{
    if (m_main_loop != nullptr && !current_thread_is_main_loop_thread())
        pa_threaded_mainloop_lock(m_main_loop);
}

void PulseAudioOutputDriver::unlock_main_loop() const
{
    if (m_main_loop != nullptr && !current_thread_is_main_loop_thread())
        pa_threaded_mainloop_unlock(m_main_loop);
}

void PulseAudioOutputDriver::wait_for_signal() const
{
    if (m_main_loop != nullptr)
        pa_threaded_mainloop_wait(m_main_loop);
}

void PulseAudioOutputDriver::signal_to_wake() const
{
    if (m_main_loop != nullptr)
        pa_threaded_mainloop_signal(m_main_loop, 0);
}

void PulseAudioOutputDriver::on_write_requested(size_t bytes_to_write)
{
    if (m_suspended || !m_data_request_callback)
        return;

    while (bytes_to_write > 0) {
        void* data_pointer = nullptr;
        size_t data_size = bytes_to_write;
        if (pa_stream_begin_write(m_stream, &data_pointer, &data_size) != 0 || data_pointer == nullptr)
            return;

        Bytes buffer { data_pointer, data_size };
        size_t frame_size = pa_frame_size(pa_stream_get_sample_spec(m_stream));
        if (frame_size == 0 || (buffer.size() % frame_size) != 0) {
            (void)pa_stream_cancel_write(m_stream);
            return;
        }

        auto requested_data = m_data_request_callback(buffer.reinterpret<float>()).reinterpret<u8 const>();
        if (requested_data.is_empty()) {
            (void)pa_stream_cancel_write(m_stream);
            return;
        }

        if (pa_stream_write(m_stream, requested_data.data(), requested_data.size(), nullptr, 0, PA_SEEK_RELATIVE) != 0)
            return;

        if (requested_data.size() >= bytes_to_write)
            break;

        bytes_to_write -= requested_data.size();
    }
}

void PulseAudioOutputDriver::shutdown()
{
    if (m_main_loop == nullptr)
        return;

    {
        auto locker = main_loop_locker();

        if (m_stream != nullptr) {
            pa_stream_set_write_callback(m_stream, nullptr, nullptr);
            pa_stream_set_underflow_callback(m_stream, nullptr, nullptr);
            pa_stream_set_started_callback(m_stream, nullptr, nullptr);
            pa_stream_disconnect(m_stream);
            pa_stream_unref(m_stream);
            m_stream = nullptr;
        }

        if (m_context != nullptr) {
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
    }

    pa_threaded_mainloop_stop(m_main_loop);
    pa_threaded_mainloop_free(m_main_loop);
    m_main_loop = nullptr;
}

}
