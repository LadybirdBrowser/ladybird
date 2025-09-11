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
#include <LibMedia/Audio/SampleFormats.h>
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

struct Task {
    enum class Kind : u8 {
        Play,
        DrainAndSuspend,
        DiscardAndSuspend,
    };

    void resolve(Optional<AK::Duration> time)
    {
        promise.visit(
            [](Empty) { VERIFY_NOT_REACHED(); },
            [&](NonnullRefPtr<Core::ThreadedPromise<void>>& promise) {
                promise->resolve();
            },
            [&](NonnullRefPtr<Core::ThreadedPromise<AK::Duration>>& promise) {
                promise->resolve(time.release_value());
            });
    }
    void reject(Error& error)
    {
        promise.visit(
            [](Empty) { VERIFY_NOT_REACHED(); },
            [&error](auto& promise) {
                promise->reject(move(error));
            });
    }
    Variant<Empty, NonnullRefPtr<Core::ThreadedPromise<void>>, NonnullRefPtr<Core::ThreadedPromise<AK::Duration>>> promise;
    Kind kind;
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

    NonnullOwnPtr<WAVEFORMATEXTENSIBLE> wave_format;
    UINT32 buffer_frame_count;
    HANDLE buffer_event;

    PlaybackStreamWASAPI::AudioDataRequestCallback data_request_callback;
    Function<void()> underrun_callback;

    Threading::Mutex task_queue_mutex;
    Queue<Task> task_queue;
    // FIXME: Create a owning handle type to be shared in the codebase
    HANDLE task_event;

    bool playing = false;
    bool drain_and_suspend = false;
    Atomic<bool> exit_requested = false;

    static int render_thread_loop(AudioState& state);
    RefPtr<Threading::Thread> audio_thread;
    RefPtr<Core::ThreadedPromise<AK::Duration>> resume_promise;
    RefPtr<Core::ThreadedPromise<void>> suspend_promise;

    // I doubt we will have anyone with more than 7.1
    Vector<float, 8> channel_volumes;
    UINT64 audio_client_clock_frequency;
};

PlaybackStreamWASAPI::AudioState::AudioState()
    : wave_format(make<WAVEFORMATEXTENSIBLE>())
{
    task_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    VERIFY(task_event);
}

PlaybackStreamWASAPI::AudioState::~AudioState()
{
    CloseHandle(buffer_event);
}

ALWAYS_INLINE AK::Duration PlaybackStreamWASAPI::total_time_played_with_com_initialized(PlaybackStreamWASAPI::AudioState& state)
{
    UINT64 position;
    MUST_HR(state.clock->GetPosition(&position, nullptr));
    // FIXME: This is a narrowing conversion. We might want a u64 overload of from_nanoseconds.
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

static void print_audio_format(WAVEFORMATEXTENSIBLE* format)
{
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
        format->Format.wFormatTag,
        format->Format.nChannels,
        format->Format.nSamplesPerSec,
        format->Format.nAvgBytesPerSec,
        format->Format.nBlockAlign,
        format->Format.wBitsPerSample,
        format->Format.cbSize,
        format->Samples.wValidBitsPerSample,
        format->dwChannelMask,
        Span<u8> { reinterpret_cast<char*>(&format->SubFormat), 16 });
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

    dbgln("Mixing engine audio format:\n");
    print_audio_format(device_format);

    Vector<Channel, 32> channel_vec;
    VERIFY(device_format->Format.nChannels <= 32);
    VERIFY(device_format->dwChannelMask != 0);
    auto channels = device_format->Format.nChannels;

    // Conveniently the enum is in the same order. The difference is that KSMedia uses a bitmask instaead
    for (u64 i = 0; i < channels; i++) {
        u32 mask = 1 << i;
        if (device_format->dwChannelMask & mask)
            // i + 1 as the first bit is at i == 0
            channel_vec.append(static_cast<Channel>(i + 1));
    }
    ChannelMap channel_map { channel_vec };

    auto sample_specification = SampleSpecification { device_format->Format.nSamplesPerSec, channel_map };
    sample_specification_callback(sample_specification);

    // Set up whatever we were given.

    state->wave_format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    state->wave_format->Format.nChannels = device_format->Format.nChannels;
    state->wave_format->Format.nSamplesPerSec = device_format->Format.nSamplesPerSec;
    state->wave_format->Format.nAvgBytesPerSec = device_format->Format.nAvgBytesPerSec;
    state->wave_format->Format.nBlockAlign = device_format->Format.nBlockAlign;
    state->wave_format->Format.wBitsPerSample = device_format->Format.wBitsPerSample;
    state->wave_format->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    state->wave_format->Samples.wValidBitsPerSample = device_format->Samples.wValidBitsPerSample;
    state->wave_format->dwChannelMask = device_format->dwChannelMask;
    state->wave_format->SubFormat = device_format->SubFormat;

    // FIXME: From what I understand the audio mixer uses 32bit floats internally. If that is not always the case we need to handle it.
    VERIFY(device_format->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    VERIFY(device_format->Format.wBitsPerSample == 32);

    state->channel_volumes.resize(channels);

    WAVEFORMATEXTENSIBLE* closest_match;
    hr = state->audio_client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &state->wave_format->Format, reinterpret_cast<WAVEFORMATEX**>(&closest_match));
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    if (hr == S_FALSE) {
        dbgln("Audio format not supported. Current format:\n");
        print_audio_format(state->wave_format.ptr());
        dbgln("Closest supported audio format:\n");
        print_audio_format(closest_match);
        CoTaskMemFree(closest_match);
        VERIFY_NOT_REACHED();
    }
    // TODO: check the actual format of the engine and use it if possible to reduce overhead
    DWORD stream_flags
        = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
    // For event driven buffering we can't specify the buffer duration.
    TRY_HR(state->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, &state->wave_format->Format, &PlaybackSessionGUID));
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

    state->audio_thread = Threading::Thread::construct([state] {
        return AudioState::render_thread_loop(*state);
    });

    if (initial_output_state == OutputState::Playing)
        TRY_HR(state->audio_client->Start());

    state->audio_thread->start();
    state->audio_thread->detach();

    return TRY(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackStreamWASAPI(move(state))));
}

int PlaybackStreamWASAPI::AudioState::render_thread_loop(PlaybackStreamWASAPI::AudioState& state)
{
    MUST_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    WORD block_align = state.wave_format->Format.nBlockAlign;

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
            auto task = state.task_queue.dequeue();
            state.task_queue_mutex.unlock();
            using enum Task::Kind;
            switch (task.kind) {
            case Play:
                MUST_HR(state.audio_client->Start());
                task.resolve(total_time_played_with_com_initialized(state));
                state.playing = true;
                break;
            case DrainAndSuspend: {
                u32 padding;
                MUST_HR(state.audio_client->GetCurrentPadding(&padding));

                u32 ms_to_sleep = padding * 1'000ull / state.wave_format->Format.nSamplesPerSec;
                // u32 us_to_sleep = padding * 1'000'000ull / state.wave_format->Format.nSamplesPerSec;
                if (ms_to_sleep > 0) {
                    // MUST(Core::System::sleep_us(us_to_sleep));
                    Sleep(ms_to_sleep - 1);
                    MUST_HR(state.audio_client->GetCurrentPadding(&padding));
                }
                if (padding == 0)
                    dbgln("------- We overslept --------");
                while (padding > 0) {
                    _mm_pause();
                    MUST_HR(state.audio_client->GetCurrentPadding(&padding));
                }

                MUST_HR(state.audio_client->Stop());
                state.playing = false;
                task.resolve({});
                continue;
            }
            case DiscardAndSuspend:
                MUST_HR(state.audio_client->Stop());
                MUST_HR(state.audio_client->Reset());
                state.playing = false;
                task.resolve({});
                continue;
            default:
                VERIFY_NOT_REACHED();
            }
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
        auto bytes_written = state.data_request_callback(output_buffer);
        if (bytes_written.is_empty()) [[unlikely]] {
            if (state.underrun_callback)
                state.underrun_callback();
            buffer_flags |= AUDCLNT_BUFFERFLAGS_SILENT;
        }
        // TODO: Have some heuristic on how many bytes need to be in the buffer before silence needs to be added to the end.
        // If there was just one byte written we would quickly run out.
        frames_available = bytes_written.size() / block_align;
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
    Task task = { .promise = promise, .kind = Task::Kind::Play };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    m_state->task_queue_mutex.unlock();
    SetEvent(m_state->task_event);

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    Task task = { .promise = promise, .kind = Task::Kind::DrainAndSuspend };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    m_state->task_queue_mutex.unlock();
    SetEvent(m_state->task_event);

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    Task task = { .promise = promise, .kind = Task::Kind::DiscardAndSuspend };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    m_state->task_queue_mutex.unlock();
    SetEvent(m_state->task_event);

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
