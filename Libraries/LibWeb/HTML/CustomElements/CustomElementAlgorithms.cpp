/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/ImplementedInBindings.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/HTML/CustomElements/CustomElementAlgorithms.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Bindings {

struct CustomElementDefinitionPrototypeCacheEntry {
    GC::Weak<HTML::CustomElementDefinition> definition;
    GC::Weak<JS::Object> prototype;
};

static Vector<CustomElementDefinitionPrototypeCacheEntry>& custom_element_definition_prototype_cache()
{
    static NeverDestroyed<Vector<CustomElementDefinitionPrototypeCacheEntry>> cache;
    return *cache;
}

static void prune_custom_element_definition_prototype_cache()
{
    custom_element_definition_prototype_cache().remove_all_matching([](auto const& entry) {
        return !entry.definition || !entry.prototype;
    });
}

void remember_custom_element_definition_prototype(HTML::CustomElementDefinition& definition, JS::Object& prototype)
{
    auto& cache = custom_element_definition_prototype_cache();
    prune_custom_element_definition_prototype_cache();

    for (auto& entry : cache) {
        if (entry.definition.ptr() == &definition && entry.prototype && &entry.prototype->shape().realm() == &prototype.shape().realm()) {
            entry.prototype = prototype;
            return;
        }
    }

    cache.append({ definition, prototype });
}

static GC::Ptr<JS::Object> custom_element_definition_prototype(HTML::CustomElementDefinition& definition, JS::Realm& realm)
{
    prune_custom_element_definition_prototype_cache();

    for (auto const& entry : custom_element_definition_prototype_cache()) {
        if (entry.definition.ptr() == &definition && entry.prototype && &entry.prototype->shape().realm() == &realm)
            return entry.prototype.ptr();
    }

    return nullptr;
}

void set_prototype_from_custom_element_definition_if_needed(DOM::Element& element, PlatformObject& wrapper)
{
    if (!element.is_custom())
        return;

    auto definition = element.custom_element_definition();
    if (!definition)
        return;

    auto prototype = custom_element_definition_prototype(*definition, wrapper.realm());
    if (!prototype)
        return;

    auto did_set_prototype = MUST(wrapper.internal_set_prototype_of(prototype));
    VERIFY(did_set_prototype);
}

static GC::RootVector<JS::Value> custom_element_reaction_arguments_to_js(JS::Realm& realm, DOM::CustomElementCallbackReactionArguments const& arguments)
{
    auto& vm = realm.vm();
    GC::RootVector<JS::Value> js_arguments;

    arguments.visit(
        [](Empty) {},
        [&](DOM::CustomElementAdoptedCallbackReactionArguments const& adopted_arguments) {
            js_arguments.append(Bindings::document(realm, adopted_arguments.old_document));
            js_arguments.append(Bindings::document(realm, adopted_arguments.new_document));
        },
        [&](DOM::CustomElementAttributeChangedCallbackReactionArguments const& attribute_changed_arguments) {
            js_arguments.append(JS::PrimitiveString::create(vm, attribute_changed_arguments.attribute_name));
            js_arguments.append(!attribute_changed_arguments.old_value.has_value() ? JS::js_null() : JS::PrimitiveString::create(vm, attribute_changed_arguments.old_value.value()));
            js_arguments.append(!attribute_changed_arguments.new_value.has_value() ? JS::js_null() : JS::PrimitiveString::create(vm, attribute_changed_arguments.new_value.value()));
            js_arguments.append(!attribute_changed_arguments.namespace_uri.has_value() ? JS::js_null() : JS::PrimitiveString::create(vm, attribute_changed_arguments.namespace_uri.value()));
        },
        [&](DOM::CustomElementFormAssociatedCallbackReactionArguments const& form_associated_arguments) {
            js_arguments.append(form_associated_arguments.form ? Bindings::element(realm, GC::Ref { static_cast<DOM::Element&>(*form_associated_arguments.form) }) : JS::js_null());
        },
        [&](DOM::CustomElementFormDisabledCallbackReactionArguments const& form_disabled_arguments) {
            js_arguments.append(JS::Value(form_disabled_arguments.is_disabled));
        });

    return js_arguments;
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#concept-upgrade-an-element
JS::ThrowCompletionOr<void> upgrade_custom_element(DOM::Element& element, GC::Ref<HTML::CustomElementDefinition> custom_element_definition)
{
    auto& realm = element.document().relevant_settings_object().realm();
    auto& vm = realm.vm();

    // 1. If element's custom element state is not "undefined" or "uncustomized", then return.
    if (!element.can_upgrade_custom_element())
        return {};

    // 2. Set element's custom element definition to definition.
    element.set_custom_element_definition(custom_element_definition);

    // 3. Set element's custom element state to "failed".
    element.set_custom_element_state(DOM::CustomElementState::Failed);

    // 4. For each attribute in element's attribute list, in order, enqueue a custom element callback reaction with
    //    element, callback name "attributeChangedCallback", and « attribute's local name, null, attribute's value,
    //    attribute's namespace ».
    if (auto attributes = element.attributes()) {
        for (size_t attribute_index = 0; attribute_index < attributes->length(); ++attribute_index) {
            auto const* attribute = attributes->item(attribute_index);
            VERIFY(attribute);

            element.enqueue_an_attribute_changed_callback_reaction(attribute->local_name(), {}, attribute->value(), attribute->namespace_uri());
        }
    }

    // 5. If element is connected, then enqueue a custom element callback reaction with element, callback name
    //    "connectedCallback", and « ».
    if (element.is_connected())
        element.enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::connectedCallback);

    // 6. Add element to the end of definition's construction stack.
    custom_element_definition->construction_stack().append(GC::Ref { element });

    // 7. Let C be definition's constructor.
    auto& constructor = custom_element_definition->constructor();

    // 8. Set the surrounding agent's active custom element constructor map[C] to element's custom element registry.
    auto& surrounding_agent = HTML::relevant_similar_origin_window_agent(element);
    surrounding_agent.active_custom_element_constructor_map.set(static_cast<JS::FunctionObject&>(*constructor.callback), element.custom_element_registry());

    // 9. Run the following steps while catching any exceptions:
    auto attempt_to_construct_custom_element = [&]() -> JS::ThrowCompletionOr<void> {
        // 1. If definition's disable shadow is true and element's shadow root is non-null, then throw a
        //    "NotSupportedError" DOMException.
        if (custom_element_definition->disable_shadow() && element.shadow_root())
            return Web::throw_completion(realm, WebIDL::NotSupportedError::create(realm, "Custom element definition disables shadow DOM and the custom element has a shadow root"_utf16));

        // 2. Set element's custom element state to "precustomized".
        element.set_custom_element_state(DOM::CustomElementState::Precustomized);

        auto element_wrapper = GC::make_root(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { element }));

        // 3. Let constructResult be the result of constructing C, with no arguments.
        auto construct_result = TRY(WebIDL::construct(constructor, {}));

        // 4. If SameValue(constructResult, element) is false, then throw a TypeError.
        if (!JS::same_value(construct_result, element_wrapper.ptr()))
            return vm.throw_completion<JS::TypeError>("Constructing the custom element returned a different element from the custom element"sv);

        return {};
    };

    auto maybe_exception = attempt_to_construct_custom_element();

    // Then, perform the following steps, regardless of whether the above steps threw an exception or not:
    // 1. Remove the surrounding agent's active custom element constructor map[C].
    surrounding_agent.active_custom_element_constructor_map.remove(static_cast<JS::FunctionObject&>(*constructor.callback));

    // 2. Remove the last entry from the end of definition's construction stack.
    (void)custom_element_definition->construction_stack().take_last();

    // Finally, if the above steps threw an exception, then:
    if (maybe_exception.is_throw_completion()) {
        // 1. Set element's custom element definition to null.
        element.set_custom_element_definition(nullptr);

        // 2. Empty element's custom element reaction queue.
        element.clear_custom_element_reaction_queue();

        // 3. Rethrow the exception (thus terminating this algorithm).
        return maybe_exception.release_error();
    }

    // 10. If element is a form-associated custom element, then:
    if (auto* html_element = as_if<HTML::HTMLElement>(&element); html_element && html_element->is_form_associated_custom_element()) {
        // 1. Reset the form owner of element.
        // FIXME: If element is associated with a form element, then enqueue a custom element callback reaction with element, callback name "formAssociatedCallback", and « the associated form ».
        // AD-HOC: We don't do the second part of this step here, because it's inside reset_form_owner.
        html_element->reset_form_owner();

        // 2. If element is disabled, then enqueue a custom element callback reaction with element, callback name "formDisabledCallback", and « true ».
        html_element->update_face_disabled_state();
    }

    // 11. Set element's custom element state to "custom".
    element.set_custom_element_state(DOM::CustomElementState::Custom);

    return {};
}

JS::ThrowCompletionOr<GC::Ref<HTML::HTMLElement>> construct_autonomous_custom_element(DOM::Document& document, FlyString const& local_name, Optional<FlyString> const& prefix, GC::Ptr<HTML::CustomElementRegistry> registry, GC::Ref<HTML::CustomElementDefinition> custom_element_definition)
{
    auto& realm = document.relevant_settings_object().realm();

    // 1. Let C be definition's constructor.
    auto& constructor = custom_element_definition->constructor();

    // 2. Set the surrounding agent's active custom element constructor map[C] to registry.
    auto& surrounding_agent = HTML::relevant_similar_origin_window_agent(document);
    surrounding_agent.active_custom_element_constructor_map.set(static_cast<JS::FunctionObject&>(*constructor.callback), registry);

    auto construct_custom_element = [&]() -> JS::ThrowCompletionOr<GC::Ref<HTML::HTMLElement>> {
        // 3.1. Set result to the result of constructing C, with no arguments.
        // NOTE: IDL does not currently convert the object for us, so we will have to do it here.
        auto constructed_element = TRY(WebIDL::construct(constructor, {}));
        GC::Ptr<HTML::HTMLElement> result;
        if (constructed_element.is_object())
            result = Bindings::impl_from<HTML::HTMLElement>(&constructed_element.as_object());
        if (!result)
            return JS::throw_completion(JS::TypeError::create(realm, "Custom element constructor must return an object that implements HTMLElement"_string));

        // FIXME: 3.2. Assert: result's custom element state and custom element definition are initialized.

        // 3.3. Assert: result's namespace is the HTML namespace.
        // Note: IDL enforces that result is an HTMLElement object, which all use the HTML namespace.
        VERIFY(result->namespace_uri() == Namespace::HTML);

        // 3.4. If result's attribute list is not empty, then throw a "NotSupportedError" DOMException.
        if (result->has_attributes())
            return Web::throw_completion(realm, WebIDL::NotSupportedError::create(realm, "Synchronously created custom element cannot have attributes"_utf16));

        // 3.5. If result has children, then throw a "NotSupportedError" DOMException.
        if (result->has_children())
            return Web::throw_completion(realm, WebIDL::NotSupportedError::create(realm, "Synchronously created custom element cannot have children"_utf16));

        // 3.6. If result's parent is not null, then throw a "NotSupportedError" DOMException.
        if (result->parent())
            return Web::throw_completion(realm, WebIDL::NotSupportedError::create(realm, "Synchronously created custom element cannot have a parent"_utf16));

        // 3.7. If result's node document is not document, then throw a "NotSupportedError" DOMException.
        if (&result->document() != &document)
            return Web::throw_completion(realm, WebIDL::NotSupportedError::create(realm, "Synchronously created custom element must be in the same document that element creation was invoked in"_utf16));

        // 3.8. If result's local name is not equal to localName, then throw a "NotSupportedError" DOMException.
        if (result->local_name() != local_name)
            return Web::throw_completion(realm, WebIDL::NotSupportedError::create(realm, "Synchronously created custom element must have the same local name that element creation was invoked with"_utf16));

        // 3.9. Set result's namespace prefix to prefix.
        result->set_prefix(prefix);

        // 3.10. Set result's is value to null.
        result->set_is_value(Optional<String> {});

        // 3.11. Set result's custom element registry to registry.
        result->set_custom_element_registry(registry);

        return GC::Ref { *result };
    };

    auto maybe_result = construct_custom_element();

    // 4. Remove the surrounding agent's active custom element constructor map[C].
    surrounding_agent.active_custom_element_constructor_map.remove(static_cast<JS::FunctionObject&>(*constructor.callback));

    if (maybe_result.is_throw_completion())
        return maybe_result.release_error();

    return maybe_result.release_value();
}

void invoke_custom_element_lifecycle_callback(DOM::Element& element, WebIDL::CallbackType& callback)
{
    auto& realm = callback.callback->shape().realm();
    auto this_value = Bindings::element(realm, GC::Ref { element });
    GC::RootVector<JS::Value> arguments;
    (void)WebIDL::invoke_callback(callback, this_value, WebIDL::ExceptionBehavior::Report, arguments);
}

void invoke_custom_element_callback_reaction(DOM::Element& element, WebIDL::CallbackType& callback, DOM::CustomElementCallbackReactionArguments const& arguments)
{
    auto& realm = callback.callback->shape().realm();
    auto this_value = Bindings::element(realm, GC::Ref { element });
    auto js_arguments = custom_element_reaction_arguments_to_js(realm, arguments);
    (void)WebIDL::invoke_callback(callback, this_value, WebIDL::ExceptionBehavior::Report, js_arguments);
}

void report_custom_element_upgrade_exception(HTML::CustomElementDefinition& custom_element_definition, JS::Value exception)
{
    auto& callback = custom_element_definition.constructor();
    auto& realm = callback.callback->shape().realm();
    auto& global = realm.global_object();

    auto* window_or_worker = HTML::window_or_worker_global_scope_from_global_object(global);
    VERIFY(window_or_worker);
    window_or_worker->report_an_exception(exception);
}

}
