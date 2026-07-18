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
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/Page/EventResult.h>
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

    // Text controls have their own selection instead of a document selection.
    GC::Ptr<HTML::HTMLElement> text_control;
    size_t text_control_start { 0 };
    size_t text_control_end { 0 };
    HTML::SelectionDirection text_control_direction { HTML::SelectionDirection::None };
};

// One user-level editing action: the ordered list of reversible edit commands it performed, and
// the selections to restore when moving through history. Undoing a step unapplies its commands
// in reverse order and restores the starting selection; redoing reapplies them in order and
// restores the ending selection.
class UndoStep final : public JS::Cell {
    GC_CELL(UndoStep, JS::Cell);
    GC_DECLARE_ALLOCATOR(UndoStep);

public:
    // The kind of user action that created (or was merged into) a step, used to decide which
    // subsequent actions may coalesce into it.
    enum class Category : u8 {
        Insertion,
        BackwardDeletion,
        ForwardDeletion,
        Other,
    };

    GC::Ref<DOM::Node> editing_host() const { return m_editing_host; }
    Category category() const { return m_category; }

    void add_command(GC::Ref<EditCommand> command) { m_commands.append(command); }
    bool has_commands() const { return !m_commands.is_empty(); }

    SelectionSnapshot const& starting_selection() const { return m_starting_selection; }
    void set_starting_selection(SelectionSnapshot const& snapshot) { m_starting_selection = snapshot; }
    SelectionSnapshot const& ending_selection() const { return m_ending_selection; }
    void set_ending_selection(SelectionSnapshot const& snapshot) { m_ending_selection = snapshot; }

    bool unapply();
    bool reapply();

    bool accepts_merge_of(Category);
    void merge(UndoStep&);
    void finalize_starting_selection();
    bool performed_lasting_node_removal() const;

    // INTEROP: Chromium's text controls end a typing unit one character into a new line (their
    //          internal editor removes a placeholder br there); the recording site sets this
    //          after a line break so the next merged action closes the unit.
    bool closes_after_next_merge() const { return m_closes_after_next_merge; }
    void set_closes_after_next_merge() { m_closes_after_next_merge = true; }

private:
    UndoStep(GC::Ref<DOM::Node> editing_host, Category);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Node> m_editing_host;
    Category m_category { Category::Other };
    Category m_last_merged_category { Category::Other };
    bool m_closes_after_next_merge { false };
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

    void begin_recording(DOM::Node& editing_host, UndoStep::Category);
    void end_recording();

    bool can_undo();
    bool can_redo();
    bool undo(DOM::Document&);
    bool redo(DOM::Document&);

    GC::Ptr<UndoStep> next_undo_step();
    GC::Ptr<UndoStep> next_redo_step();

    // Called whenever the document selection changes; ends typing coalescence unless the change
    // came from an editing command or from history application itself.
    void selection_changed();

private:
    EditingHistory() = default;

    virtual void visit_edges(Cell::Visitor&) override;

    void prune_steps_for_disconnected_hosts();
    void restore_selection(DOM::Document&, SelectionSnapshot const&);
    void notify_state_if_changed(DOM::Document&);

    GC::Ptr<UndoStep> m_undo_step_being_recorded;
    GC::Ptr<UndoStep> m_open_step;
    Vector<GC::Ref<UndoStep>> m_undo_stack;
    Vector<GC::Ref<UndoStep>> m_redo_stack;
    bool m_applying_history_step { false };
    bool m_last_notified_can_undo { false };
    bool m_last_notified_can_redo { false };
};

enum class HistoryAction : u8 {
    Undo,
    Redo,
};

// The user-initiated (keyboard shortcut) entry point for undo and redo. Unlike execCommand(),
// this dispatches a cancelable beforeinput event first; canceling it leaves both the DOM and the
// history untouched.
EventResult perform_history_action(DOM::Document&, HistoryAction);

}
