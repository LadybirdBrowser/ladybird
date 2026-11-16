/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Mutex.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMediaClient/RemotePlaybackManager.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibWeb/HTML/MediaStreamPlayback.h>

namespace Web::HTML {

// Every audio track is handed to the playback in this format, whatever its source produces.
static constexpr u32 PLAYBACK_SAMPLE_RATE = 48'000;
static constexpr u8 PLAYBACK_CHANNEL_COUNT = 2;

// Audio that the main thread can't hand over in time is dropped rather than played late.
static constexpr size_t MAXIMUM_PENDING_AUDIO_DELIVERIES = 16;

// Fed on whichever thread produces the track's audio, which is handed to the playback on the main thread.
struct MediaStreamPlayback::AudioInput final : public AtomicRefCounted<AudioInput> {
    Mutex mutex;
    OwnPtr<Media::FFmpeg::FFmpegAudioConverter> converter;
    i64 frames_received { 0 };
    Atomic<size_t> pending_deliveries { 0 };
    RefPtr<Core::WeakEventLoopReference> main_event_loop;

    // Only touched on the main thread.
    Function<void(ReadonlySpan<float> interleaved_samples)> on_samples;
};

static void receive_audio(NonnullRefPtr<MediaStreamPlayback::AudioInput> const& input, float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate)
{
    if (frame_count == 0 || channel_count == 0 || sample_rate == 0)
        return;

    Vector<float> converted_samples;
    {
        MutexLocker locker(input->mutex);
        if (!input->converter) {
            auto converter_or_error = Media::FFmpeg::FFmpegAudioConverter::try_create();
            if (converter_or_error.is_error())
                return;
            auto converter = converter_or_error.release_value();
            if (converter->set_output_sample_specification(Audio::SampleSpecification(PLAYBACK_SAMPLE_RATE, Audio::ChannelMap::stereo())).is_error())
                return;
            input->converter = move(converter);
        }

        // FIXME: Downmix sources with more than two channels instead of keeping the first two.
        auto used_channel_count = min<size_t>(channel_count, 2);
        auto channel_map = used_channel_count == 1 ? Audio::ChannelMap::mono() : Audio::ChannelMap::stereo();
        Media::AudioBlock block;
        block.initialize(Audio::SampleSpecification(sample_rate, channel_map), input->frames_received, frame_count);
        input->frames_received += static_cast<i64>(frame_count);
        for (size_t channel = 0; channel < used_channel_count; ++channel) {
            auto destination = block.channel_data(channel);
            for (size_t frame = 0; frame < frame_count; ++frame)
                destination[frame] = samples[(frame * channel_count) + channel];
        }
        if (input->converter->push_block(block).is_error())
            return;

        while (true) {
            Media::AudioBlock converted;
            if (input->converter->retrieve_block(converted).is_error() || converted.is_empty())
                break;
            auto offset = converted_samples.size();
            converted_samples.resize(offset + converted.sample_count());
            converted.copy_to_interleaved(converted_samples.span().slice(offset));
        }
    }
    if (converted_samples.is_empty())
        return;

    auto main_event_loop = input->main_event_loop->take();
    if (!main_event_loop || input->pending_deliveries.load() >= MAXIMUM_PENDING_AUDIO_DELIVERIES)
        return;
    input->pending_deliveries.fetch_add(1);
    main_event_loop->deferred_invoke([input, converted_samples = move(converted_samples)] {
        input->pending_deliveries.fetch_sub(1);
        if (input->on_samples)
            input->on_samples(converted_samples);
    });
}

NonnullOwnPtr<MediaStreamPlayback> MediaStreamPlayback::create(MediaClient::RemotePlaybackManager& playback_manager, MediaCapture::MediaStream& stream, ActivityOf activity_of)
{
    auto playback = adopt_own(*new MediaStreamPlayback(playback_manager, stream, activity_of));
    stream.add_observer(*playback);
    playback->m_playback_manager.on_live_source_key_frame_needed = [playback = playback.ptr()](u64 live_source_id) {
        auto* subscription = playback->find_subscription_for_live_source(live_source_id);
        if (!subscription)
            return;
        if (auto track = subscription->track.ptr())
            track->request_video_key_frame();
    };
    playback->update_subscriptions();
    playback->m_is_active = playback->stream_has_active_tracks();
    return playback;
}

MediaStreamPlayback::MediaStreamPlayback(MediaClient::RemotePlaybackManager& playback_manager, MediaCapture::MediaStream& stream, ActivityOf activity_of)
    : m_playback_manager(playback_manager)
    , m_stream(stream)
    , m_activity_of(activity_of)
{
}

bool MediaStreamPlayback::stream_has_active_tracks() const
{
    auto stream = m_stream.ptr();
    if (!stream)
        return false;
    if (m_activity_of == ActivityOf::AnyTrack)
        return stream->active();
    // A MediaStream object is said to be audible when it has at least one MediaStreamTrack whose [[Kind]] is "audio"
    // that has not ended.
    return any_of(stream->get_audio_tracks(), [](auto const& track) { return track->track_ready_state() != Bindings::MediaStreamTrackState::Ended; });
}

MediaStreamPlayback::~MediaStreamPlayback()
{
    // This can run while the garbage collector is sweeping, when the stream and its tracks may already be gone, so the
    // sinks are only switched off here. Owners stop() beforehand to let go of them properly.
    m_playback_manager.on_live_source_key_frame_needed = nullptr;
    for (auto& subscription : m_subscriptions) {
        if (subscription.audio_sink)
            subscription.audio_sink->deactivate();
        if (subscription.audio_input)
            subscription.audio_input->on_samples = nullptr;
        if (subscription.video_sink)
            subscription.video_sink->on_frame = nullptr;
    }
}

void MediaStreamPlayback::stop()
{
    m_playback_manager.on_live_source_key_frame_needed = nullptr;
    if (auto stream = m_stream.ptr())
        stream->remove_observer(*this);
    m_stream = nullptr;
    for (auto& subscription : m_subscriptions)
        unsubscribe(subscription);
    m_subscriptions.clear();
}

void MediaStreamPlayback::media_stream_tracks_changed()
{
    update_subscriptions();
    update_active_state();
}

void MediaStreamPlayback::update_active_state()
{
    auto is_active = stream_has_active_tracks();
    if (is_active == m_is_active)
        return;
    m_is_active = is_active;
    if (on_active_change)
        on_active_change(is_active);
}

MediaStreamPlayback::Subscription* MediaStreamPlayback::find_subscription_for_live_source(u64 live_source_id)
{
    for (auto& subscription : m_subscriptions) {
        if (subscription.live_source_id == live_source_id)
            return &subscription;
    }
    return nullptr;
}

void MediaStreamPlayback::update_subscriptions()
{
    auto stream = m_stream.ptr();
    if (!stream)
        return;
    auto tracks = stream->get_tracks();

    for (size_t index = m_subscriptions.size(); index-- > 0;) {
        auto track = m_subscriptions[index].track.ptr();
        auto still_present = track && any_of(tracks, [&](auto const& candidate) { return candidate.ptr() == track.ptr(); });
        if (still_present)
            continue;
        auto live_source_id = m_subscriptions[index].live_source_id;
        unsubscribe(m_subscriptions[index]);
        m_subscriptions.remove(index);
        // NB: A playback track is identified by the live source that plays it.
        if (live_source_id.has_value() && on_track_removed)
            on_track_removed(*live_source_id);
    }

    for (auto const& track : tracks) {
        if (any_of(m_subscriptions, [&](auto const& subscription) { return subscription.track.ptr().ptr() == track.ptr(); }))
            continue;
        subscribe(track);
    }
}

void MediaStreamPlayback::announce_live_source(MediaClient::RemotePlaybackManager::LiveSource const& live_source, Utf16String media_stream_track_id)
{
    live_source.track->when_resolved([weak_self = make_weak_ptr<MediaStreamPlayback>(), live_source_id = live_source.id, media_stream_track_id = move(media_stream_track_id)](Media::Track const& track) {
        if (!weak_self)
            return;
        // The track may have left the stream in the meantime, which ended its source.
        if (!weak_self->find_subscription_for_live_source(live_source_id))
            return;
        if (weak_self->on_track_added)
            weak_self->on_track_added(track, media_stream_track_id);
    });
    live_source.track->when_rejected([](Media::DecoderError const& error) {
        dbgln("MediaStreamPlayback: Unable to play a track: {}", error.description());
    });
}

void MediaStreamPlayback::subscribe(MediaCapture::MediaStreamTrack& track)
{
    Subscription subscription;
    subscription.track = track;
    // An ended track stays in its stream, which is inactive once all of its tracks have ended.
    subscription.ended_callback_id = track.add_ended_callback([weak_self = make_weak_ptr<MediaStreamPlayback>()] {
        if (weak_self)
            weak_self->update_active_state();
    });

    if (track.is_audio()) {
        auto live_source = m_playback_manager.add_live_audio_source(PLAYBACK_SAMPLE_RATE, PLAYBACK_CHANNEL_COUNT, track.label());
        subscription.live_source_id = live_source.id;

        auto input = adopt_ref(*new AudioInput);
        input->main_event_loop = Core::EventLoop::current_weak();
        input->on_samples = [this, live_source_id = live_source.id](ReadonlySpan<float> interleaved_samples) {
            m_playback_manager.add_to_live_audio_source(live_source_id, ReadonlyBytes { interleaved_samples.data(), interleaved_samples.size() * sizeof(float) });
        };
        auto sink = adopt_ref(*new MediaCapture::AudioFrameSink);
        sink->on_frames = [input](float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate) {
            receive_audio(input, samples, frame_count, channel_count, sample_rate);
        };
        subscription.audio_input = input;
        subscription.audio_sink = sink;
        m_subscriptions.append(move(subscription));
        track.add_audio_sink(sink);
        announce_live_source(live_source, track.id());
        return;
    }

    // A video track joins the playback with its first frame, so that a track that has nothing to show doesn't keep
    // the rest of the stream from playing.
    auto sink = adopt_ref(*new MediaCapture::VideoFrameSink);
    sink->on_frame = [this, track = GC::Weak { track }](MediaCapture::EncodedVideoFrame const& frame) {
        auto index = m_subscriptions.find_first_index_if([&](auto const& subscription) { return subscription.track.ptr().ptr() == track.ptr().ptr(); });
        if (!index.has_value())
            return;
        auto& subscription = m_subscriptions[*index];
        if (!subscription.live_source_id.has_value()) {
            auto live_source = m_playback_manager.add_live_video_source(track ? track->label() : Utf16String {});
            subscription.live_source_id = live_source.id;
            announce_live_source(live_source, track ? track->id() : Utf16String {});
        }
        m_playback_manager.add_to_live_video_source(*subscription.live_source_id, frame.codec_id, frame.is_key_frame, frame.timestamp, frame.data);
    };
    subscription.video_sink = sink;
    m_subscriptions.append(move(subscription));
    track.add_video_sink(sink);
}

void MediaStreamPlayback::unsubscribe(Subscription& subscription)
{
    auto track = subscription.track.ptr();
    if (track && subscription.ended_callback_id.has_value())
        track->remove_ended_callback(*subscription.ended_callback_id);
    if (subscription.audio_sink) {
        if (track)
            track->remove_audio_sink(*subscription.audio_sink);
        else
            subscription.audio_sink->deactivate();
    }
    if (subscription.audio_input)
        subscription.audio_input->on_samples = nullptr;
    if (subscription.video_sink) {
        subscription.video_sink->on_frame = nullptr;
        if (track)
            track->remove_video_sink(*subscription.video_sink);
    }
    if (subscription.live_source_id.has_value())
        m_playback_manager.end_live_source(*subscription.live_source_id);
}

}
