/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/CustomElements/CustomElementAlgorithms.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactions.h>
#include <LibWeb/HTML/HTMLFormElement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#invoke-custom-element-reactions
void invoke_custom_element_reactions(Vector<GC::Weak<DOM::Element>>& element_queue)
{
    // 1. While queue is not empty:
    while (!element_queue.is_empty()) {
        // 1. Let element be the result of dequeuing from queue.
        auto element = element_queue.take_first();
        if (!element)
            continue;

        // 2. Let reactions be element's custom element reaction queue.
        auto* reactions = element->custom_element_reaction_queue();

        // 3. Repeat until reactions is empty:
        if (!reactions)
            continue;
        while (!reactions->is_empty()) {
            // 1. Remove the first element of reactions, and let reaction be that element. Switch on reaction's type:
            auto reaction = reactions->take_first();

            reaction.visit(
                [&](DOM::CustomElementUpgradeReaction const& custom_element_upgrade_reaction) -> void {
                    // -> upgrade reaction
                    //      Upgrade element using reaction's custom element definition.
                    auto maybe_exception = Bindings::upgrade_custom_element(*element, custom_element_upgrade_reaction.custom_element_definition);
                    // If this throws an exception, catch it, and report it for reaction's custom element definition's constructor's corresponding JavaScript object's associated realm's global object.
                    if (maybe_exception.is_error())
                        Bindings::report_custom_element_upgrade_exception(*custom_element_upgrade_reaction.custom_element_definition, maybe_exception.error_value());
                },
                [&](DOM::CustomElementCallbackReaction& custom_element_callback_reaction) -> void {
                    // -> callback reaction
                    //      Invoke reaction's callback function with reaction's arguments and "report", and callback this value set to element.
                    Bindings::invoke_custom_element_callback_reaction(*element, *custom_element_callback_reaction.callback, custom_element_callback_reaction.arguments);
                },
                [&](DOM::CustomElementConnectedMoveCallbackReaction& connected_move_reaction) -> void {
                    // -> callback reaction with connectedMoveCallback fallback steps
                    //      If disconnectedCallback is not null, then call disconnectedCallback with no arguments.
                    if (connected_move_reaction.disconnected_callback)
                        Bindings::invoke_custom_element_lifecycle_callback(*element, *connected_move_reaction.disconnected_callback);

                    //      If connectedCallback is not null, then call connectedCallback with no arguments.
                    if (connected_move_reaction.connected_callback)
                        Bindings::invoke_custom_element_lifecycle_callback(*element, *connected_move_reaction.connected_callback);
                });
        }
    }
}

}
