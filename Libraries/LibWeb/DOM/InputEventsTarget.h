/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web {

class InputEventsTarget {
public:
    virtual ~InputEventsTarget() = default;

    virtual GC::Ref<JS::Cell> as_cell() = 0;

    virtual void handle_insert(String const&) = 0;
    virtual void handle_return_key() = 0;

    enum class DeleteDirection {
        Backward,
        Forward,
    };
    virtual void handle_delete(DeleteDirection) = 0;

    virtual void select_all() = 0;
    virtual void set_selection_anchor(GC::Ref<DOM::Node>, size_t offset) = 0;
    virtual void set_selection_focus(GC::Ref<DOM::Node>, size_t offset) = 0;
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
};

}
