/*
 * Copyright (c) 2024-2025, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/VTTCue.h>
#include <LibWeb/HTML/TextTrackCue.h>
#include <LibWeb/WebIDL/Types.h>
#include <LibWeb/WebVTT/VTTRegion.h>

namespace Web::WebVTT {

using AutoKeyword = Bindings::AutoKeyword;
using DirectionSetting = Bindings::DirectionSetting;
using LineAlignSetting = Bindings::LineAlignSetting;
using PositionAlignSetting = Bindings::PositionAlignSetting;
using AlignSetting = Bindings::AlignSetting;

// https://w3c.github.io/webvtt/#vttcue
class VTTCue final : public HTML::TextTrackCue {
    WEB_WRAPPABLE(VTTCue, HTML::TextTrackCue);
    GC_DECLARE_ALLOCATOR(VTTCue);

public:
    enum class WritingDirection : u8 {
        // https://w3c.github.io/webvtt/#webvtt-cue-horizontal-writing-direction
        Horizontal,

        // https://w3c.github.io/webvtt/#webvtt-cue-vertical-growing-left-writing-direction
        VerticalGrowingLeft,

        // https://w3c.github.io/webvtt/#webvtt-cue-vertical-growing-right-writing-direction
        VerticalGrowingRight,
    };

    using LineAndPositionSetting = Variant<double, AutoKeyword>;

    static WebIDL::ExceptionOr<GC::Ref<VTTCue>> create(double start_time, double end_time, String const& text);
    virtual ~VTTCue() override = default;

    GC::Ptr<VTTRegion> region() const { return m_region; }
    void set_region(GC::Ptr<VTTRegion> region) { m_region = region; }

    DirectionSetting vertical() const;
    void set_vertical(DirectionSetting);

    bool snap_to_lines() const { return m_snap_to_lines; }
    void set_snap_to_lines(bool snap_to_lines) { m_snap_to_lines = snap_to_lines; }

    LineAndPositionSetting line() const;
    void set_line(LineAndPositionSetting);

    LineAlignSetting line_align() const;
    void set_line_align(LineAlignSetting);

    LineAndPositionSetting position() const;
    void set_position(LineAndPositionSetting);

    PositionAlignSetting position_align() const;
    void set_position_align(PositionAlignSetting);

    double size() const { return m_size; }
    void set_size(double size) { m_size = size; }

    AlignSetting align() const;
    void set_align(AlignSetting);

    String const& text() const { return m_text; }
    void set_text(String const& text) { m_text = text; }

protected:
    double computed_line();
    double computed_position();
    PositionAlignSetting computed_position_alignment();

private:
    VTTCue(GC::Ptr<HTML::TextTrack>);

    virtual void visit_edges(Visitor&) override;

    // https://w3c.github.io/webvtt/#cue-text
    String m_text;

    // https://w3c.github.io/webvtt/#webvtt-cue-writing-direction
    WritingDirection m_writing_direction { WritingDirection::Horizontal };

    // https://w3c.github.io/webvtt/#webvtt-cue-snap-to-lines-flag
    bool m_snap_to_lines { true };

    // https://w3c.github.io/webvtt/#webvtt-cue-line
    LineAndPositionSetting m_line { AutoKeyword::Auto };

    // https://w3c.github.io/webvtt/#webvtt-cue-line-alignment
    LineAlignSetting m_line_alignment { LineAlignSetting::Start };

    // https://w3c.github.io/webvtt/#webvtt-cue-position
    LineAndPositionSetting m_position { AutoKeyword::Auto };

    // https://w3c.github.io/webvtt/#webvtt-cue-position-alignment
    PositionAlignSetting m_position_alignment { PositionAlignSetting::Auto };

    // https://w3c.github.io/webvtt/#webvtt-cue-size
    double m_size { 100 };

    // https://w3c.github.io/webvtt/#webvtt-cue-text-alignment
    AlignSetting m_text_alignment { AlignSetting::Center };

    // https://w3c.github.io/webvtt/#webvtt-cue-region
    GC::Ptr<VTTRegion> m_region;
};

}
