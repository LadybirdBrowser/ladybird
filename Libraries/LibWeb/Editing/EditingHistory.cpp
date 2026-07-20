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
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/EventNames.h>
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
    if (snapshot.text_control)
        return snapshot.text_control_start == snapshot.text_control_end;
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
    m_closes_after_next_merge = other.m_closes_after_next_merge;

    // INTEROP: Chromium makes undoing a run of backspaces select everything that was deleted:
    //          the starting selection keeps its anchor at the caret before the first backspace
    //          and moves its focus to the caret after the most recent one.
    if (other.m_category == Category::BackwardDeletion && is_collapsed(other.m_ending_selection)) {
        if (m_starting_selection.text_control) {
            m_starting_selection.text_control_start = m_ending_selection.text_control_start;
        } else {
            m_starting_selection.focus_node = m_ending_selection.focus_node;
            m_starting_selection.focus_offset = m_ending_selection.focus_offset;
        }
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
        if (!is_collapsed(m_ending_selection))
            return;
        if (m_starting_selection.text_control) {
            m_starting_selection.text_control_start = m_ending_selection.text_control_start;
        } else if (m_ending_selection.focus_node) {
            m_starting_selection.focus_node = m_ending_selection.focus_node;
            m_starting_selection.focus_offset = m_ending_selection.focus_offset;
        }
        return;
    }
    if (m_category == Category::ForwardDeletion && !m_commands.is_empty()) {
        auto* replace_data_command = as_if<ReplaceDataCommand>(*m_commands.first());
        if (!replace_data_command || replace_data_command->removed_data().is_empty())
            return;
        if (m_starting_selection.text_control) {
            m_starting_selection.text_control_start = replace_data_command->offset();
            m_starting_selection.text_control_end = replace_data_command->offset() + replace_data_command->removed_data().length_in_code_units();
            return;
        }
        GC::Ptr<DOM::Node> node = replace_data_command->node();
        m_starting_selection.anchor_node = node;
        m_starting_selection.anchor_offset = replace_data_command->offset();
        m_starting_selection.focus_node = node;
        m_starting_selection.focus_offset = replace_data_command->offset() + replace_data_command->removed_data().length_in_code_units();
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

// A step replays all of its commands or none of them. Script mutations since the step was recorded
// can invalidate individual commands, and replaying only the surviving subset would garble whatever
// content the script produced. When a command refuses to replay, the commands already replayed are
// rolled back (they are reversible, and their preconditions were just reestablished), and the step
// reports that it performed nothing so the caller drops it.
bool UndoStep::unapply()
{
    for (size_t i = m_commands.size(); i > 0; --i) {
        if (m_commands[i - 1]->unapply())
            continue;
        for (size_t j = i; j < m_commands.size(); ++j)
            (void)m_commands[j]->reapply();
        return false;
    }
    return !m_commands.is_empty();
}

bool UndoStep::reapply()
{
    for (size_t i = 0; i < m_commands.size(); ++i) {
        if (m_commands[i]->reapply())
            continue;
        for (size_t j = i; j > 0; --j)
            (void)m_commands[j - 1]->unapply();
        return false;
    }
    return !m_commands.is_empty();
}

void UndoStep::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_editing_host);
    visitor.visit(m_commands);
    visitor.visit(m_starting_selection.anchor_node);
    visitor.visit(m_starting_selection.focus_node);
    visitor.visit(m_starting_selection.text_control);
    visitor.visit(m_ending_selection.anchor_node);
    visitor.visit(m_ending_selection.focus_node);
    visitor.visit(m_ending_selection.text_control);
}

GC::Ref<EditingHistory> EditingHistory::create(JS::Realm& realm)
{
    return realm.heap().allocate<EditingHistory>();
}

static SelectionSnapshot capture_selection(DOM::Node& editing_host)
{
    SelectionSnapshot snapshot;

    // Text controls have their own selection instead of a document selection.
    if (auto* control = dynamic_cast<HTML::FormAssociatedTextControlElement*>(&editing_host)) {
        snapshot.text_control = &as<HTML::HTMLElement>(editing_host);
        snapshot.text_control_start = control->selection_start();
        snapshot.text_control_end = control->selection_end();
        snapshot.text_control_direction = control->selection_direction_state();
        return snapshot;
    }

    auto selection = editing_host.document().get_selection();
    if (!selection || !selection->range())
        return snapshot;

    auto anchor_node = selection->anchor_node();
    auto focus_node = selection->focus_node();
    auto anchor_offset = selection->anchor_offset();
    auto focus_offset = selection->focus_offset();
    if (selection->is_collapsed() && focus_node) {
        // A collapsed selection can have multiple DOM representations for one rendered caret. Store the canonical
        // visible position in history without moving the live selection: replacement still acts at the exact DOM
        // boundary chosen by script, while undo restores the position Chromium exposes after the edit.
        auto visible_position = VisiblePosition::create(editing_host.document(), { *focus_node, static_cast<WebIDL::UnsignedLong>(focus_offset) }, selection->focus_affinity());
        auto deep_equivalent = visible_position.deep_equivalent();
        anchor_node = deep_equivalent.node;
        focus_node = deep_equivalent.node;
        anchor_offset = deep_equivalent.offset;
        focus_offset = deep_equivalent.offset;
    }
    snapshot.anchor_node = anchor_node;
    snapshot.anchor_offset = anchor_offset;
    snapshot.focus_node = focus_node;
    snapshot.focus_offset = focus_offset;
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
    m_undo_step_being_recorded->set_starting_selection(capture_selection(editing_host));
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

    step->set_ending_selection(capture_selection(step->editing_host()));

    // NB: Decide this before merging, since merging moves the commands into the open step.
    bool closes_coalescing = step->performed_lasting_node_removal();

    if (m_open_step && !m_undo_stack.is_empty() && m_open_step == m_undo_stack.last().ptr()
        && m_open_step->editing_host() == step->editing_host()
        && m_open_step->accepts_merge_of(step->category())) {
        // NB: The redo stack is necessarily empty here: it only fills up through undo, which
        //     ends coalescing, so a merge can never bypass the redo invalidation below.
        bool deferred_close = m_open_step->closes_after_next_merge();
        m_open_step->merge(*step);
        if (deferred_close)
            m_open_step = nullptr;
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

    notify_state_if_changed(step->editing_host()->document());
}

EditingHistory::ProxyMutationScope::ProxyMutationScope(DOM::Node& node)
    : m_history(node.document().editing_history_if_exists())
{
    if (m_history)
        m_history->m_proxy_mutation_depth++;
}

EditingHistory::ProxyMutationScope::~ProxyMutationScope()
{
    if (m_history)
        m_history->m_proxy_mutation_depth--;
}

void EditingHistory::notify_dom_mutation()
{
    if (!m_undo_step_being_recorded || m_proxy_mutation_depth > 0 || m_applying_history_step)
        return;
    dbgln("Editing: DOM mutated during a recorded editing command without going through the Editing proxy; this mutation will not be undoable!");
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

GC::Ptr<UndoStep> EditingHistory::next_undo_step()
{
    prune_steps_for_disconnected_hosts();
    return m_undo_stack.is_empty() ? nullptr : m_undo_stack.last().ptr();
}

GC::Ptr<UndoStep> EditingHistory::next_redo_step()
{
    prune_steps_for_disconnected_hosts();
    return m_redo_stack.is_empty() ? nullptr : m_redo_stack.last().ptr();
}

bool EditingHistory::undo(DOM::Document& document)
{
    if (m_applying_history_step || m_undo_step_being_recorded)
        return false;
    if (!can_undo())
        return false;

    m_open_step = nullptr;
    auto step = m_undo_stack.take_last();
    bool performed_any_mutation;
    {
        TemporaryChange applying { m_applying_history_step, true };
        performed_any_mutation = step->unapply();
    }
    restore_selection(document, step->starting_selection());

    // NB: A step that could not perform anything was invalidated by script mutation after it was
    //     recorded; replaying it in the other direction would garble content, so drop it.
    if (performed_any_mutation)
        m_redo_stack.append(step);

    notify_state_if_changed(document);
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
    bool performed_any_mutation;
    {
        TemporaryChange applying { m_applying_history_step, true };
        performed_any_mutation = step->reapply();
    }
    restore_selection(document, step->ending_selection());

    // NB: A step that could not perform anything was invalidated by script mutation after it was
    //     recorded; replaying it in the other direction would garble content, so drop it.
    if (performed_any_mutation)
        m_undo_stack.append(step);

    notify_state_if_changed(document);
    dispatch_history_input_event(document, step->editing_host(), UIEvents::InputTypes::historyRedo);
    return true;
}

// Tells the UI whether undo and redo are available, e.g. for enabling menu items.
void EditingHistory::notify_state_if_changed(DOM::Document& document)
{
    bool new_can_undo = can_undo();
    bool new_can_redo = can_redo();
    if (new_can_undo == m_last_notified_can_undo && new_can_redo == m_last_notified_can_redo)
        return;
    m_last_notified_can_undo = new_can_undo;
    m_last_notified_can_redo = new_can_redo;
    document.page().client().page_did_update_editing_history_state(new_can_undo, new_can_redo);
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
    // Text controls have their own selection instead of a document selection.
    if (snapshot.text_control) {
        if (!snapshot.text_control->is_connected() || &snapshot.text_control->document() != &document)
            return;
        if (auto* control = dynamic_cast<HTML::FormAssociatedTextControlElement*>(snapshot.text_control.ptr()))
            control->set_the_selection_range(snapshot.text_control_start, snapshot.text_control_end, snapshot.text_control_direction);
        return;
    }

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

// INTEROP: Chromium's keyboard undo/redo dispatches a cancelable beforeinput event with
//          inputType "historyUndo" or "historyRedo" at the step's editing host before touching
//          anything; a canceled event means no mutation happens and no history entry is
//          consumed. With nothing to undo or redo, no events fire at all. execCommand("undo")
//          takes the command path instead, which never fires beforeinput.
EventResult perform_history_action(DOM::Document& document, HistoryAction action)
{
    auto history = document.editing_history_if_exists();
    if (!history)
        return EventResult::Handled;

    auto step = action == HistoryAction::Undo ? history->next_undo_step() : history->next_redo_step();
    if (!step)
        return EventResult::Handled;

    auto const& input_type = action == HistoryAction::Undo ? UIEvents::InputTypes::historyUndo : UIEvents::InputTypes::historyRedo;

    Bindings::InputEventInit event_init {};
    event_init.bubbles = true;
    event_init.cancelable = true;
    event_init.input_type = input_type;
    auto event = UIEvents::InputEvent::create_from_platform_event(document.realm(), UIEvents::EventNames::beforeinput, event_init);
    event->set_is_trusted(true);
    if (!step->editing_host()->dispatch_event(event))
        return EventResult::Handled;

    if (action == HistoryAction::Undo)
        history->undo(document);
    else
        history->redo(document);
    return EventResult::Handled;
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
