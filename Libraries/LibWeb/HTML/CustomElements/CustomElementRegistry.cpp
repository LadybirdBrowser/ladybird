/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/CustomElementRegistryPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CustomElementRegistry);
GC_DEFINE_ALLOCATOR(CustomElementDefinition);

CustomElementRegistry::CustomElementRegistry(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

CustomElementRegistry::~CustomElementRegistry() = default;

void CustomElementRegistry::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CustomElementRegistry);
}

void CustomElementRegistry::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_custom_element_definitions);
    visitor.visit(m_when_defined_promise_map);
}

// https://webidl.spec.whatwg.org/#es-callback-function
// https://github.com/whatwg/html/pull/9893
static JS::ThrowCompletionOr<GC::Ref<WebIDL::CallbackType>> convert_value_to_callback_function(JS::VM& vm, JS::Value value)
{
    // FIXME: De-duplicate this from the IDL generator.
    // 1. If the result of calling IsCallable(V) is false and the conversion to an IDL value is not being performed due to V being assigned to an attribute whose type is a nullable callback function that is annotated with [LegacyTreatNonObjectAsNull], then throw a TypeError.
    if (!value.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, value.to_string_without_side_effects());

    // 2. Return the IDL callback function type value that represents a reference to the same object that V represents, with the incumbent realm as the callback context.
    return vm.heap().allocate<WebIDL::CallbackType>(value.as_object(), HTML::incumbent_realm());
}

// https://webidl.spec.whatwg.org/#es-sequence
static JS::ThrowCompletionOr<Vector<String>> convert_value_to_sequence_of_strings(JS::VM& vm, JS::Value value)
{
    // FIXME: De-duplicate this from the IDL generator.
    // An ECMAScript value V is converted to an IDL sequence<T> value as follows:
    // 1. If V is not an Object, throw a TypeError.
    if (!value.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, value.to_string_without_side_effects());

    // 2. Let method be ? GetMethod(V, @@iterator).
    auto method = TRY(value.get_method(vm, vm.well_known_symbol_iterator()));

    // 3. If method is undefined, throw a TypeError.
    if (!method)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotIterable, value.to_string_without_side_effects());

    // 4. Return the result of creating a sequence from V and method.

    // https://webidl.spec.whatwg.org/#create-sequence-from-iterable
    // To create an IDL value of type sequence<T> given an iterable iterable and an iterator getter method, perform the following steps:
    // 1. Let iter be ? GetIterator(iterable, sync, method).
    // FIXME: The WebIDL spec is out of date - it should be using GetIteratorFromMethod.
    auto iterator = TRY(JS::get_iterator_from_method(vm, value, *method));

    // 2. Initialize i to be 0.
    Vector<String> sequence_of_strings;

    // 3. Repeat
    for (;;) {
        // 1. Let next be ? IteratorStep(iter).
        auto next = TRY(JS::iterator_step(vm, iterator));

        // 2. If next is false, then return an IDL sequence value of type sequence<T> of length i, where the value of the element at index j is Sj.
        if (!next)
            return sequence_of_strings;

        // 3. Let nextItem be ? IteratorValue(next).
        auto next_item = TRY(JS::iterator_value(vm, *next));

        // 4. Initialize Si to the result of converting nextItem to an IDL value of type T.

        // https://webidl.spec.whatwg.org/#es-DOMString
        // An ECMAScript value V is converted to an IDL DOMString value by running the following algorithm:
        // 1. If V is null and the conversion is to an IDL type associated with the [LegacyNullToEmptyString] extended attribute, then return the DOMString value that represents the empty string.
        // NOTE: This doesn't apply.

        // 2. Let x be ? ToString(V).
        // 3. Return the IDL DOMString value that represents the same sequence of code units as the one the ECMAScript String value x represents.
        auto string_value = TRY(next_item.to_string(vm));

        sequence_of_strings.append(move(string_value));

        // 5. Set i to i + 1.
    }
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-customelementregistry-define
JS::ThrowCompletionOr<void> CustomElementRegistry::define(String const& name, WebIDL::CallbackType* constructor, ElementDefinitionOptions options)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. If IsConstructor(constructor) is false, then throw a TypeError.
    if (!JS::Value(constructor->callback).is_constructor())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, JS::Value(constructor->callback).to_string_without_side_effects());

    // 2. If name is not a valid custom element name, then throw a "SyntaxError" DOMException.
    if (!is_valid_custom_element_name(name))
        return JS::throw_completion(WebIDL::SyntaxError::create(realm, MUST(String::formatted("'{}' is not a valid custom element name"sv, name))));

    // 3. If this's custom element definition set contains an item with name name, then throw a "NotSupportedError" DOMException.
    auto existing_definition_with_name_iterator = m_custom_element_definitions.find_if([&name](auto const& definition) {
        return definition->name() == name;
    });

    if (existing_definition_with_name_iterator != m_custom_element_definitions.end())
        return JS::throw_completion(WebIDL::NotSupportedError::create(realm, MUST(String::formatted("A custom element with name '{}' is already defined"sv, name))));

    // 4. If this's custom element definition set contains an item with constructor constructor, then throw a "NotSupportedError" DOMException.
    auto existing_definition_with_constructor_iterator = m_custom_element_definitions.find_if([&constructor](auto const& definition) {
        return definition->constructor().callback == constructor->callback;
    });

    if (existing_definition_with_constructor_iterator != m_custom_element_definitions.end())
        return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "The given constructor is already in use by another custom element"_string));

    // 5. Let localName be name.
    String local_name = name;

    // 6. Let extends be options["extends"] if it exists; otherwise null.
    auto& extends = options.extends;

    // 7. If extends is not null:
    if (extends.has_value()) {
        // 1. If extends is a valid custom element name, then throw a "NotSupportedError" DOMException.
        if (is_valid_custom_element_name(extends.value()))
            return JS::throw_completion(WebIDL::NotSupportedError::create(realm, MUST(String::formatted("'{}' is a custom element name, only non-custom elements can be extended"sv, extends.value()))));

        // 2. If the element interface for extends and the HTML namespace is HTMLUnknownElement
        //    (e.g., if extends does not indicate an element definition in this specification),
        //    then throw a "NotSupportedError" DOMException.
        if (DOM::is_unknown_html_element(extends.value()))
            return JS::throw_completion(WebIDL::NotSupportedError::create(realm, MUST(String::formatted("'{}' is an unknown HTML element"sv, extends.value()))));

        // 3. Set localName to extends.
        local_name = extends.value();
    }

    // 8. If this's element definition is running is true, then throw a "NotSupportedError" DOMException.
    if (m_element_definition_is_running)
        return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Cannot recursively define custom elements"_string));

    // 9. Set this's element definition is running to true.
    m_element_definition_is_running = true;

    // 10. Let formAssociated be false.
    bool form_associated = false;

    // 11. Let disableInternals be false.
    bool disable_internals = false;

    // 12. Let disableShadow be false.
    bool disable_shadow = false;

    // 13. Let observedAttributes be an empty sequence<DOMString>.
    Vector<String> observed_attributes;

    // NOTE: This is not in the spec, but is required because of how we catch the exception by using a lambda, meaning we need to define this
    //       variable outside of it to use it later.
    OrderedHashMap<FlyString, GC::Root<WebIDL::CallbackType>> lifecycle_callbacks;

    // 14. Run the following steps while catching any exceptions:
    auto get_definition_attributes_from_constructor = [&]() -> JS::ThrowCompletionOr<void> {
        // 1. Let prototype be ? Get(constructor, "prototype").
        auto prototype_value = TRY(constructor->callback->get(vm.names.prototype));

        // 2. If prototype is not an Object, then throw a TypeError exception.
        if (!prototype_value.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, prototype_value.to_string_without_side_effects());

        auto& prototype = prototype_value.as_object();

        // 3. Let lifecycleCallbacks be the ordered map «[ "connectedCallback" → null, "disconnectedCallback" → null, "adoptedCallback" → null,
        //    "attributeChangedCallback" → null ]».
        lifecycle_callbacks.set(CustomElementReactionNames::connectedCallback, {});
        lifecycle_callbacks.set(CustomElementReactionNames::disconnectedCallback, {});
        lifecycle_callbacks.set(CustomElementReactionNames::adoptedCallback, {});
        lifecycle_callbacks.set(CustomElementReactionNames::attributeChangedCallback, {});

        // 4. For each callbackName of the keys of lifecycleCallbacks:
        for (auto const& callback_name : { CustomElementReactionNames::connectedCallback, CustomElementReactionNames::disconnectedCallback, CustomElementReactionNames::adoptedCallback, CustomElementReactionNames::attributeChangedCallback }) {
            // 1. Let callbackValue be ? Get(prototype, callbackName).
            auto callback_value = TRY(prototype.get(callback_name));

            // 2. If callbackValue is not undefined, then set the value of the entry in lifecycleCallbacks with key callbackName to the result of
            //    converting callbackValue to the Web IDL Function callback type.
            if (!callback_value.is_undefined()) {
                auto callback = TRY(convert_value_to_callback_function(vm, callback_value));
                lifecycle_callbacks.set(callback_name, callback);
            }
        }

        // 5. If lifecycleCallbacks["attributeChangedCallback"] is not null:
        auto attribute_changed_callback_iterator = lifecycle_callbacks.find(CustomElementReactionNames::attributeChangedCallback);
        VERIFY(attribute_changed_callback_iterator != lifecycle_callbacks.end());
        if (attribute_changed_callback_iterator->value) {
            // 1. Let observedAttributesIterable be ? Get(constructor, "observedAttributes").
            auto observed_attributes_iterable = TRY(constructor->callback->get(vm.names.observedAttributes));

            // 2. If observedAttributesIterable is not undefined, then set observedAttributes to the result of converting observedAttributesIterable
            //    to a sequence<DOMString>. Rethrow any exceptions from the conversion.
            if (!observed_attributes_iterable.is_undefined())
                observed_attributes = TRY(convert_value_to_sequence_of_strings(vm, observed_attributes_iterable));
        }

        // 6. Let disabledFeatures be an empty sequence<DOMString>.
        Vector<String> disabled_features;

        // 7. Let disabledFeaturesIterable be ? Get(constructor, "disabledFeatures").
        auto disabled_features_iterable = TRY(constructor->callback->get(vm.names.disabledFeatures));

        // 8. If disabledFeaturesIterable is not undefined, then set disabledFeatures to the result of converting disabledFeaturesIterable to a
        //    sequence<DOMString>. Rethrow any exceptions from the conversion.
        if (!disabled_features_iterable.is_undefined())
            disabled_features = TRY(convert_value_to_sequence_of_strings(vm, disabled_features_iterable));

        // 9. If disabledFeatures contains "internals", then set disableInternals to true.
        disable_internals = disabled_features.contains_slow("internals"sv);

        // 10. If disabledFeatures contains "shadow", then set disableShadow to true.
        disable_shadow = disabled_features.contains_slow("shadow"sv);

        // 11. Let formAssociatedValue be ? Get( constructor, "formAssociated").
        auto form_associated_value = TRY(constructor->callback->get(vm.names.formAssociated));

        // 12. Set formAssociated to the result of converting formAssociatedValue to a boolean.
        form_associated = form_associated_value.to_boolean();

        // 13. If formAssociated is true, then for each callbackName of « "formAssociatedCallback", "formResetCallback", "formDisabledCallback",
        //     "formStateRestoreCallback" »:
        if (form_associated) {
            for (auto const& callback_name : { CustomElementReactionNames::formAssociatedCallback, CustomElementReactionNames::formResetCallback, CustomElementReactionNames::formDisabledCallback, CustomElementReactionNames::formStateRestoreCallback }) {
                // 1. Let callbackValue be ? Get(prototype, callbackName).
                auto callback_value = TRY(prototype.get(callback_name));

                // 2. If callbackValue is not undefined, then set lifecycleCallbacks[callbackName] to the result of converting callbackValue
                //    to the Web IDL Function callback type.
                if (!callback_value.is_undefined())
                    lifecycle_callbacks.set(callback_name, TRY(convert_value_to_callback_function(vm, callback_value)));
            }
        }

        return {};
    };

    auto maybe_exception = get_definition_attributes_from_constructor();

    //     Then, regardless of whether the above steps threw an exception or not: set this's element definition is running to false.
    m_element_definition_is_running = false;

    //     Finally, if the steps threw an exception, rethrow that exception.
    if (maybe_exception.is_throw_completion())
        return maybe_exception.release_error();

    // 15. Let definition be a new custom element definition with name name, local name localName, constructor constructor,
    //     observed attributes observedAttributes, lifecycle callbacks lifecycleCallbacks, form-associated formAssociated,
    //     disable internals disableInternals, and disable shadow disableShadow.
    auto definition = CustomElementDefinition::create(realm, name, local_name, *constructor, move(observed_attributes), move(lifecycle_callbacks), form_associated, disable_internals, disable_shadow);

    // 16. Append definition to this's custom element definition set.
    m_custom_element_definitions.append(definition);

    // 17. Let document be this's relevant global object's associated Document.
    auto& document = as<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 18. Let upgradeCandidates be all elements that are shadow-including descendants of document, whose namespace is the HTML namespace
    //     and whose local name is localName, in shadow-including tree order.
    //     Additionally, if extends is non-null, only include elements whose is value is equal to name.
    Vector<GC::Root<DOM::Element>> upgrade_candidates;

    document.for_each_shadow_including_descendant([&](DOM::Node& inclusive_descendant) {
        if (!is<DOM::Element>(inclusive_descendant))
            return TraversalDecision::Continue;

        auto& inclusive_descendant_element = static_cast<DOM::Element&>(inclusive_descendant);

        if (inclusive_descendant_element.namespace_uri() == Namespace::HTML && inclusive_descendant_element.local_name() == local_name && (!extends.has_value() || inclusive_descendant_element.is_value() == name))
            upgrade_candidates.append(GC::make_root(inclusive_descendant_element));

        return TraversalDecision::Continue;
    });

    // 19. For each element element of upgradeCandidates, enqueue a custom element upgrade reaction given element and definition.
    for (auto& element : upgrade_candidates)
        element->enqueue_a_custom_element_upgrade_reaction(definition);

    // 20. If this's when-defined promise map[name] exists:
    auto promise_when_defined_iterator = m_when_defined_promise_map.find(name);
    if (promise_when_defined_iterator != m_when_defined_promise_map.end()) {
        // 1. Resolve this's when-defined promise map[name] with constructor.
        WebIDL::resolve_promise(realm, promise_when_defined_iterator->value, constructor->callback);

        // 2. Remove this's when-defined promise map[name].
        m_when_defined_promise_map.remove(name);
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-customelementregistry-get
Variant<GC::Root<WebIDL::CallbackType>, Empty> CustomElementRegistry::get(String const& name) const
{
    // 1. If this's custom element definition set contains an item with name name, then return that item's constructor.
    auto existing_definition_iterator = m_custom_element_definitions.find_if([&name](auto const& definition) {
        return definition->name() == name;
    });

    if (!existing_definition_iterator.is_end())
        return GC::make_root((*existing_definition_iterator)->constructor());

    // 2. Return undefined.
    return Empty {};
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-customelementregistry-getname
Optional<String> CustomElementRegistry::get_name(GC::Root<WebIDL::CallbackType> const& constructor) const
{
    // 1. If this's custom element definition set contains an item with constructor constructor, then return that item's name.
    auto existing_definition_iterator = m_custom_element_definitions.find_if([&constructor](auto const& definition) {
        return definition->constructor().callback == constructor.cell()->callback;
    });

    if (!existing_definition_iterator.is_end())
        return (*existing_definition_iterator)->name();

    // 2. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-customelementregistry-whendefined
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> CustomElementRegistry::when_defined(String const& name)
{
    auto& realm = this->realm();

    // 1. If name is not a valid custom element name, then return a promise rejected with a "SyntaxError" DOMException.
    if (!is_valid_custom_element_name(name))
        return WebIDL::create_rejected_promise(realm, WebIDL::SyntaxError::create(realm, MUST(String::formatted("'{}' is not a valid custom element name"sv, name))));

    // 2. If this's custom element definition set contains an item with name name, then return a promise resolved with that item's constructor.
    auto existing_definition_iterator = m_custom_element_definitions.find_if([&name](GC::Root<CustomElementDefinition> const& definition) {
        return definition->name() == name;
    });

    if (existing_definition_iterator != m_custom_element_definitions.end())
        return WebIDL::create_resolved_promise(realm, (*existing_definition_iterator)->constructor().callback);

    // 3. If this's when-defined promise map[name] does not exist, then set this's when-defined promise map[name] to a new promise.
    auto existing_promise_iterator = m_when_defined_promise_map.find(name);
    GC::Ptr<WebIDL::Promise> promise;
    if (existing_promise_iterator == m_when_defined_promise_map.end()) {
        promise = WebIDL::create_promise(realm);
        m_when_defined_promise_map.set(name, *promise);
    } else {
        promise = existing_promise_iterator->value;
    }

    // 4. Return this's when-defined promise map[name].
    VERIFY(promise);
    return GC::Ref { *promise };
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-customelementregistry-upgrade
void CustomElementRegistry::upgrade(GC::Ref<DOM::Node> root) const
{
    // 1. Let candidates be a list of all of root's shadow-including inclusive descendant elements, in shadow-including tree order.
    Vector<GC::Root<DOM::Element>> candidates;

    root->for_each_shadow_including_inclusive_descendant([&](DOM::Node& inclusive_descendant) {
        if (!is<DOM::Element>(inclusive_descendant))
            return TraversalDecision::Continue;

        auto& inclusive_descendant_element = static_cast<DOM::Element&>(inclusive_descendant);
        candidates.append(GC::make_root(inclusive_descendant_element));

        return TraversalDecision::Continue;
    });

    // 2. For each candidate of candidates, try to upgrade candidate.
    for (auto& candidate : candidates)
        candidate->try_to_upgrade();
}

GC::Ptr<CustomElementDefinition> CustomElementRegistry::get_definition_with_name_and_local_name(String const& name, String const& local_name) const
{
    auto definition_iterator = m_custom_element_definitions.find_if([&](GC::Root<CustomElementDefinition> const& definition) {
        return definition->name() == name && definition->local_name() == local_name;
    });

    return definition_iterator.is_end() ? nullptr : definition_iterator->ptr();
}

GC::Ptr<CustomElementDefinition> CustomElementRegistry::get_definition_from_new_target(JS::FunctionObject const& new_target) const
{
    auto definition_iterator = m_custom_element_definitions.find_if([&](GC::Root<CustomElementDefinition> const& definition) {
        return definition->constructor().callback.ptr() == &new_target;
    });

    return definition_iterator.is_end() ? nullptr : definition_iterator->ptr();
}

}
