/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibMedia/Audio/SampleFormats.h"
#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/ThreadedPromise.h>
#include <LibMedia/Audio/PlaybackStreamWasapi.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

#include <AK/Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

// NOTE: Not using the newer winrt that supersedes wrl as that uses exceptions for error handling
#include <wrl/client.h>

namespace Audio {

using namespace Microsoft::WRL;

// GUID for the playback session. That way all render streams have a single volume slider in the OS interface
constexpr GUID PlaybackSessionGUID = { /* 22f2ca89-210a-492c-a0aa-f25b1d2f33a1 */
    0x22f2ca89,
    0x210a,
    0x492c,
    { 0xa0, 0xaa, 0xf2, 0x5b, 0x1d, 0x2f, 0x33, 0xa1 }
};

struct AudioState : public RefCounted<AudioState> {
    AudioState();
    ~AudioState();

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audio_client;
    ComPtr<IAudioRenderClient> render_client;
    ComPtr<IAudioStreamVolume> audio_stream_volume;
    ComPtr<IAudioClock> clock;

    NonnullOwnPtr<WAVEFORMATEX> wave_format;
    UINT32 buffer_frame_count;
    HANDLE buffer_event;

    PlaybackStreamWasapi::AudioDataRequestCallback data_request_callback;
    Function<void()> underrun_callback;

    Threading::Mutex mutex;
    Atomic<bool> playing = false;
    Atomic<bool> drain_and_suspend = false;
    Atomic<bool> exit_requested = false;

    static int render_thread_loop(void* context);
    RefPtr<Threading::Thread> audio_thread;
    RefPtr<Core::ThreadedPromise<AK::Duration>> resume_promise;
    RefPtr<Core::ThreadedPromise<void>> suspend_promise;
};

AudioState::AudioState()
    : wave_format(make<WAVEFORMATEX>())
{
}

AudioState::~AudioState()
{
    CloseHandle(buffer_event);
}

PlaybackStreamWasapi::PlaybackStreamWasapi(NonnullRefPtr<AudioState> state)
    : m_state(move(state))
{
}

PlaybackStreamWasapi::~PlaybackStreamWasapi()
{
    m_state->exit_requested.store(true, MemoryOrder::memory_order_release);
    // Poke the event to wake the thread up from wait
    if (m_state->buffer_event)
        SetEvent(m_state->buffer_event);

    MUST(m_state->audio_thread->join());
}

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStream::create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32 target_latency_ms, AudioDataRequestCallback&& callback)
{
    return PlaybackStreamWasapi::create(initial_output_state, sample_rate, channels, target_latency_ms, move(callback));
}

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStreamWasapi::create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32, AudioDataRequestCallback&& data_request_callback)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    ScopeGuard uninitizalize_com = []() { CoUninitialize(); };
    auto state = make_ref_counted<AudioState>();

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&state->enumerator));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    hr = state->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &state->device);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    hr = state->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &state->audio_client);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    state->data_request_callback = move(data_request_callback);

    // Set up 32bit float PCM
    u16 bits_per_sample = 32;
    u16 block_align = (channels * sample_rate) / 8;
    state->wave_format->wFormatTag = WAVE_FORMAT_PCM;
    state->wave_format->nChannels = channels;
    state->wave_format->nSamplesPerSec = sample_rate;
    state->wave_format->nAvgBytesPerSec = sample_rate * block_align;
    state->wave_format->nBlockAlign = block_align;
    state->wave_format->wBitsPerSample = bits_per_sample;
    state->wave_format->cbSize = 0;

    // We make the audio engine convert the format to what it needs
    // TODO: check the actual format of the engine and use it if possible to reduce overhead
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
    // For event driven buffering we can't specify the buffer duration.
    hr = state->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, state->wave_format, &PlaybackSessionGUID);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    hr = state->audio_client->GetBufferSize(&state->buffer_frame_count);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    hr = state->audio_client->GetService(IID_PPV_ARGS(&state->render_client));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    hr = state->audio_client->GetService(IID_PPV_ARGS(&state->audio_stream_volume));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    hr = state->audio_client->GetService(IID_PPV_ARGS(&state->clock));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    state->buffer_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!state->buffer_event)
        return Error::from_windows_error(hr);

    hr = state->audio_client->SetEventHandle(state->buffer_event);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    state->audio_thread = Threading::Thread::construct([&state] { return AudioState::render_thread_loop(state.ptr()); });
    state->audio_thread->start();

    if (initial_output_state == OutputState::Playing) {
        state->playing.store(true, MemoryOrder::memory_order_relaxed);
        hr = state->audio_client->Start();
        if (FAILED(hr))
            return Error::from_windows_error(hr);
    }

    return TRY(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackStreamWasapi(move(state))));
}

int AudioState::render_thread_loop(void* context)
{
    auto* state = static_cast<AudioState*>(context);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return 1;
    ScopeGuard uninitialize_com = [] { CoUninitialize(); };

    DWORD task_index = 0;
    HANDLE task_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    while (!state->exit_requested.load(MemoryOrder::memory_order_acquire)) {
        WaitForSingleObject(state->buffer_event, INFINITE);
        // Verify the memory order here
        if (state->exit_requested.load(MemoryOrder::memory_order_relaxed))
            break;
        if (!state->playing.load(MemoryOrder::memory_order_acquire))
            continue;

        u32 padding = 0;
        hr = state->audio_client->GetCurrentPadding(&padding);
        // Shoud I be quitting here?
        if (FAILED(hr)) {
            dbgln("ReleaseBuffer failed with: {}", Error::from_windows_error(hr));
            VERIFY_NOT_REACHED();
        }
        u32 frames_available = state->buffer_frame_count - padding;

        BYTE* buffer;
        hr = state->render_client->GetBuffer(frames_available, &buffer);
        if (FAILED(hr)) {
            dbgln("ReleaseBuffer failed with: {}", Error::from_windows_error(hr));
            VERIFY_NOT_REACHED();
        }

        u32 buffer_size = frames_available * state->wave_format->nBlockAlign;
        ReadonlyBytes bytes_written = state->data_request_callback({ buffer, buffer_size }, PcmSampleFormat::Float32, frames_available);
        if (bytes_written.is_empty()) {
            if (state->underrun_callback)
                state->underrun_callback();
            // TODO: Handle this
        }
        // TODO: Handle writing silence using flag
        hr = state->render_client->ReleaseBuffer(frames_available, 0);
        if (FAILED(hr)) {
            dbgln("ReleaseBuffer failed with: {}", Error::from_windows_error(hr));
            VERIFY_NOT_REACHED();
        }
    }

    if (task_handle)
        AvRevertMmThreadCharacteristics(task_handle);

    return 0;
}

void PlaybackStreamWasapi::set_underrun_callback(Function<void()> underrun_callback)
{
    m_state->underrun_callback = move(underrun_callback);
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> PlaybackStreamWasapi::resume()
{
    // should this be using a separate promise or not?
    m_state->resume_promise = Core::ThreadedPromise<AK::Duration>::create();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        m_state->resume_promise->reject(Error::from_windows_error(hr));
        return m_state->resume_promise.release_nonnull();
    }
    Threading::MutexLocker locker(m_state->mutex);
    if (m_state->playing.load(MemoryOrder::memory_order_relaxed)) {
        m_state->resume_promise->resolve(MUST(total_time_played()));
        return m_state->resume_promise.release_nonnull();
    }

    m_state->playing.store(true, MemoryOrder::memory_order_relaxed);
    m_state->drain_and_suspend.store(false, MemoryOrder::memory_order_relaxed);
    hr = m_state->audio_client->Start();
    if (FAILED(hr)) {
        m_state->resume_promise->reject(Error::from_windows_error(hr));
        m_state->playing.store(false, MemoryOrder::memory_order_relaxed);
        return m_state->resume_promise.release_nonnull();
    }

    // TODO: check if this is close enough
    m_state->resume_promise->resolve(MUST(total_time_played()));
    return m_state->resume_promise.release_nonnull();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWasapi::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        promise->reject(Error::from_windows_error(hr));
        return promise;
    }
    Threading::MutexLocker locker(m_state->mutex);
    if (m_state->playing.load(MemoryOrder::memory_order_relaxed)) {
        m_state->drain_and_suspend.store(true, MemoryOrder::memory_order_relaxed);
    } else {
        m_state->audio_client->Reset();
        m_state->suspend_promise->resolve();
    }
    return *m_state->suspend_promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWasapi::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        promise->reject(Error::from_windows_error(hr));
        return promise;
    }
    ScopeGuard uninitialize_com = [] { CoUninitialize(); };
    Threading::MutexLocker locker(m_state->mutex);
    if (m_state->playing.load(MemoryOrder::memory_order_relaxed)) {
        m_state->playing.store(false, MemoryOrder::memory_order_relaxed);
        m_state->audio_client->Stop();
    }

    m_state->audio_client->Reset();
    promise->resolve();
    return promise;
}

ErrorOr<AK::Duration> PlaybackStreamWasapi::total_time_played()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return Error::from_windows_error();
    ScopeGuard uninitialize_com = [] { CoUninitialize(); };

    UINT64 frequency;
    UINT64 position;
    hr = m_state->clock->GetFrequency(&frequency);
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    hr = m_state->clock->GetPosition(&position, NULL);
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    // This is a narrowing conversion. Not much can be done though.
    return AK::Duration::from_seconds(position / frequency);
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWasapi::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        promise->reject(Error::from_windows_error(hr));
        return promise;
    }
    ScopeGuard uninitialize_com = [] { CoUninitialize(); };

    float clamped_volume = static_cast<float>(clamp(volume, 0.0, 1.0));
    auto channels = m_state->wave_format->nChannels;
    Vector<float> volumes;
    volumes.resize(channels);
    for (u64 i = 0; i < channels; i++)
        volumes[i] = clamped_volume;

    hr = m_state->audio_stream_volume->SetAllVolumes(channels, volumes.data());
    if (FAILED(hr)) {
        promise->reject(Error::from_windows_error(hr));
    } else {
        promise->resolve();
    }
    return promise;
}

}
