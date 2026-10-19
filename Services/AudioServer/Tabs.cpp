/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/NeverDestroyed.h>
#include <AudioServer/ControlConnection.h>
#include <AudioServer/PlatformAudio.h>
#include <AudioServer/Tabs.h>
#include <LibAudio/NullPlaybackStream.h>
#include <LibAudio/PlaybackStream.h>
#include <LibAudio/RecordStream.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>

namespace AudioServer {

// The device never underruns, since the mix is silence where no client has frames, so its own buffer can stay well
// below the lookahead the clients keep in their rings.
static constexpr u32 DEVICE_TARGET_LATENCY_MS = 40;

Tabs& Tabs::the()
{
    static NeverDestroyed<Tabs> instance;
    return *instance;
}

void Tabs::set_control_connection(ControlConnection& connection)
{
    m_control_connection = connection.make_weak_ptr<ControlConnection>();
}

// The null fallback needs the callback the failed platform stream was given, and the mixer's callback is not copyable.
struct SharedDataRequestCallback : public AtomicRefCounted<SharedDataRequestCallback> {
    explicit SharedDataRequestCallback(Audio::PlaybackStream::AudioDataRequestCallback callback)
        : callback(move(callback))
    {
    }
    Audio::PlaybackStream::AudioDataRequestCallback callback;
};

static NonnullRefPtr<Audio::PlaybackStream::CreatePromise> create_device_stream(Audio::AudioOutput audio_output, Audio::OutputState state, u32 target_latency_ms, Audio::PlaybackStream::AudioDataRequestCallback callback)
{
    if (audio_output == Audio::AudioOutput::Null) {
        auto promise = Audio::PlaybackStream::CreatePromise::construct();
        promise->resolve(Audio::NullPlaybackStream::create(state, target_latency_ms, move(callback)));
        return promise;
    }

    auto shared_callback = make_ref_counted<SharedDataRequestCallback>(move(callback));
    auto make_data_request_callback = [shared_callback] {
        return [shared_callback](Span<float> buffer, MonotonicTime buffer_starts_playing_at) {
            return shared_callback->callback(buffer, buffer_starts_playing_at);
        };
    };

    // A device that cannot be opened plays into nothing, so the clients' clocks run and the tab keeps working.
    auto promise = Audio::PlaybackStream::CreatePromise::construct();
    auto platform_promise = Audio::create_platform_playback_stream(state, target_latency_ms, make_data_request_callback());
    platform_promise->when_resolved([promise](NonnullRefPtr<Audio::PlaybackStream>& stream) {
        promise->resolve(stream);
    });
    platform_promise->when_rejected([promise, make_data_request_callback, state, target_latency_ms](Error& error) {
        warnln("Failed to open the audio output: {}; playing into nothing", error);
        promise->resolve(Audio::NullPlaybackStream::create(state, target_latency_ms, make_data_request_callback()));
    });
    return promise;
}

Tabs::Tab& Tabs::tab_for(u64 tab_id)
{
    return m_tabs.ensure(tab_id, [this, tab_id] {
        auto mixer = Audio::PlaybackStreamMixer::create(Core::EventLoop::current(), DEVICE_TARGET_LATENCY_MS, [audio_output = m_audio_output](Audio::OutputState state, u32 target_latency_ms, Audio::PlaybackStream::AudioDataRequestCallback callback) {
            return create_device_stream(audio_output, state, target_latency_ms, move(callback));
        });
        if (auto authorized_groups = m_authorized_playback_groups.get(tab_id); authorized_groups.has_value()) {
            for (auto group : *authorized_groups)
                mixer->set_group_authorization(group, Audio::PlaybackGroupAuthorization::Authorized);
        }
        if (auto muted_groups = m_muted_playback_groups.get(tab_id); muted_groups.has_value()) {
            for (auto group : *muted_groups)
                mixer->set_group_mute_state(group, Audio::PlaybackGroupMuteState::Muted);
        }
        mixer->on_group_audibility_changed = [this, tab_id](Audio::PlaybackGroupId group, Audio::Audibility audibility) {
            if (auto control_connection = m_control_connection.strong_ref())
                control_connection->async_playback_group_audibility_changed(tab_id, group, audibility);
        };
        return Tab { .mixer = move(mixer), .connections = {} };
    });
}

Audio::CaptureDevices& Tabs::capture_devices()
{
    if (!m_capture_devices) {
        m_capture_devices = Audio::CaptureDevices::create([audio_output = m_audio_output](Audio::SampleSpecification const& specification, u32 fragment_size_bytes, StringView device_id, Audio::RecordStream::RecordCallback callback) {
            if (audio_output == Audio::AudioOutput::Null)
                return Audio::RecordStream::CreatePromise::rejected(Error::from_string_literal("A headless AudioServer has no capture devices"));
            return Audio::create_platform_record_stream(specification, fragment_size_bytes, device_id, move(callback));
        });
    }
    return *m_capture_devices;
}

ErrorOr<IPC::TransportHandle> Tabs::connect_client(u64 tab_id)
{
    auto paired_transports = TRY(IPC::Transport::create_paired());
    auto handle = move(paired_transports.remote_handle);

    auto& tab = tab_for(tab_id);
    auto client_id = m_next_client_id++;
    auto device_enumeration = Audio::ServerConnection::DeviceEnumeration::Platform;
    if (m_audio_output == Audio::AudioOutput::Null)
        device_enumeration = Audio::ServerConnection::DeviceEnumeration::None;
    auto connection = Audio::ServerConnection::construct(move(paired_transports.local), client_id, tab.mixer, device_enumeration, capture_devices(), [tab_id](ByteString const& device_id, Function<void(bool)> on_decided) {
        Tabs::the().request_capture_permission(tab_id, device_id, move(on_decided));
    });
    connection->set_capture_permitted(m_tabs_with_capture_permission.contains(tab_id));
    connection->on_death = [tab_id, client_id] {
        Tabs::the().connection_died(tab_id, client_id);
    };
    tab.connections.set(client_id, move(connection));
    return handle;
}

void Tabs::set_capture_permission(u64 tab_id, bool granted)
{
    if (granted)
        m_tabs_with_capture_permission.set(tab_id);
    else
        m_tabs_with_capture_permission.remove(tab_id);
    auto tab = m_tabs.get(tab_id);
    if (!tab.has_value())
        return;
    for (auto& [client_id, connection] : tab->connections)
        connection->set_capture_permitted(granted);
}

static void remove_group(HashMap<u64, HashTable<Audio::PlaybackGroupId>>& groups_by_tab, u64 tab_id, Audio::PlaybackGroupId group)
{
    auto groups = groups_by_tab.get(tab_id);
    if (!groups.has_value())
        return;
    groups->remove(group);
    if (groups->is_empty())
        groups_by_tab.remove(tab_id);
}

void Tabs::set_playback_group_authorization(u64 tab_id, Audio::PlaybackGroupId group, Audio::PlaybackGroupAuthorization authorization)
{
    switch (authorization) {
    case Audio::PlaybackGroupAuthorization::Authorized:
        m_authorized_playback_groups.ensure(tab_id).set(group);
        break;
    case Audio::PlaybackGroupAuthorization::Unauthorized:
        remove_group(m_authorized_playback_groups, tab_id, group);
        break;
    }
    if (auto tab = m_tabs.get(tab_id); tab.has_value())
        tab->mixer->set_group_authorization(group, authorization);
}

void Tabs::set_playback_group_mute_state(u64 tab_id, Audio::PlaybackGroupId group, Audio::PlaybackGroupMuteState mute_state)
{
    switch (mute_state) {
    case Audio::PlaybackGroupMuteState::Muted:
        m_muted_playback_groups.ensure(tab_id).set(group);
        break;
    case Audio::PlaybackGroupMuteState::Unmuted:
        remove_group(m_muted_playback_groups, tab_id, group);
        break;
    }
    if (auto tab = m_tabs.get(tab_id); tab.has_value())
        tab->mixer->set_group_mute_state(group, mute_state);
}

void Tabs::request_capture_permission(u64 tab_id, ByteString const& device_id, Function<void(bool)> on_decided)
{
    auto control_connection = m_control_connection.strong_ref();
    if (!control_connection) {
        on_decided(false);
        return;
    }
    auto request_id = m_next_capture_request_id++;
    m_pending_capture_decisions.set(request_id, move(on_decided));
    control_connection->async_capture_requested(tab_id, request_id, device_id);
}

void Tabs::capture_decided(u64 request_id, bool allowed)
{
    if (auto on_decided = m_pending_capture_decisions.take(request_id); on_decided.has_value())
        on_decided.value()(allowed);
}

void Tabs::connection_died(u64 tab_id, int client_id)
{
    auto tab = m_tabs.get(tab_id);
    if (!tab.has_value())
        return;
    tab->connections.remove(client_id);
    if (!tab->connections.is_empty())
        return;
    tab->mixer->report_every_group_inaudible();
    m_tabs.remove(tab_id);
}

}
