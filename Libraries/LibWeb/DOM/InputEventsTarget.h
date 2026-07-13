/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/TextAffinity.h>

namespace Web {

class InputEventsTarget {
public:
    virtual ~InputEventsTarget() = default;

    virtual GC::Ref<JS::Cell> as_cell() = 0;

    virtual void handle_insert(Utf16FlyString const& input_type, Utf16View) = 0;
    virtual EventResult handle_return_key(Utf16FlyString const& input_type) = 0;
    enum class DispatchInputEvent {
        No,
        Yes,
    };
    virtual void handle_delete(Utf16FlyString const& input_type, DispatchInputEvent = DispatchInputEvent::Yes) = 0;

    // The node that mouse-driven selection through this target is constrained to (e.g. the text node inside a text
    // control, or the active editing host). Dragging outside it snaps the selection focus to its closest position.
    virtual GC::Ptr<DOM::Node> mouse_selection_scope() = 0;

    virtual void select_all() = 0;
    virtual void set_selection_anchor(GC::Ref<DOM::Node>, size_t offset, TextAffinity = TextAffinity::Downstream) = 0;
    virtual void set_selection_focus(GC::Ref<DOM::Node>, size_t offset, TextAffinity = TextAffinity::Downstream) = 0;
    enum class CollapseSelection {
        No,
        Yes,
    };
    virtual void move_cursor_to_start(CollapseSelection) = 0;
    virtual void move_cursor_to_end(CollapseSelection) = 0;
    virtual void increment_cursor_position_offset(CollapseSelection) = 0;
    virtual void decrement_cursor_position_offset(CollapseSelection) = 0;
    virtual void increment_cursor_position_to_next_word(CollapseSelection) = 0;
    virtual void decrement_cursor_position_to_previous_word(CollapseSelection) = 0;
    virtual void increment_cursor_position_to_next_line(CollapseSelection) = 0;
    virtual void decrement_cursor_position_to_previous_line(CollapseSelection) = 0;
};

}
