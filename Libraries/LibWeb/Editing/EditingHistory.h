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
#include <LibWeb/TextAffinity.h>

namespace Web::Editing {

// Snapshot of the document selection around an undo step. Raw endpoints are stored instead of a
// live range so that later DOM mutation cannot drag them around; restoration re-validates the
// endpoints and is skipped when they are no longer meaningful. Storing anchor and focus
// separately preserves the selection's direction.
struct SelectionSnapshot {
    GC::Ptr<DOM::Node> anchor_node;
    size_t anchor_offset { 0 };
    GC::Ptr<DOM::Node> focus_node;
    size_t focus_offset { 0 };
    TextAffinity focus_affinity { TextAffinity::Downstream };
};

// One user-level editing action: the ordered list of reversible edit commands it performed, and
// the selections to restore when moving through history. Undoing a step unapplies its commands
// in reverse order and restores the starting selection; redoing reapplies them in order and
// restores the ending selection.
class UndoStep final : public JS::Cell {
    GC_CELL(UndoStep, JS::Cell);
    GC_DECLARE_ALLOCATOR(UndoStep);

public:
    GC::Ref<DOM::Node> editing_host() const { return m_editing_host; }

    void add_command(GC::Ref<EditCommand> command) { m_commands.append(command); }
    bool has_commands() const { return !m_commands.is_empty(); }

    SelectionSnapshot const& starting_selection() const { return m_starting_selection; }
    void set_starting_selection(SelectionSnapshot const& snapshot) { m_starting_selection = snapshot; }
    SelectionSnapshot const& ending_selection() const { return m_ending_selection; }
    void set_ending_selection(SelectionSnapshot const& snapshot) { m_ending_selection = snapshot; }

    void unapply();
    void reapply();

private:
    explicit UndoStep(GC::Ref<DOM::Node> editing_host);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Node> m_editing_host;
    Vector<GC::Ref<EditCommand>> m_commands;
    SelectionSnapshot m_starting_selection;
    SelectionSnapshot m_ending_selection;
};

// Per-document history of user editing actions, shared by all editing hosts in the document.
// Modeled on the undo stacks in Blink and Gecko: each entry is an UndoStep that knows how to
// reverse and replay itself, together with the selections to restore around it. Only mutations
// performed by editing commands enter the history; script-driven DOM mutation is invisible to it.
class EditingHistory final : public JS::Cell {
    GC_CELL(EditingHistory, JS::Cell);
    GC_DECLARE_ALLOCATOR(EditingHistory);

public:
    [[nodiscard]] static GC::Ref<EditingHistory> create(JS::Realm&);

    // The undo step for the editing command currently executing, if any. DOM mutations made
    // through the Editing proxy functions are recorded onto this step.
    GC::Ptr<UndoStep> undo_step_being_recorded() { return m_undo_step_being_recorded; }

    void begin_recording(DOM::Node& editing_host);
    void end_recording();

    bool can_undo();
    bool can_redo();
    bool undo(DOM::Document&);
    bool redo(DOM::Document&);

private:
    EditingHistory() = default;

    virtual void visit_edges(Cell::Visitor&) override;

    void prune_steps_for_disconnected_hosts();
    void restore_selection(DOM::Document&, SelectionSnapshot const&);

    GC::Ptr<UndoStep> m_undo_step_being_recorded;
    Vector<GC::Ref<UndoStep>> m_undo_stack;
    Vector<GC::Ref<UndoStep>> m_redo_stack;
    bool m_applying_history_step { false };
};

}
