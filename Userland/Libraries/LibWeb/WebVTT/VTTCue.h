/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/VTTCuePrototype.h>
#include <LibWeb/HTML/TextTrackCue.h>
#include <LibWeb/WebIDL/Types.h>
#include <LibWeb/WebVTT/VTTRegion.h>

namespace Web::WebVTT {

// https://w3c.github.io/webvtt/#vttcue
class VTTCue final : public HTML::TextTrackCue {
    WEB_PLATFORM_OBJECT(VTTCue, HTML::TextTrackCue);
    JS_DECLARE_ALLOCATOR(VTTCue);

public:
    enum class WritingDirection : u8 {
        // https://w3c.github.io/webvtt/#webvtt-cue-horizontal-writing-direction
        Horizontal,

        // https://w3c.github.io/webvtt/#webvtt-cue-vertical-growing-left-writing-direction
        VerticalGrowingLeft,

        // https://w3c.github.io/webvtt/#webvtt-cue-vertical-growing-right-writing-direction
        VerticalGrowingRight,
    };

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<VTTCue>> construct_impl(JS::Realm&, double start_time, double end_time, String const& text);
    virtual ~VTTCue() override = default;

    JS::GCPtr<VTTRegion> region() const { return m_region; }
    void set_region(JS::GCPtr<VTTRegion> region) { m_region = region; }

    Bindings::DirectionSetting vertical() const;
    void set_vertical(Bindings::DirectionSetting);

    bool snap_to_lines() const { return m_snap_to_lines; }
    void set_snap_to_lines(bool snap_to_lines) { m_snap_to_lines = snap_to_lines; }

    Bindings::LineAlignSetting line_align() const { return m_line_alignment; }
    void set_line_align(Bindings::LineAlignSetting line_align) { m_line_alignment = line_align; }

    Bindings::PositionAlignSetting position_align() const { return m_position_alignment; }
    void set_position_align(Bindings::PositionAlignSetting position_align) { m_position_alignment = position_align; }

    double size() const { return m_size; }
    void set_size(double size) { m_size = size; }

    Bindings::AlignSetting align() const { return m_text_alignment; }
    void set_align(Bindings::AlignSetting align) { m_text_alignment = align; }

    String const& text() const { return m_text; }
    void set_text(String const& text) { m_text = text; }

protected:
    Bindings::PositionAlignSetting computed_position_alignment();

private:
    VTTCue(JS::Realm&, JS::GCPtr<HTML::TextTrack>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    // https://w3c.github.io/webvtt/#cue-text
    String m_text;

    // https://w3c.github.io/webvtt/#webvtt-cue-writing-direction
    WritingDirection m_writing_direction { WritingDirection::Horizontal };

    // https://w3c.github.io/webvtt/#webvtt-cue-snap-to-lines-flag
    bool m_snap_to_lines { true };

    // https://w3c.github.io/webvtt/#webvtt-cue-line-alignment
    Bindings::LineAlignSetting m_line_alignment { Bindings::LineAlignSetting::Start };

    // https://w3c.github.io/webvtt/#webvtt-cue-position-alignment
    Bindings::PositionAlignSetting m_position_alignment { Bindings::PositionAlignSetting::Auto };

    // https://w3c.github.io/webvtt/#webvtt-cue-size
    double m_size { 100 };

    // https://w3c.github.io/webvtt/#webvtt-cue-text-alignment
    Bindings::AlignSetting m_text_alignment { Bindings::AlignSetting::Center };

    // https://w3c.github.io/webvtt/#webvtt-cue-region
    JS::GCPtr<VTTRegion> m_region;
};

}
