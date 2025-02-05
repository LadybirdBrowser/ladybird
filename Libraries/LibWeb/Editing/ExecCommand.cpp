/*
 * Copyright (c) 2024-2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Editing/Commands.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/InputEvent.h>

namespace Web::DOM {

// https://w3c.github.io/editing/docs/execCommand/#execcommand()
WebIDL::ExceptionOr<bool> Document::exec_command(FlyString const& command, [[maybe_unused]] bool show_ui, String const& value)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create(realm(), "execCommand is only supported on HTML documents"_string);

    // AD-HOC: All major browsers refuse to recursively execute execCommand() (e.g. inside input event handlers).
    if (m_inside_exec_command)
        return false;
    ScopeGuard guard_recursion = [&] { m_inside_exec_command = false; };
    m_inside_exec_command = true;

    // 1. If only one argument was provided, let show UI be false.
    // 2. If only one or two arguments were provided, let value be the empty string.
    // NOTE: these steps are dealt by the default values for both show_ui and value

    // 3. If command is not supported or not enabled, return false.
    // NOTE: query_command_enabled() also checks if command is supported
    if (!MUST(query_command_enabled(command)))
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
        // NOTE: We can reuse Editing::editing_host_of_node() here since query_command_enabled() above already checked
        //       that both the start and end nodes are either editable or an editing host.
        auto range = Editing::active_range(*this);
        auto& start_node_editing_host = *Editing::editing_host_of_node(range->start_container());
        auto& end_node_editing_host = *Editing::editing_host_of_node(range->end_container());
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

    // NOTE: Step 7 below asks us whether the DOM tree was modified, so keep track of the document versions.
    auto old_dom_tree_version = dom_tree_version();
    auto old_character_data_version = character_data_version();

    // 5. Take the action for command, passing value to the instructions as an argument.
    auto command_result = command_definition.action(*this, value);

    // https://w3c.github.io/editing/docs/execCommand/#preserves-overrides
    // After taking the action, if the active range is collapsed, it must restore states and values from the recorded
    // list.
    if (!overrides.is_empty() && m_selection && m_selection->is_collapsed())
        Editing::restore_states_and_values(*this, overrides);

    // 6. If the previous step returned false, return false.
    if (!command_result)
        return false;

    // 7. If the action modified DOM tree, then fire an event named "input" at affected editing host using InputEvent,
    //    with its isTrusted and bubbles attributes initialized to true, inputType attribute initialized to the mapped
    //    value of command, and its data attribute initialized to null.
    bool tree_was_modified = dom_tree_version() != old_dom_tree_version
        || character_data_version() != old_character_data_version;
    if (tree_was_modified && affected_editing_host) {
        UIEvents::InputEventInit event_init {};
        event_init.bubbles = true;
        event_init.input_type = command_definition.mapped_value;
        auto event = realm().create<UIEvents::InputEvent>(realm(), HTML::EventNames::input, event_init);
        event->set_is_trusted(true);
        affected_editing_host->dispatch_event(event);
    }

    // 8. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandenabled()
WebIDL::ExceptionOr<bool> Document::query_command_enabled(FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create(realm(), "queryCommandEnabled is only supported on HTML documents"_string);

    // 2. Return true if command is both supported and enabled, false otherwise.
    if (!MUST(query_command_supported(command)))
        return false;

    // https://w3c.github.io/editing/docs/execCommand/#enabled
    // Among commands defined in this specification, those listed in Miscellaneous commands are always enabled, except
    // for the cut command and the paste command.
    // NOTE: cut and paste are actually in the Clipboard commands section
    if (command.is_one_of_ignoring_ascii_case(
            Editing::CommandNames::defaultParagraphSeparator,
            Editing::CommandNames::redo,
            Editing::CommandNames::styleWithCSS,
            Editing::CommandNames::undo,
            Editing::CommandNames::useCSS))
        return true;

    // AD-HOC: selectAll requires a selection object to exist.
    if (command.equals_ignoring_ascii_case(Editing::CommandNames::selectAll))
        return get_selection() != nullptr;

    // The other commands defined here are enabled if the active range is not null,
    auto active_range = Editing::active_range(*this);
    if (!active_range)
        return false;

    // its start node is either editable or an editing host,
    auto start_node = active_range->start_container();
    if (!start_node->is_editable_or_editing_host())
        return false;

    // FIXME: the editing host of its start node is not an EditContext editing host,
    [[maybe_unused]] auto start_node_editing_host = Editing::editing_host_of_node(start_node);

    // its end node is either editable or an editing host,
    auto& end_node = *active_range->end_container();
    if (!end_node.is_editable_or_editing_host())
        return false;

    // FIXME: the editing host of its end node is not an EditContext editing host,
    [[maybe_unused]] auto end_node_editing_host = Editing::editing_host_of_node(end_node);

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
WebIDL::ExceptionOr<bool> Document::query_command_indeterm(FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create(realm(), "queryCommandIndeterm is only supported on HTML documents"_string);

    // 1. If command is not supported or has no indeterminacy, return false.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return false;
    auto const& command_definition = optional_command.value();
    if (!command_definition.indeterminate) {
        // https://w3c.github.io/editing/docs/execCommand/#inline-command-activated-values
        // If a command is a standard inline value command, it is indeterminate if among formattable nodes that are
        // effectively contained in the active range, there are two that have distinct effective command values.
        if (command_definition.command.is_one_of(Editing::CommandNames::backColor, Editing::CommandNames::fontName,
                Editing::CommandNames::foreColor, Editing::CommandNames::hiliteColor)) {
            Optional<String> first_node_value;
            auto range = Editing::active_range(*this);
            bool has_distinct_values = false;
            Editing::for_each_node_effectively_contained_in_range(range, [&](GC::Ref<Node> descendant) {
                if (!Editing::is_formattable_node(descendant))
                    return TraversalDecision::Continue;

                auto node_value = Editing::effective_command_value(descendant, command);
                if (!node_value.has_value())
                    return TraversalDecision::Continue;

                if (!first_node_value.has_value()) {
                    first_node_value = node_value.value();
                } else if (first_node_value.value() != node_value.value()) {
                    has_distinct_values = true;
                    return TraversalDecision::Break;
                }

                return TraversalDecision::Continue;
            });
            return has_distinct_values;
        }

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

                auto node_value = Editing::effective_command_value(descendant, command);
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
WebIDL::ExceptionOr<bool> Document::query_command_state(FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create(realm(), "queryCommandState is only supported on HTML documents"_string);

    // 1. If command is not supported or has no state, return false.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return false;
    auto const& command_definition = optional_command.release_value();
    auto state_override = command_state_override(command);
    if (!command_definition.state && !state_override.has_value()) {
        // https://w3c.github.io/editing/docs/execCommand/#inline-command-activated-values
        // If a command has inline command activated values defined, its state is true if either no formattable node is
        // effectively contained in the active range, and the active range's start node's effective command value is one
        // of the given values;
        auto const& inline_values = command_definition.inline_activated_values;
        if (inline_values.is_empty())
            return false;
        auto range = Editing::active_range(*this);
        Vector<GC::Ref<Node>> formattable_nodes;
        Editing::for_each_node_effectively_contained_in_range(range, [&](GC::Ref<Node> descendant) {
            if (Editing::is_formattable_node(descendant))
                formattable_nodes.append(descendant);
            return TraversalDecision::Continue;
        });
        if (formattable_nodes.is_empty())
            return inline_values.contains_slow(Editing::effective_command_value(range->start_container(), command).value_or({}));

        // or if there is at least one formattable node effectively contained in the active range, and all of them have
        // an effective command value equal to one of the given values.
        return all_of(formattable_nodes, [&](GC::Ref<Node> node) {
            return inline_values.contains_slow(Editing::effective_command_value(node, command).value_or({}));
        });
    }

    // 2. If the state override for command is set, return it.
    if (state_override.has_value())
        return state_override.release_value();

    // 3. Return true if command's state is true, otherwise false.
    return command_definition.state(*this);
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandsupported()
WebIDL::ExceptionOr<bool> Document::query_command_supported(FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create(realm(), "queryCommandSupported is only supported on HTML documents"_string);

    // When the queryCommandSupported(command) method on the Document interface is invoked, the
    // user agent must return true if command is supported and available within the current script
    // on the current site, and false otherwise.
    return Editing::find_command_definition(command).has_value();
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandvalue()
WebIDL::ExceptionOr<String> Document::query_command_value(FlyString const& command)
{
    // AD-HOC: This is not directly mentioned in the spec, but all major browsers limit editing API calls to HTML documents
    if (!is_html_document())
        return WebIDL::InvalidStateError::create(realm(), "queryCommandValue is only supported on HTML documents"_string);

    // 1. If command is not supported or has no value, return the empty string.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return String {};
    auto const& command_definition = optional_command.release_value();
    auto value_override = command_value_override(command);
    if (!command_definition.value && !value_override.has_value())
        return String {};

    // 2. If command is "fontSize" and its value override is set, convert the value override to an
    //    integer number of pixels and return the legacy font size for the result.
    if (command == Editing::CommandNames::fontSize && value_override.has_value()) {
        auto pixel_size = Editing::font_size_to_pixel_size(value_override.release_value());
        return Editing::legacy_font_size(pixel_size.to_int());
    }

    // 3. If the value override for command is set, return it.
    if (value_override.has_value())
        return value_override.release_value();

    // 4. Return command's value.
    return command_definition.value(*this);
}

// https://w3c.github.io/editing/docs/execCommand/#value-override
void Document::set_command_value_override(FlyString const& command, String const& value)
{
    m_command_value_override.set(command, value);

    // The value override for the backColor command must be the same as the value override for the hiliteColor command,
    // such that setting one sets the other to the same thing and unsetting one unsets the other.
    if (command == Editing::CommandNames::backColor)
        m_command_value_override.set(Editing::CommandNames::hiliteColor, value);
    else if (command == Editing::CommandNames::hiliteColor)
        m_command_value_override.set(Editing::CommandNames::backColor, value);
}

// https://w3c.github.io/editing/docs/execCommand/#value-override
void Document::clear_command_value_override(FlyString const& command)
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
