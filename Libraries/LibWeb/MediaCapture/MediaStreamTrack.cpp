/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Value.h>
#include <LibMedia/Audio/RecordStream.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaStreamTrack);

Atomic<u64> MediaStreamTrack::s_next_provider_id { 1 };

// A single make-up gain point for audio delivered by this track's source.
// FIXME: This is where the autoGainControl constraint belongs once constraint processing can
//        negotiate it; until then the track source applies unity gain.
static constexpr float TRACK_SOURCE_GAIN = 1.0f;

bool AudioFrameFanout::add_sink(NonnullRefPtr<AudioFrameSink> sink)
{
    Sync::MutexLocker locker(m_mutex);
    m_sinks.append(move(sink));
    return m_sinks.size() == 1;
}

bool AudioFrameFanout::remove_sink(AudioFrameSink const& sink)
{
    sink.deactivate();
    Sync::MutexLocker locker(m_mutex);
    m_sinks.remove_first_matching([&](auto const& existing) { return existing.ptr() == &sink; });
    return m_sinks.is_empty();
}

void AudioFrameFanout::deliver(float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate)
{
    if (frame_count == 0 || channel_count == 0)
        return;

    // Snapshot the sinks so a slow consumer never holds up add/remove on other threads.
    Vector<NonnullRefPtr<AudioFrameSink>> sinks;
    {
        Sync::MutexLocker locker(m_mutex);
        sinks = m_sinks;
    }
    if (sinks.is_empty())
        return;

    auto sample_count = frame_count * channel_count;
    if (m_silenced.load(AK::MemoryOrder::memory_order_relaxed)) {
        // A muted or disabled track delivers silence in the same shape as the live signal,
        // so consumers keep observing the track's timing and format.
        // https://w3c.github.io/mediacapture-main/#dfn-enabled
        m_scratch.resize(sample_count);
        m_scratch.span().fill(0.f);
        samples = m_scratch.data();
    } else if constexpr (TRACK_SOURCE_GAIN != 1.0f) {
        m_scratch.resize(sample_count);
        for (size_t sample_index = 0; sample_index < sample_count; ++sample_index)
            m_scratch[sample_index] = samples[sample_index] * TRACK_SOURCE_GAIN;
        samples = m_scratch.data();
    }

    // AudioBlock consumers accept at most 1024 frames per delivery.
    for (size_t offset = 0; offset < frame_count; offset += 1024) {
        auto frames = min(static_cast<size_t>(1024), frame_count - offset);
        for (auto const& sink : sinks)
            sink->deliver(samples + offset * channel_count, frames, channel_count, sample_rate);
    }
}

MediaStreamTrack::MediaStreamTrack()
    : DOM::EventTarget()
    , m_audio_fanout(adopt_ref(*new AudioFrameFanout))
{
}

MediaStreamTrack::~MediaStreamTrack() = default;

// https://w3c.github.io/mediacapture-main/#mediastreamtrack
GC::Ref<MediaStreamTrack> MediaStreamTrack::create(MediaStreamTrackKind kind, Optional<Utf16String> label, bool muted)
{
    // https://w3c.github.io/mediacapture-main/#dfn-create-a-mediastreamtrack
    // 1. Let track be a new object of type source's MediaStreamTrack source type.
    auto track = GC::Heap::the().allocate<MediaStreamTrack>();

    // Initialize track with the following internal slots.
    // FIXME: [[Source]], initialized to source.

    // [[Id]]: See MediaStream.id attribute for guidelines on how to generate such an identifier.
    track->m_id = Utf16String::from_utf8(Crypto::generate_random_uuid());

    // [[Kind]]: "audio" if source is an audio source, or "video" if source is a video source.
    track->m_kind = kind;

    // [[Label]]: source label or empty string.
    track->m_label = label.value_or({});

    // [[ReadyState]]: "live".
    track->m_state = MediaStreamTrackState::Live;

    // [[Enabled]]: true.
    track->m_enabled = true;

    // [[Muted]]: true if source is muted, false otherwise.
    track->m_muted = muted;
    track->update_audio_silence_state();

    // [[Capabilities]], [[Constraints]], and [[Settings]], all initialized as specified in the ConstrainablePattern.
    // [[Restrictable]], initialized to false.
    track->m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    // FIXME: 2. If mediaDevicesToTieSourceTo is not null, tie track source to MediaDevices with source and mediaDevicesToTieSourceTo.
    // FIXME: 3. Run source's MediaStreamTrack source-specific construction steps with track as parameter.

    // 4. Return track.
    return track;
}

void MediaStreamTrack::set_settings(MediaTrackSettings settings)
{
    m_settings = move(settings);
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-enabled
void MediaStreamTrack::set_enabled(bool enabled)
{
    // On setting, it must be set to the new value.
    m_enabled = enabled;

    // If a track is disabled, meaning enabled is false, the track's source only provides
    // silence (for audio) or black frames (for video). The source itself keeps running so
    // that re-enabling the track is cheap.
    update_audio_silence_state();
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-stop
void MediaStreamTrack::stop()
{
    // 1. Let track be the current MediaStreamTrack object.
    // 2. If track's [[ReadyState]] is "ended", then abort these steps.
    if (m_state == MediaStreamTrackState::Ended)
        return;

    // 3. Notify track's source that track is ended.
    // For device-backed audio tracks this releases the capture stream.
    // FIXME: Model track sources properly so tracks sharing a source keep it alive.
    stop_audio_capture();

    // 4. Set track's [[ReadyState]] to "ended".
    m_state = MediaStreamTrackState::Ended;

    update_audio_silence_state();
}

void MediaStreamTrack::end()
{
    if (m_state == MediaStreamTrackState::Ended)
        return;
    stop();
    dispatch_event(DOM::Event::create(HTML::EventNames::ended,
        HighResolutionTime::current_high_resolution_time(HTML::current_global_object())));
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-clone
GC::Ref<MediaStreamTrack> MediaStreamTrack::clone() const
{
    // When the clone() method is invoked, the User Agent MUST return the result of clone a track with this.
    // https://w3c.github.io/mediacapture-main/#clone-a-track
    // 1. Let track be the MediaStreamTrack object to be cloned.
    //
    // FIXME: 2. Let source be track's [[Source]].

    // 3. Let trackClone be the result of creating a MediaStreamTrack with source and null.
    auto track_clone = create(m_kind, m_label, m_muted);

    // 4. Set trackClone's [[ReadyState]] to track's [[ReadyState]] value.
    track_clone->m_state = m_state;
    // 5. FIXME: Set trackClone's [[Capabilities]] to a clone of track's [[Capabilities]].
    // 6. Set trackClone's [[Constraints]] to a clone of track's [[Constraints]].
    track_clone->m_constraints = m_constraints;
    // 7. Set trackClone's [[Settings]] to a clone of track's [[Settings]].
    track_clone->m_settings = m_settings;

    // Initialize the remaining internal slots to match the source track.
    track_clone->m_enabled = m_enabled;
    track_clone->update_audio_silence_state();
    track_clone->m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    // FIXME: 8. Run source MediaStreamTrack source-specific clone steps with track and trackClone as parameters.

    // 9. Return trackClone.
    return track_clone;
}

bool MediaStreamTrack::is_audio() const
{
    return m_kind == MediaStreamTrackKind::Audio;
}

bool MediaStreamTrack::is_video() const
{
    return m_kind == MediaStreamTrackKind::Video;
}

Optional<Utf16String> MediaStreamTrack::device_id() const
{
    return m_settings.device_id;
}

u32 MediaStreamTrack::sample_rate_hz() const
{
    if (m_settings.sample_rate.has_value())
        return *m_settings.sample_rate;
    return 0;
}

u32 MediaStreamTrack::channel_count() const
{
    if (m_settings.channel_count.has_value())
        return *m_settings.channel_count;
    return 0;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-getconstraints
MediaTrackConstraints const& MediaStreamTrack::get_constraints() const
{
    return m_constraints;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-getsettings
MediaTrackSettings const& MediaStreamTrack::get_settings() const
{
    return m_settings;
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-applyconstraints
GC::Ref<WebIDL::Promise> MediaStreamTrack::apply_constraints(JS::Object const& relevant_global_object, Optional<MediaTrackConstraints> constraints)
{
    apply_constraints_impl(move(constraints));
    return WebIDL::create_resolved_promise_for(relevant_global_object, JS::js_undefined());
}

// https://w3c.github.io/mediacapture-main/#dom-mediastreamtrack-applyconstraints
void MediaStreamTrack::apply_constraints_impl(Optional<MediaTrackConstraints> constraints)
{
    if (constraints.has_value())
        m_constraints = constraints.release_value();

    // FIXME: Apply constraints to the underlying source and update settings.
}

void MediaStreamTrack::add_audio_sink(NonnullRefPtr<AudioFrameSink> sink)
{
    auto was_first_sink = m_audio_fanout->add_sink(move(sink));

    // Synthetic tracks have no deviceId and are fed through deliver_audio_frames().
    if (was_first_sink && is_audio() && m_state == MediaStreamTrackState::Live && device_id().has_value())
        ensure_audio_capture_started();
}

void MediaStreamTrack::remove_audio_sink(AudioFrameSink const& sink)
{
    if (m_audio_fanout->remove_sink(sink))
        stop_audio_capture();
}

void MediaStreamTrack::deliver_audio_frames(float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate)
{
    m_audio_fanout->deliver(samples, frame_count, channel_count, sample_rate);
}

void MediaStreamTrack::ensure_audio_capture_started()
{
    if (m_audio_capture_stream || m_audio_capture_start_pending)
        return;

    m_audio_capture_start_pending = true;
    auto capture_request_id = ++m_audio_capture_request_id;

    // FIXME: Support channel layouts beyond mono and stereo.
    auto sample_rate = sample_rate_hz() != 0 ? sample_rate_hz() : 48000;
    auto channel_map = channel_count() == 1 ? Audio::ChannelMap::mono() : Audio::ChannelMap::stereo();
    Audio::SampleSpecification specification(sample_rate, channel_map);

    // Use 20 ms fragments to balance latency and callback overhead.
    auto fragment_frames = sample_rate / 50;
    auto fragment_size_bytes = static_cast<u32>(fragment_frames * channel_map.channel_count() * sizeof(float));

    ByteString device_id_string;
    if (auto id = device_id(); id.has_value())
        device_id_string = id->to_byte_string();

    auto stream_promise = Audio::RecordStream::create(specification, fragment_size_bytes, device_id_string,
        [fanout = m_audio_fanout](ReadonlyBytes data, Audio::SampleSpecification const& stream_specification) {
            // Runs on the audio backend's capture thread. Only the ref-counted fan-out is
            // captured here: the GC may reap the track while a callback is in flight, so the
            // callback must never reference the track itself.
            auto channel_count = stream_specification.channel_count();
            if (channel_count == 0)
                return;
            // RecordStream fragments contain complete interleaved stream frames.
            auto bytes_per_frame = sizeof(float) * channel_count;
            if (data.size() % bytes_per_frame != 0) {
                dbgln("MediaStreamTrack: Dropping unaligned audio capture fragment ({} bytes for {} channels)", data.size(), channel_count);
                return;
            }
            auto frame_count = data.size() / bytes_per_frame;
            if (frame_count == 0)
                return;
            fanout->deliver(reinterpret_cast<float const*>(data.data()), frame_count, channel_count, stream_specification.sample_rate());
        });
    stream_promise->when_resolved([track = GC::Ref(*this), capture_request_id](NonnullRefPtr<Audio::RecordStream>& stream) {
        if (track->m_audio_capture_request_id != capture_request_id)
            return;
        track->m_audio_capture_start_pending = false;
        if (track->m_state != MediaStreamTrackState::Live)
            return;
        track->m_audio_capture_stream = stream;
    });
    stream_promise->when_rejected([track = GC::Ref(*this), capture_request_id](Error& error) {
        if (track->m_audio_capture_request_id != capture_request_id)
            return;
        track->m_audio_capture_start_pending = false;
        dbgln("MediaStreamTrack: Unable to open a capture stream: {}", error);
    });
}

void MediaStreamTrack::stop_audio_capture()
{
    m_audio_capture_start_pending = false;
    ++m_audio_capture_request_id;
    m_audio_capture_stream = nullptr;
}

void MediaStreamTrack::finalize()
{
    Base::finalize();

    // Tearing the capture stream down here cannot race a capture callback into freed memory:
    // the callback only references the atomically ref-counted fan-out, never this GC-managed
    // track.
    stop_audio_capture();
}

}
