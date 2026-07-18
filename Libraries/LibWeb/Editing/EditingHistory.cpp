/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/InputEvent.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWeb/UIEvents/InputTypes.h>

namespace Web::Editing {

GC_DEFINE_ALLOCATOR(UndoStep);
GC_DEFINE_ALLOCATOR(EditingHistory);

// INTEROP: Blink caps its undo stack at 1000 steps; the oldest step is dropped beyond that.
static constexpr size_t maximum_undo_stack_depth = 1000;

UndoStep::UndoStep(GC::Ref<DOM::Node> editing_host, Category category)
    : m_editing_host(editing_host)
    , m_category(category)
    , m_last_merged_category(category)
{
}

static bool is_collapsed(SelectionSnapshot const& snapshot)
{
    return snapshot.anchor_node == snapshot.focus_node && snapshot.anchor_offset == snapshot.focus_offset;
}

// INTEROP: Chromium's typing coalescence: an insertion (typing, line breaks, paragraph breaks)
//          joins any open unit, while a deletion only joins an open unit whose most recent
//          action was the same kind of deletion. Switching from typing to Backspace starts a new
//          unit, but typing right after a run of deletions extends that run's unit.
bool UndoStep::accepts_merge_of(Category category)
{
    switch (category) {
    case Category::Insertion:
        return true;
    case Category::BackwardDeletion:
        return m_last_merged_category == Category::BackwardDeletion;
    case Category::ForwardDeletion:
        return m_last_merged_category == Category::ForwardDeletion;
    case Category::Other:
        return false;
    }
    VERIFY_NOT_REACHED();
}

void UndoStep::merge(UndoStep& other)
{
    VERIFY(&other != this);
    m_commands.extend(other.m_commands);
    m_ending_selection = other.m_ending_selection;
    m_last_merged_category = other.m_category;

    // INTEROP: Chromium makes undoing a run of backspaces select everything that was deleted:
    //          the starting selection keeps its anchor at the caret before the first backspace
    //          and moves its focus to the caret after the most recent one.
    if (other.m_category == Category::BackwardDeletion && is_collapsed(other.m_ending_selection)) {
        m_starting_selection.focus_node = m_ending_selection.focus_node;
        m_starting_selection.focus_offset = m_ending_selection.focus_offset;
    }
}

void UndoStep::finalize_starting_selection()
{
    if (!is_collapsed(m_starting_selection))
        return;

    // INTEROP: Chromium expands a collapsed selection to the content a deletion is about to
    //          remove before deleting it, and undo restores that expanded selection. Reproduce
    //          the effect from the recorded commands: a backward deletion keeps its anchor at
    //          the original caret with the focus at the caret after deleting, and a forward
    //          deletion selects the first removed run of text.
    if (m_category == Category::BackwardDeletion) {
        if (is_collapsed(m_ending_selection) && m_ending_selection.focus_node) {
            m_starting_selection.focus_node = m_ending_selection.focus_node;
            m_starting_selection.focus_offset = m_ending_selection.focus_offset;
        }
        return;
    }
    if (m_category == Category::ForwardDeletion && !m_commands.is_empty()) {
        if (auto* replace_data_command = as_if<ReplaceDataCommand>(*m_commands.first()); replace_data_command
            && !replace_data_command->removed_data().is_empty()) {
            GC::Ptr<DOM::Node> node = replace_data_command->node();
            m_starting_selection.anchor_node = node;
            m_starting_selection.anchor_offset = replace_data_command->offset();
            m_starting_selection.focus_node = node;
            m_starting_selection.focus_offset = replace_data_command->offset() + replace_data_command->removed_data().length_in_code_units();
        }
    }
}

// INTEROP: Chromium ends an open typing unit when a node near the selection is removed, e.g.
//          when typing into a fresh paragraph removes its placeholder br. A node removal that
//          was part of moving the node elsewhere does not count; only removals whose node is
//          still detached when the command finishes do.
bool UndoStep::performed_lasting_node_removal() const
{
    for (auto command : m_commands) {
        if (command->is_lasting_node_removal())
            return true;
    }
    return false;
}

void UndoStep::unapply()
{
    for (auto command : m_commands.in_reverse())
        command->unapply();
}

void UndoStep::reapply()
{
    for (auto command : m_commands)
        command->reapply();
}

void UndoStep::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_editing_host);
    visitor.visit(m_commands);
    visitor.visit(m_starting_selection.anchor_node);
    visitor.visit(m_starting_selection.focus_node);
    visitor.visit(m_ending_selection.anchor_node);
    visitor.visit(m_ending_selection.focus_node);
}

GC::Ref<EditingHistory> EditingHistory::create(JS::Realm& realm)
{
    return realm.heap().allocate<EditingHistory>();
}

static SelectionSnapshot capture_selection(DOM::Document& document)
{
    SelectionSnapshot snapshot;
    auto selection = document.get_selection();
    if (!selection || !selection->range())
        return snapshot;
    snapshot.anchor_node = selection->anchor_node();
    snapshot.anchor_offset = selection->anchor_offset();
    snapshot.focus_node = selection->focus_node();
    snapshot.focus_offset = selection->focus_offset();
    snapshot.focus_affinity = selection->focus_affinity();
    return snapshot;
}

// INTEROP: Chromium fires a non-cancelable input event with inputType "historyUndo" or
//          "historyRedo" at the undo step's editing host after applying it, for both
//          execCommand() and the keyboard shortcuts.
static void dispatch_history_input_event(DOM::Document& document, GC::Ref<DOM::Node> editing_host, Utf16FlyString const& input_type)
{
    Bindings::InputEventInit event_init {};
    event_init.bubbles = true;
    event_init.input_type = input_type;

    auto event = UIEvents::InputEvent::create_from_platform_event(document.realm(), HTML::EventNames::input, event_init);
    event->set_is_trusted(true);
    editing_host->dispatch_event(event);
}

void EditingHistory::begin_recording(DOM::Node& editing_host, UndoStep::Category category)
{
    // NB: Editing commands cannot nest: execCommand() refuses to run recursively, and history
    //     application never records. A nested recording would mean an editing command invoked
    //     another one through some new path without thinking about history bookkeeping.
    VERIFY(!m_undo_step_being_recorded);
    VERIFY(!m_applying_history_step);

    m_undo_step_being_recorded = editing_host.heap().allocate<UndoStep>(editing_host, category);
    m_undo_step_being_recorded->set_starting_selection(capture_selection(editing_host.document()));
}

void EditingHistory::end_recording()
{
    auto step = m_undo_step_being_recorded;
    if (!step)
        return;
    m_undo_step_being_recorded = nullptr;

    // A command that did not modify the DOM leaves no trace in the history.
    if (!step->has_commands())
        return;

    step->set_ending_selection(capture_selection(step->editing_host()->document()));

    // NB: Decide this before merging, since merging moves the commands into the open step.
    bool closes_coalescing = step->performed_lasting_node_removal();

    if (m_open_step && !m_undo_stack.is_empty() && m_open_step == m_undo_stack.last().ptr()
        && m_open_step->editing_host() == step->editing_host()
        && m_open_step->accepts_merge_of(step->category())) {
        // NB: The redo stack is necessarily empty here: it only fills up through undo, which
        //     ends coalescing, so a merge can never bypass the redo invalidation below.
        m_open_step->merge(*step);
    } else {
        step->finalize_starting_selection();

        // A new editing action discards everything that was undone.
        m_redo_stack.clear();

        if (m_undo_stack.size() >= maximum_undo_stack_depth)
            m_undo_stack.take_first();
        m_undo_stack.append(*step);
        m_open_step = step->category() != UndoStep::Category::Other ? step.ptr() : nullptr;
    }

    if (closes_coalescing)
        m_open_step = nullptr;
}

void EditingHistory::selection_changed()
{
    // Selection changes performed by the recorded command itself or by history application do
    // not end coalescence; everything else (caret movement, clicks, script) does.
    if (m_undo_step_being_recorded || m_applying_history_step)
        return;
    m_open_step = nullptr;
}

bool EditingHistory::can_undo()
{
    prune_steps_for_disconnected_hosts();
    return !m_undo_stack.is_empty();
}

bool EditingHistory::can_redo()
{
    prune_steps_for_disconnected_hosts();
    return !m_redo_stack.is_empty();
}

bool EditingHistory::undo(DOM::Document& document)
{
    if (m_applying_history_step || m_undo_step_being_recorded)
        return false;
    if (!can_undo())
        return false;

    m_open_step = nullptr;
    auto step = m_undo_stack.take_last();
    {
        TemporaryChange applying { m_applying_history_step, true };
        step->unapply();
    }
    restore_selection(document, step->starting_selection());
    m_redo_stack.append(step);

    dispatch_history_input_event(document, step->editing_host(), UIEvents::InputTypes::historyUndo);
    return true;
}

bool EditingHistory::redo(DOM::Document& document)
{
    if (m_applying_history_step || m_undo_step_being_recorded)
        return false;
    if (!can_redo())
        return false;

    m_open_step = nullptr;
    auto step = m_redo_stack.take_last();
    {
        TemporaryChange applying { m_applying_history_step, true };
        step->reapply();
    }
    restore_selection(document, step->ending_selection());
    m_undo_stack.append(step);

    dispatch_history_input_event(document, step->editing_host(), UIEvents::InputTypes::historyRedo);
    return true;
}

void EditingHistory::prune_steps_for_disconnected_hosts()
{
    // INTEROP: Blink erases the undo steps owned by a root editable element when it is removed
    //          from the document (except in designMode documents, where the host may well be
    //          reinserted). We prune lazily whenever the stacks are consulted instead, which is
    //          observably equivalent for command state queries and for undo/redo themselves, and
    //          preserves the history when a host is briefly detached and reinserted.
    m_undo_stack.remove_all_matching([](GC::Ref<UndoStep> const& step) { return !step->editing_host()->is_connected(); });
    m_redo_stack.remove_all_matching([](GC::Ref<UndoStep> const& step) { return !step->editing_host()->is_connected(); });
}

void EditingHistory::restore_selection(DOM::Document& document, SelectionSnapshot const& snapshot)
{
    auto selection = document.get_selection();
    if (!selection)
        return;

    // NB: Scripts may have removed or moved the captured endpoints since the step was recorded;
    //     leave the selection alone if they are no longer valid, like Blink does.
    auto anchor = snapshot.anchor_node;
    auto focus = snapshot.focus_node;
    if (!anchor || !focus)
        return;
    if (!anchor->is_connected() || !focus->is_connected())
        return;
    if (&anchor->document() != &document || &focus->document() != &document)
        return;
    if (snapshot.anchor_offset > anchor->length() || snapshot.focus_offset > focus->length())
        return;

    MUST(selection->set_base_and_extent(*anchor, snapshot.anchor_offset, *focus, snapshot.focus_offset));
    selection->set_focus_affinity(snapshot.focus_affinity);
}

void EditingHistory::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_undo_step_being_recorded);
    visitor.visit(m_open_step);
    visitor.visit(m_undo_stack);
    visitor.visit(m_redo_stack);
}

}
