/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <AudioServer/Debug.h>
#include <AudioServer/Platform/Wasapi.h>
#include <AudioServer/Server.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

// NOTE: Not using the newer winrt that supersedes wrl as that uses exceptions for error handling
#include <wrl/client.h>

namespace AudioServer {

using Audio::Channel;
using Audio::ChannelMap;
using namespace Microsoft::WRL;

ErrorOr<ScopedComInitialization> ScopedComInitialization::create()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    return ScopedComInitialization { true };
}

ScopedComInitialization::ScopedComInitialization(ScopedComInitialization&& other)
    : m_initialized(exchange(other.m_initialized, false))
{
}

ScopedComInitialization& ScopedComInitialization::operator=(ScopedComInitialization&& other)
{
    if (this == &other)
        return *this;

    if (m_initialized)
        CoUninitialize();

    m_initialized = exchange(other.m_initialized, false);
    return *this;
}

ScopedComInitialization::~ScopedComInitialization()
{
    if (m_initialized)
        CoUninitialize();
}

ScopedComInitialization::ScopedComInitialization(bool initialized)
    : m_initialized(initialized)
{
}

ByteString wide_string_to_utf8(wchar_t const* wide_string)
{
    if (wide_string == nullptr || wide_string[0] == L'\0')
        return {};

    int utf8_bytes = WideCharToMultiByte(CP_UTF8, 0, wide_string, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_bytes <= 1)
        return {};

    auto buffer_or_error = ByteBuffer::create_uninitialized(static_cast<size_t>(utf8_bytes));
    if (buffer_or_error.is_error())
        return {};

    ByteBuffer buffer = buffer_or_error.release_value();
    int bytes_written = WideCharToMultiByte(CP_UTF8, 0, wide_string, -1, reinterpret_cast<char*>(buffer.data()), utf8_bytes, nullptr, nullptr);
    if (bytes_written <= 0)
        return {};

    return ByteString(reinterpret_cast<char const*>(buffer.data()));
}

ErrorOr<ByteString> endpoint_id_for_device(IMMDevice& device)
{
    LPWSTR endpoint_id = nullptr;
    HRESULT hr = device.GetId(&endpoint_id);
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    ScopeGuard free_endpoint_id = [&] {
        CoTaskMemFree(endpoint_id);
    };

    ByteString id = wide_string_to_utf8(endpoint_id);
    if (id.is_empty())
        return Error::from_string_literal("WASAPI endpoint id was empty");
    return id;
}

u64 backend_handle_for_endpoint_id(ByteString const& endpoint_id)
{
    if (endpoint_id.is_empty())
        return 0;

    u32 endpoint_hash = string_hash(endpoint_id.characters(), endpoint_id.length());
    return (static_cast<u64>(endpoint_hash) << 32u) | static_cast<u64>(endpoint_id.length());
}

ErrorOr<Audio::ChannelMap> convert_ksmedia_channel_bitmask_to_channel_map(u32 channel_bitmask)
{
    struct BitToChannel {
        u32 bit;
        Channel channel;
    };

    constexpr Array<BitToChannel, 18> bit_to_channel_map { {
        { SPEAKER_FRONT_LEFT, Channel::FrontLeft },
        { SPEAKER_FRONT_RIGHT, Channel::FrontRight },
        { SPEAKER_FRONT_CENTER, Channel::FrontCenter },
        { SPEAKER_LOW_FREQUENCY, Channel::LowFrequency },
        { SPEAKER_BACK_LEFT, Channel::BackLeft },
        { SPEAKER_BACK_RIGHT, Channel::BackRight },
        { SPEAKER_FRONT_LEFT_OF_CENTER, Channel::FrontLeftOfCenter },
        { SPEAKER_FRONT_RIGHT_OF_CENTER, Channel::FrontRightOfCenter },
        { SPEAKER_BACK_CENTER, Channel::BackCenter },
        { SPEAKER_SIDE_LEFT, Channel::SideLeft },
        { SPEAKER_SIDE_RIGHT, Channel::SideRight },
        { SPEAKER_TOP_CENTER, Channel::TopCenter },
        { SPEAKER_TOP_FRONT_LEFT, Channel::TopFrontLeft },
        { SPEAKER_TOP_FRONT_CENTER, Channel::TopFrontCenter },
        { SPEAKER_TOP_FRONT_RIGHT, Channel::TopFrontRight },
        { SPEAKER_TOP_BACK_LEFT, Channel::TopBackLeft },
        { SPEAKER_TOP_BACK_CENTER, Channel::TopBackCenter },
        { SPEAKER_TOP_BACK_RIGHT, Channel::TopBackRight },
    } };

    Vector<Channel, ChannelMap::capacity()> channels;

    for (BitToChannel const& mapping : bit_to_channel_map) {
        if ((channel_bitmask & mapping.bit) == 0)
            continue;

        if (channels.size() == ChannelMap::capacity())
            return Error::from_string_literal("Device channel layout had too many channels");

        channels.unchecked_append(mapping.channel);
    }

    if ((channel_bitmask & SPEAKER_RESERVED) != 0)
        return Error::from_string_literal("Unsupported new KSMEDIA version");

    return ChannelMap { channels };
}

static ChannelMap create_unknown_channel_layout(u32 channel_count)
{
    if (channel_count > ChannelMap::capacity())
        return ChannelMap::invalid();

    Vector<Channel, ChannelMap::capacity()> channel_layout;
    channel_layout.resize(channel_count);

    for (Channel& channel : channel_layout)
        channel = Channel::Unknown;

    return ChannelMap(channel_layout);
}

static ByteString friendly_name_for_device(IMMDevice& device)
{
    ComPtr<IPropertyStore> property_store;
    HRESULT hr = device.OpenPropertyStore(STGM_READ, &property_store);
    if (FAILED(hr))
        return {};

    PROPVARIANT value;
    PropVariantInit(&value);
    ScopeGuard clear_property_variant = [&] {
        PropVariantClear(&value);
    };

    hr = property_store->GetValue(PKEY_Device_FriendlyName, &value);
    if (FAILED(hr) || value.vt != VT_LPWSTR || value.pwszVal == nullptr)
        return {};

    return wide_string_to_utf8(value.pwszVal);
}

static Optional<ByteString> default_endpoint_id(IMMDeviceEnumerator& enumerator, EDataFlow flow)
{
    ComPtr<IMMDevice> default_device;
    HRESULT hr = enumerator.GetDefaultAudioEndpoint(flow, eConsole, &default_device);
    if (FAILED(hr))
        return {};

    auto endpoint_id = endpoint_id_for_device(*default_device.Get());
    if (endpoint_id.is_error())
        return {};

    return endpoint_id.release_value();
}

static ChannelMap channel_map_for_wave_format(WAVEFORMATEX const& format, u32 channel_count)
{
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        auto const& extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE const&>(format);
        if (extensible.dwChannelMask != 0) {
            auto channel_map_or_error = convert_ksmedia_channel_bitmask_to_channel_map(extensible.dwChannelMask);
            if (!channel_map_or_error.is_error()) {
                ChannelMap map = channel_map_or_error.release_value();
                if (map.channel_count() == channel_count)
                    return map;
            }
        }
    }

    return create_unknown_channel_layout(channel_count);
}

class DeviceNotificationClient final : public IMMNotificationClient {
public:
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr)
            return E_POINTER;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    virtual ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_ref_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) + 1;
    }

    virtual ULONG STDMETHODCALLTYPE Release() override
    {
        return m_ref_count.fetch_sub(1, AK::MemoryOrder::memory_order_relaxed) - 1;
    }

    virtual HRESULT STDMETHODCALLTYPE OnDeviceStateChanged([[maybe_unused]] LPCWSTR device_id, [[maybe_unused]] DWORD new_state) override
    {
        Server::the().update_devices();
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE OnDeviceAdded([[maybe_unused]] LPCWSTR device_id) override
    {
        Server::the().update_devices();
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE OnDeviceRemoved([[maybe_unused]] LPCWSTR device_id) override
    {
        Server::the().update_devices();
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged([[maybe_unused]] EDataFlow flow, [[maybe_unused]] ERole role, [[maybe_unused]] LPCWSTR default_device_id) override
    {
        Server::the().update_devices();
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE OnPropertyValueChanged([[maybe_unused]] LPCWSTR device_id, [[maybe_unused]] PROPERTYKEY key) override
    {
        Server::the().update_devices();
        return S_OK;
    }

private:
    Atomic<ULONG> m_ref_count { 1 };
};

static void ensure_wasapi_device_change_notifications_registered()
{
    static bool attempted_registration = false;
    static bool registered = false;
    static ComPtr<IMMDeviceEnumerator> notification_enumerator;
    static DeviceNotificationClient notification_client;

    if (attempted_registration)
        return;
    attempted_registration = true;

    auto com_or_error = ScopedComInitialization::create();
    if (com_or_error.is_error()) {
        warnln("WASAPI device notifications: COM initialization failed: {}", com_or_error.error());
        return;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&notification_enumerator));
    if (FAILED(hr)) {
        warnln("WASAPI device notifications: failed to create device enumerator: {}", Error::from_windows_error(hr));
        return;
    }

    hr = notification_enumerator->RegisterEndpointNotificationCallback(&notification_client);
    if (FAILED(hr)) {
        warnln("WASAPI device notifications: register callback failed: {}", Error::from_windows_error(hr));
        return;
    }

    registered = true;
    if (should_log_audio_server() && registered)
        dbgln("WASAPI device notifications: registered endpoint notification callback");
}

Vector<DeviceInfo> Server::enumerate_platform_devices()
{
    ensure_wasapi_device_change_notifications_registered();

    Vector<DeviceInfo> devices;

    auto com_or_error = ScopedComInitialization::create();
    if (com_or_error.is_error())
        return devices;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
        return devices;

    Optional<ByteString> default_output_id = default_endpoint_id(*enumerator.Get(), eRender);
    Optional<ByteString> default_input_id = default_endpoint_id(*enumerator.Get(), eCapture);

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr))
        return devices;

    UINT device_count = 0;
    hr = collection->GetCount(&device_count);
    if (FAILED(hr))
        return devices;

    devices.ensure_capacity(device_count);

    for (UINT index = 0; index < device_count; ++index) {
        ComPtr<IMMDevice> device;
        hr = collection->Item(index, &device);
        if (FAILED(hr))
            continue;

        auto endpoint_id_or_error = endpoint_id_for_device(*device.Get());
        if (endpoint_id_or_error.is_error())
            continue;
        ByteString endpoint_id = endpoint_id_or_error.release_value();

        ComPtr<IMMEndpoint> endpoint;
        hr = device.As(&endpoint);
        if (FAILED(hr))
            continue;

        EDataFlow flow = eAll;
        hr = endpoint->GetDataFlow(&flow);
        if (FAILED(hr))
            continue;
        if (flow != eRender && flow != eCapture)
            continue;

        ComPtr<IAudioClient> audio_client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio_client);
        if (FAILED(hr))
            continue;

        WAVEFORMATEX* mix_format = nullptr;
        hr = audio_client->GetMixFormat(&mix_format);
        if (FAILED(hr) || mix_format == nullptr)
            continue;
        ScopeGuard free_mix_format = [&] {
            CoTaskMemFree(mix_format);
        };

        u32 sample_rate_hz = mix_format->nSamplesPerSec;
        u32 channel_count = mix_format->nChannels;
        if (sample_rate_hz == 0 || channel_count == 0)
            continue;

        DeviceInfo::Type device_type = flow == eRender ? DeviceInfo::Type::Output : DeviceInfo::Type::Input;
        bool is_default = false;
        if (flow == eRender)
            is_default = default_output_id.has_value() && endpoint_id == default_output_id.value();
        else
            is_default = default_input_id.has_value() && endpoint_id == default_input_id.value();

        StringView kind = (device_type == DeviceInfo::Type::Output) ? "audiooutput"sv : "audioinput"sv;
        ByteString label = friendly_name_for_device(*device.Get());
        if (label.is_empty())
            label = endpoint_id;

        u64 backend_handle = backend_handle_for_endpoint_id(endpoint_id);
        devices.append(DeviceInfo {
            .type = device_type,
            .device_handle = Server::make_device_handle(backend_handle, device_type),
            .label = label,
            .dom_device_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id(kind, endpoint_id, backend_handle),
            .group_id = is_default ? ByteString("default"sv) : Server::the().generate_dom_device_id("group"sv, endpoint_id, backend_handle),
            .sample_rate_hz = sample_rate_hz,
            .channel_count = channel_count,
            .channel_layout = channel_map_for_wave_format(*mix_format, channel_count),
            .is_default = is_default,
        });

        if (should_log_audio_server())
            dbgln("WASAPI enumerate {}: label='{}', channels={}, sample_rate={}, default={}", kind, label, channel_count, sample_rate_hz, is_default);
    }

    return devices;
}

}
