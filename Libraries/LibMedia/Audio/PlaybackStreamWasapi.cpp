/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Platform.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibCore/ThreadedPromise.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Audio/PlaybackStreamWasapi.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

#include <AK/Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <timeapi.h>

// NOTE: Not using the newer winrt that supersedes wrl as that uses exceptions for error handling
#include <wrl/client.h>

namespace Audio {

using namespace Microsoft::WRL;

#define MUST_HR(expression)                                                                \
    ({                                                                                     \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", HRESULT&& _temporary_hr = (expression));          \
        if (FAILED(_temporary_hr)) [[unlikely]] {                                          \
            dbgln("Expression failed with: {}", Error::from_windows_error(_temporary_hr)); \
            VERIFY_NOT_REACHED();                                                          \
        }                                                                                  \
    })

#define TRY_HR(expression)                                                        \
    ({                                                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", HRESULT&& _temporary_hr = (expression)); \
        if (FAILED(_temporary_hr)) [[unlikely]]                                   \
            return Error::from_windows_error(_temporary_hr);                      \
    })

// GUID for the playback session. That way all render streams have a single volume slider in the OS interface
constexpr GUID PlaybackSessionGUID = { // 22f2ca89-210a-492c-a0aa-f25b1d2f33a1
    0x22f2ca89,
    0x210a,
    0x492c,
    { 0xa0, 0xaa, 0xf2, 0x5b, 0x1d, 0x2f, 0x33, 0xa1 }
};

struct TaskPlay {
    NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> promise;
};

struct TaskDrainAndSuspend {
    NonnullRefPtr<Core::ThreadedPromise<void>> promise;
};

struct TaskDiscardAndSuspend {
    NonnullRefPtr<Core::ThreadedPromise<void>> promise;
};

class ComUninitializer {
public:
    ~ComUninitializer()
    {
        if (initialized)
            CoUninitialize();
    }
    bool initialized = false;
};

static thread_local ComUninitializer s_com_uninitializer {};

struct PlaybackStreamWASAPI::AudioState : public AtomicRefCounted<PlaybackStreamWASAPI::AudioState> {
    AudioState();
    ~AudioState();

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audio_client;
    ComPtr<IAudioRenderClient> render_client;
    ComPtr<IAudioStreamVolume> audio_stream_volume;
    ComPtr<IAudioClock> clock;

    WAVEFORMATEXTENSIBLE wave_format;
    UINT32 buffer_frame_count;
    HANDLE buffer_event = 0;

    PlaybackStreamWASAPI::AudioDataRequestCallback data_request_callback;
    Function<void()> underrun_callback;

    Threading::Mutex task_queue_mutex;
    Queue<Variant<TaskPlay, TaskDrainAndSuspend, TaskDiscardAndSuspend>> task_queue;
    // FIXME: Create a owning handle type to be shared in the codebase
    HANDLE task_event = 0;

    bool playing = false;
    bool drain_and_suspend = false;
    Atomic<bool> exit_requested = false;

    static int render_thread_loop(AudioState& state);
    RefPtr<Core::ThreadedPromise<AK::Duration>> resume_promise;
    RefPtr<Core::ThreadedPromise<void>> suspend_promise;

    Vector<float, ChannelMap::capacity()> channel_volumes;
    UINT64 audio_client_clock_frequency;
};

PlaybackStreamWASAPI::AudioState::AudioState()
{
    task_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    VERIFY(task_event);
}

PlaybackStreamWASAPI::AudioState::~AudioState()
{
    if (buffer_event)
        CloseHandle(buffer_event);
    if (task_event)
        CloseHandle(task_event);
}

ALWAYS_INLINE AK::Duration PlaybackStreamWASAPI::total_time_played_with_com_initialized(PlaybackStreamWASAPI::AudioState& state)
{
    UINT64 position;
    MUST_HR(state.clock->GetPosition(&position, nullptr));
    return AK::Duration::from_time_units(AK::clamp_to<i64>(position), 1, state.audio_client_clock_frequency);
}

PlaybackStreamWASAPI::PlaybackStreamWASAPI(NonnullRefPtr<AudioState> state)
    : m_state(move(state))
{
}

PlaybackStreamWASAPI::~PlaybackStreamWASAPI()
{
    m_state->exit_requested.store(true, MemoryOrder::memory_order_release);
    // Poke the event to wake the thread up from wait
    VERIFY(m_state->buffer_event != NULL);
    SetEvent(m_state->buffer_event);
}

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStream::create(OutputState initial_output_state, u32 target_latency_ms, SampleSpecificationCallback&& specification_callback, AudioDataRequestCallback&& data_callback)
{
    return PlaybackStreamWASAPI::create(initial_output_state, target_latency_ms, move(specification_callback), move(data_callback));
}

static void print_audio_format(WAVEFORMATEXTENSIBLE& format)
{
    VERIFY(format.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    dbgln("wFormatTag: {:x}\n"
          "nChannels: {}\n"
          "nSamplesPerSec: {}\n"
          "nAvgBytesPerSec: {}\n"
          "nBlockAlign: {}\n"
          "wBitsPerSample: {}\n"
          "cbSize: {}\n"
          "Samples.wValidBitsPerSample: {}\n"
          "dwChannelMask: {:b}\n"
          "SubFormat: {}",
        format.Format.wFormatTag,
        format.Format.nChannels,
        format.Format.nSamplesPerSec,
        format.Format.nAvgBytesPerSec,
        format.Format.nBlockAlign,
        format.Format.wBitsPerSample,
        format.Format.cbSize,
        format.Samples.wValidBitsPerSample,
        format.dwChannelMask,
        Span<u8> { reinterpret_cast<char*>(&format.SubFormat), 16 });
}

// This needs to be kept up to date with KSAUDIO_CHANNEL_LAYOUT in ksmedia.h
#define ENUMERATE_CHANNEL_POSITIONS(C)                            \
    C(SPEAKER_FRONT_LEFT, Channel::FrontLeft)                     \
    C(SPEAKER_FRONT_RIGHT, Channel::FrontRight)                   \
    C(SPEAKER_FRONT_CENTER, Channel::FrontCenter)                 \
    C(SPEAKER_LOW_FREQUENCY, Channel::LowFrequency)               \
    C(SPEAKER_BACK_LEFT, Channel::BackLeft)                       \
    C(SPEAKER_BACK_RIGHT, Channel::BackRight)                     \
    C(SPEAKER_FRONT_LEFT_OF_CENTER, Channel::FrontLeftOfCenter)   \
    C(SPEAKER_FRONT_RIGHT_OF_CENTER, Channel::FrontRightOfCenter) \
    C(SPEAKER_BACK_CENTER, Channel::BackCenter)                   \
    C(SPEAKER_SIDE_LEFT, Channel::SideLeft)                       \
    C(SPEAKER_SIDE_RIGHT, Channel::SideRight)                     \
    C(SPEAKER_TOP_CENTER, Channel::TopCenter)                     \
    C(SPEAKER_TOP_FRONT_LEFT, Channel::TopFrontLeft)              \
    C(SPEAKER_TOP_FRONT_CENTER, Channel::TopFrontCenter)          \
    C(SPEAKER_TOP_FRONT_RIGHT, Channel::TopFrontRight)            \
    C(SPEAKER_TOP_BACK_LEFT, Channel::TopBackLeft)                \
    C(SPEAKER_TOP_BACK_CENTER, Channel::TopBackCenter)            \
    C(SPEAKER_TOP_BACK_RIGHT, Channel::TopBackRight)

static ErrorOr<ChannelMap> convert_bitmask_to_channel_map(u32 channel_bitmask)
{
    Vector<Channel, ChannelMap::capacity()> channels;

#define MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG(ksmedia_channel_name, audio_channel)               \
    if ((channel_bitmask & ksmedia_channel_name) != 0) {                                      \
        if (channels.size() == ChannelMap::capacity()) [[unlikely]]                           \
            return Error::from_string_literal("Device channel layout had too many channels"); \
        channels.unchecked_append(audio_channel);                                             \
    }

    ENUMERATE_CHANNEL_POSITIONS(MAYBE_ADD_CHANNEL_FROM_BITMAP_FLAG);

    if ((channel_bitmask & SPEAKER_RESERVED) != 0) [[unlikely]]
        return Error::from_string_literal("Unsupported new KSMEDIA version");

    return ChannelMap { channels };
}

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStreamWASAPI::create(OutputState initial_output_state, u32, SampleSpecificationCallback&& sample_specification_callback, AudioDataRequestCallback&& data_request_callback)
{
    HRESULT hr;
    if (!s_com_uninitializer.initialized) {
        TRY_HR(CoInitializeEx(NULL, COINIT_MULTITHREADED));
        s_com_uninitializer.initialized = true;
    }

    auto state = make_ref_counted<PlaybackStreamWASAPI::AudioState>();

    TRY_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&state->enumerator)));
    TRY_HR(state->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &state->device));
    TRY_HR(state->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &state->audio_client));

    state->data_request_callback = move(data_request_callback);

    WAVEFORMATEXTENSIBLE* device_format;
    state->audio_client->GetMixFormat(reinterpret_cast<WAVEFORMATEX**>(&device_format));
    ScopeGuard free_mix_format = [&device_format] { CoTaskMemFree(device_format); };

    dbgln_if(AUDIO_DEBUG, "PlaybackStreamWASAPI: Mixing engine audio format:\n");
    if (AUDIO_DEBUG)
        print_audio_format(*device_format);

    VERIFY(device_format->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    VERIFY(device_format->Format.nChannels <= ChannelMap::capacity());
    VERIFY(popcount(device_format->dwChannelMask) == device_format->Format.nChannels);
    auto channels = device_format->Format.nChannels;

    ChannelMap channel_map = MUST(convert_bitmask_to_channel_map(device_format->dwChannelMask));

    sample_specification_callback(SampleSpecification { device_format->Format.nSamplesPerSec, channel_map });

    // Set up a 32bit float pcm stream with whatever sample rate and channels we were given.

    auto block_align = channels * sizeof(float);
    state->wave_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    state->wave_format.Format.nChannels = channels;
    state->wave_format.Format.nSamplesPerSec = device_format->Format.nSamplesPerSec;
    state->wave_format.Format.nAvgBytesPerSec = device_format->Format.nSamplesPerSec * block_align;
    state->wave_format.Format.nBlockAlign = block_align;
    state->wave_format.Format.wBitsPerSample = 32;
    state->wave_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    state->wave_format.Samples.wValidBitsPerSample = 32;
    state->wave_format.dwChannelMask = device_format->dwChannelMask;
    state->wave_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    state->channel_volumes.resize(channels);

    WAVEFORMATEXTENSIBLE* closest_match;
    hr = state->audio_client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &state->wave_format.Format, reinterpret_cast<WAVEFORMATEX**>(&closest_match));
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    if (hr == S_FALSE) {
        dbgln("Audio format not supported. Current format:\n");
        print_audio_format(state->wave_format);
        dbgln("Closest supported audio format:\n");
        print_audio_format(*closest_match);
        CoTaskMemFree(closest_match);
        VERIFY_NOT_REACHED();
    }
    // TODO: check the actual format of the engine and use it if possible to reduce overhead
    DWORD stream_flags
        = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
    // For event driven buffering we can't specify the buffer duration.
    TRY_HR(state->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, &state->wave_format.Format, &PlaybackSessionGUID));
    TRY_HR(state->audio_client->GetBufferSize(&state->buffer_frame_count));
    TRY_HR(state->audio_client->GetService(IID_PPV_ARGS(&state->render_client)));
    TRY_HR(state->audio_client->GetService(IID_PPV_ARGS(&state->audio_stream_volume)));
    TRY_HR(state->audio_client->GetService(IID_PPV_ARGS(&state->clock)));

    state->buffer_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!state->buffer_event)
        return Error::from_windows_error(hr);

    TRY_HR(state->audio_client->SetEventHandle(state->buffer_event));
    TRY_HR(state->clock->GetFrequency(&state->audio_client_clock_frequency));

    if (initial_output_state == OutputState::Playing)
        state->playing = true;

    auto audio_thread = Threading::Thread::construct("Audio Render"sv, [state] {
        return AudioState::render_thread_loop(*state);
    });

    if (initial_output_state == OutputState::Playing)
        TRY_HR(state->audio_client->Start());

    audio_thread->start();
    audio_thread->detach();

    return TRY(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackStreamWASAPI(move(state))));
}

int PlaybackStreamWASAPI::AudioState::render_thread_loop(PlaybackStreamWASAPI::AudioState& state)
{
    MUST_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    WORD block_align = state.wave_format.Format.nBlockAlign;

    ScopeGuard uninitialize_com = [] { CoUninitialize(); };

    VERIFY(timeBeginPeriod(1) == TIMERR_NOERROR);
    DWORD task_index = 0;
    HANDLE task_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    ScopeGuard revert_thread_priority = [&task_handle] { AvRevertMmThreadCharacteristics(task_handle); };

    while (!state.exit_requested.load(MemoryOrder::memory_order_acquire)) {
        Array handles = { state.task_event, state.buffer_event };
        DWORD result = WaitForMultipleObjects(handles.size(), handles.data(), FALSE, INFINITE);
        switch (result) {
        case WAIT_OBJECT_0: {
            state.task_queue_mutex.lock();
            while (!state.task_queue.is_empty()) {
                auto task = state.task_queue.dequeue();
                task.visit(
                    [&state](TaskPlay const& task) {
                        HRESULT hr = state.audio_client->Start();
                        if (hr == AUDCLNT_E_NOT_STOPPED)
                            dbgln_if(AUDIO_DEBUG, "PlaybackStreamWASAPI: Trying to start an already running stream.");
                        else
                            MUST_HR(move(hr));
                        task.promise->resolve(total_time_played_with_com_initialized(state));
                        state.playing = true;
                    },
                    [&state](TaskDrainAndSuspend const& task) {
                        u32 padding;
                        MUST_HR(state.audio_client->GetCurrentPadding(&padding));
                        if (padding > 0) {
                            u32 ms_to_sleep = padding * 1'000ull / state.wave_format.Format.nSamplesPerSec;
                            if (ms_to_sleep > 0) {
                                Sleep(ms_to_sleep - 1);
                                MUST_HR(state.audio_client->GetCurrentPadding(&padding));
                            }
                            if (padding == 0)
                                dbgln_if(AUDIO_DEBUG, "------- PlaybackStreamWASAPI: overslept draining buffer --------");
                            while (padding > 0) {
                                AK::atomic_pause();
                                MUST_HR(state.audio_client->GetCurrentPadding(&padding));
                            }
                        }

                        MUST_HR(state.audio_client->Stop());
                        state.playing = false;
                        task.promise->resolve();
                    },
                    [&state](TaskDiscardAndSuspend const& task) {
                        MUST_HR(state.audio_client->Stop());
                        MUST_HR(state.audio_client->Reset());
                        state.playing = false;
                        task.promise->resolve();
                    });
            }
            state.task_queue_mutex.unlock();
            DWORD res = WaitForSingleObject(handles[1], 0);
            // Both the task event and buffer event were signaled
            if (res == WAIT_OBJECT_0)
                break;
            // The buffer event wasn't signaled, so we skip the iteration
            if (res == WAIT_TIMEOUT)
                continue;
            // Unless the wait errors we should never hit this
            VERIFY_NOT_REACHED();
        }
        case WAIT_OBJECT_0 + 1:
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        // We check that after the wait we weren't asked to exit.
        if (state.exit_requested.load(MemoryOrder::memory_order_acquire)) [[unlikely]]
            break;

        if (!state.playing)
            continue;

        u32 padding = 0;
        // TODO: Try to handle some of the errors.
        MUST_HR(state.audio_client->GetCurrentPadding(&padding));

        u32 frames_available = state.buffer_frame_count - padding;
        if (frames_available == 0) [[unlikely]]
            continue;

        BYTE* buffer;
        MUST_HR(state.render_client->GetBuffer(frames_available, &buffer));

        DWORD buffer_flags = 0;
        u32 buffer_size = frames_available * block_align;
        auto output_buffer = Bytes(buffer, buffer_size).reinterpret<float>();
        auto floats_written = state.data_request_callback(output_buffer);
        if (floats_written.is_empty()) [[unlikely]] {
            if (state.underrun_callback)
                state.underrun_callback();
            buffer_flags |= AUDCLNT_BUFFERFLAGS_SILENT;
        }
        frames_available = floats_written.size() / state.wave_format.Format.nChannels;

        MUST_HR(state.render_client->ReleaseBuffer(frames_available, buffer_flags));
    }

    VERIFY(timeEndPeriod(1) == TIMERR_NOERROR);

    return 0;
}

void PlaybackStreamWASAPI::set_underrun_callback(Function<void()> underrun_callback)
{
    m_state->underrun_callback = move(underrun_callback);
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> PlaybackStreamWASAPI::resume()
{
    auto promise = Core::ThreadedPromise<AK::Duration>::create();
    TaskPlay task = { .promise = promise };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    SetEvent(m_state->task_event);
    m_state->task_queue_mutex.unlock();

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TaskDrainAndSuspend task = { .promise = promise };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    SetEvent(m_state->task_event);
    m_state->task_queue_mutex.unlock();

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TaskDiscardAndSuspend task = { .promise = promise };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    SetEvent(m_state->task_event);
    m_state->task_queue_mutex.unlock();

    return promise;
}

AK::Duration PlaybackStreamWASAPI::total_time_played() const
{
    if (!s_com_uninitializer.initialized) [[unlikely]] {
        MUST_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        s_com_uninitializer.initialized = true;
    }

    return total_time_played_with_com_initialized(m_state);
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::set_volume(double volume)
{
    HRESULT hr;
    auto promise = Core::ThreadedPromise<void>::create();
    if (!s_com_uninitializer.initialized) [[unlikely]] {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) [[unlikely]] {
            promise->reject(Error::from_windows_error(hr));
            return promise;
        }
        s_com_uninitializer.initialized = true;
    }

    float clamped_volume = static_cast<float>(clamp(volume, 0.0, 1.0));

    m_state->channel_volumes.fill(clamped_volume);

    hr = m_state->audio_stream_volume->SetAllVolumes(m_state->channel_volumes.size(), m_state->channel_volumes.data());
    if (FAILED(hr)) [[unlikely]] {
        promise->reject(Error::from_windows_error(hr));
    } else [[likely]] {
        promise->resolve();
    }
    return promise;
}

}
