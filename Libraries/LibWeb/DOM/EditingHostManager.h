/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/DOM/InputEventsTarget.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class EditingHostManager
    : public JS::Cell
    , public InputEventsTarget {
    GC_CELL(EditingHostManager, JS::Cell);
    GC_DECLARE_ALLOCATOR(EditingHostManager);

public:
    [[nodiscard]] static GC::Ref<EditingHostManager> create(JS::Realm&, GC::Ref<Document>);

    virtual void handle_insert(FlyString const& input_type, Utf16String const&) override;
    virtual void handle_delete(FlyString const& input_type) override;
    virtual EventResult handle_return_key(FlyString const& ui_input_type) override;
    virtual void select_all() override;
    virtual void set_selection_anchor(GC::Ref<DOM::Node>, size_t offset) override;
    virtual void set_selection_focus(GC::Ref<DOM::Node>, size_t offset) override;
    virtual void move_cursor_to_start(CollapseSelection) override;
    virtual void move_cursor_to_end(CollapseSelection) override;
    virtual void increment_cursor_position_offset(CollapseSelection) override;
    virtual void decrement_cursor_position_offset(CollapseSelection) override;
    virtual void increment_cursor_position_to_next_word(CollapseSelection) override;
    virtual void decrement_cursor_position_to_previous_word(CollapseSelection) override;
    virtual void increment_cursor_position_to_next_line(CollapseSelection) override;
    virtual void decrement_cursor_position_to_previous_line(CollapseSelection) override;

    virtual void visit_edges(Cell::Visitor& visitor) override;
    bool is_within_active_contenteditable(DOM::Node const& node) const;
    void set_active_contenteditable_element(GC::Ptr<DOM::Node> element)
    {
        m_active_contenteditable_element = element;
    }

private:
    EditingHostManager(GC::Ref<Document>);

    virtual GC::Ref<JS::Cell> as_cell() override { return *this; }

    GC::Ptr<Selection::Selection> get_selection_for_navigation(CollapseSelection) const;

    GC::Ref<Document> m_document;
    GC::Ptr<DOM::Node> m_active_contenteditable_element;
};

}
