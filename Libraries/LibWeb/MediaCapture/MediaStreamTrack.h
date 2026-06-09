/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/MediaStreamConstraints.h>
#include <LibWeb/Bindings/MediaStreamTrack.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

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

// Spec: https://w3c.github.io/mediacapture-main/#mediastreamtrack
class MediaStreamTrack final : public DOM::EventTarget {
    WEB_WRAPPABLE(MediaStreamTrack, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaStreamTrack);

public:
    static GC::Ref<MediaStreamTrack> create(MediaStreamTrackKind, Optional<String> label = {}, bool muted = false);

    virtual ~MediaStreamTrack() override = default;

    MediaStreamTrackKind track_kind() const { return m_kind; }
    String id() const { return m_id; }
    String label() const { return m_label; }

    bool enabled() const { return m_enabled; }
    void set_enabled(bool enabled) { m_enabled = enabled; }

    bool muted() const { return m_muted; }

    MediaStreamTrackState track_ready_state() const { return m_state; }

    void stop();
    GC::Ref<MediaStreamTrack> clone() const;

    bool is_audio() const;
    bool is_video() const;

    MediaTrackCapabilities get_capabilities() const { return {}; }
    MediaTrackConstraints const& get_constraints() const;
    MediaTrackSettings const& get_settings() const;
    GC::Ref<WebIDL::Promise> apply_constraints(JS::Realm&, Optional<MediaTrackConstraints> constraints);
    void set_settings(MediaTrackSettings settings);

    Optional<String> device_id() const;
    u32 sample_rate_hz() const;
    u32 channel_count() const;

    u64 provider_id() const { return m_provider_id; }

private:
    explicit MediaStreamTrack();

    void apply_constraints_impl(Optional<MediaTrackConstraints> constraints);

    static Atomic<u64> s_next_provider_id;

    MediaStreamTrackKind m_kind { MediaStreamTrackKind::Audio };
    String m_id;
    String m_label;
    bool m_enabled { true };
    bool m_muted { false };
    MediaStreamTrackState m_state { MediaStreamTrackState::Live };

    MediaTrackConstraints m_constraints;
    MediaTrackSettings m_settings;

    u64 m_provider_id { 0 };
};

}
