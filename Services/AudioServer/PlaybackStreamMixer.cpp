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

NonnullRefPtr<PlaybackStreamMixer> PlaybackStreamMixer::create(Core::EventLoop& control_loop, u32 target_latency_ms, DeviceStreamFactory factory, AK::Duration audibility_linger)
{
    auto state = make_ref_counted<MixState>(control_loop);
    auto mixer = adopt_ref(*new PlaybackStreamMixer(state, target_latency_ms, audibility_linger));
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

PlaybackStreamMixer::PlaybackStreamMixer(NonnullRefPtr<MixState> state, u32 device_target_latency_ms, AK::Duration audibility_linger)
    : m_state(move(state))
    , m_device_target_latency_ms(device_target_latency_ms)
    , m_audibility_linger(audibility_linger)
    , m_audibility_timer(Core::Timer::create_single_shot(AK::Duration::zero(), [this] { expire_audibility(); }))
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

void PlaybackStreamMixer::add_client(MixerClientId id, SharedAudioFrameRing ring, float gain, Optional<PlaybackGroupId> group)
{
    VERIFY(m_device_sample_specification.has_value());
    VERIFY(ring.sample_rate() == m_device_sample_specification->sample_rate());
    VERIFY(ring.channel_count() == m_device_sample_specification->channel_count());

    MutexLocker locker(m_state->m_mutex);
    auto result = m_state->m_clients.set(id, Client { .ring = move(ring), .gain = gain, .group = group, .state = ClientState::Suspended, .on_drained = nullptr });
    VERIFY(result == HashSetResult::InsertedNewEntry);
}

void PlaybackStreamMixer::set_client_group(MixerClientId id, Optional<PlaybackGroupId> group)
{
    MutexLocker locker(m_state->m_mutex);
    if (auto client = m_state->m_clients.get(id); client.has_value())
        client->group = group;
}

void PlaybackStreamMixer::set_group_authorization(PlaybackGroupId group, PlaybackGroupAuthorization authorization)
{
    {
        MutexLocker locker(m_state->m_mutex);
        switch (authorization) {
        case PlaybackGroupAuthorization::Authorized:
            m_state->m_authorized_groups.set(group);
            return;
        case PlaybackGroupAuthorization::Unauthorized:
            m_state->m_authorized_groups.remove(group);
            m_state->m_group_mixes.remove(group);
            m_state->m_audible_until_ns.remove(group);
            break;
        }
    }
    report_group_inaudible(group);
}

void PlaybackStreamMixer::set_group_mute_state(PlaybackGroupId group, PlaybackGroupMuteState mute_state)
{
    MutexLocker locker(m_state->m_mutex);
    switch (mute_state) {
    case PlaybackGroupMuteState::Muted:
        m_state->m_muted_groups.set(group);
        break;
    case PlaybackGroupMuteState::Unmuted:
        m_state->m_muted_groups.remove(group);
        break;
    }
}

void PlaybackStreamMixer::group_became_audible(PlaybackGroupId group)
{
    {
        // The group may have been revoked since it was heard.
        MutexLocker locker(m_state->m_mutex);
        if (!m_state->m_audible_until_ns.contains(group))
            return;
    }
    if (m_audible_groups.set(group) != HashSetResult::InsertedNewEntry)
        return;
    if (on_group_audibility_changed)
        on_group_audibility_changed(group, Audibility::Audible);
    expire_audibility();
}

void PlaybackStreamMixer::expire_audibility()
{
    auto now_ns = MonotonicTime::now().nanoseconds();
    auto linger_ns = m_audibility_linger.to_nanoseconds();
    Vector<PlaybackGroupId> expired_groups;
    Optional<i64> next_expiry_ns;
    {
        MutexLocker locker(m_state->m_mutex);
        m_state->m_audible_until_ns.remove_all_matching([&](PlaybackGroupId group, i64 audible_until_ns) {
            auto expiry_ns = audible_until_ns + linger_ns;
            if (expiry_ns <= now_ns) {
                expired_groups.append(group);
                return true;
            }
            next_expiry_ns = min(next_expiry_ns.value_or(expiry_ns), expiry_ns);
            return false;
        });
    }
    for (auto group : expired_groups)
        report_group_inaudible(group);
    if (next_expiry_ns.has_value())
        m_audibility_timer->restart(AK::Duration::from_nanoseconds(*next_expiry_ns - now_ns));
}

void PlaybackStreamMixer::report_group_inaudible(PlaybackGroupId group)
{
    if (m_audible_groups.remove(group) && on_group_audibility_changed)
        on_group_audibility_changed(group, Audibility::Inaudible);
}

void PlaybackStreamMixer::report_every_group_inaudible()
{
    {
        MutexLocker locker(m_state->m_mutex);
        m_state->m_audible_until_ns.clear();
    }
    m_audibility_timer->stop();
    for (auto group : exchange(m_audible_groups, {})) {
        if (on_group_audibility_changed)
            on_group_audibility_changed(group, Audibility::Inaudible);
    }
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
    auto timer = Core::Timer::create_single_shot(remaining, [this, id] {
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
    Vector<PlaybackGroupId, 4> newly_audible_groups;
    {
        MutexLocker locker(m_mutex);
        for (auto& [group, group_mix] : m_group_mixes)
            group_mix.has_samples = false;

        for (auto& [id, client] : m_clients) {
            if (client.state == ClientState::Suspended || client.state == ClientState::Drained)
                continue;
            size_t consumed_frames = 0;
            if (client.group.has_value() && m_authorized_groups.contains(*client.group)) {
                auto& group_mix = m_group_mixes.ensure(*client.group);
                if (!group_mix.has_samples) {
                    group_mix.samples.resize(buffer.size());
                    group_mix.samples.span().fill(0);
                    group_mix.has_samples = true;
                }
                consumed_frames = client.ring.pop_mixing_into(group_mix.samples, client.gain);
            } else {
                consumed_frames = client.ring.discard(buffer.size() / client.ring.channel_count());
            }
            if (consumed_frames > 0)
                client.played_at_ns = (buffer_starts_playing_at + AK::Duration::from_time_units(static_cast<i64>(consumed_frames), 1, client.ring.sample_rate())).nanoseconds();
            if (client.state == ClientState::Draining && client.ring.frames_available() == 0) {
                client.state = ClientState::Drained;
                drained_clients.append({ .id = id, .played_at_ns = client.played_at_ns });
            }
            client.ring.publish_consumption(client.played_at_ns, client.state == ClientState::Playing && consumed_frames > 0);
        }

        // A group is heard unless it is muted, and measured either way.
        bool an_unmuted_group_is_audible = false;
        for (auto& [group, group_mix] : m_group_mixes) {
            if (!group_mix.has_samples)
                continue;
            auto samples = group_mix.samples.span();
            group_mix.peak = 0;
            for (auto sample : samples)
                group_mix.peak = max(group_mix.peak, fabsf(sample));
            if (m_muted_groups.contains(group))
                continue;
            for (size_t i = 0; i < buffer.size(); ++i)
                buffer[i] += samples[i];
            if (group_mix.peak >= AUDIBLE_PEAK)
                an_unmuted_group_is_audible = true;
        }

        // Unmuted groups too quiet to be audible on their own are all audible while what they play together is, so
        // that sound split between groups is still noticed.
        bool quiet_groups_are_audible_together = false;
        if (!an_unmuted_group_is_audible) {
            float output_peak = 0;
            for (auto sample : buffer)
                output_peak = max(output_peak, fabsf(sample));
            quiet_groups_are_audible_together = output_peak >= AUDIBLE_PEAK;
        }

        for (auto& [group, group_mix] : m_group_mixes) {
            if (!group_mix.has_samples)
                continue;
            auto is_audible = group_mix.peak >= AUDIBLE_PEAK
                || (quiet_groups_are_audible_together && group_mix.peak > 0 && !m_muted_groups.contains(group));
            if (is_audible && m_audible_until_ns.set(group, buffer_starts_playing_at.nanoseconds()) == HashSetResult::InsertedNewEntry)
                newly_audible_groups.append(group);
        }
    }
    // The mix is clipped to the [-1, 1] that devices play, rather than leaving sound beyond it to each platform.
    for (auto& sample : buffer)
        sample = clamp(sample, -1.0f, 1.0f);
    for (auto drained : drained_clients) {
        m_control_loop.deferred_invoke([self = NonnullRefPtr(*this), drained] {
            if (self->m_mixer)
                self->m_mixer->finish_drain_once_played(drained.id, drained.played_at_ns);
        });
    }
    for (auto group : newly_audible_groups) {
        m_control_loop.deferred_invoke([self = NonnullRefPtr(*this), group] {
            if (self->m_mixer)
                self->m_mixer->group_became_audible(group);
        });
    }
    return buffer;
}

}
