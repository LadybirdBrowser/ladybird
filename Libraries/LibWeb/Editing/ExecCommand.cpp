/*
 * Copyright (c) 2024-2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/ScopeGuard.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Editing/Commands.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/SpellCheckController.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/Editing/WritingSuggestionController.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWeb/UIEvents/InputTypes.h>

namespace Web::DOM {

GC::Ref<Editing::EditingHistory> Document::editing_history()
{
    if (!m_editing_history)
        m_editing_history = Editing::EditingHistory::create();
    return *m_editing_history;
}

static bool command_is_enabled(Document&, Utf16FlyString const& command);

// INTEROP: The editing commands act on the selection, but when a text control has focus, browsers apply the commands
//          that make sense for plain text to that text control instead, and disable the other commands.
static HTML::FormAssociatedTextControlElement* focused_text_control(Document& document)
{
    auto focused_area = document.focused_area();
    if (!focused_area)
        return nullptr;
    if (auto* input = as_if<HTML::HTMLInputElement>(*focused_area); input && input->has_selectable_text())
        return input;
    return as_if<HTML::HTMLTextAreaElement>(*focused_area);
}

static bool command_applies_to_text_control(Utf16FlyString const& command, HTML::FormAssociatedTextControlElement& control)
{
    if (command.is_one_of(Editing::CommandNames::insertLineBreak, Editing::CommandNames::insertParagraph))
        return is<HTML::HTMLTextAreaElement>(control.text_control_to_html_element());
    return command.is_one_of(Editing::CommandNames::delete_, Editing::CommandNames::forwardDelete,
        Editing::CommandNames::insertHTML, Editing::CommandNames::insertText, Editing::CommandNames::redo,
        Editing::CommandNames::selectAll, Editing::CommandNames::undo);
}

static Optional<bool> query_command_enabled_in_text_control(Document& document, Utf16FlyString const& command)
{
    auto* control = focused_text_control(document);
    if (!control)
        return {};
    auto& element = control->text_control_to_html_element();

    // NB: These commands only change editing state, which only makes sense if the text control is in editable
    //     content.
    if (command.is_one_of(Editing::CommandNames::defaultParagraphSeparator, Editing::CommandNames::styleWithCSS,
            Editing::CommandNames::useCSS))
        return element.is_editable_or_editing_host();

    if (!command_applies_to_text_control(command, *control))
        return false;

    // NB: Undo and redo use the editing history, which also records the edits in text controls.
    if (command.is_one_of(Editing::CommandNames::undo, Editing::CommandNames::redo))
        return {};

    if (command == Editing::CommandNames::selectAll)
        return true;
    return as<HTML::FormAssociatedElement>(element).is_mutable();
}

static bool exec_command_in_text_control(HTML::FormAssociatedTextControlElement& control, Utf16FlyString const& command, Utf16View value)
{
    // NB: See the note at CursorRevealAfterEdit.
    using CursorRevealAfterEdit = HTML::FormAssociatedTextControlElement::CursorRevealAfterEdit;
    control.set_cursor_reveal_after_edit(CursorRevealAfterEdit::InNextRenderingUpdate);
    ScopeGuard reveal_cursor_immediately_again = [&] {
        control.set_cursor_reveal_after_edit(CursorRevealAfterEdit::Immediately);
    };

    if (command == Editing::CommandNames::delete_) {
        control.handle_delete(UIEvents::InputTypes::deleteContentBackward);
    } else if (command == Editing::CommandNames::forwardDelete) {
        control.handle_delete(UIEvents::InputTypes::deleteContentForward);
    } else if (command == Editing::CommandNames::insertText) {
        control.handle_insert(UIEvents::InputTypes::insertText, value);
    } else if (command == Editing::CommandNames::insertHTML) {
        // NB: A text control can only contain text, so insert the text of the HTML. Parse it with a div as the
        //     context element, since a textarea as the context element would parse it as text.
        auto context = MUST(DOM::create_element(control.text_control_to_html_element().document(), HTML::TagNames::div, Namespace::HTML));
        auto fragment = HTML::HTMLParser::parse_html_fragment(context, value);
        auto text = fragment.is_error() ? Utf16String {} : fragment.value()->text_content().value_or({});
        control.handle_insert(UIEvents::InputTypes::insertText, text);
    } else if (command == Editing::CommandNames::insertLineBreak) {
        control.handle_insert(UIEvents::InputTypes::insertLineBreak, u"\n"sv);
    } else if (command == Editing::CommandNames::insertParagraph) {
        control.handle_insert(UIEvents::InputTypes::insertParagraph, u"\n"sv);
    } else if (command == Editing::CommandNames::selectAll) {
        control.select_all();
    } else {
        VERIFY_NOT_REACHED();
    }
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#execcommand()
WebIDL::ExceptionOr<bool> Document::exec_command(Utf16FlyString const& command, [[maybe_unused]] bool show_ui, Utf16View value)
{
    // NB: See the INTEROP note at focused_text_control(). exec_command_internal() checks whether the command is
    //     enabled for the active range only, so check whether it is enabled for the text control here.
    if (auto* control = focused_text_control(*this); control && is_html_document() && !m_inside_exec_command) {
        if (!MUST(query_command_enabled(command)))
            return false;
        auto command_definition = Editing::find_command_definition(command);
        if (command_definition.has_value() && command_definition->command != Editing::CommandNames::undo
            && command_definition->command != Editing::CommandNames::redo
            && command_applies_to_text_control(command_definition->command, *control)) {
            TemporaryChange guard_recursion { m_inside_exec_command, true };
            return exec_command_in_text_control(*control, command_definition->command, value);
        }
    }

    return exec_command_internal(command, show_ui, value, DispatchInputEvent::Yes);
}

WebIDL::ExceptionOr<bool> Document::exec_command_internal(Utf16FlyString const& command, [[maybe_unused]] bool show_ui, Utf16View value, DispatchInputEvent dispatch_input_event, Optional<Utf16FlyString> const& user_input_type, GC::Ptr<StaticRange> deletion_range)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create("execCommand is only supported on HTML documents"_utf16);

    // AD-HOC: All major browsers refuse to recursively execute execCommand() (e.g. inside input event handlers).
    if (m_inside_exec_command)
        return false;
    TemporaryChange guard_recursion { m_inside_exec_command, true };

    // 1. If only one argument was provided, let show UI be false.
    // 2. If only one or two arguments were provided, let value be the empty string.
    // NOTE: these steps are dealt by the default values for both show_ui and value

    // 3. If command is not supported or not enabled, return false.
    // NB: This does not use query_command_enabled(), which reports whether a command is enabled in a focused text
    //     control. The commands below act on the active range, and rely on it being checked here.
    if (!MUST(query_command_supported(command)) || !command_is_enabled(*this, command))
        return false;

    // 4. If command is not in the Miscellaneous commands section:
    //
    //    We don't fire events for copy/cut/paste/undo/redo/selectAll because they should all have their own events. We
    //    don't fire events for styleWithCSS/useCSS because it's not obvious where to fire them, or why anyone would
    //    want them. We don't fire events for unsupported commands, because then if they became supported and were
    //    classified with the miscellaneous events, we'd have to stop firing events for consistency's sake.
    //
    // AD-HOC: The defaultParagraphSeparator command is also in the Miscellaneous commands section
    auto optional_command = Editing::find_command_definition(command);
    VERIFY(optional_command.has_value());
    auto const& command_definition = optional_command.release_value();
    GC::Ptr<Node> affected_editing_host;
    if (!command_definition.command.is_one_of(Editing::CommandNames::copy, Editing::CommandNames::cut,
            Editing::CommandNames::defaultParagraphSeparator, Editing::CommandNames::paste, Editing::CommandNames::redo,
            Editing::CommandNames::selectAll, Editing::CommandNames::styleWithCSS, Editing::CommandNames::undo,
            Editing::CommandNames::useCSS)) {
        // 1. Let affected editing host be the editing host that is an inclusive ancestor of the active range's start
        //    node and end node, and is not the ancestor of any editing host that is an inclusive ancestor of the active
        //    range's start node and end node.
        //
        // NOTE: Because either the start or end node of the range could be inside an editing host that is part of the
        //       other node's editing host, we can probe both and see if either one is the other's ancestor.
        // NOTE: We can reuse ->editing_host() here since command_is_enabled() above already checked that both the
        //       start and end nodes are either editable or an editing host.
        auto range = Editing::active_range(*this);
        auto& start_node_editing_host = *range->start_container()->editing_host();
        auto& end_node_editing_host = *range->end_container()->editing_host();
        affected_editing_host = start_node_editing_host.is_ancestor_of(end_node_editing_host)
            ? end_node_editing_host
            : start_node_editing_host;

        // 2. Fire an event named "beforeinput" at affected editing host using InputEvent, with its
        //    bubbles and cancelable attributes initialized to true, and its data attribute
        //    initialized to null.
        // 3. If the value returned by the previous step is false, return false.
        // 4. If command is not enabled, return false.
        //
        //    We have to check again whether the command is enabled, because the beforeinput handler might have done
        //    something annoying like getSelection().removeAllRanges().
        // 5. Let affected editing host be the editing host that is an inclusive ancestor of the active range's start
        //    node and end node, and is not the ancestor of any editing host that is an inclusive ancestor of the active
        //    range's start node and end node.
        //
        //    This new affected editing host is what we'll fire the input event at in a couple of lines. We want to
        //    compute it beforehand just to be safe: bugs in the command action might remove the selection or something
        //    bad like that, and we don't want to have to handle it later. We recompute it after the beforeinput event
        //    is handled so that if the handler moves the selection to some other editing host, the input event will be
        //    fired at the editing host that was actually affected.

        // AD-HOC: No, we don't. Neither Chrome nor Firefox fire the "beforeinput" event for execCommand(). This is an
        //         open discussion for the spec: https://github.com/w3c/editing/issues/200
    }

    // https://w3c.github.io/editing/docs/execCommand/#preserves-overrides
    // If a command preserves overrides, then before taking its action, the user agent must record current overrides.
    Vector<Editing::RecordedOverride> overrides;
    if (command_definition.preserves_overrides)
        overrides = Editing::record_current_overrides(*this);

    // INTEROP: Chromium distinguishes pastes that begin inside text from pastes at an element boundary when deciding
    //          whether a trailing line break is merely a placeholder. Record this before caret canonicalization can
    //          turn an element-level boundary into an equivalent text position.
    auto range_before_command = Editing::active_range(*this);
    bool paste_started_in_text_node = user_input_type == UIEvents::InputTypes::insertFromPaste
        && range_before_command && is<DOM::Text>(*range_before_command->start_container())
        // INTEROP: An Apple-interchange-newline represents an intentional paragraph boundary, not a fragment-ending
        //          placeholder. Blink and WebKit preserve the destination paragraph's trailing break in this case.
        && !value.contains(u"Apple-interchange-newline"sv);
    GC::Root<DOM::Node> paste_start_block;
    if (paste_started_in_text_node)
        paste_start_block = Editing::block_node_of_node(*range_before_command->start_container());

    // INTEROP: Deleting out of the first or last ordinary paragraph is a no-op, including when the DOM caret
    //          lies in collapsed whitespace. Do this before canonicalization so neither the DOM nor the selection
    //          changes. List items and indentation containers still need their structural deletion behavior.
    if (affected_editing_host && range_before_command && range_before_command->collapsed()
        && command_definition.command.is_one_of(Editing::CommandNames::delete_, Editing::CommandNames::forwardDelete)) {
        auto position = Editing::VisiblePosition::create(*this, range_before_command->start());
        auto block = Editing::block_node_of_node(*range_before_command->start_container());
        if (block && block->parent() == affected_editing_host.ptr() && Editing::is_non_list_single_line_container(*block)) {
            auto* previous = block->previous_sibling();
            while (previous && (Editing::is_invisible_node(*previous) || Editing::is_whitespace_node(*previous)))
                previous = previous->previous_sibling();
            auto* next = block->next_sibling();
            while (next && (Editing::is_invisible_node(*next) || Editing::is_whitespace_node(*next)))
                next = next->next_sibling();
            if (command_definition.command == Editing::CommandNames::delete_ && !previous
                && position.is_start_of_containing_block())
                return true;
            if (command_definition.command == Editing::CommandNames::forwardDelete && !next
                && position.is_end_of_containing_block())
                return true;
        }
    }

    // NOTE: Step 7 below asks us whether the DOM tree was modified, so keep track of the document versions.
    auto old_dom_tree_version = dom_tree_version();
    auto old_character_data_version = character_data_version();

    // NB: Canonicalize the caret before the command acts and records its starting selection, so that e.g. typing at an
    //     element-level caret inserts into the adjacent content like Chromium does. Inline formatting commands with a
    //     collapsed selection only set overrides, and Chromium does not move the caret for those.
    bool command_edits_at_caret = command_definition.command.is_one_of(
        Editing::CommandNames::delete_, Editing::CommandNames::formatBlock, Editing::CommandNames::forwardDelete,
        Editing::CommandNames::indent, Editing::CommandNames::insertHorizontalRule, Editing::CommandNames::insertHTML,
        Editing::CommandNames::insertImage, Editing::CommandNames::insertLineBreak,
        Editing::CommandNames::insertOrderedList, Editing::CommandNames::insertParagraph,
        Editing::CommandNames::insertText, Editing::CommandNames::insertUnorderedList,
        Editing::CommandNames::justifyCenter, Editing::CommandNames::justifyFull, Editing::CommandNames::justifyLeft,
        Editing::CommandNames::justifyRight, Editing::CommandNames::outdent);
    if (affected_editing_host && m_selection && command_edits_at_caret)
        Editing::canonicalize_collapsed_selection_for_editing(*m_selection);

    // AD-HOC: Record the mutations performed by the command action on the editing history, so the user can undo them.
    //         end_recording() is a no-op if the guard below already ended the recording.
    bool is_cut_or_paste = user_input_type == UIEvents::InputTypes::deleteByCut || user_input_type == UIEvents::InputTypes::insertFromPaste;
    if (affected_editing_host) {
        auto category = Editing::UndoStep::Category::Other;
        // INTEROP: Cut and paste are standalone undo units in Chromium: they never coalesce with typing or deletion
        //          runs, so they categorize as Other even though they run the delete and insertText commands.
        if (!is_cut_or_paste && !deletion_range) {
            if (command_definition.command.is_one_of(Editing::CommandNames::insertText, Editing::CommandNames::insertLineBreak, Editing::CommandNames::insertParagraph))
                category = Editing::UndoStep::Category::Insertion;
            else if (command_definition.command == Editing::CommandNames::delete_)
                category = Editing::UndoStep::Category::BackwardDeletion;
            else if (command_definition.command == Editing::CommandNames::forwardDelete)
                category = Editing::UndoStep::Category::ForwardDeletion;
        }
        editing_history()->begin_recording(*affected_editing_host, category);
    }
    ScopeGuard end_recording_guard = [&] {
        if (auto history = editing_history_if_exists())
            history->end_recording();
    };

    if (deletion_range) {
        MUST(m_selection->set_base_and_extent(deletion_range->start_container(), deletion_range->start_offset(),
            deletion_range->end_container(), deletion_range->end_offset()));
    }

    // NB: Moving the caret above focuses its editing host, and focusing an editing host such as a contenteditable
    //     textarea can remove the selection. Commands with an affected editing host require an active range, so stop
    //     here if it is gone.
    if (affected_editing_host && !Editing::active_range(*this))
        return false;

    // 5. Take the action for command, passing value to the instructions as an argument.
    auto command_result = [&] {
        TemporaryChange running_action { m_running_editing_command_action, true };
        return command_definition.action(*this, value);
    }();

    // INTEROP: Chromium removes the trailing placeholder line break after pasting into an existing text node. The
    //          execCommand draft only removes it when insertion creates a new text node. Perform this while the paste
    //          history transaction is still open so undo restores the placeholder and redo removes it again.
    if (paste_started_in_text_node) {
        if (auto range = Editing::active_range(*this)) {
            // INTEROP: A trailing break remains the list item's line terminator after a block fragment is merged into
            //          that item. Blink removes paragraph placeholders here, but preserves list-item terminators.
            if (auto block = Editing::block_node_of_node(*range->start_container()); block && block.ptr() == paste_start_block.ptr()
                && !is<HTML::HTMLLIElement>(*block))
                Editing::remove_extraneous_line_breaks_at_the_end_of_node(*block);
        }
    }

    // NB: Canonicalize the caret the command produced before the ending selection is recorded, but only if the
    //     command actually performed an edit; Chromium leaves the caret alone otherwise.
    // INTEROP: A user cut or paste counts as an edit whenever its command ran: Chromium fires the input event and
    //          settles the caret even when the pasted fragment reduces to nothing but transport markers.
    bool tree_was_modified = dom_tree_version() != old_dom_tree_version
        || character_data_version() != old_character_data_version
        || (is_cut_or_paste && command_result);
    if (affected_editing_host && m_selection && tree_was_modified)
        Editing::canonicalize_collapsed_selection_for_editing(*m_selection);

    // https://w3c.github.io/editing/docs/execCommand/#preserves-overrides
    // After taking the action, if the active range is collapsed, it must restore states and values from the recorded
    // list.
    // NB: Do this after canonicalizing the caret above, since moving the caret unsets the overrides.
    if (!overrides.is_empty() && m_selection && m_selection->is_collapsed()) {
        auto* focus = m_selection->focus_node().ptr();
        // INTEROP: A deleted link's inherited style must not recreate the link on the next keystroke.
        if (any_of(overrides, [](auto const& override) { return override.command == Editing::CommandNames::createLink; })
            && command_definition.command.is_one_of(Editing::CommandNames::delete_, Editing::CommandNames::forwardDelete)
            && focus && !is<HTML::HTMLAnchorElement>(*focus) && !focus->first_ancestor_of_type<HTML::HTMLAnchorElement>()) {
            overrides.remove_all_matching([](auto const& override) {
                return override.command.is_one_of(Editing::CommandNames::createLink, Editing::CommandNames::underline, Editing::CommandNames::foreColor);
            });
        }
        Editing::restore_states_and_values(*this, overrides);
    }

    // NB: End the recording before dispatching the input event below, so that the undo step's ending selection is the
    //     selection produced by the command itself, not whatever an input event handler changed it to.
    if (auto history = editing_history_if_exists())
        history->end_recording();

    // 6. If the previous step returned false, return false.
    if (!command_result)
        return false;

    // A command the user ran by editing, rather than one a script called, is a user edit for spellchecking and writing
    // suggestions.
    if (user_input_type.has_value() && tree_was_modified && affected_editing_host) {
        spell_check_controller().did_user_edit(*affected_editing_host);
        writing_suggestion_controller().did_user_edit(*affected_editing_host);
    }

    // 7. If the action modified DOM tree, then fire an event named "input" at affected editing host using InputEvent,
    //    with its isTrusted and bubbles attributes initialized to true, inputType attribute initialized to the mapped
    //    value of command, and its data attribute initialized to null.
    if (dispatch_input_event == DispatchInputEvent::Yes && tree_was_modified && affected_editing_host) {
        UIEvents::InputEventInit event_init {};
        event_init.bubbles = true;
        // INTEROP: When the command runs on behalf of a user cut or paste, the input event carries the user's input
        //          type (deleteByCut or insertFromPaste) rather than the command's mapped value, like other browsers.
        event_init.input_type = user_input_type.value_or(command_definition.mapped_value);

        // AD-HOC: For insertText, we do what other browsers do and set data to value. A paste carries null data even
        //         though it runs the insertText command.
        if (event_init.input_type == UIEvents::InputTypes::insertText || event_init.input_type == UIEvents::InputTypes::insertCompositionText)
            event_init.data = Utf16String::from_utf16(value);

        // https://w3c.github.io/uievents/#dom-inputevent-iscomposing
        // true if the input event occurs as part of a composition session; that is, after a compositionstart event and
        // before the corresponding compositionend event.
        event_init.is_composing = is_input_method_composing();

        auto event = UIEvents::InputEvent::create_from_platform_event(HTML::EventNames::input, event_init, {}, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this)));
        event->set_is_trusted(true);

        TemporaryChange preserve_selection_offsets { m_preserve_selection_offsets_during_identical_character_data_replacement, true };
        affected_editing_host->dispatch_event(event);
    }

    // AD-HOC: Scroll the cursor into view after executing a command.
    if (auto selection = get_selection())
        selection->scroll_focus_into_view();

    // 8. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandenabled()
WebIDL::ExceptionOr<bool> Document::query_command_enabled(Utf16FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create("queryCommandEnabled is only supported on HTML documents"_utf16);

    // 2. Return true if command is both supported and enabled, false otherwise.
    if (!MUST(query_command_supported(command)))
        return false;

    // NB: See the INTEROP note at focused_text_control().
    if (auto command_definition = Editing::find_command_definition(command); command_definition.has_value()) {
        if (auto enabled = query_command_enabled_in_text_control(*this, command_definition->command); enabled.has_value())
            return *enabled;
    }

    return command_is_enabled(*this, command);
}

// https://w3c.github.io/editing/docs/execCommand/#enabled
static bool command_is_enabled(Document& document, Utf16FlyString const& command)
{
    // Among commands defined in this specification, those listed in Miscellaneous commands are always enabled, except
    // for the cut command and the paste command.
    // NOTE: cut and paste are actually in the Clipboard commands section
    // INTEROP: Browsers do not enable these commands if the active range's start node is neither editable nor an
    //          editing host.
    if (command.is_one_of_ignoring_ascii_case(
            Editing::CommandNames::defaultParagraphSeparator,
            Editing::CommandNames::styleWithCSS,
            Editing::CommandNames::useCSS)) {
        auto active_range = Editing::active_range(document);
        return !active_range || active_range->start_container()->is_editable_or_editing_host();
    }

    // INTEROP: The spec lists undo and redo among the always-enabled miscellaneous commands, but in Chromium
    //          queryCommandEnabled("undo") is true only when there is a step to undo, and execCommand("undo") on an
    //          empty history returns false. Redo behaves symmetrically.
    if (command.equals_ignoring_ascii_case(Editing::CommandNames::undo)) {
        auto history = document.editing_history_if_exists();
        return history && history->can_undo();
    }
    if (command.equals_ignoring_ascii_case(Editing::CommandNames::redo)) {
        auto history = document.editing_history_if_exists();
        return history && history->can_redo();
    }

    // AD-HOC: selectAll requires a selection object to exist.
    if (command.equals_ignoring_ascii_case(Editing::CommandNames::selectAll))
        return document.get_selection() != nullptr;

    // The other commands defined here are enabled if the active range is not null,
    auto active_range = Editing::active_range(document);
    if (!active_range)
        return false;

    // its start node is either editable or an editing host,
    auto start_node = active_range->start_container();
    if (!start_node->is_editable_or_editing_host())
        return false;

    // FIXME: the editing host of its start node is not an EditContext editing host,
    [[maybe_unused]] auto start_node_editing_host = start_node->editing_host();

    // its end node is either editable or an editing host,
    auto& end_node = *active_range->end_container();
    if (!end_node.is_editable_or_editing_host())
        return false;

    // FIXME: the editing host of its end node is not an EditContext editing host,
    [[maybe_unused]] auto end_node_editing_host = end_node.editing_host();

    // and there is some editing host that is an inclusive ancestor of both its start node and its end node.
    GC::Ptr<Node> inclusive_ancestor_editing_host;
    start_node->for_each_inclusive_ancestor([&](GC::Ref<Node> ancestor) {
        if (ancestor->is_editing_host() && ancestor->is_inclusive_ancestor_of(end_node)) {
            inclusive_ancestor_editing_host = ancestor;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!inclusive_ancestor_editing_host)
        return false;

    // NOTE: Commands can define additional conditions for being enabled, and currently the only condition mentioned in
    //       the spec is that certain commands must not be enabled if the editing host is in the plaintext-only state.
    if (auto const* html_element = as_if<HTML::HTMLElement>(inclusive_ancestor_editing_host.ptr()); html_element
        && html_element->content_editable_state() == HTML::ContentEditableState::PlaintextOnly
        && command.is_one_of_ignoring_ascii_case(
            Editing::CommandNames::backColor,
            Editing::CommandNames::bold,
            Editing::CommandNames::createLink,
            Editing::CommandNames::fontName,
            Editing::CommandNames::fontSize,
            Editing::CommandNames::foreColor,
            Editing::CommandNames::formatBlock, // AD-HOC: https://github.com/w3c/editing/issues/478
            Editing::CommandNames::hiliteColor,
            Editing::CommandNames::indent,
            Editing::CommandNames::insertHorizontalRule,
            Editing::CommandNames::insertImage,
            Editing::CommandNames::insertOrderedList,
            Editing::CommandNames::insertUnorderedList,
            Editing::CommandNames::italic,
            Editing::CommandNames::justifyCenter,
            Editing::CommandNames::justifyFull,
            Editing::CommandNames::justifyLeft,
            Editing::CommandNames::justifyRight,
            Editing::CommandNames::outdent,
            Editing::CommandNames::removeFormat,
            Editing::CommandNames::strikethrough,
            Editing::CommandNames::subscript,
            Editing::CommandNames::superscript,
            Editing::CommandNames::underline,
            Editing::CommandNames::unlink))
        return false;

    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandindeterm()
WebIDL::ExceptionOr<bool> Document::query_command_indeterm(Utf16FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create("queryCommandIndeterm is only supported on HTML documents"_utf16);

    // 1. If command is not supported or has no indeterminacy, return false.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return false;
    auto const& command_definition = optional_command.value();
    if (!command_definition.indeterminate) {
        // If a command has inline command activated values defined but nothing else defines when it is indeterminate,
        // it is indeterminate if among formattable nodes effectively contained in the active range, there is at least
        // one whose effective command value is one of the given values and at least one whose effective command value
        // is not one of the given values.
        if (!command_definition.inline_activated_values.is_empty()) {
            auto range = Editing::active_range(*this);
            bool has_at_least_one_match = false;
            bool has_at_least_one_mismatch = false;
            Editing::for_each_node_effectively_contained_in_range(range, [&](GC::Ref<Node> descendant) {
                if (!Editing::is_formattable_node(descendant))
                    return TraversalDecision::Continue;

                auto node_value = Editing::effective_command_value(descendant, command_definition.command);
                if (!node_value.has_value())
                    return TraversalDecision::Continue;

                if (command_definition.inline_activated_values.contains_slow(node_value.value()))
                    has_at_least_one_match = true;
                else
                    has_at_least_one_mismatch = true;

                if (has_at_least_one_match && has_at_least_one_mismatch)
                    return TraversalDecision::Break;
                return TraversalDecision::Continue;
            });
            return has_at_least_one_match && has_at_least_one_mismatch;
        }

        return false;
    }

    // 2. Return true if command is indeterminate, otherwise false.
    return command_definition.indeterminate(*this);
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandstate()
WebIDL::ExceptionOr<bool> Document::query_command_state(Utf16FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create("queryCommandState is only supported on HTML documents"_utf16);

    // 1. If command is not supported or has no state, return false.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return false;
    auto const& command_definition = optional_command.release_value();
    auto state_override = command_state_override(command_definition.command);
    if (!command_definition.state && !state_override.has_value()) {
        // https://w3c.github.io/editing/docs/execCommand/#inline-command-activated-values
        // If a command has inline command activated values defined, its state is true if either no formattable node is
        // effectively contained in the active range, and the active range's start node's effective command value is one
        // of the given values;
        auto const& inline_values = command_definition.inline_activated_values;
        if (inline_values.is_empty())
            return false;
        auto range = Editing::active_range(*this);
        if (!range)
            return false;

        GC::RootVector<GC::Ref<Node>> formattable_nodes;
        Editing::for_each_node_effectively_contained_in_range(range, [&](GC::Ref<Node> descendant) {
            if (Editing::is_formattable_node(descendant))
                formattable_nodes.append(descendant);
            return TraversalDecision::Continue;
        });
        if (formattable_nodes.is_empty())
            return inline_values.contains_slow(Editing::effective_command_value(range->start_container(), command_definition.command).value_or({}));

        // or if there is at least one formattable node effectively contained in the active range, and all of them have
        // an effective command value equal to one of the given values.
        return all_of(formattable_nodes, [&](GC::Ref<Node> node) {
            return inline_values.contains_slow(Editing::effective_command_value(node, command_definition.command).value_or({}));
        });
    }

    // 2. If the state override for command is set, return it.
    if (state_override.has_value())
        return state_override.release_value();

    // 3. Return true if command's state is true, otherwise false.
    return command_definition.state(*this);
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandsupported()
WebIDL::ExceptionOr<bool> Document::query_command_supported(Utf16FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create("queryCommandSupported is only supported on HTML documents"_utf16);

    // When the queryCommandSupported(command) method on the Document interface is invoked, the user agent must return
    // true if command is supported and available within the current script on the current site, and false otherwise.
    return Editing::find_command_definition(command).has_value();
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandvalue()
WebIDL::ExceptionOr<Utf16String> Document::query_command_value(Utf16FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create("queryCommandValue is only supported on HTML documents"_utf16);

    // 1. If command is not supported or has no value, return the empty string.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return Utf16String {};
    auto const& command_definition = optional_command.release_value();
    if (!command_definition.value)
        return Utf16String {};
    auto value_override = command_value_override(command_definition.command);

    // 2. If command is "fontSize" and its value override is set, convert the value override to an
    //    integer number of pixels and return the legacy font size for the result.
    if (command_definition.command == Editing::CommandNames::fontSize && value_override.has_value()) {
        auto pixel_size = Editing::font_size_to_pixel_size(value_override.release_value());
        return Editing::legacy_font_size(pixel_size.to_int());
    }

    // 3. If the value override for command is set, return it.
    if (value_override.has_value())
        return Utf16String::from_utf16(value_override.release_value());

    // 4. Return command's value.
    return command_definition.value(*this);
}

// https://w3c.github.io/editing/docs/execCommand/#value-override
void Document::set_command_value_override(Utf16FlyString const& command, Utf16View value)
{
    m_command_value_override.set(command, Utf16String::from_utf16(value));

    // The value override for the backColor command must be the same as the value override for the hiliteColor command,
    // such that setting one sets the other to the same thing and unsetting one unsets the other.
    if (command == Editing::CommandNames::backColor)
        m_command_value_override.set(Editing::CommandNames::hiliteColor, Utf16String::from_utf16(value));
    else if (command == Editing::CommandNames::hiliteColor)
        m_command_value_override.set(Editing::CommandNames::backColor, Utf16String::from_utf16(value));
}

// https://w3c.github.io/editing/docs/execCommand/#value-override
void Document::clear_command_value_override(Utf16FlyString const& command)
{
    m_command_value_override.remove(command);

    // The value override for the backColor command must be the same as the value override for the hiliteColor command,
    // such that setting one sets the other to the same thing and unsetting one unsets the other.
    if (command == Editing::CommandNames::backColor)
        m_command_value_override.remove(Editing::CommandNames::hiliteColor);
    else if (command == Editing::CommandNames::hiliteColor)
        m_command_value_override.remove(Editing::CommandNames::backColor);
}

}
