/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/InputEvent.h>
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

UndoStep::UndoStep(GC::Ref<DOM::Node> editing_host)
    : m_editing_host(editing_host)
{
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

void EditingHistory::begin_recording(DOM::Node& editing_host)
{
    // NB: Editing commands cannot nest: execCommand() refuses to run recursively, and history
    //     application never records. A nested recording would mean an editing command invoked
    //     another one through some new path without thinking about history bookkeeping.
    VERIFY(!m_undo_step_being_recorded);
    VERIFY(!m_applying_history_step);

    m_undo_step_being_recorded = editing_host.heap().allocate<UndoStep>(editing_host);
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

    // A new editing action discards everything that was undone.
    m_redo_stack.clear();

    if (m_undo_stack.size() >= maximum_undo_stack_depth)
        m_undo_stack.take_first();
    m_undo_stack.append(*step);
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
    visitor.visit(m_undo_stack);
    visitor.visit(m_redo_stack);
}

}
