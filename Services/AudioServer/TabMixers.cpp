/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/NeverDestroyed.h>
#include <AudioServer/TabMixers.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibMedia/Audio/PlaybackStream.h>

namespace AudioServer {

// The device never underruns, since the mix is silence where no client has frames, so its own buffer can stay well
// below the lookahead the clients keep in their rings.
static constexpr u32 DEVICE_TARGET_LATENCY_MS = 40;

TabMixers& TabMixers::the()
{
    static NeverDestroyed<TabMixers> instance;
    return *instance;
}

// The platform factory copies its callback for the null fallback, and the mixer's callback is not copyable.
struct SharedDataRequestCallback : public AtomicRefCounted<SharedDataRequestCallback> {
    explicit SharedDataRequestCallback(Audio::PlaybackStream::AudioDataRequestCallback callback)
        : callback(move(callback))
    {
    }
    Audio::PlaybackStream::AudioDataRequestCallback callback;
};

static NonnullRefPtr<Audio::PlaybackStream::CreatePromise> create_device_stream(Audio::OutputState state, u32 target_latency_ms, Audio::PlaybackStream::AudioDataRequestCallback callback)
{
    auto shared_callback = make_ref_counted<SharedDataRequestCallback>(move(callback));
    return Audio::PlaybackStream::create_platform_or_null(state, target_latency_ms, [shared_callback](Span<float> buffer, MonotonicTime buffer_starts_playing_at) {
        return shared_callback->callback(buffer, buffer_starts_playing_at);
    });
}

TabMixers::Tab& TabMixers::tab_for(u64 tab_id)
{
    return m_tabs.ensure(tab_id, [] {
        auto mixer = Audio::PlaybackStreamMixer::create(Core::EventLoop::current(), DEVICE_TARGET_LATENCY_MS, create_device_stream);
        return Tab { .mixer = move(mixer), .connections = {} };
    });
}

ErrorOr<IPC::TransportHandle> TabMixers::connect_client(u64 tab_id)
{
    auto paired_transports = TRY(IPC::Transport::create_paired());
    auto handle = move(paired_transports.remote_handle);

    auto& tab = tab_for(tab_id);
    auto client_id = m_next_client_id++;
    auto connection = Audio::ServerConnection::construct(move(paired_transports.local), client_id, tab.mixer);
    connection->on_death = [tab_id, client_id] {
        TabMixers::the().connection_died(tab_id, client_id);
    };
    tab.connections.set(client_id, move(connection));
    return handle;
}

void TabMixers::connection_died(u64 tab_id, int client_id)
{
    auto tab = m_tabs.get(tab_id);
    if (!tab.has_value())
        return;
    tab->connections.remove(client_id);
    if (tab->connections.is_empty())
        m_tabs.remove(tab_id);
}

}
