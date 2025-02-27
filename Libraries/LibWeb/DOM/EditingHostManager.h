/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
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

    virtual void handle_insert(String const&) override;
    virtual void handle_delete(DeleteDirection) override;
    virtual void handle_return_key() override;
    virtual void select_all() override;
    virtual void set_selection_anchor(GC::Ref<DOM::Node>, size_t offset) override;
    virtual void set_selection_focus(GC::Ref<DOM::Node>, size_t offset) override;
    virtual void move_cursor_to_start(CollapseSelection) override;
    virtual void move_cursor_to_end(CollapseSelection) override;
    virtual void increment_cursor_position_offset(CollapseSelection) override;
    virtual void decrement_cursor_position_offset(CollapseSelection) override;
    virtual void increment_cursor_position_to_next_word(CollapseSelection) override;
    virtual void decrement_cursor_position_to_previous_word(CollapseSelection) override;

    virtual void visit_edges(Cell::Visitor& visitor) override;

    void set_active_contenteditable_element(GC::Ptr<DOM::Node> element)
    {
        m_active_contenteditable_element = element;
    }

private:
    EditingHostManager(GC::Ref<Document>);

    virtual GC::Ref<JS::Cell> as_cell() override { return *this; }

    GC::Ref<Document> m_document;
    GC::Ptr<DOM::Node> m_active_contenteditable_element;
};

}
