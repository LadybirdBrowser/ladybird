/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Location.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/CrossOrigin/AbstractOperations.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#integration-with-idl
JS::ThrowCompletionOr<void> perform_a_security_check(JS::VM& vm, JS::Value js_value, Utf16View const& identifier, SecurityCheckType type)
{
    // 1. If platformObject is not a Window or Location object, then return.
    // NOTE: A WindowProxy is checked against its [[Window]], which is the Window object the bindings operate on.
    Optional<Variant<Location const*, Window const*>> platform_object;
    if (js_value.is_object()) {
        auto& object = js_value.as_object();
        if (auto const* window_proxy = as_if<WindowProxy>(object)) {
            if (auto window = window_proxy->window())
                platform_object = window.ptr();
        } else if (auto const* wrappable = Bindings::wrappable_impl_from(&object)) {
            if (auto const* window = as_if<Window>(*wrappable))
                platform_object = window;
            else if (auto const* location = as_if<Location>(*wrappable))
                platform_object = location;
        }
    }
    if (!platform_object.has_value())
        return {};

    // NOTE: Steps 2 and 3 can only throw if platformObject is not same origin-domain with the current settings object,
    //       so check that first to avoid computing CrossOriginProperties(platformObject) for same-origin access.
    auto is_same_origin = platform_object->visit([](auto const* object) { return is_platform_object_same_origin(*object); });
    if (is_same_origin)
        return {};

    // 2. For each e of CrossOriginProperties(platformObject):
    for (auto const& entry : cross_origin_properties(*platform_object)) {
        // 1. If SameValue(e.[[Property]], identifier) is true:
        if (entry.property != identifier)
            continue;

        // 1. If type is "method" and e has neither [[NeedsGetter]] nor [[NeedsSetter]], then return.
        if (type == SecurityCheckType::Method && !entry.needs_get.has_value() && !entry.needs_set.has_value())
            return {};

        // 2. Otherwise, if type is "getter" and e.[[NeedsGetter]] is true, then return.
        if (type == SecurityCheckType::Getter && entry.needs_get == true)
            return {};

        // 3. Otherwise, if type is "setter" and e.[[NeedsSetter]] is true, then return.
        if (type == SecurityCheckType::Setter && entry.needs_set == true)
            return {};
    }

    // 3. If IsPlatformObjectSameOrigin(platformObject) is false, then throw a "SecurityError" DOMException.
    return throw_completion(*vm.current_realm(), WebIDL::SecurityError::create(Utf16String::formatted("Can't access property '{}' on cross-origin object", identifier)));
}

// 7.2.3.1 CrossOriginProperties ( O ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginproperties-(-o-)
Vector<CrossOriginProperty> cross_origin_properties(Variant<HTML::Location const*, HTML::Window const*> const& object)
{
    // 1. Assert: O is a Location or Window object.

    return object.visit(
        // 2. If O is a Location object, then return « { [[Property]]: "href", [[NeedsGet]]: false, [[NeedsSet]]: true }, { [[Property]]: "replace" } ».
        [](HTML::Location const*) -> Vector<CrossOriginProperty> {
            return {
                { .property = "href"_utf16_fly_string, .needs_get = false, .needs_set = true },
                { .property = "replace"_utf16_fly_string },
            };
        },
        // 3. Return « { [[Property]]: "window", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "self", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "location", [[NeedsGet]]: true, [[NeedsSet]]: true }, { [[Property]]: "close" }, { [[Property]]: "closed", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "focus" }, { [[Property]]: "blur" }, { [[Property]]: "frames", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "length", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "top", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "opener", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "parent", [[NeedsGet]]: true, [[NeedsSet]]: false }, { [[Property]]: "postMessage" } ».
        [](HTML::Window const*) { return cross_origin_window_properties(); });
}

Vector<CrossOriginProperty> cross_origin_window_properties()
{
    return {
        { .property = "window"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "self"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "location"_utf16_fly_string, .needs_get = true, .needs_set = true },
        { .property = "close"_utf16_fly_string },
        { .property = "closed"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "focus"_utf16_fly_string },
        { .property = "blur"_utf16_fly_string },
        { .property = "frames"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "length"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "top"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "opener"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "parent"_utf16_fly_string, .needs_get = true, .needs_set = false },
        { .property = "postMessage"_utf16_fly_string },
    };
}

// https://html.spec.whatwg.org/multipage/browsers.html#cross-origin-accessible-window-property-name
bool is_cross_origin_accessible_window_property_name(JS::PropertyKey const& property_key)
{
    // A JavaScript property name P is a cross-origin accessible window property name if it is "window", "self", "location", "close", "closed", "focus", "blur", "frames", "length", "top", "opener", "parent", "postMessage", or an array index property name.
    static NeverDestroyed<Array<Utf16FlyString, 13>> property_names { Array<Utf16FlyString, 13> {
        "window"_utf16_fly_string, "self"_utf16_fly_string, "location"_utf16_fly_string, "close"_utf16_fly_string, "closed"_utf16_fly_string, "focus"_utf16_fly_string, "blur"_utf16_fly_string, "frames"_utf16_fly_string, "length"_utf16_fly_string, "top"_utf16_fly_string, "opener"_utf16_fly_string, "parent"_utf16_fly_string, "postMessage"_utf16_fly_string } };
    return (property_key.is_string() && any_of(*property_names, [&](auto const& name) { return property_key.as_string() == name; })) || property_key.is_number();
}

// 7.2.3.2 CrossOriginPropertyFallback ( P ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginpropertyfallback-(-p-)
JS::ThrowCompletionOr<JS::PropertyDescriptor> cross_origin_property_fallback(JS::VM& vm, JS::PropertyKey const& property_key)
{
    // 1. If P is "then", @@toStringTag, @@hasInstance, or @@isConcatSpreadable, then return PropertyDescriptor { [[Value]]: undefined, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }.
    auto property_key_is_then = property_key.is_string() && property_key.as_string() == vm.names.then.as_string();
    auto property_key_is_allowed_symbol = property_key.is_symbol()
        && (property_key.as_symbol() == vm.well_known_symbol_to_string_tag().ptr()
            || property_key.as_symbol() == vm.well_known_symbol_has_instance().ptr()
            || property_key.as_symbol() == vm.well_known_symbol_is_concat_spreadable().ptr());
    if (property_key_is_then || property_key_is_allowed_symbol)
        return JS::PropertyDescriptor { .value = JS::js_undefined(), .writable = false, .enumerable = false, .configurable = true };

    // 2. Throw a "SecurityError" DOMException.
    return throw_completion(*vm.current_realm(), WebIDL::SecurityError::create(Utf16String::formatted("Can't access property '{}' on cross-origin object", property_key)));
}

// 7.2.3.3 IsPlatformObjectSameOrigin ( O ), https://html.spec.whatwg.org/multipage/nav-history-apis.html#isplatformobjectsameorigin-(-o-)
bool is_platform_object_same_origin(JS::Object const& object)
{
    // 1. Return true if the current settings object's origin is same origin-domain with O's relevant settings object's origin, and false otherwise.
    return HTML::current_settings_object().origin().is_same_origin_domain(HTML::relevant_settings_object(object).origin());
}

bool is_platform_object_same_origin(Location const& location)
{
    // 1. Return true if the current settings object's origin is same origin-domain with O's relevant settings object's origin, and false otherwise.
    return HTML::current_settings_object().origin().is_same_origin_domain(HTML::relevant_settings_object(location.window()).origin());
}

bool is_platform_object_same_origin(Window const& window)
{
    // 1. Return true if the current settings object's origin is same origin-domain with O's relevant settings object's origin, and false otherwise.
    return HTML::current_settings_object().origin().is_same_origin_domain(HTML::relevant_settings_object(window).origin());
}

// 7.2.3.4 CrossOriginGetOwnPropertyHelper ( O, P ), https://html.spec.whatwg.org/multipage/nav-history-apis.html#crossorigingetownpropertyhelper-(-o,-p-)
static Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper_impl(JS::Object& object,
    Variant<HTML::Location const*, HTML::Window*> const& platform_object,
    CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map, JS::PropertyKey const& property_key)
{
    auto& vm = Bindings::main_thread_vm();
    auto& realm = *vm.current_realm();
    auto const* object_ptr = &object;

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
    auto const& property_key_string = property_key.as_string();

    auto const platform_object_const_variant = platform_object.visit([](auto* object) {
        return Variant<HTML::Location const*, HTML::Window const*> { object };
    });

    // 2. For each e of CrossOriginProperties(O):
    for (auto const& entry : cross_origin_properties(platform_object_const_variant)) {
        if (entry.property != property_key_string)
            continue;

        // 1. If the value of the [[CrossOriginPropertyDescriptorMap]] internal slot of O contains an entry whose key is crossOriginKey, then return that entry's value.
        auto it = cross_origin_property_descriptor_map.find(cross_origin_key);
        if (it != cross_origin_property_descriptor_map.end())
            return it->value.descriptor;

        // 2. Let originalDesc be OrdinaryGetOwnProperty(O, P).
        // NB: originalDesc might have been replaced by page script, for example via [Replaceable], so the functions
        //     below run the steps of the IDL member directly instead of reading originalDesc.

        // 3. Let crossOriginDesc be undefined.
        auto cross_origin_descriptor = JS::PropertyDescriptor {};

        // 4. If e.[[NeedsGet]] and e.[[NeedsSet]] are absent, then:
        if (!entry.needs_get.has_value() && !entry.needs_set.has_value()) {
            // 1. Let value be originalDesc.[[Value]].
            // 2. If IsCallable(value) is true, then set value to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the IDL operation P on object O.
            JS::Value value = platform_object.visit(
                [&](HTML::Location const*) {
                    return Bindings::LocationWrapper::create_cross_origin_method(realm, entry.property);
                },
                [&](HTML::Window*) {
                    return Bindings::WindowWrapper::create_cross_origin_method(realm, entry.property);
                });

            // 3. Set crossOriginDesc to PropertyDescriptor { [[Value]]: value, [[Enumerable]]: false, [[Writable]]: false, [[Configurable]]: true }.
            cross_origin_descriptor = JS::PropertyDescriptor { .value = value, .writable = false, .enumerable = false, .configurable = true };
        }
        // 5. Otherwise:
        else {
            // 1. Let crossOriginGet be undefined.
            Optional<GC::Ptr<JS::FunctionObject>> cross_origin_get;

            // 2. If e.[[NeedsGet]] is true, then set crossOriginGet to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the getter of the IDL attribute P on object O.
            if (*entry.needs_get) {
                VERIFY(platform_object.has<HTML::Window*>());
                cross_origin_get = Bindings::WindowWrapper::create_cross_origin_getter(realm, entry.property).ptr();
            }

            // 3. Let crossOriginSet be undefined.
            Optional<GC::Ptr<JS::FunctionObject>> cross_origin_set;

            // If e.[[NeedsSet]] is true, then set crossOriginSet to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the setter of the IDL attribute P on object O.
            if (*entry.needs_set) {
                cross_origin_set = platform_object.visit(
                    [&](HTML::Location const*) -> GC::Ptr<JS::FunctionObject> {
                        return Bindings::LocationWrapper::create_cross_origin_setter(realm, entry.property).ptr();
                    },
                    [&](HTML::Window*) -> GC::Ptr<JS::FunctionObject> {
                        return Bindings::WindowWrapper::create_cross_origin_setter(realm, entry.property).ptr();
                    });
            }

            // 5. Set crossOriginDesc to PropertyDescriptor { [[Get]]: crossOriginGet, [[Set]]: crossOriginSet, [[Enumerable]]: false, [[Configurable]]: true }.
            cross_origin_descriptor = JS::PropertyDescriptor { .get = cross_origin_get, .set = cross_origin_set, .enumerable = false, .configurable = true };
        }

        // 6. Create an entry in the value of the [[CrossOriginPropertyDescriptorMap]] internal slot of O with key crossOriginKey and value crossOriginDesc.
        cross_origin_property_descriptor_map.set(cross_origin_key, CrossOriginCachedPropertyDescriptor { cross_origin_descriptor });

        // 7. Return crossOriginDesc.
        return cross_origin_descriptor;
    }

    // 3. Return undefined.
    return {};
}

Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(JS::Object& object, HTML::Location const& location,
    CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map, JS::PropertyKey const& property_key)
{
    return cross_origin_get_own_property_helper_impl(object, Variant<HTML::Location const*, HTML::Window*> { &location }, cross_origin_property_descriptor_map, property_key);
}

Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(JS::Object& object, HTML::Window& window,
    CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map, JS::PropertyKey const& property_key)
{
    return cross_origin_get_own_property_helper_impl(object, Variant<HTML::Location const*, HTML::Window*> { &window }, cross_origin_property_descriptor_map, property_key);
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
        return throw_completion(*vm.current_realm(), WebIDL::SecurityError::create(Utf16String::formatted("Can't get property '{}' on cross-origin object", property_key)));

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
        // 1. Perform ? Call(desc.[[Set]], Receiver, « V »).
        TRY(JS::call(vm, *descriptor->set, receiver, value));

        // 2. Return true.
        return true;
    }

    // 4. Throw a "SecurityError" DOMException.
    return throw_completion(*vm.current_realm(), WebIDL::SecurityError::create(Utf16String::formatted("Can't set property '{}' on cross-origin object", property_key)));
}

// 7.2.3.7 CrossOriginOwnPropertyKeys ( O ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginownpropertykeys-(-o-)
static GC::RootVector<JS::Value> cross_origin_own_property_keys_impl(Vector<CrossOriginProperty> const& properties)
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // 1. Let keys be a new empty List.
    GC::RootVector<JS::Value> keys;

    // 2. For each e of CrossOriginProperties(O), append e.[[Property]] to keys.
    for (auto const& entry : properties)
        keys.append(JS::PrimitiveString::create(vm, entry.property));

    // 3. Return the concatenation of keys and « "then", @@toStringTag, @@hasInstance, @@isConcatSpreadable ».
    keys.append(JS::PrimitiveString::create(vm, vm.names.then.as_string()));
    keys.append(vm.well_known_symbol_to_string_tag());
    keys.append(vm.well_known_symbol_has_instance());
    keys.append(vm.well_known_symbol_is_concat_spreadable());
    return keys;
}

GC::RootVector<JS::Value> cross_origin_own_property_keys(HTML::Location const& location)
{
    return cross_origin_own_property_keys_impl(cross_origin_properties(Variant<HTML::Location const*, HTML::Window const*> { &location }));
}

GC::RootVector<JS::Value> cross_origin_own_property_keys(HTML::Window const&)
{
    return cross_origin_window_own_property_keys();
}

GC::RootVector<JS::Value> cross_origin_window_own_property_keys()
{
    return cross_origin_own_property_keys_impl(cross_origin_window_properties());
}

}
