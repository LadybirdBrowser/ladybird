/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AudioServer/PlaybackStreamMixer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>

namespace Audio {

NonnullRefPtr<PlaybackStreamMixer> PlaybackStreamMixer::create(Core::EventLoop& control_loop, u32 target_latency_ms, DeviceStreamFactory factory)
{
    auto state = make_ref_counted<MixState>(control_loop);
    auto mixer = adopt_ref(*new PlaybackStreamMixer(state, target_latency_ms));
    state->m_mixer = mixer.ptr();

    auto promise = factory(OutputState::Suspended, target_latency_ms, [state](Span<float> buffer, MonotonicTime buffer_starts_playing_at) {
        return state->mix_into(buffer, buffer_starts_playing_at);
    });
    promise->when_resolved([state](NonnullRefPtr<PlaybackStream>& stream) {
        if (state->m_mixer)
            state->m_mixer->set_device_stream(stream);
    });
    promise->when_rejected([state](Error& error) {
        if (state->m_mixer)
            state->m_mixer->set_device_error(move(error));
    });
    return mixer;
}

PlaybackStreamMixer::PlaybackStreamMixer(NonnullRefPtr<MixState> state, u32 device_target_latency_ms)
    : m_state(move(state))
    , m_device_target_latency_ms(device_target_latency_ms)
{
}

PlaybackStreamMixer::~PlaybackStreamMixer()
{
    m_state->m_mixer = nullptr;
}

void PlaybackStreamMixer::set_device_stream(NonnullRefPtr<PlaybackStream> stream)
{
    m_device_sample_specification = stream->sample_specification();
    m_device_stream = move(stream);
    run_device_ready_callbacks();
}

void PlaybackStreamMixer::set_device_error(Error error)
{
    m_device_error = move(error);
    run_device_ready_callbacks();
}

void PlaybackStreamMixer::when_device_ready(DeviceReadyCallback callback)
{
    m_device_ready_callbacks.append(move(callback));
    run_device_ready_callbacks();
}

void PlaybackStreamMixer::run_device_ready_callbacks()
{
    if (!m_device_sample_specification.has_value() && !m_device_error.has_value())
        return;
    auto callbacks = move(m_device_ready_callbacks);
    for (auto& callback : callbacks) {
        if (m_device_error.has_value())
            callback(ErrorOr<SampleSpecification>(Error::copy(m_device_error.value())));
        else
            callback(ErrorOr<SampleSpecification>(m_device_sample_specification.value()));
    }
}

void PlaybackStreamMixer::add_client(MixerClientId id, SharedAudioFrameRing ring, float gain)
{
    VERIFY(m_device_sample_specification.has_value());
    VERIFY(ring.sample_rate() == m_device_sample_specification->sample_rate());
    VERIFY(ring.channel_count() == m_device_sample_specification->channel_count());

    MutexLocker locker(m_state->m_mutex);
    auto result = m_state->m_clients.set(id, Client { .ring = move(ring), .gain = gain, .state = ClientState::Suspended, .on_drained = nullptr });
    VERIFY(result == HashSetResult::InsertedNewEntry);
}

void PlaybackStreamMixer::remove_client(MixerClientId id)
{
    bool was_active = false;
    Function<void()> cancelled_drain;
    {
        MutexLocker locker(m_state->m_mutex);
        auto client = m_state->m_clients.take(id);
        if (!client.has_value())
            return;
        was_active = client->state != ClientState::Suspended;
        cancelled_drain = move(client->on_drained);
    }
    if (was_active)
        client_became_inactive();
    if (cancelled_drain)
        cancelled_drain();
}

void PlaybackStreamMixer::resume_client(MixerClientId id)
{
    bool became_active = false;
    Function<void()> cancelled_drain;
    {
        MutexLocker locker(m_state->m_mutex);
        auto client = m_state->m_clients.get(id);
        if (!client.has_value())
            return;
        became_active = client->state == ClientState::Suspended;
        client->state = ClientState::Playing;
        cancelled_drain = move(client->on_drained);
    }
    if (became_active)
        client_became_active();
    if (cancelled_drain)
        cancelled_drain();
}

void PlaybackStreamMixer::drain_client(MixerClientId id, Function<void()> on_drained)
{
    {
        MutexLocker locker(m_state->m_mutex);
        auto client = m_state->m_clients.get(id);
        if (!client.has_value())
            return;
        if (client->state != ClientState::Suspended) {
            if (client->state == ClientState::Playing)
                client->state = ClientState::Draining;
            if (client->on_drained) {
                client->on_drained = [previous = move(client->on_drained), on_drained = move(on_drained)]() mutable {
                    previous();
                    on_drained();
                };
            } else {
                client->on_drained = move(on_drained);
            }
            return;
        }
    }
    on_drained();
}

void PlaybackStreamMixer::discard_client(MixerClientId id)
{
    bool was_active = false;
    Function<void()> cancelled_drain;
    {
        MutexLocker locker(m_state->m_mutex);
        auto client = m_state->m_clients.get(id);
        if (!client.has_value())
            return;
        was_active = client->state != ClientState::Suspended;
        client->ring.discard_all();
        client->ring.publish_consumption(MonotonicTime::now().nanoseconds(), false);
        client->state = ClientState::Suspended;
        cancelled_drain = move(client->on_drained);
    }
    if (was_active)
        client_became_inactive();
    if (cancelled_drain)
        cancelled_drain();
}

void PlaybackStreamMixer::set_client_gain(MixerClientId id, float gain)
{
    MutexLocker locker(m_state->m_mutex);
    if (auto client = m_state->m_clients.get(id); client.has_value())
        client->gain = gain;
}

bool PlaybackStreamMixer::has_clients() const
{
    MutexLocker locker(m_state->m_mutex);
    return !m_state->m_clients.is_empty();
}

void PlaybackStreamMixer::client_became_active()
{
    m_active_client_count++;
    if (m_active_client_count != 1 || m_device_is_playing)
        return;
    m_device_is_playing = true;
    VERIFY(m_device_stream);
    // A ThreadedPromise resolved on the device thread spins until it has a rejection handler.
    m_device_stream->resume()->when_rejected([](Error&& error) {
        warnln("Unexpected error while resuming the mixer's device stream: {}", error);
    });
}

void PlaybackStreamMixer::client_became_inactive()
{
    VERIFY(m_active_client_count > 0);
    m_active_client_count--;
    if (m_active_client_count != 0 || !m_device_is_playing)
        return;
    m_device_is_playing = false;
    m_device_stream->drain_buffer_and_suspend()->when_rejected([](Error&& error) {
        warnln("Unexpected error while suspending the mixer's device stream: {}", error);
    });
}

void PlaybackStreamMixer::finish_drain_once_played(MixerClientId id, i64 played_at_ns)
{
    auto remaining = AK::Duration::from_nanoseconds(played_at_ns - MonotonicTime::now().nanoseconds());
    if (remaining <= AK::Duration::zero()) {
        finish_drain(id);
        return;
    }
    auto timer = Core::Timer::create_single_shot(max(1, AK::clamp_to<int>(remaining.to_milliseconds() + 1)), [this, id] {
        m_drain_timers.remove(id);
        finish_drain(id);
    });
    timer->start();
    m_drain_timers.set(id, move(timer));
}

void PlaybackStreamMixer::finish_drain(MixerClientId id)
{
    Function<void()> on_drained;
    {
        MutexLocker locker(m_state->m_mutex);
        auto client = m_state->m_clients.get(id);
        if (!client.has_value() || client->state != ClientState::Drained)
            return;
        client->state = ClientState::Suspended;
        on_drained = move(client->on_drained);
    }
    client_became_inactive();
    if (on_drained)
        on_drained();
}

ReadonlySpan<float> PlaybackStreamMixer::MixState::mix_into(Span<float> buffer, MonotonicTime buffer_starts_playing_at)
{
    buffer.fill(0);
    struct DrainedClient {
        MixerClientId id;
        i64 played_at_ns;
    };
    Vector<DrainedClient, 4> drained_clients;
    {
        MutexLocker locker(m_mutex);
        for (auto& [id, client] : m_clients) {
            if (client.state == ClientState::Suspended || client.state == ClientState::Drained)
                continue;
            auto consumed_frames = client.ring.pop_mixing_into(buffer, client.gain);
            if (consumed_frames > 0)
                client.played_at_ns = (buffer_starts_playing_at + AK::Duration::from_time_units(static_cast<i64>(consumed_frames), 1, client.ring.sample_rate())).nanoseconds();
            if (client.state == ClientState::Draining && client.ring.frames_available() == 0) {
                client.state = ClientState::Drained;
                drained_clients.append({ .id = id, .played_at_ns = client.played_at_ns });
            }
            client.ring.publish_consumption(client.played_at_ns, client.state == ClientState::Playing && consumed_frames > 0);
        }
    }
    for (auto drained : drained_clients) {
        m_control_loop.deferred_invoke([self = NonnullRefPtr(*this), drained] {
            if (self->m_mixer)
                self->m_mixer->finish_drain_once_played(drained.id, drained.played_at_ns);
        });
    }
    return buffer;
}

}
