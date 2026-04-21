/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/Bindings/MediaStreamTrack.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MediaCapture/MediaStreamConstraints.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

// Spec: https://w3c.github.io/mediacapture-main/#mediastreamtrack
class MediaStreamTrack final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaStreamTrack, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaStreamTrack);

public:
    static GC::Ref<MediaStreamTrack> create(JS::Realm&, Bindings::MediaStreamTrackKind, Optional<String> label = {}, bool muted = false);

    virtual ~MediaStreamTrack() override = default;

    Bindings::MediaStreamTrackKind kind() const { return m_kind; }
    String id() const { return m_id; }
    String label() const { return m_label; }

    bool enabled() const { return m_enabled; }
    void set_enabled(bool enabled) { m_enabled = enabled; }

    bool muted() const { return m_muted; }

    Bindings::MediaStreamTrackState ready_state() const { return m_state; }

    void stop();
    GC::Ref<MediaStreamTrack> clone() const;

    bool is_audio() const;
    bool is_video() const;

    MediaTrackCapabilities get_capabilities() const;
    MediaTrackConstraints get_constraints() const;
    MediaTrackSettings get_settings() const;
    GC::Ref<WebIDL::Promise> apply_constraints(Optional<MediaTrackConstraints> const& constraints);
    void set_settings(MediaTrackSettings settings);

    Optional<String> device_id() const;
    u32 sample_rate_hz() const;
    u32 channel_count() const;

    u64 provider_id() const { return m_provider_id; }

private:
    explicit MediaStreamTrack(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    static Atomic<u64> s_next_provider_id;

    Bindings::MediaStreamTrackKind m_kind { static_cast<Bindings::MediaStreamTrackKind>(0) };
    String m_id;
    String m_label;
    bool m_enabled { true };
    bool m_muted { false };
    Bindings::MediaStreamTrackState m_state { static_cast<Bindings::MediaStreamTrackState>(0) };

    MediaTrackCapabilities m_capabilities;
    MediaTrackConstraints m_constraints;
    MediaTrackSettings m_settings;

    u64 m_provider_id { 0 };
};

}
