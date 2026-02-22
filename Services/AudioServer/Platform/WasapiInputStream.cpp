/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NonnullRefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AK/Windows.h>
#include <AudioServer/Debug.h>
#include <AudioServer/InputStream.h>
#include <AudioServer/Platform/Wasapi.h>
#include <AudioServer/Server.h>
#include <LibThreading/Thread.h>
#include <audioclient.h>

// NOTE: Not using the newer winrt that supersedes wrl as that uses exceptions for error handling
#include <wrl/client.h>

namespace AudioServer {

using namespace Microsoft::WRL;

// Must stay function-local in this TU since it assumes ErrorOr<void> return type.
#define TRY_HR(expression)                                                        \
    ({                                                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", HRESULT&& _temporary_hr = (expression)); \
        if (FAILED(_temporary_hr)) [[unlikely]]                                   \
            return Error::from_windows_error(_temporary_hr);                      \
    })

static ErrorOr<ComPtr<IMMDevice>> resolve_input_device(IMMDeviceEnumerator& enumerator, DeviceHandle backend_handle)
{
    if (backend_handle == 0) {
        ComPtr<IMMDevice> default_device;
        TRY_HR(enumerator.GetDefaultAudioEndpoint(eCapture, eConsole, &default_device));
        return default_device;
    }

    ComPtr<IMMDeviceCollection> collection;
    TRY_HR(enumerator.EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection));

    UINT device_count = 0;
    TRY_HR(collection->GetCount(&device_count));

    for (UINT index = 0; index < device_count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device)))
            continue;

        auto endpoint_id_or_error = endpoint_id_for_device(*device.Get());
        if (endpoint_id_or_error.is_error())
            continue;

        if (backend_handle_for_endpoint_id(endpoint_id_or_error.value()) == backend_handle)
            return device;
    }

    return Error::from_string_literal("Could not resolve input device for backend handle");
}

static DWORD channel_mask_for_count(u32 channel_count)
{
    switch (channel_count) {
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    default:
        return 0;
    }
}

class WasapiInputStream final : public InputStream {
public:
    static ErrorOr<NonnullRefPtr<InputStream>> create(DeviceHandle backend_handle, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
    {
        auto stream = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) WasapiInputStream()));
        TRY(stream->initialize_shared_ring_storage(sample_rate_hz, channel_count, capacity_frames));
        TRY(stream->initialize_stream(backend_handle, sample_rate_hz, channel_count));
        return stream;
    }

    ~WasapiInputStream() override
    {
        shutdown();
    }

private:
    WasapiInputStream() = default;

    ErrorOr<void> initialize_stream(DeviceHandle backend_handle, u32 sample_rate_hz, u32 channel_count)
    {
        m_com_initialization = TRY(ScopedComInitialization::create());

        TRY_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator)));

        auto device_or_error = resolve_input_device(*m_enumerator.Get(), backend_handle);
        if (device_or_error.is_error())
            return device_or_error.release_error();
        m_device = device_or_error.release_value();

        TRY_HR(m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_audio_client));

        DWORD channel_mask = channel_mask_for_count(channel_count);
        WAVEFORMATEX* mix_format = nullptr;
        HRESULT hr = m_audio_client->GetMixFormat(&mix_format);
        if (SUCCEEDED(hr) && mix_format != nullptr) {
            ScopeGuard free_mix_format = [&] {
                CoTaskMemFree(mix_format);
            };

            if (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_format->cbSize >= 22) {
                auto const& extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE const&>(*mix_format);
                if (extensible.Format.nChannels == channel_count && extensible.dwChannelMask != 0)
                    channel_mask = extensible.dwChannelMask;
            }
        }

        m_wave_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        m_wave_format.Format.nChannels = static_cast<WORD>(channel_count);
        m_wave_format.Format.nSamplesPerSec = sample_rate_hz;
        m_wave_format.Format.wBitsPerSample = 32;
        m_wave_format.Format.nBlockAlign = static_cast<WORD>(channel_count * sizeof(float));
        m_wave_format.Format.nAvgBytesPerSec = sample_rate_hz * m_wave_format.Format.nBlockAlign;
        m_wave_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        m_wave_format.Samples.wValidBitsPerSample = 32;
        m_wave_format.dwChannelMask = channel_mask;
        m_wave_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
        TRY_HR(m_audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, &m_wave_format.Format, nullptr));

        TRY_HR(m_audio_client->GetBufferSize(&m_buffer_frame_count));
        TRY_HR(m_audio_client->GetService(IID_PPV_ARGS(&m_capture_client)));

        m_buffer_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_buffer_event == nullptr)
            return Error::from_windows_error(GetLastError());

        TRY_HR(m_audio_client->SetEventHandle(m_buffer_event));
        TRY_HR(m_audio_client->Start());

        m_capture_thread = Threading::Thread::construct("Audio Capture"sv, [this] {
            return capture_thread_loop();
        });
        m_capture_thread->start();

        return {};
    }

    int capture_thread_loop()
    {
        auto com_or_error = ScopedComInitialization::create();
        if (com_or_error.is_error())
            return 0;

        u32 channels = channel_count();
        while (!m_exit_requested.load(AK::MemoryOrder::memory_order_acquire)) {
            DWORD wait_result = WaitForSingleObject(m_buffer_event, INFINITE);
            if (wait_result != WAIT_OBJECT_0)
                continue;

            if (m_exit_requested.load(AK::MemoryOrder::memory_order_acquire))
                break;

            UINT32 packet_frames = 0;
            HRESULT hr = m_capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr))
                continue;

            while (packet_frames > 0) {
                BYTE* data = nullptr;
                UINT32 frames_to_read = 0;
                DWORD flags = 0;

                hr = m_capture_client->GetBuffer(&data, &frames_to_read, &flags, nullptr, nullptr);
                if (FAILED(hr))
                    break;

                if (frames_to_read > 0) {
                    size_t samples_to_read = static_cast<size_t>(frames_to_read) * channels;

                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                        if (m_scratch_samples.size() < samples_to_read)
                            m_scratch_samples.resize(samples_to_read);
                        m_scratch_samples.span().slice(0, samples_to_read).fill(0.0f);
                        (void)try_push_interleaved(m_scratch_samples.span().slice(0, samples_to_read), channels);
                    } else {
                        ReadonlySpan<float> interleaved_samples { reinterpret_cast<float const*>(data), samples_to_read };
                        (void)try_push_interleaved(interleaved_samples, channels);
                    }
                }

                (void)m_capture_client->ReleaseBuffer(frames_to_read);

                hr = m_capture_client->GetNextPacketSize(&packet_frames);
                if (FAILED(hr))
                    break;
            }
        }

        return 0;
    }

    void shutdown()
    {
        m_exit_requested.store(true, AK::MemoryOrder::memory_order_release);

        if (m_buffer_event != nullptr)
            SetEvent(m_buffer_event);

        if (m_capture_thread && m_capture_thread->needs_to_be_joined())
            (void)m_capture_thread->join();
        m_capture_thread = nullptr;

        if (m_audio_client)
            (void)m_audio_client->Stop();

        if (m_buffer_event != nullptr) {
            CloseHandle(m_buffer_event);
            m_buffer_event = nullptr;
        }

        m_capture_client.Reset();
        m_audio_client.Reset();
        m_device.Reset();
        m_enumerator.Reset();
        m_com_initialization.clear();
    }

    Optional<ScopedComInitialization> m_com_initialization;
    ComPtr<IMMDeviceEnumerator> m_enumerator;
    ComPtr<IMMDevice> m_device;
    ComPtr<IAudioClient> m_audio_client;
    ComPtr<IAudioCaptureClient> m_capture_client;

    WAVEFORMATEXTENSIBLE m_wave_format {};
    UINT32 m_buffer_frame_count { 0 };
    HANDLE m_buffer_event { nullptr };

    Atomic<bool> m_exit_requested { false };
    RefPtr<Threading::Thread> m_capture_thread;
    Vector<float> m_scratch_samples;
};

ErrorOr<NonnullRefPtr<InputStream>> create_platform_input_stream(DeviceHandle device_handle, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
{
    DeviceHandle backend_handle = (device_handle == 0) ? 0 : static_cast<DeviceHandle>(Server::device_handle_to_os_device_id(device_handle));
    return WasapiInputStream::create(backend_handle, sample_rate_hz, channel_count, capacity_frames);
}

}
