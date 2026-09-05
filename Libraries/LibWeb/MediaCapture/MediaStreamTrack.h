/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibSync/Mutex.h>
#include <LibWeb/Bindings/MediaStreamConstraints.h>
#include <LibWeb/Bindings/MediaStreamTrack.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Audio {

class RecordStream;

}

namespace Web::MediaCapture {

using MediaStreamTrackKind = Bindings::MediaStreamTrackKind;
using MediaStreamTrackState = Bindings::MediaStreamTrackState;
using ConstrainBooleanOrDOMStringParameters = Bindings::ConstrainBooleanOrDOMStringParameters;
using ConstrainBooleanParameters = Bindings::ConstrainBooleanParameters;
using ConstrainDOMStringParameters = Bindings::ConstrainDOMStringParameters;
using ConstrainDoubleRange = Bindings::ConstrainDoubleRange;
using ConstrainULongRange = Bindings::ConstrainULongRange;
using DoubleRange = Bindings::DoubleRange;
using MediaTrackCapabilities = Bindings::MediaTrackCapabilities;
using MediaTrackConstraintSet = Bindings::MediaTrackConstraintSet;
using MediaTrackConstraints = Bindings::MediaTrackConstraints;
using MediaTrackSettings = Bindings::MediaTrackSettings;
using ULongRange = Bindings::ULongRange;

// Receives interleaved float32 PCM on the producing thread. Implementations must hand it off
// without blocking or touching GC-managed objects.
struct AudioFrameSink final : public AtomicRefCounted<AudioFrameSink> {
    void deliver(float const* samples, size_t frames, u8 channels, u32 rate)
    {
        Sync::MutexLocker locker(m_mutex);
        if (on_frames)
            on_frames(samples, frames, channels, rate);
    }
    void deactivate() const
    {
        Sync::MutexLocker locker(m_mutex);
        on_frames = nullptr;
    }
    mutable Sync::Mutex m_mutex;
    mutable Function<void(float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate)> on_frames;
};

// Atomically ref-counted because backend callbacks may outlive the GC-managed track.
class AudioFrameFanout final : public AtomicRefCounted<AudioFrameFanout> {
public:
    // Returns true when this was the first sink to register.
    bool add_sink(NonnullRefPtr<AudioFrameSink>);
    // Returns true when the last sink was removed.
    bool remove_sink(AudioFrameSink const&);

    // While a track is muted or disabled it must render silence rather than nothing, so
    // consumers keep observing the track's timing and format.
    // https://w3c.github.io/mediacapture-main/#dfn-enabled
    void set_silenced(bool silenced) { m_silenced.store(silenced, AK::MemoryOrder::memory_order_relaxed); }

    void deliver(float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate);

private:
    Sync::Mutex m_mutex;
    Vector<NonnullRefPtr<AudioFrameSink>> m_sinks;
    Atomic<bool> m_silenced { false };

    Vector<float> m_scratch;
};

// Spec: https://w3c.github.io/mediacapture-main/#mediastreamtrack
class MediaStreamTrack final : public DOM::EventTarget {
    WEB_WRAPPABLE(MediaStreamTrack, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaStreamTrack);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<MediaStreamTrack> create(MediaStreamTrackKind, Optional<Utf16String> label = {}, bool muted = false);

    // Out-of-line: destroying RefPtr<Audio::RecordStream> requires the complete type.
    virtual ~MediaStreamTrack() override;

    MediaStreamTrackKind track_kind() const { return m_kind; }
    Utf16String const& id() const { return m_id; }
    Utf16String const& label() const { return m_label; }

    bool enabled() const { return m_enabled; }
    void set_enabled(bool enabled);

    bool muted() const { return m_muted; }

    MediaStreamTrackState track_ready_state() const { return m_state; }

    void stop();
    void end();
    GC::Ref<MediaStreamTrack> clone() const;

    bool is_audio() const;
    bool is_video() const;

    MediaTrackCapabilities get_capabilities() const { return {}; }
    MediaTrackConstraints const& get_constraints() const;
    MediaTrackSettings const& get_settings() const;
    GC::Ref<WebIDL::Promise> apply_constraints(JS::Object const& relevant_global_object, Optional<MediaTrackConstraints> constraints);
    void set_settings(MediaTrackSettings settings);

    Optional<Utf16String> device_id() const;
    u32 sample_rate_hz() const;
    u32 channel_count() const;

    u64 provider_id() const { return m_provider_id; }

    // The first sink starts device capture; removing the last stops it.
    void add_audio_sink(NonnullRefPtr<AudioFrameSink>);
    void remove_audio_sink(AudioFrameSink const&);

    void deliver_audio_frames(float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate);

private:
    explicit MediaStreamTrack();

    virtual void finalize() override;

    void apply_constraints_impl(Optional<MediaTrackConstraints> constraints);

    void ensure_audio_capture_started();
    void stop_audio_capture();
    void update_audio_silence_state() { m_audio_fanout->set_silenced(!m_enabled || m_muted || m_state == MediaStreamTrackState::Ended); }

    static Atomic<u64> s_next_provider_id;

    MediaStreamTrackKind m_kind { MediaStreamTrackKind::Audio };
    Utf16String m_id;
    Utf16String m_label;
    bool m_enabled { true };
    bool m_muted { false };
    MediaStreamTrackState m_state { MediaStreamTrackState::Live };

    MediaTrackConstraints m_constraints;
    MediaTrackSettings m_settings;

    u64 m_provider_id { 0 };

    NonnullRefPtr<AudioFrameFanout> m_audio_fanout;
    RefPtr<Audio::RecordStream> m_audio_capture_stream;
    bool m_audio_capture_start_pending { false };
    u64 m_audio_capture_request_id { 0 };
};

}
