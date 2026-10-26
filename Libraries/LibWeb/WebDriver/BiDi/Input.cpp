/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/RefCounted.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebDriver/Actions.h>
#include <LibWeb/WebDriver/BiDi/Input.h>
#include <LibWeb/WebDriver/BiDi/RemoteValue.h>
#include <LibWeb/WebDriver/InputState.h>

namespace Web::WebDriver::BiDi {

// https://w3c.github.io/webdriver-bidi/#is-inputelementorigin
static bool is_input_element_origin(JsonValue const& object)
{
    // input.ElementOrigin = {
    //   type: "element",
    //   element: script.SharedReference
    // }

    // 1. If object is a map matching the input.ElementOrigin production, return true.
    // 2. Return false.
    if (!object.is_object() || object.as_object().get_string("type"sv) != "element"sv)
        return false;
    auto element = object.as_object().get_object("element"sv);
    if (!element.has_value() || !element->get_string("sharedId"sv).has_value())
        return false;
    if (auto handle = element->get("handle"sv); handle.has_value() && !handle->is_string())
        return false;
    return true;
}

// NB: The actions keep the shared id of the element an input.ElementOrigin names.
static String extract_input_element_origin(JsonObject const& origin)
{
    return *origin.get_object("element"sv)->get_string("sharedId"sv);
}

// https://w3c.github.io/webdriver-bidi/#get-element-from-inputelementorigin-steps
static ErrorOr<GC::Ref<DOM::Element>, Error> get_element_from_input_element_origin(HTML::BrowsingContext const& browsing_context, StringView shared_id)
{
    // 1. Assert: origin matches input.ElementOrigin.
    // 2. Let document be navigable's active document.
    auto document = browsing_context.active_document();

    // 3. Let reference be origin["element"]
    JsonObject reference;
    reference.set("sharedId"sv, shared_id);

    // 4. Let environment settings be the environment settings object whose relevant global object's associated
    //    Document is document.
    // 5. Let realm be environment settings' realm execution context's Realm component.
    auto& settings = document->relevant_settings_object();

    // 6. Let element be the result of trying to deserialize remote reference with reference, realm, and session.
    auto element_value = TRY(deserialize_shared_reference(reference, settings));

    // 7. If element doesn't implement Element return error with error code no such element.
    auto* element = element_value.is_object() ? Bindings::impl_from<DOM::Element>(&element_value.as_object()) : nullptr;
    if (!element)
        return Error::from_code(ErrorCode::NoSuchElement, "The origin is not an element"sv);

    // 8. Return success with data element.
    return GC::Ref { *element };
}

static ActionsOptions actions_options()
{
    return {
        .is_element_origin = &is_input_element_origin,
        .get_element_origin = &get_element_from_input_element_origin,
        .extract_element_origin = &extract_input_element_origin,
    };
}

// NB: The actions running for a command stay alive until they complete.
struct ActionsInFlight : RefCounted<ActionsInFlight> {
    AK_ALLOC_WITH_KMALLOC;

    Optional<GC::Root<JS::Cell>> executor;
    bool completed { false };
};

// NB: The actions of a top-level traversable wait in its input state's actions queue until the actions before them
//     are dispatched. Commands for a traversable run one at a time here, so that only one of them waits in the queue.
using InputCommand = Function<void(OnCommandComplete)>;
static HashMap<HTML::CrossProcessId, Vector<InputCommand>>& input_command_queues()
{
    static NeverDestroyed<HashMap<HTML::CrossProcessId, Vector<InputCommand>>> queues;
    return *queues;
}

static void run_the_first_input_command(HTML::CrossProcessId top_level_traversable_id)
{
    auto& queue = input_command_queues().find(top_level_traversable_id)->value;
    auto command = move(queue.first());
    command([top_level_traversable_id](Response) {
        auto queue = input_command_queues().find(top_level_traversable_id);
        queue->value.take_first();
        if (queue->value.is_empty())
            input_command_queues().remove(queue);
        else {
            // NB: An empty action sequence completes before dispatch_actions() dequeues its action queue token.
            Core::deferred_invoke([top_level_traversable_id] {
                run_the_first_input_command(top_level_traversable_id);
            });
        }
    });
}

static void enqueue_an_input_command(HTML::Navigable& navigable, OnCommandComplete on_complete, Function<void(HTML::Navigable&, OnCommandComplete)> steps)
{
    auto top_level_traversable_id = navigable.top_level_traversable()->id();
    auto& queue = input_command_queues().ensure(top_level_traversable_id);
    queue.append([navigable = GC::make_root(navigable), on_complete = move(on_complete), steps = move(steps)](OnCommandComplete on_command_complete) mutable {
        steps(*navigable, [on_complete = move(on_complete), on_command_complete = move(on_command_complete)](Response response) {
            on_complete(move(response));
            on_command_complete(JsonValue {});
        });
    });
    if (queue.size() == 1)
        run_the_first_input_command(top_level_traversable_id);
}

// https://w3c.github.io/webdriver-bidi/#command-input-performActions
static void perform_actions_steps(HTML::Navigable& navigable, JsonObject const& params, OnCommandComplete on_complete)
{
    // 1. Let navigable id be the value of the context field of command parameters.
    // 2. Let navigable be the result of trying to get a navigable with navigable id.
    auto* local_navigable = as_if<HTML::LocalNavigable>(navigable);
    if (!local_navigable || local_navigable->has_been_destroyed() || !local_navigable->active_browsing_context() || !local_navigable->active_document())
        return on_complete(Error::from_code(ErrorCode::NoSuchFrame, "Browsing context not found"sv));

    HTML::TemporaryExecutionContext execution_context { local_navigable->active_document()->relevant_settings_object() };

    // 3. Let input state be get the input state with session and navigable's top-level traversable.
    auto& input_state = get_input_state(*navigable.top_level_traversable());

    // 4. Let actions options be a new actions options with the is element origin steps set to is input.ElementOrigin,
    //    and the get element origin steps set to the result of get Element from input.ElementOrigin steps given session.
    auto options = actions_options();

    // 5. Let actions by tick be the result of trying to extract an action sequence with input state, command parameters,
    //    and actions options.
    auto actions_by_tick = extract_an_action_sequence(input_state, JsonValue { params }, options);
    if (actions_by_tick.is_error())
        return on_complete(actions_by_tick.release_error());

    // 6. Try to dispatch actions with input state, actions by tick, navigable, and actions options.
    // 7. Return success with data null.
    auto in_flight = adopt_ref(*new ActionsInFlight);
    auto on_actions_complete = GC::create_function(GC::Heap::the(), [in_flight, on_complete = move(on_complete)](Response result) {
        in_flight->completed = true;
        in_flight->executor.clear();
        on_complete(move(result));
    });

    auto executor = dispatch_actions(input_state, actions_by_tick.release_value(), *local_navigable->active_browsing_context(), options, on_actions_complete);
    if (!in_flight->completed)
        in_flight->executor = GC::make_root(executor);
}

void perform_actions(HTML::Navigable& navigable, JsonObject const& params, OnCommandComplete on_complete)
{
    enqueue_an_input_command(navigable, move(on_complete), [params](HTML::Navigable& navigable, OnCommandComplete on_complete) {
        perform_actions_steps(navigable, params, move(on_complete));
    });
}

// https://w3c.github.io/webdriver-bidi/#command-input-releaseActions
static void release_actions_steps(HTML::Navigable& navigable, OnCommandComplete on_complete)
{
    // 1. Let navigable id be the value of the context field of command parameters.
    // 2. Let navigable be the result of trying to get a navigable with navigable id.
    auto* local_navigable = as_if<HTML::LocalNavigable>(navigable);
    if (!local_navigable || local_navigable->has_been_destroyed() || !local_navigable->active_browsing_context() || !local_navigable->active_document())
        return on_complete(Error::from_code(ErrorCode::NoSuchFrame, "Browsing context not found"sv));

    HTML::TemporaryExecutionContext execution_context { local_navigable->active_document()->relevant_settings_object() };

    // 3. Let top-level traversable be navigable's top-level traversable.
    GC::Ref top_level_traversable = *navigable.top_level_traversable();

    // 4. Let input state be get the input state with session and top-level traversable.
    auto& input_state = get_input_state(top_level_traversable);

    // 5. Let actions options be a new actions options with the is element origin steps set to is input.ElementOrigin,
    //    and the get element origin steps set to get Element from input.ElementOrigin steps given session.
    auto options = actions_options();

    // 6. Let undo actions be input state’s input cancel list in reverse order.
    auto undo_actions = input_state.input_cancel_list;
    undo_actions.reverse();

    // 7. Try to dispatch tick actions with undo actions, 0, navigable, and actions options.
    // NB: The actions are dispatched the way WebDriver's Release Actions command dispatches them, which waits for an
    //     action queue token first.
    wait_for_an_action_queue_token(input_state);
    input_state.actions_queue.take_first();

    auto in_flight = adopt_ref(*new ActionsInFlight);
    auto on_actions_complete = GC::create_function(GC::Heap::the(), [in_flight, top_level_traversable, on_complete = move(on_complete)](Response result) {
        in_flight->completed = true;
        in_flight->executor.clear();

        // 8. Reset the input state with session and top-level traversable.
        reset_input_state(top_level_traversable);

        // 9. Return success with data null.
        on_complete(move(result));
    });

    auto executor = dispatch_actions(input_state, { move(undo_actions) }, *local_navigable->active_browsing_context(), options, on_actions_complete);
    if (!in_flight->completed)
        in_flight->executor = GC::make_root(executor);
}

void release_actions(HTML::Navigable& navigable, OnCommandComplete on_complete)
{
    enqueue_an_input_command(navigable, move(on_complete), [](HTML::Navigable& navigable, OnCommandComplete on_complete) {
        release_actions_steps(navigable, move(on_complete));
    });
}

}
