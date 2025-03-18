/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 * Copyright (c) 2025, Felipe Muñoz Mazur <felipe.munoz.mazur@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/EventTargetPrototype.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/BeforeUnloadEvent.h>
#include <LibWeb/HTML/CloseWatcherManager.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(EventTarget);

EventTarget::EventTarget(JS::Realm& realm, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : PlatformObject(realm, may_interfere_with_indexed_property_access)
{
}

EventTarget::~EventTarget() = default;

// https://dom.spec.whatwg.org/#dom-eventtarget-eventtarget
WebIDL::ExceptionOr<GC::Ref<EventTarget>> EventTarget::construct_impl(JS::Realm& realm)
{
    // The new EventTarget() constructor steps are to do nothing.
    return realm.create<EventTarget>(realm);
}

void EventTarget::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    // FIXME: We can't do this for HTML::Window or HTML::WorkerGlobalScope, as this will run when creating the initial global object.
    //        During this time, the ESO is not setup, so it will cause a nullptr dereference in host_defined_intrinsics.
    if (!is<HTML::WindowOrWorkerGlobalScopeMixin>(this))
        WEB_SET_PROTOTYPE_FOR_INTERFACE(EventTarget);
}

void EventTarget::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    if (auto const* data = m_data.ptr()) {
        visitor.visit(data->event_listener_list);
        visitor.visit(data->event_handler_map);
    }
}

Vector<GC::Root<DOMEventListener>> EventTarget::event_listener_list()
{
    Vector<GC::Root<DOMEventListener>> list;
    if (!m_data)
        return list;
    for (auto& listener : m_data->event_listener_list)
        list.append(*listener);
    return list;
}

// https://dom.spec.whatwg.org/#concept-flatten-options
static bool flatten_event_listener_options(Variant<EventListenerOptions, bool> const& options)
{
    // 1. If options is a boolean, then return options.
    if (options.has<bool>())
        return options.get<bool>();

    // 2. Return options["capture"].
    return options.get<EventListenerOptions>().capture;
}

static bool flatten_event_listener_options(Variant<AddEventListenerOptions, bool> const& options)
{
    // 1. If options is a boolean, then return options.
    if (options.has<bool>())
        return options.get<bool>();

    // 2. Return options["capture"].
    return options.get<AddEventListenerOptions>().capture;
}

struct FlattenedAddEventListenerOptions {
    bool capture { false };
    Optional<bool> passive;
    bool once { false };
    GC::Ptr<AbortSignal> signal;
};

// https://dom.spec.whatwg.org/#event-flatten-more
static FlattenedAddEventListenerOptions flatten_add_event_listener_options(Variant<AddEventListenerOptions, bool> const& options)
{
    // 1. Let capture be the result of flattening options.
    bool capture = flatten_event_listener_options(options);

    // 2. Let once be false.
    bool once = false;

    // 3. Let passive and signal be null.
    Optional<bool> passive;
    GC::Ptr<AbortSignal> signal;

    // 4. If options is a dictionary, then:
    if (options.has<AddEventListenerOptions>()) {
        auto const& add_event_listener_options = options.get<AddEventListenerOptions>();

        // 1. Set once to options["once"].
        once = add_event_listener_options.once;

        // 2. If options["passive"] exists, then set passive to options["passive"].
        if (add_event_listener_options.passive.has_value())
            passive = add_event_listener_options.passive;

        // 3. If options["signal"] exists, then set signal to options["signal"].
        if (add_event_listener_options.signal)
            signal = add_event_listener_options.signal;
    }

    // 5. Return capture, passive, once, and signal.
    return FlattenedAddEventListenerOptions { .capture = capture, .passive = passive, .once = once, .signal = signal.ptr() };
}

// https://dom.spec.whatwg.org/#default-passive-value
static bool default_passive_value(FlyString const& type, EventTarget* event_target)
{
    // 1. Return true if all of the following are true:
    //    - type is one of "touchstart", "touchmove", "wheel", or "mousewheel".
    //    - eventTarget is a Window object, or is a node whose node document is eventTarget, or is a node whose node document’s document element is eventTarget,
    //      or is a node whose node document’s body element is eventTarget.
    if (AK::first_is_one_of(type, "touchstart"sv, "touchmove"sv, "wheel"sv, "mousewheel"sv)) {
        if (is<HTML::Window>(event_target))
            return true;

        if (is<Node>(event_target)) {
            auto* node = as<Node>(event_target);
            if (&node->document() == event_target || node->document().document_element() == event_target || node->document().body() == event_target)
                return true;
        }
    }

    // 2. Return false.
    return false;
}

// https://dom.spec.whatwg.org/#dom-eventtarget-addeventlistener
void EventTarget::add_event_listener(FlyString const& type, IDLEventListener* callback, Variant<AddEventListenerOptions, bool> const& options)
{
    // 1. Let capture, passive, once, and signal be the result of flattening more options.
    auto flattened_options = flatten_add_event_listener_options(options);

    // 2. Add an event listener with this and an event listener whose type is type, callback is callback, capture is capture, passive is passive,
    //    once is once, and signal is signal.

    auto event_listener = heap().allocate<DOMEventListener>();
    event_listener->type = type;
    event_listener->callback = callback;
    event_listener->signal = move(flattened_options.signal);
    event_listener->capture = flattened_options.capture;
    event_listener->passive = flattened_options.passive;
    event_listener->once = flattened_options.once;
    add_an_event_listener(*event_listener);
}

void EventTarget::add_event_listener_without_options(FlyString const& type, IDLEventListener& callback)
{
    add_event_listener(type, &callback, AddEventListenerOptions {});
}

// https://dom.spec.whatwg.org/#add-an-event-listener
void EventTarget::add_an_event_listener(DOMEventListener& listener)
{
    // FIXME: 1. If eventTarget is a ServiceWorkerGlobalScope object, its service worker’s script resource’s has ever been evaluated flag is set,
    //           and listener’s type matches the type attribute value of any of the service worker events, then report a warning to the console
    //           that this might not give the expected results. [SERVICE-WORKERS]

    auto& event_listener_list = ensure_data().event_listener_list;

    // 2. If listener’s signal is not null and is aborted, then return.
    if (listener.signal && listener.signal->aborted())
        return;

    // 3. If listener’s callback is null, then return.
    if (!listener.callback)
        return;

    // 4. If listener’s passive is null, then set it to the default passive value given listener’s type and eventTarget.
    if (!listener.passive.has_value()) {
        listener.passive = default_passive_value(listener.type, this);
    }

    // 5. If eventTarget’s event listener list does not contain an event listener whose type is listener’s type, callback is listener’s callback,
    //    and capture is listener’s capture, then append listener to eventTarget’s event listener list.
    auto it = event_listener_list.find_if([&](auto& entry) {
        return entry->type == listener.type
            && entry->callback->callback().callback == listener.callback->callback().callback
            && entry->capture == listener.capture;
    });
    if (it == event_listener_list.end())
        event_listener_list.append(listener);

    // 6. If listener’s signal is not null, then add the following abort steps to it:
    if (listener.signal) {
        // NOTE: `this` and `listener` are protected by AbortSignal using GC::HeapFunction.
        listener.signal->add_abort_algorithm([this, &listener] {
            // 1. Remove an event listener with eventTarget and listener.
            remove_an_event_listener(listener);
        });
    }
}

// https://dom.spec.whatwg.org/#dom-eventtarget-removeeventlistener
void EventTarget::remove_event_listener(FlyString const& type, IDLEventListener* callback, Variant<EventListenerOptions, bool> const& options)
{
    auto& event_listener_list = ensure_data().event_listener_list;

    // 1. Let capture be the result of flattening options.
    bool capture = flatten_event_listener_options(options);

    // 2. If this’s event listener list contains an event listener whose type is type, callback is callback, and capture is capture,
    //    then remove an event listener with this and that event listener.
    auto callbacks_match = [&](DOMEventListener& entry) {
        if (!entry.callback && !callback)
            return true;
        if (!entry.callback || !callback)
            return false;
        return entry.callback->callback().callback == callback->callback().callback;
    };
    auto it = event_listener_list.find_if([&](auto& entry) {
        return entry->type == type
            && callbacks_match(*entry)
            && entry->capture == capture;
    });
    if (it != event_listener_list.end())
        remove_an_event_listener(**it);
}

void EventTarget::remove_event_listener_without_options(FlyString const& type, IDLEventListener& callback)
{
    remove_event_listener(type, &callback, EventListenerOptions {});
}

// https://dom.spec.whatwg.org/#remove-an-event-listener
void EventTarget::remove_an_event_listener(DOMEventListener& listener)
{
    // FIXME: 1. If eventTarget is a ServiceWorkerGlobalScope object and its service worker’s set of event types to handle contains type,
    //           then report a warning to the console that this might not give the expected results. [SERVICE-WORKERS]

    // 2. Set listener’s removed to true and remove listener from eventTarget’s event listener list.
    listener.removed = true;
    VERIFY(m_data);
    m_data->event_listener_list.remove_first_matching([&](auto& entry) { return entry.ptr() == &listener; });
}

// https://dom.spec.whatwg.org/#dom-eventtarget-dispatchevent
WebIDL::ExceptionOr<bool> EventTarget::dispatch_event_binding(Event& event)
{
    // 1. If event’s dispatch flag is set, or if its initialized flag is not set, then throw an "InvalidStateError" DOMException.
    if (event.dispatched())
        return WebIDL::InvalidStateError::create(realm(), "The event is already being dispatched."_string);

    if (!event.initialized())
        return WebIDL::InvalidStateError::create(realm(), "Cannot dispatch an uninitialized event."_string);

    // 2. Initialize event’s isTrusted attribute to false.
    event.set_is_trusted(false);

    // 3. Return the result of dispatching event to this.
    return dispatch_event(event);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#window-reflecting-body-element-event-handler-set
bool is_window_reflecting_body_element_event_handler(FlyString const& name)
{
    return name.is_one_of(
        HTML::EventNames::blur,
        HTML::EventNames::error,
        HTML::EventNames::focus,
        HTML::EventNames::load,
        UIEvents::EventNames::resize,
        "scroll");
}

// https://html.spec.whatwg.org/multipage/webappapis.html#windoweventhandlers
static bool is_window_event_handler(FlyString const& name)
{
    return name.is_one_of(
        HTML::EventNames::afterprint,
        HTML::EventNames::beforeprint,
        HTML::EventNames::beforeunload,
        HTML::EventNames::hashchange,
        HTML::EventNames::languagechange,
        HTML::EventNames::message,
        HTML::EventNames::messageerror,
        HTML::EventNames::offline,
        HTML::EventNames::online,
        HTML::EventNames::pagehide,
        HTML::EventNames::pageshow,
        HTML::EventNames::popstate,
        HTML::EventNames::rejectionhandled,
        HTML::EventNames::storage,
        HTML::EventNames::unhandledrejection,
        HTML::EventNames::unload);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#determining-the-target-of-an-event-handler
static EventTarget* determine_target_of_event_handler(EventTarget& event_target, FlyString const& name)
{
    // To determine the target of an event handler, given an EventTarget object eventTarget on which the event handler is exposed,
    // and an event handler name name, the following steps are taken:

    // 1. If eventTarget is not a body element or a frameset element, then return eventTarget.
    if (!is<HTML::HTMLBodyElement>(event_target) && !is<HTML::HTMLFrameSetElement>(event_target))
        return &event_target;

    auto& event_target_element = static_cast<HTML::HTMLElement&>(event_target);

    // 2. If name is not the name of an attribute member of the WindowEventHandlers interface mixin and the Window-reflecting
    //    body element event handler set does not contain name, then return eventTarget.
    if (!is_window_event_handler(name) && !is_window_reflecting_body_element_event_handler(name))
        return &event_target;

    // 3. If eventTarget's node document is not an active document, then return null.
    if (!event_target_element.document().is_active())
        return nullptr;

    // 4. Return eventTarget's node document's relevant global object.
    return &as<EventTarget>(HTML::relevant_global_object(event_target_element.document()));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-attributes:event-handler-idl-attributes-2
WebIDL::CallbackType* EventTarget::event_handler_attribute(FlyString const& name)
{
    // 1. Let eventTarget be the result of determining the target of an event handler given this object and name.
    auto target = determine_target_of_event_handler(*this, name);

    // 2. If eventTarget is null, then return null.
    if (!target)
        return nullptr;

    // 3. Return the result of getting the current value of the event handler given eventTarget and name.
    return target->get_current_value_of_event_handler(name);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#getting-the-current-value-of-the-event-handler
WebIDL::CallbackType* EventTarget::get_current_value_of_event_handler(FlyString const& name)
{
    // 1. Let handlerMap be eventTarget's event handler map. (NOTE: Not necessary)
    auto& handler_map = ensure_data().event_handler_map;

    // 2. Let eventHandler be handlerMap[name].
    auto event_handler_iterator = handler_map.find(name);

    // Optimization: The spec creates all the event handlers exposed on an object up front and has the initial value of the handler set to null.
    //               If the event handler hasn't been set, null would be returned in step 4.
    //               However, this would be very allocation heavy. For example, each DOM::Element includes GlobalEventHandlers, which defines 60+(!) event handler attributes.
    //               Plus, the vast majority of these allocations would be likely wasted, as I imagine web content will only use a handful of these attributes on certain elements, if any at all.
    //               Thus, we treat the event handler not being in the event handler map as being equivalent to an event handler with an initial null value.
    if (event_handler_iterator == handler_map.end())
        return nullptr;

    auto& event_handler = event_handler_iterator->value;

    // 3. If eventHandler's value is an internal raw uncompiled handler, then:
    if (event_handler->value.has<ByteString>()) {
        // 1. If eventTarget is an element, then let element be eventTarget, and document be element's node document.
        //    Otherwise, eventTarget is a Window object, let element be null, and document be eventTarget's associated Document.
        GC::Ptr<Element> element;
        GC::Ptr<Document> document;

        if (is<Element>(this)) {
            auto* element_event_target = as<Element>(this);
            element = element_event_target;
            document = &element_event_target->document();
        } else {
            VERIFY(is<HTML::Window>(this));
            auto* window_event_target = as<HTML::Window>(this);
            document = &window_event_target->associated_document();
        }

        VERIFY(document);

        // 2. If scripting is disabled for document, then return null.
        if (document->is_scripting_disabled())
            return nullptr;

        // 3. Let body be the uncompiled script body in eventHandler's value.
        auto& body = event_handler->value.get<ByteString>();

        // FIXME: 4. Let location be the location where the script body originated, as given by eventHandler's value.

        // 5. If element is not null and element has a form owner, let form owner be that form owner. Otherwise, let form owner be null.
        GC::Ptr<HTML::HTMLFormElement> form_owner;
        if (is<HTML::FormAssociatedElement>(element.ptr())) {
            auto* form_associated_element = dynamic_cast<HTML::FormAssociatedElement*>(element.ptr());
            VERIFY(form_associated_element);

            if (form_associated_element->form())
                form_owner = form_associated_element->form();
        }

        // 6. Let settings object be the relevant settings object of document.
        auto& settings_object = document->relevant_settings_object();

        // NOTE: ECMAScriptFunctionObject::create expects a parsed body as input, so we must do the spec's sourceText steps here.
        StringBuilder builder;

        // sourceText
        if (name == HTML::EventNames::error && is<HTML::Window>(this)) {
            //  -> If name is onerror and eventTarget is a Window object
            //      The string formed by concatenating "function ", name, "(event, source, lineno, colno, error) {", U+000A LF, body, U+000A LF, and "}".
            builder.appendff("function {}(event, source, lineno, colno, error) {{\n{}\n}}", name, body);
        } else {
            //  -> Otherwise
            //      The string formed by concatenating "function ", name, "(event) {", U+000A LF, body, U+000A LF, and "}".
            builder.appendff("function {}(event) {{\n{}\n}}", name, body);
        }

        auto source_text = builder.to_byte_string();

        auto parser = JS::Parser(JS::Lexer(source_text));

        // FIXME: This should only be parsing the `body` instead of `source_text` and therefore use `JS::FunctionBody` instead of `JS::FunctionExpression`.
        //        However, JS::ECMAScriptFunctionObject::create wants parameters and length and JS::FunctionBody does not inherit JS::FunctionNode.
        auto program = parser.parse_function_node<JS::FunctionExpression>();

        // 7. If body is not parsable as FunctionBody or if parsing detects an early error, then follow these substeps:
        if (parser.has_errors()) {
            // 1. Set eventHandler's value to null.
            //    Note: This does not deactivate the event handler, which additionally removes the event handler's listener (if present).
            handler_map.remove(event_handler_iterator);

            // FIXME: 2. Report the error for the appropriate script and with the appropriate position (line number and column number) given by location, using settings object's global object.
            //           If the error is still not handled after this, then the error may be reported to a developer console.

            // 3. Return null.
            return nullptr;
        }

        auto& vm = Bindings::main_thread_vm();

        // 8. Push settings object's realm execution context onto the JavaScript execution context stack; it is now the running JavaScript execution context.
        vm.push_execution_context(settings_object.realm_execution_context());

        // 9. Let function be the result of calling OrdinaryFunctionCreate, with arguments:
        // functionPrototype
        //  %Function.prototype% (This is enforced by using JS::ECMAScriptFunctionObject)

        // sourceText was handled above.

        // ParameterList
        //  If name is onerror and eventTarget is a Window object
        //    Let the function have five arguments, named event, source, lineno, colno, and error.
        //  Otherwise
        //    Let the function have a single argument called event.
        // (This was handled above for us by the parser using sourceText)

        // body
        //  The result of parsing body above. (This is given by program->body())

        // thisMode
        //  non-lexical-this (For JS::ECMAScriptFunctionObject, this means passing is_arrow_function as false)
        constexpr bool is_arrow_function = false;

        // scope
        //  1. Let realm be settings object's Realm.
        auto& realm = settings_object.realm();

        //  2. Let scope be realm.[[GlobalEnv]].
        auto scope = GC::Ref<JS::Environment> { realm.global_environment() };

        // 3. If eventHandler is an element's event handler, then set scope to NewObjectEnvironment(document, true, scope).
        //    (Otherwise, eventHandler is a Window object's event handler.)
        if (is<Element>(this))
            scope = JS::new_object_environment(*document, true, scope);

        //  4. If form owner is not null, then set scope to NewObjectEnvironment(form owner, true, scope).
        if (form_owner)
            scope = JS::new_object_environment(*form_owner, true, scope);

        //  5. If element is not null, then set scope to NewObjectEnvironment(element, true, scope).
        if (element)
            scope = JS::new_object_environment(*element, true, scope);

        //  6. Return scope. (NOTE: Not necessary)

        auto function = JS::ECMAScriptFunctionObject::create(realm, name, builder.to_byte_string(), program->body(), program->parameters(), program->function_length(), program->local_variables_names(), scope, nullptr, JS::FunctionKind::Normal, program->is_strict_mode(),
            program->parsing_insights(), is_arrow_function);

        // 10. Remove settings object's realm execution context from the JavaScript execution context stack.
        VERIFY(vm.execution_context_stack().last() == &settings_object.realm_execution_context());
        vm.pop_execution_context();

        // 11. Set function.[[ScriptOrModule]] to null.
        function->set_script_or_module({});

        // 12. Set eventHandler's value to the result of creating a Web IDL EventHandler callback function object whose object reference is function and whose callback context is settings object.
        // FIXME: Update this comment once the ShadowRealm proposal is merged to pass realm.
        event_handler->value = GC::Ptr(realm.heap().allocate<WebIDL::CallbackType>(*function, realm));
    }

    // 4. Return eventHandler's value.
    VERIFY(event_handler->value.has<GC::Ptr<WebIDL::CallbackType>>());
    return *event_handler->value.get_pointer<GC::Ptr<WebIDL::CallbackType>>();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-attributes:event-handler-idl-attributes-3
void EventTarget::set_event_handler_attribute(FlyString const& name, WebIDL::CallbackType* value)
{
    // 1. Let eventTarget be the result of determining the target of an event handler given this object and name.
    auto event_target = determine_target_of_event_handler(*this, name);

    // 2. If eventTarget is null, then return.
    if (!event_target)
        return;

    // 3. If the given value is null, then deactivate an event handler given eventTarget and name.
    if (!value) {
        event_target->deactivate_event_handler(name);
        return;
    }

    // 4. Otherwise:
    //  1. Let handlerMap be eventTarget's event handler map.
    auto& handler_map = event_target->ensure_data().event_handler_map;

    //  2. Let eventHandler be handlerMap[name].
    auto event_handler_iterator = handler_map.find(name);

    //  3. Set eventHandler's value to the given value.
    if (event_handler_iterator == handler_map.end()) {
        // NOTE: See the optimization comment in get_current_value_of_event_handler about why this is done.
        auto new_event_handler = heap().allocate<HTML::EventHandler>(*value);

        //  4. Activate an event handler given eventTarget and name.
        // Optimization: We pass in the event handler here instead of having activate_event_handler do another hash map lookup just to get the same object.
        //               This handles a new event handler while the other path handles an existing event handler. As such, both paths must have their own
        //               unique call to activate_event_handler.
        event_target->activate_event_handler(name, *new_event_handler);

        handler_map.set(name, new_event_handler);
        return;
    }

    auto& event_handler = event_handler_iterator->value;

    event_handler->value = GC::Ptr(value);

    //  4. Activate an event handler given eventTarget and name.
    //  NOTE: See the optimization comment above.
    event_target->activate_event_handler(name, *event_handler);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#activate-an-event-handler
void EventTarget::activate_event_handler(FlyString const& name, HTML::EventHandler& event_handler)
{
    // 1. Let handlerMap be eventTarget's event handler map.
    // 2. Let eventHandler be handlerMap[name].
    // NOTE: These are achieved by using the passed in event handler.

    // 3. If eventHandler's listener is not null, then return.
    if (event_handler.listener)
        return;

    JS::Realm& realm = shape().realm();

    // 4. Let callback be the result of creating a Web IDL EventListener instance representing a reference to a function of one argument that executes the steps of the event handler processing algorithm, given eventTarget, name, and its argument.
    //    The EventListener's callback context can be arbitrary; it does not impact the steps of the event handler processing algorithm. [DOM]

    // NOTE: The callback must keep `this` alive. For example:
    //          document.body.onunload = () => { console.log("onunload called!"); }
    //          document.body.remove();
    //          location.reload();
    //       The body element is no longer in the DOM and there is no variable holding onto it. However, the onunload handler is still called, meaning the callback keeps the body element alive.
    auto callback_function = JS::NativeFunction::create(
        realm, [event_target = this, name](JS::VM& vm) mutable -> JS::ThrowCompletionOr<JS::Value> {
            // The event dispatcher should only call this with one argument.
            VERIFY(vm.argument_count() == 1);

            // The argument must be an object and it must be an Event.
            auto event_wrapper_argument = vm.argument(0);
            VERIFY(event_wrapper_argument.is_object());
            auto& event = as<DOM::Event>(event_wrapper_argument.as_object());

            TRY(event_target->process_event_handler_for_event(name, event));
            return JS::js_undefined();
        },
        0, FlyString {}, &realm);

    // NOTE: As per the spec, the callback context is arbitrary.
    auto callback = realm.heap().allocate<WebIDL::CallbackType>(*callback_function, realm);

    // 5. Let listener be a new event listener whose type is the event handler event type corresponding to eventHandler and callback is callback.
    auto listener = realm.heap().allocate<DOMEventListener>();
    listener->type = name;
    listener->callback = IDLEventListener::create(realm, *callback);

    // 6. Add an event listener with eventTarget and listener.
    add_an_event_listener(*listener);

    // 7. Set eventHandler's listener to listener.
    event_handler.listener = listener;
}

void EventTarget::deactivate_event_handler(FlyString const& name)
{
    // 1. Let handlerMap be eventTarget's event handler map.
    auto& handler_map = ensure_data().event_handler_map;

    // 2. Let eventHandler be handlerMap[name].
    auto event_handler_iterator = handler_map.find(name);

    // NOTE: See the optimization comment in get_current_value_of_event_handler about why this is done.
    if (event_handler_iterator == handler_map.end())
        return;

    auto& event_handler = event_handler_iterator->value;

    // 4. Let listener be eventHandler's listener. (NOTE: Not necessary)

    // 5. If listener is not null, then remove an event listener with eventTarget and listener.
    if (event_handler->listener) {
        remove_an_event_listener(*event_handler->listener);
    }

    // 6. Set eventHandler's listener to null.
    event_handler->listener = nullptr;

    // 3. Set eventHandler's value to null.
    // NOTE: This is done out of order since our equivalent of setting value to null is removing the event handler from the map.
    //       Given that event_handler is a reference to an entry, this would invalidate event_handler if we did it in order.
    handler_map.remove(event_handler_iterator);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#the-event-handler-processing-algorithm
JS::ThrowCompletionOr<void> EventTarget::process_event_handler_for_event(FlyString const& name, Event& event)
{
    // 1. Let callback be the result of getting the current value of the event handler given eventTarget and name.
    auto* callback = get_current_value_of_event_handler(name);

    // 2. If callback is null, then return.
    if (!callback)
        return {};

    // 3. Let special error event handling be true if event is an ErrorEvent object, event's type is error, and event's currentTarget implements the WindowOrWorkerGlobalScope mixin.
    //    Otherwise, let special error event handling be false.
    bool special_error_event_handling = is<HTML::ErrorEvent>(event) && event.type() == HTML::EventNames::error && is<HTML::WindowOrWorkerGlobalScopeMixin>(event.current_target().ptr());

    // 4. Process the Event object event as follows:
    JS::Completion return_value_or_error;

    if (special_error_event_handling) {
        // -> If special error event handling is true
        //    Invoke callback with five arguments, the first one having the value of event's message attribute, the second having the value of event's filename attribute, the third having the value of event's lineno attribute,
        //    the fourth having the value of event's colno attribute, the fifth having the value of event's error attribute, and with the callback this value set to event's currentTarget.
        //    Let return value be the callback's return value. [WEBIDL]
        auto& error_event = as<HTML::ErrorEvent>(event);
        auto wrapped_message = JS::PrimitiveString::create(vm(), error_event.message());
        auto wrapped_filename = JS::PrimitiveString::create(vm(), error_event.filename());
        auto wrapped_lineno = JS::Value(error_event.lineno());
        auto wrapped_colno = JS::Value(error_event.colno());

        // NOTE: error_event.error() is a JS::Value, so it does not require wrapping.

        // NOTE: current_target is always non-null here, as the event dispatcher takes care to make sure it's non-null (and uses it as the this value for the callback!)
        // FIXME: This is rewrapping the this value of the callback defined in activate_event_handler. While I don't think this is observable as the event dispatcher
        //        calls directly into the callback without considering things such as proxies, it is a waste. However, if it observable, then we must reuse the this_value that was given to the callback.
        auto* this_value = error_event.current_target().ptr();

        return_value_or_error = WebIDL::invoke_callback(*callback, this_value, wrapped_message, wrapped_filename, wrapped_lineno, wrapped_colno, error_event.error());
    } else {
        // -> Otherwise
        // Invoke callback with one argument, the value of which is the Event object event, with the callback this value set to event's currentTarget. Let return value be the callback's return value. [WEBIDL]

        // FIXME: This has the same rewrapping issue as this_value.
        auto* wrapped_event = &event;

        // FIXME: The comments about this in the special_error_event_handling path also apply here.
        auto* this_value = event.current_target().ptr();

        return_value_or_error = WebIDL::invoke_callback(*callback, this_value, wrapped_event);
    }

    // If an exception gets thrown by the callback, end these steps and allow the exception to propagate. (It will propagate to the DOM event dispatch logic, which will then report the exception.)
    if (return_value_or_error.is_error())
        return return_value_or_error.release_error();

    // FIXME: Ideally, invoke_callback would convert JS::Value to the appropriate return type for us as per the spec, but it doesn't currently.
    auto return_value = *return_value_or_error.value();

    auto is_beforeunload = event.type() == HTML::EventNames::beforeunload;

    // 5. Process return value as follows:
    if (is<HTML::BeforeUnloadEvent>(event) && is_beforeunload) {
        // ->  If event is a BeforeUnloadEvent object and event's type is "beforeunload"
        //         If return value is not null, then:
        if (!return_value.is_nullish()) {
            // 1. Set event's canceled flag.
            event.set_cancelled(true);

            // 2. If event's returnValue attribute's value is the empty string, then set event's returnValue attribute's value to return value.
            auto& before_unload_event = static_cast<HTML::BeforeUnloadEvent&>(event);
            if (before_unload_event.return_value().is_empty())
                before_unload_event.set_return_value(TRY(return_value.to_string(vm())));
        }
    }

    if (special_error_event_handling) {
        // -> If special error event handling is true
        //      If return value is true, then set event's canceled flag.
        // NOTE: the return type of EventHandler is `any`, so no coercion happens, meaning we have to check if it's a boolean first.
        if (return_value.is_boolean() && return_value.as_bool())
            event.set_cancelled(true);
    } else {
        // -> Otherwise
        //      If return value is false, then set event's canceled flag.
        // NOTE: the return type of EventHandler is `any`, so no coercion happens, meaning we have to check if it's a boolean first.
        // Spec-Note: If we've gotten to this "Otherwise" clause because event's type is "beforeunload" but event is
        //            not a BeforeUnloadEvent object, then return value will never be false, since in such cases
        //            return value will have been coerced into either null or a DOMString.
        if (return_value.is_boolean() && !return_value.as_bool() && !is_beforeunload)
            event.set_cancelled(true);
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-attributes:concept-element-attributes-change-ext
void EventTarget::element_event_handler_attribute_changed(FlyString const& local_name, Optional<String> const& value)
{
    // NOTE: Step 1 of this algorithm was handled in HTMLElement::attribute_changed.

    // 2. Let eventTarget be the result of determining the target of an event handler given element and localName.
    // NOTE: element is `this`.
    auto* event_target = determine_target_of_event_handler(*this, local_name);

    // 3. If eventTarget is null, then return.
    if (!event_target)
        return;

    // 4. If value is null, then deactivate an event handler given eventTarget and localName.
    if (!value.has_value()) {
        event_target->deactivate_event_handler(local_name);
        return;
    }

    // 5. Otherwise:
    //  FIXME: 1. If the Should element's inline behavior be blocked by Content Security Policy? algorithm returns "Blocked" when executed upon element, "script attribute", and value, then return. [CSP]

    //  2. Let handlerMap be eventTarget's event handler map.
    auto& handler_map = event_target->ensure_data().event_handler_map;

    //  3. Let eventHandler be handlerMap[localName].
    auto event_handler_iterator = handler_map.find(local_name);

    //  FIXME: 4. Let location be the script location that triggered the execution of these steps.

    //  FIXME: 5. Set eventHandler's value to the internal raw uncompiled handler value/location.
    //            (This currently sets the value to the uncompiled source code instead of the named struct)

    // NOTE: See the optimization comments in set_event_handler_attribute.

    if (event_handler_iterator == handler_map.end()) {
        auto new_event_handler = heap().allocate<HTML::EventHandler>(value->to_byte_string());

        //  6. Activate an event handler given eventTarget and name.
        event_target->activate_event_handler(local_name, *new_event_handler);

        handler_map.set(local_name, new_event_handler);
        return;
    }

    auto& event_handler = event_handler_iterator->value;

    //  6. Activate an event handler given eventTarget and name.
    event_handler->value = value->to_byte_string();
    event_target->activate_event_handler(local_name, *event_handler);
}

bool EventTarget::dispatch_event(Event& event)
{
    // https://html.spec.whatwg.org/multipage/interaction.html#activation-triggering-input-event
    auto is_activation_triggering_input_event = [&]() -> bool {
        // An activation triggering input event is any event whose isTrusted attribute is true and whose type is one of:
        if (!event.is_trusted())
            return false;

        // keydown, provided the key is neither the Esc key nor a shortcut key reserved by the user agent.
        if (event.type() == UIEvents::EventNames::keydown)
            return static_cast<UIEvents::KeyboardEvent*>(&event)->key_code() != UIEvents::KeyCode::Key_Escape;

        // mousedown.
        if (event.type() == UIEvents::EventNames::mousedown)
            return true;

        // FIXME:
        // pointerdown, provided the event's pointerType is "mouse".
        // pointerup, provided the event's pointerType is not "mouse".
        // touchend.

        return false;
    };

    // https://html.spec.whatwg.org/multipage/interaction.html#user-activation-processing-model
    // When a user interaction causes firing of an activation triggering input event in a Document document,
    // the user agent must perform the following activation notification steps before dispatching the event:

    // FIXME: 1. Assert: document is fully active.
    // FIXME: 2. Let windows be « document's relevant global object ».
    // FIXME: 3. Extend windows with the active window of each of document's ancestor navigables.
    // FIXME: 4. Extend windows with the active window of each of document's descendant navigables,
    //           filtered to include only those navigables whose active document's origin is same origin with document's origin.
    // FIXME: 5. For each window in windows:
    // FIXME: 5.1 Set window's last activation timestamp to the current high resolution time.
    // FIXME: 5.2 Notify the close watcher manager about user activation given window.

    // FIXME: This is ad-hoc, but works for now.
    if (is_activation_triggering_input_event()) {
        auto unsafe_shared_time = HighResolutionTime::unsafe_shared_current_time();
        auto current_time = HighResolutionTime::relative_high_resolution_time(unsafe_shared_time, realm().global_object());

        if (is<HTML::Window>(this)) {
            auto* window = static_cast<HTML::Window*>(this);
            window->set_last_activation_timestamp(current_time);
            window->close_watcher_manager()->notify_about_user_activation();
        } else if (is<DOM::Element>(this)) {
            auto const* element = static_cast<DOM::Element const*>(this);
            if (auto window = element->document().window()) {
                window->set_last_activation_timestamp(current_time);
                window->close_watcher_manager()->notify_about_user_activation();
            }
        }
    }

    return EventDispatcher::dispatch(*this, event);
}

bool EventTarget::has_event_listener(FlyString const& type) const
{
    if (!m_data)
        return false;
    for (auto& listener : m_data->event_listener_list) {
        if (listener->type == type)
            return true;
    }
    return false;
}

bool EventTarget::has_event_listeners() const
{
    return m_data && !m_data->event_listener_list.is_empty();
}

bool EventTarget::has_activation_behavior() const
{
    return false;
}

void EventTarget::activation_behavior(Event const&)
{
}

auto EventTarget::ensure_data() -> Data&
{
    if (!m_data)
        m_data = make<Data>();
    return *m_data;
}

}
