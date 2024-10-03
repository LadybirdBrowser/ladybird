/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/CrossOrigin/AbstractOperations.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

// 7.2.3.1 CrossOriginProperties ( O ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginproperties-(-o-)
Vector<CrossOriginProperty> cross_origin_properties(Variant<HTML::Location const*, HTML::Window const*> const& object)
{
    // 1. Assert: O is a Location or Window object.

    return object.visit(
        // 2. If O is a Location object, then return « { [[Property]]: "href", [[NeedsGet]]: false, [[NeedsSet]]: true }, { [[Property]]: "replace" } ».
        [](HTML::Location const*) -> Vector<CrossOriginProperty> {
            return {
                { .property = "href"_string, .needs_get = false, .needs_set = true },
                { .property = "replace"_string },
            };
        },
        // 3. Return « { [[Property]]: "window", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "self", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "location", [[NeedsGet]]: true, [[NeedsSet]]: true }, { [[Property]]: "close" }, { [[Property]]: "closed", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "focus" }, { [[Property]]: "blur" }, { [[Property]]: "frames", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "length", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "top", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "opener", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "parent", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "postMessage" } ».
        [](HTML::Window const*) -> Vector<CrossOriginProperty> {
            return {
                { .property = "window"_string, .needs_get = true, .needs_set = false },
                { .property = "self"_string, .needs_get = true, .needs_set = false },
                { .property = "location"_string, .needs_get = true, .needs_set = true },
                { .property = "close"_string },
                { .property = "closed"_string, .needs_get = true, .needs_set = false },
                { .property = "focus"_string },
                { .property = "blur"_string },
                { .property = "frames"_string, .needs_get = true, .needs_set = false },
                { .property = "length"_string, .needs_get = true, .needs_set = false },
                { .property = "top"_string, .needs_get = true, .needs_set = false },
                { .property = "opener"_string, .needs_get = true, .needs_set = false },
                { .property = "parent"_string, .needs_get = true, .needs_set = false },
                { .property = "postMessage"_string },
            };
        });
}

// https://html.spec.whatwg.org/multipage/browsers.html#cross-origin-accessible-window-property-name
bool is_cross_origin_accessible_window_property_name(JS::PropertyKey const& property_key)
{
    // A JavaScript property name P is a cross-origin accessible window property name if it is "window", "self", "location", "close", "closed", "focus", "blur", "frames", "length", "top", "opener", "parent", "postMessage", or an array index property name.
    static Array<DeprecatedFlyString, 13> property_names {
        "window"sv, "self"sv, "location"sv, "close"sv, "closed"sv, "focus"sv, "blur"sv, "frames"sv, "length"sv, "top"sv, "opener"sv, "parent"sv, "postMessage"sv
    };
    return (property_key.is_string() && any_of(property_names, [&](auto const& name) { return property_key.as_string() == name; })) || property_key.is_number();
}

// 7.2.3.2 CrossOriginPropertyFallback ( P ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginpropertyfallback-(-p-)
JS::ThrowCompletionOr<JS::PropertyDescriptor> cross_origin_property_fallback(JS::VM& vm, JS::PropertyKey const& property_key)
{
    // 1. If P is "then", @@toStringTag, @@hasInstance, or @@isConcatSpreadable, then return PropertyDescriptor{ [[Value]]: undefined, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }.
    auto property_key_is_then = property_key.is_string() && property_key.as_string() == vm.names.then.as_string();
    auto property_key_is_allowed_symbol = property_key.is_symbol()
        && (property_key.as_symbol() == vm.well_known_symbol_to_string_tag()
            || property_key.as_symbol() == vm.well_known_symbol_has_instance()
            || property_key.as_symbol() == vm.well_known_symbol_is_concat_spreadable());
    if (property_key_is_then || property_key_is_allowed_symbol)
        return JS::PropertyDescriptor { .value = JS::js_undefined(), .writable = false, .enumerable = false, .configurable = true };

    // 2. Throw a "SecurityError" DOMException.
    return throw_completion(WebIDL::SecurityError::create(*vm.current_realm(), MUST(String::formatted("Can't access property '{}' on cross-origin object", property_key))));
}

// 7.2.3.3 IsPlatformObjectSameOrigin ( O ), https://html.spec.whatwg.org/multipage/browsers.html#isplatformobjectsameorigin-(-o-)
bool is_platform_object_same_origin(JS::Object const& object)
{
    // 1. Return true if the current settings object's origin is same origin-domain with O's relevant settings object's origin, and false otherwise.
    return HTML::current_settings_object().origin().is_same_origin_domain(HTML::relevant_settings_object(object).origin());
}

// 7.2.3.4 CrossOriginGetOwnPropertyHelper ( O, P ), https://html.spec.whatwg.org/multipage/browsers.html#crossorigingetownpropertyhelper-(-o,-p-)
Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(Variant<HTML::Location*, HTML::Window*> const& object, JS::PropertyKey const& property_key)
{
    auto& realm = *Bindings::main_thread_vm().current_realm();
    auto const* object_ptr = object.visit([](auto* o) { return static_cast<JS::Object const*>(o); });
    auto const object_const_variant = object.visit([](auto* o) { return Variant<HTML::Location const*, HTML::Window const*> { o }; });

    // 1. Let crossOriginKey be a tuple consisting of the current settings object, O's relevant settings object, and P.
    auto cross_origin_key = CrossOriginKey {
        .current_settings_object = (FlatPtr)&HTML::current_settings_object(),
        .relevant_settings_object = (FlatPtr)&HTML::relevant_settings_object(*object_ptr),
        .property_key = property_key,
    };

    // SameValue(e.[[Property]], P) can never be true at step 2.1 if P is not a string due to the different type, so we can return early.
    if (!property_key.is_string()) {
        return {};
    }
    auto const& property_key_string = MUST(FlyString::from_deprecated_fly_string(property_key.as_string()));

    // 2. For each e of CrossOriginProperties(O):
    for (auto const& entry : cross_origin_properties(object_const_variant)) {
        if (entry.property != property_key_string)
            continue;
        // 1. If SameValue(e.[[Property]], P) is true, then:
        auto& cross_origin_property_descriptor_map = object.visit([](auto* o) -> CrossOriginPropertyDescriptorMap& { return o->cross_origin_property_descriptor_map(); });

        // 1. If the value of the [[CrossOriginPropertyDescriptorMap]] internal slot of O contains an entry whose key is crossOriginKey, then return that entry's value.
        auto it = cross_origin_property_descriptor_map.find(cross_origin_key);
        if (it != cross_origin_property_descriptor_map.end())
            return it->value;

        // 2. Let originalDesc be OrdinaryGetOwnProperty(O, P).
        auto original_descriptor = MUST((object_ptr->JS::Object::internal_get_own_property)(property_key));

        // 3. Let crossOriginDesc be undefined.
        auto cross_origin_descriptor = JS::PropertyDescriptor {};

        // 4. If e.[[NeedsGet]] and e.[[NeedsSet]] are absent, then:
        if (!entry.needs_get.has_value() && !entry.needs_set.has_value()) {
            // 1. Let value be originalDesc.[[Value]].
            auto value = original_descriptor->value;

            // 2. If IsCallable(value) is true, then set value to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the IDL operation P on object O.
            if (value->is_function()) {
                value = JS::NativeFunction::create(
                    realm, [function = JS::make_handle(*value)](auto& vm) {
                        return JS::call(vm, function.value(), JS::js_undefined(), vm.running_execution_context().arguments.span());
                    },
                    0, "");
            }

            // 3. Set crossOriginDesc to PropertyDescriptor{ [[Value]]: value, [[Enumerable]]: false, [[Writable]]: false, [[Configurable]]: true }.
            cross_origin_descriptor = JS::PropertyDescriptor { .value = value, .writable = false, .enumerable = false, .configurable = true };
        }
        // 5. Otherwise:
        else {
            // 1. Let crossOriginGet be undefined.
            Optional<JS::GCPtr<JS::FunctionObject>> cross_origin_get;

            // 2. If e.[[NeedsGet]] is true, then set crossOriginGet to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the getter of the IDL attribute P on object O.
            if (*entry.needs_get) {
                cross_origin_get = JS::NativeFunction::create(
                    realm, [object_ptr, getter = JS::make_handle(*original_descriptor->get)](auto& vm) {
                        return JS::call(vm, getter.cell(), object_ptr, vm.running_execution_context().arguments.span());
                    },
                    0, "");
            }

            // 3. Let crossOriginSet be undefined.
            Optional<JS::GCPtr<JS::FunctionObject>> cross_origin_set;

            // If e.[[NeedsSet]] is true, then set crossOriginSet to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the setter of the IDL attribute P on object O.
            if (*entry.needs_set) {
                cross_origin_set = JS::NativeFunction::create(
                    realm, [object_ptr, setter = JS::make_handle(*original_descriptor->set)](auto& vm) {
                        return JS::call(vm, setter.cell(), object_ptr, vm.running_execution_context().arguments.span());
                    },
                    0, "");
            }

            // 5. Set crossOriginDesc to PropertyDescriptor{ [[Get]]: crossOriginGet, [[Set]]: crossOriginSet, [[Enumerable]]: false, [[Configurable]]: true }.
            cross_origin_descriptor = JS::PropertyDescriptor { .get = cross_origin_get, .set = cross_origin_set, .enumerable = false, .configurable = true };
        }

        // 6. Create an entry in the value of the [[CrossOriginPropertyDescriptorMap]] internal slot of O with key crossOriginKey and value crossOriginDesc.
        cross_origin_property_descriptor_map.set(cross_origin_key, cross_origin_descriptor);

        // 7. Return crossOriginDesc.
        return cross_origin_descriptor;
    }

    // 3. Return undefined.
    return {};
}

// 7.2.3.5 CrossOriginGet ( O, P, Receiver ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginget-(-o,-p,-receiver-)
JS::ThrowCompletionOr<JS::Value> cross_origin_get(JS::VM& vm, JS::Object const& object, JS::PropertyKey const& property_key, JS::Value receiver)
{
    // 1. Let desc be ? O.[[GetOwnProperty]](P).
    auto descriptor = TRY(object.internal_get_own_property(property_key));

    // 2. Assert: desc is not undefined.
    VERIFY(descriptor.has_value());

    // 3. If IsDataDescriptor(desc) is true, then return desc.[[Value]].
    if (descriptor->is_data_descriptor())
        return *descriptor->value;

    // 4. Assert: IsAccessorDescriptor(desc) is true.
    VERIFY(descriptor->is_accessor_descriptor());

    // 5. Let getter be desc.[[Get]].
    auto& getter = descriptor->get;

    // 6. If getter is undefined, then throw a "SecurityError" DOMException.
    if (!getter.has_value())
        return throw_completion(WebIDL::SecurityError::create(*vm.current_realm(), MUST(String::formatted("Can't get property '{}' on cross-origin object", property_key))));

    // 7. Return ? Call(getter, Receiver).
    return JS::call(vm, *getter, receiver);
}

// 7.2.3.6 CrossOriginSet ( O, P, V, Receiver ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginset-(-o,-p,-v,-receiver-)
JS::ThrowCompletionOr<bool> cross_origin_set(JS::VM& vm, JS::Object& object, JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver)
{
    // 1. Let desc be ? O.[[GetOwnProperty]](P).
    auto descriptor = TRY(object.internal_get_own_property(property_key));

    // 2. Assert: desc is not undefined.
    VERIFY(descriptor.has_value());

    // 3. If desc.[[Set]] is present and its value is not undefined, then:
    if (descriptor->set.has_value() && *descriptor->set) {
        // FIXME: Spec issue, `setter` isn't being defined.
        // 1. Perform ? Call(setter, Receiver, «V»).
        TRY(JS::call(vm, *descriptor->set, receiver, value));

        // 2. Return true.
        return true;
    }

    // 4. Throw a "SecurityError" DOMException.
    return throw_completion(WebIDL::SecurityError::create(*vm.current_realm(), MUST(String::formatted("Can't set property '{}' on cross-origin object", property_key))));
}

// 7.2.3.7 CrossOriginOwnPropertyKeys ( O ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginownpropertykeys-(-o-)
JS::MarkedVector<JS::Value> cross_origin_own_property_keys(Variant<HTML::Location const*, HTML::Window const*> const& object)
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // 1. Let keys be a new empty List.
    auto keys = JS::MarkedVector<JS::Value> { vm.heap() };

    // 2. For each e of CrossOriginProperties(O), append e.[[Property]] to keys.
    for (auto& entry : cross_origin_properties(object))
        keys.append(JS::PrimitiveString::create(vm, move(entry.property)));

    // 3. Return the concatenation of keys and « "then", @@toStringTag, @@hasInstance, @@isConcatSpreadable ».
    keys.append(JS::PrimitiveString::create(vm, vm.names.then.as_string()));
    keys.append(vm.well_known_symbol_to_string_tag());
    keys.append(vm.well_known_symbol_has_instance());
    keys.append(vm.well_known_symbol_is_concat_spreadable());
    return keys;
}

}
