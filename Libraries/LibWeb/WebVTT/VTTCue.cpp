/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebVTT/VTTCue.h>

namespace Web::WebVTT {

JS_DEFINE_ALLOCATOR(VTTCue);

// https://w3c.github.io/webvtt/#dom-vttcue-vttcue
WebIDL::ExceptionOr<JS::NonnullGCPtr<VTTCue>> VTTCue::construct_impl(JS::Realm& realm, double start_time, double end_time, String const& text)
{
    // 1. Create a new WebVTT cue. Let cue be that WebVTT cue.
    auto cue = realm.heap().allocate<VTTCue>(realm, realm, nullptr);

    // 2. Let cue’s text track cue start time be the value of the startTime argument.
    cue->m_start_time = start_time;

    // 3. If the value of the endTime argument is negative Infinity or a Not-a-Number (NaN) value, then throw a TypeError exception.
    //    Otherwise, let cue’s text track cue end time be the value of the endTime argument.
    if (end_time == -AK::Infinity<double> || isnan(end_time))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "End time is negative infinity or NaN"_string };
    cue->m_end_time = end_time;

    // 4. Let cue’s cue text be the value of the text argument, and let the rules for extracting the chapter title be the WebVTT rules
    //    for extracting the chapter title.
    cue->m_text = text;
    // FIXME: let the rules for extracting the chapter title be the WebVTT rules for extracting the chapter title.

    // 5. Let cue’s text track cue identifier be the empty string.
    cue->m_identifier = ""_string;

    // 6. Let cue’s text track cue pause-on-exit flag be false.
    cue->m_pause_on_exit = false;

    // 7. Let cue’s WebVTT cue region be null.
    cue->m_region = nullptr;

    // 8. Let cue’s WebVTT cue writing direction be horizontal.
    cue->m_writing_direction = WritingDirection::Horizontal;

    // 9. Let cue’s WebVTT cue snap-to-lines flag be true.
    cue->m_snap_to_lines = true;

    // FIXME: 10. Let cue’s WebVTT cue line be auto.

    // 11. Let cue’s WebVTT cue line alignment be start alignment.
    cue->m_line_alignment = Bindings::LineAlignSetting::Start;

    // FIXME: 12. Let cue’s WebVTT cue position be auto.

    // 13. Let cue’s WebVTT cue position alignment be auto.
    cue->m_position_alignment = Bindings::PositionAlignSetting::Auto;

    // 14. Let cue’s WebVTT cue size be 100.
    cue->m_size = 100;

    // 15. Let cue’s WebVTT cue text alignment be center alignment.
    cue->m_text_alignment = Bindings::AlignSetting::Center;

    // 16. Return the VTTCue object representing cue.
    return cue;
}

VTTCue::VTTCue(JS::Realm& realm, JS::GCPtr<HTML::TextTrack> track)
    : HTML::TextTrackCue(realm, track)
{
}

void VTTCue::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VTTCue);
}

void VTTCue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_region);
}

// https://w3c.github.io/webvtt/#dom-vttcue-vertical
Bindings::DirectionSetting VTTCue::vertical() const
{
    switch (m_writing_direction) {
    case WritingDirection::Horizontal:
        return Bindings::DirectionSetting::Empty;
    case WritingDirection::VerticalGrowingLeft:
        return Bindings::DirectionSetting::Rl;
    case WritingDirection::VerticalGrowingRight:
        return Bindings::DirectionSetting::Lr;
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/webvtt/#dom-vttcue-vertical
void VTTCue::set_vertical(Bindings::DirectionSetting vertical)
{
    switch (vertical) {
    case Bindings::DirectionSetting::Empty:
        m_writing_direction = WritingDirection::Horizontal;
        break;
    case Bindings::DirectionSetting::Rl:
        m_writing_direction = WritingDirection::VerticalGrowingLeft;
        break;
    case Bindings::DirectionSetting::Lr:
        m_writing_direction = WritingDirection::VerticalGrowingRight;
        break;
    }
}

// https://w3c.github.io/webvtt/#cue-computed-position-alignment
Bindings::PositionAlignSetting VTTCue::computed_position_alignment()
{
    // 1. If the WebVTT cue position alignment is not auto, then return the value of the WebVTT cue position alignment and abort these
    //    steps.
    if (m_position_alignment != Bindings::PositionAlignSetting::Auto)
        return m_position_alignment;

    // 2. If the WebVTT cue text alignment is left, return line-left and abort these steps.
    if (m_text_alignment == Bindings::AlignSetting::Left)
        return Bindings::PositionAlignSetting::LineLeft;

    // 3. If the WebVTT cue text alignment is right, return line-right and abort these steps.
    if (m_text_alignment == Bindings::AlignSetting::Right)
        return Bindings::PositionAlignSetting::LineRight;

    // FIXME: 4. If the WebVTT cue text alignment is start, return line-left if the base direction of the cue text is left-to-right, line-right
    //    otherwise.

    // FIXME: 5. If the WebVTT cue text alignment is end, return line-right if the base direction of the cue text is left-to-right, line-left
    //    otherwise.

    // 6. Otherwise, return center.
    return Bindings::PositionAlignSetting::Center;
}

}
