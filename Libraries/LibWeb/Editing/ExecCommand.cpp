/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/Editing/Commands.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::DOM {

// https://w3c.github.io/editing/docs/execCommand/#execcommand()
bool Document::exec_command(FlyString const& command, [[maybe_unused]] bool show_ui, String const& value)
{
    // 1. If only one argument was provided, let show UI be false.
    // 2. If only one or two arguments were provided, let value be the empty string.
    // NOTE: these steps are dealt by the default values for both show_ui and value

    // 3. If command is not supported or not enabled, return false.
    // NOTE: query_command_enabled() also checks if command is supported
    if (!query_command_enabled(command))
        return false;

    // 4. If command is not in the Miscellaneous commands section:
    //
    //    We don't fire events for copy/cut/paste/undo/redo/selectAll because they should all have
    //    their own events. We don't fire events for styleWithCSS/useCSS because it's not obvious
    //    where to fire them, or why anyone would want them. We don't fire events for unsupported
    //    commands, because then if they became supported and were classified with the miscellaneous
    //    events, we'd have to stop firing events for consistency's sake.
    //
    // AD-HOC: The defaultParagraphSeparator command is also in the Miscellaneous commands section
    if (command != Editing::CommandNames::defaultParagraphSeparator
        && command != Editing::CommandNames::redo
        && command != Editing::CommandNames::selectAll
        && command != Editing::CommandNames::styleWithCSS
        && command != Editing::CommandNames::undo
        && command != Editing::CommandNames::useCSS) {
        // FIXME: 1. Let affected editing host be the editing host that is an inclusive ancestor of the
        //    active range's start node and end node, and is not the ancestor of any editing host
        //    that is an inclusive ancestor of the active range's start node and end node.

        // FIXME: 2. Fire an event named "beforeinput" at affected editing host using InputEvent, with its
        //    bubbles and cancelable attributes initialized to true, and its data attribute
        //    initialized to null.

        // FIXME: 3. If the value returned by the previous step is false, return false.

        // 4. If command is not enabled, return false.
        //
        //    We have to check again whether the command is enabled, because the beforeinput handler
        //    might have done something annoying like getSelection().removeAllRanges().
        if (!query_command_enabled(command))
            return false;

        // FIXME: 5. Let affected editing host be the editing host that is an inclusive ancestor of the
        //    active range's start node and end node, and is not the ancestor of any editing host
        //    that is an inclusive ancestor of the active range's start node and end node.
        //
        //    This new affected editing host is what we'll fire the input event at in a couple of
        //    lines. We want to compute it beforehand just to be safe: bugs in the command action
        //    might remove the selection or something bad like that, and we don't want to have to
        //    handle it later. We recompute it after the beforeinput event is handled so that if the
        //    handler moves the selection to some other editing host, the input event will be fired
        //    at the editing host that was actually affected.
    }

    // 5. Take the action for command, passing value to the instructions as an argument.
    auto optional_command = Editing::find_command_definition(command);
    VERIFY(optional_command.has_value());
    auto const& command_definition = optional_command.release_value();
    auto command_result = command_definition.action(*this, value);

    // 6. If the previous step returned false, return false.
    if (!command_result)
        return false;

    // FIXME: 7. If the action modified DOM tree, then fire an event named "input" at affected editing host
    //    using InputEvent, with its isTrusted and bubbles attributes initialized to true, inputType
    //    attribute initialized to the mapped value of command, and its data attribute initialized
    //    to null.

    // 8. Return true.
    return true;
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandenabled()
bool Document::query_command_enabled(FlyString const& command)
{
    // 2. Return true if command is both supported and enabled, false otherwise.
    if (!query_command_supported(command))
        return false;

    // https://w3c.github.io/editing/docs/execCommand/#enabled
    // Among commands defined in this specification, those listed in Miscellaneous commands are always enabled, except
    // for the cut command and the paste command.
    // NOTE: cut and paste are actually in the Clipboard commands section
    if (command.is_one_of(
            Editing::CommandNames::defaultParagraphSeparator,
            Editing::CommandNames::redo,
            Editing::CommandNames::selectAll,
            Editing::CommandNames::styleWithCSS,
            Editing::CommandNames::undo,
            Editing::CommandNames::useCSS))
        return true;

    // The other commands defined here are enabled if the active range is not null,
    auto selection = get_selection();
    if (!selection)
        return false;
    auto active_range = selection->range();
    if (!active_range)
        return false;

    // its start node is either editable or an editing host,
    auto start_node = active_range->start_container();
    if (!start_node->is_editable() && !Editing::is_editing_host(start_node))
        return false;

    // FIXME: the editing host of its start node is not an EditContext editing host,
    auto start_node_editing_host = Editing::editing_host_of_node(start_node);

    // its end node is either editable or an editing host,
    auto& end_node = *active_range->end_container();
    if (!end_node.is_editable() && !Editing::is_editing_host(end_node))
        return false;

    // FIXME: the editing host of its end node is not an EditContext editing host,

    // FIXME: and there is some editing host that is an inclusive ancestor of both its start node and its
    //        end node.

    // NOTE: Commands can define additional conditions for being enabled, and currently the only condition mentioned in
    //       the spec is that certain commands must not be enabled if the editing host is in the plaintext-only state.
    if (is<HTML::HTMLElement>(start_node_editing_host.ptr())
        && static_cast<HTML::HTMLElement&>(*start_node_editing_host).content_editable() == "plaintext-only"sv
        && command.is_one_of(
            Editing::CommandNames::backColor,
            Editing::CommandNames::bold,
            Editing::CommandNames::createLink,
            Editing::CommandNames::fontName,
            Editing::CommandNames::fontSize,
            Editing::CommandNames::foreColor,
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
bool Document::query_command_indeterm(FlyString const& command)
{
    // 1. If command is not supported or has no indeterminacy, return false.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return false;
    auto const& command_definition = optional_command.value();
    if (!command_definition.indeterminate)
        return false;

    // 2. Return true if command is indeterminate, otherwise false.
    return command_definition.indeterminate(*this);
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandstate()
bool Document::query_command_state(FlyString const& command)
{
    // 1. If command is not supported or has no state, return false.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return false;
    auto const& command_definition = optional_command.release_value();
    if (!command_definition.state)
        return false;

    // FIXME: 2. If the state override for command is set, return it.

    // 3. Return true if command's state is true, otherwise false.
    return command_definition.state(*this);
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandsupported()
bool Document::query_command_supported(FlyString const& command)
{
    // When the queryCommandSupported(command) method on the Document interface is invoked, the
    // user agent must return true if command is supported and available within the current script
    // on the current site, and false otherwise.
    return Editing::find_command_definition(command).has_value();
}

// https://w3c.github.io/editing/docs/execCommand/#querycommandvalue()
String Document::query_command_value(FlyString const& command)
{
    // 1. If command is not supported or has no value, return the empty string.
    auto optional_command = Editing::find_command_definition(command);
    if (!optional_command.has_value())
        return {};
    auto const& command_definition = optional_command.release_value();
    if (!command_definition.value)
        return {};

    // FIXME: 2. If command is "fontSize" and its value override is set, convert the value override to an
    //    integer number of pixels and return the legacy font size for the result.

    // FIXME: 3. If the value override for command is set, return it.

    // 4. Return command's value.
    return command_definition.value(*this);
}

}
