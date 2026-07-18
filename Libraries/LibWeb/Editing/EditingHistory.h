/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

// One user-level editing action: the ordered list of reversible edit commands it performed.
// Undoing a step unapplies its commands in reverse order; redoing reapplies them in order.
class UndoStep final : public JS::Cell {
    GC_CELL(UndoStep, JS::Cell);
    GC_DECLARE_ALLOCATOR(UndoStep);

public:
    GC::Ref<DOM::Node> editing_host() const { return m_editing_host; }

    void add_command(GC::Ref<EditCommand> command) { m_commands.append(command); }
    bool has_commands() const { return !m_commands.is_empty(); }

    void unapply();
    void reapply();

private:
    explicit UndoStep(GC::Ref<DOM::Node> editing_host);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Node> m_editing_host;
    Vector<GC::Ref<EditCommand>> m_commands;
};

// Per-document history of user editing actions, shared by all editing hosts in the document.
// Modeled on the undo stacks in Blink and Gecko: each entry is an UndoStep that knows how to
// reverse and replay itself, together with the selections to restore around it.
class EditingHistory final : public JS::Cell {
    GC_CELL(EditingHistory, JS::Cell);
    GC_DECLARE_ALLOCATOR(EditingHistory);

public:
    [[nodiscard]] static GC::Ref<EditingHistory> create(JS::Realm&);

    // The undo step for the editing command currently executing, if any. DOM mutations made
    // through the Editing proxy functions are recorded onto this step.
    GC::Ptr<UndoStep> undo_step_being_recorded() { return m_undo_step_being_recorded; }

private:
    EditingHistory() = default;

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<UndoStep> m_undo_step_being_recorded;
    Vector<GC::Ref<UndoStep>> m_undo_stack;
    Vector<GC::Ref<UndoStep>> m_redo_stack;
};

}
