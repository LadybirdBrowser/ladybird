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
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Location.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/CrossOrigin/AbstractOperations.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

static JS::ThrowCompletionOr<GC::RootVector<GC::Ref<JS::Object>>> convert_transfer_argument(JS::VM& vm, JS::Value value)
{
    GC::RootVector<GC::Ref<JS::Object>> transfer;
    if (value.is_undefined())
        return transfer;

    if (!value.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, value);

    auto iterator_method = TRY(value.get_method(vm, vm.well_known_symbol_iterator()));
    if (!iterator_method)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotIterable, value);

    auto iterator = TRY(JS::get_iterator_from_method(vm, value, *iterator_method));
    for (;;) {
        auto next = TRY(JS::iterator_step_value(vm, iterator));
        if (!next.has_value())
            break;

        auto next_value = next.release_value();
        if (!next_value.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, next_value);

        transfer.append(next_value.as_object());
    }

    return transfer;
}

static GC::Ref<JS::NativeFunction> create_cross_origin_window_method(JS::Realm& realm, GC::Ref<Window> window, String const& property)
{
    if (property == "close"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) {
                window->close();
                return JS::js_undefined();
            },
            0, "close"_utf16_fly_string);
    }

    if (property == "focus"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) {
                window->focus();
                return JS::js_undefined();
            },
            0, "focus"_utf16_fly_string);
    }

    if (property == "blur"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) {
                window->blur();
                return JS::js_undefined();
            },
            0, "blur"_utf16_fly_string);
    }

    if (property == "postMessage"sv) {
        return JS::NativeFunction::create(
            realm, [&realm, window](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto message = vm.argument(0);

                if (vm.argument_count() >= 3) {
                    auto target_origin = TRY(WebIDL::to_usv_string(vm, vm.argument(1)));
                    auto transfer = TRY(convert_transfer_argument(vm, vm.argument(2)));
                    TRY(Bindings::throw_dom_exception_if_needed(vm, realm, [&] { return window->post_message(message, target_origin, transfer); }));
                    return JS::js_undefined();
                }

                auto second_argument = vm.argument(1);
                if (vm.argument_count() == 2 && !second_argument.is_undefined() && !second_argument.is_object()) {
                    auto target_origin = TRY(WebIDL::to_usv_string(vm, second_argument));
                    GC::RootVector<GC::Ref<JS::Object>> transfer;
                    TRY(Bindings::throw_dom_exception_if_needed(vm, realm, [&] { return window->post_message(message, target_origin, transfer); }));
                    return JS::js_undefined();
                }

                Bindings::WindowPostMessageOptions options {};
                if (vm.argument_count() >= 2 && !second_argument.is_undefined())
                    options = TRY(Bindings::convert_to_idl_value_for_window_post_message_options(vm, second_argument));
                TRY(Bindings::throw_dom_exception_if_needed(vm, realm, [&] { return window->post_message(message, options); }));
                return JS::js_undefined();
            },
            1, "postMessage"_utf16_fly_string);
    }

    VERIFY_NOT_REACHED();
}

static GC::Ref<JS::NativeFunction> create_cross_origin_window_getter(JS::Realm& realm, GC::Ref<Window> window, String const& property)
{
    if (property == "window"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                return window->window().ptr();
            },
            0, "window"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "self"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                return window->self().ptr();
            },
            0, "self"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "location"sv) {
        return JS::NativeFunction::create(
            realm, [window, &realm](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, window->location());
            },
            0, "location"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "closed"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                return JS::Value(window->closed());
            },
            0, "closed"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "frames"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                return window->frames().ptr();
            },
            0, "frames"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "length"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                return JS::Value(window->length());
            },
            0, "length"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "top"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                auto value = window->top();
                if (!value)
                    return JS::js_null();
                return value;
            },
            0, "top"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "opener"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                auto value = window->opener();
                if (!value)
                    return JS::js_null();
                return value;
            },
            0, "opener"_utf16_fly_string, &realm, "get"sv);
    }

    if (property == "parent"sv) {
        return JS::NativeFunction::create(
            realm, [window](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                auto value = window->parent();
                if (!value)
                    return JS::js_null();
                return value;
            },
            0, "parent"_utf16_fly_string, &realm, "get"sv);
    }

    VERIFY_NOT_REACHED();
}

static GC::Ref<JS::NativeFunction> create_cross_origin_window_setter(JS::Realm& realm, Window& window, String const& property)
{
    if (property == "location"sv) {
        return JS::NativeFunction::create(
            realm, [&realm, window = GC::Ref { window }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto value = vm.argument(0);
                auto href = TRY(WebIDL::to_usv_string(vm, value));
                auto location = window->location();
                TRY(Bindings::throw_dom_exception_if_needed(vm, realm, [&] { return location->set_href(realm, href); }));
                return JS::js_undefined();
            },
            1, "location"_utf16_fly_string, &realm, "set"sv);
    }

    VERIFY_NOT_REACHED();
}

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
    static NeverDestroyed<Array<FlyString, 13>> property_names { Array<FlyString, 13> {
        "window"_fly_string, "self"_fly_string, "location"_fly_string, "close"_fly_string, "closed"_fly_string, "focus"_fly_string, "blur"_fly_string, "frames"_fly_string, "length"_fly_string, "top"_fly_string, "opener"_fly_string, "parent"_fly_string, "postMessage"_fly_string } };
    return (property_key.is_string() && any_of(*property_names, [&](auto const& name) { return property_key.as_string() == name; })) || property_key.is_number();
}

// 7.2.3.2 CrossOriginPropertyFallback ( P ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginpropertyfallback-(-p-)
JS::ThrowCompletionOr<JS::PropertyDescriptor> cross_origin_property_fallback(JS::VM& vm, JS::PropertyKey const& property_key)
{
    // 1. If P is "then", @@toStringTag, @@hasInstance, or @@isConcatSpreadable, then return PropertyDescriptor { [[Value]]: undefined, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }.
    auto property_key_is_then = property_key.is_string() && property_key.as_string() == vm.names.then.as_string();
    auto property_key_is_allowed_symbol = property_key.is_symbol()
        && (property_key.as_symbol() == vm.well_known_symbol_to_string_tag()
            || property_key.as_symbol() == vm.well_known_symbol_has_instance()
            || property_key.as_symbol() == vm.well_known_symbol_is_concat_spreadable());
    if (property_key_is_then || property_key_is_allowed_symbol)
        return JS::PropertyDescriptor { .value = JS::js_undefined(), .writable = false, .enumerable = false, .configurable = true };

    // 2. Throw a "SecurityError" DOMException.
    return throw_completion(*vm.current_realm(), WebIDL::SecurityError::create(*vm.current_realm(), Utf16String::formatted("Can't access property '{}' on cross-origin object", property_key)));
}

// 7.2.3.3 IsPlatformObjectSameOrigin ( O ), https://html.spec.whatwg.org/multipage/nav-history-apis.html#isplatformobjectsameorigin-(-o-)
bool is_platform_object_same_origin(JS::Object const& object)
{
    // 1. Return true if the current settings object's origin is same origin-domain with O's relevant settings object's origin, and false otherwise.
    return HTML::current_settings_object().origin().is_same_origin_domain(HTML::relevant_settings_object(object).origin());
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
            return it->value;

        // 2. Let originalDesc be OrdinaryGetOwnProperty(O, P).
        Optional<JS::PropertyDescriptor> original_descriptor;
        platform_object.visit(
            [&](HTML::Location const*) {
                original_descriptor = MUST((object_ptr->JS::Object::internal_get_own_property)(property_key));
            },
            [](HTML::Window*) {});

        // NOTE: The current same-origin property descriptor might have been replaced by page script, for example via
        // [Replaceable]. Cross-origin access still needs to expose wrappers for the original IDL member on O.

        // 3. Let crossOriginDesc be undefined.
        auto cross_origin_descriptor = JS::PropertyDescriptor {};

        // 4. If e.[[NeedsGet]] and e.[[NeedsSet]] are absent, then:
        if (!entry.needs_get.has_value() && !entry.needs_set.has_value()) {
            // 1. Let value be originalDesc.[[Value]].
            // 2. If IsCallable(value) is true, then set value to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the IDL operation P on object O.
            auto value = platform_object.visit(
                [&](HTML::Location const*) -> JS::Value {
                    VERIFY(original_descriptor.has_value());
                    auto value = original_descriptor->value;
                    VERIFY(value.has_value());
                    if (auto function = value->as_if<JS::FunctionObject>()) {
                        auto name = function->get_without_side_effects(vm.names.name).to_utf16_string_without_side_effects();
                        auto length_property = function->get_without_side_effects(vm.names.length);
                        auto length = length_property.is_int32() ? length_property.as_i32() : 0;
                        value = JS::NativeFunction::create(
                            realm, [object_ptr, function](auto& vm) {
                                return JS::call(vm, function, object_ptr, vm.running_execution_context().arguments_span());
                            },
                            length, name);
                    }
                    return *value;
                },
                [&](HTML::Window* window) -> JS::Value {
                    return create_cross_origin_window_method(realm, *window, entry.property).ptr();
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
                cross_origin_get = platform_object.visit(
                    [&](HTML::Location const*) -> GC::Ptr<JS::FunctionObject> {
                        VERIFY(original_descriptor.has_value());
                        auto const& getter = original_descriptor->get;
                        VERIFY(getter.has_value());
                        auto name = getter.value()->get_without_side_effects(vm.names.name).to_utf16_string_without_side_effects();
                        return JS::NativeFunction::create(
                            realm, [object_ptr, getter = *getter](auto& vm) {
                                return JS::call(vm, getter, object_ptr, vm.running_execution_context().arguments_span());
                            },
                            0, name);
                    },
                    [&](HTML::Window* window) -> GC::Ptr<JS::FunctionObject> {
                        return create_cross_origin_window_getter(realm, *window, entry.property).ptr();
                    });
            }

            // 3. Let crossOriginSet be undefined.
            Optional<GC::Ptr<JS::FunctionObject>> cross_origin_set;

            // If e.[[NeedsSet]] is true, then set crossOriginSet to an anonymous built-in function, created in the current Realm Record, that performs the same steps as the setter of the IDL attribute P on object O.
            if (*entry.needs_set) {
                cross_origin_set = platform_object.visit(
                    [&](HTML::Location const*) -> GC::Ptr<JS::FunctionObject> {
                        VERIFY(original_descriptor.has_value());
                        auto const& setter = original_descriptor->set;
                        VERIFY(setter.has_value());
                        auto name = setter.value()->get_without_side_effects(vm.names.name).to_utf16_string_without_side_effects();
                        return JS::NativeFunction::create(
                            realm, [object_ptr, setter = *setter](auto& vm) {
                                return JS::call(vm, setter, object_ptr, vm.running_execution_context().arguments_span());
                            },
                            1, name);
                    },
                    [&](HTML::Window* window) -> GC::Ptr<JS::FunctionObject> {
                        return create_cross_origin_window_setter(realm, *window, entry.property).ptr();
                    });
            }

            // 5. Set crossOriginDesc to PropertyDescriptor { [[Get]]: crossOriginGet, [[Set]]: crossOriginSet, [[Enumerable]]: false, [[Configurable]]: true }.
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

Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(JS::Object& object, HTML::Location const& location,
    CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map, JS::PropertyKey const& property_key)
{
    return cross_origin_get_own_property_helper_impl(object, Variant<HTML::Location const*, HTML::Window*> { &location }, cross_origin_property_descriptor_map, property_key);
}

Optional<JS::PropertyDescriptor> cross_origin_get_own_property_helper(HTML::Window& window, JS::PropertyKey const& property_key)
{
    return cross_origin_get_own_property_helper_impl(HTML::relevant_global_object(window), Variant<HTML::Location const*, HTML::Window*> { &window }, window.cross_origin_property_descriptor_map(), property_key);
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
        return throw_completion(*vm.current_realm(), WebIDL::SecurityError::create(*vm.current_realm(), Utf16String::formatted("Can't get property '{}' on cross-origin object", property_key)));

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
    return throw_completion(*vm.current_realm(), WebIDL::SecurityError::create(*vm.current_realm(), Utf16String::formatted("Can't set property '{}' on cross-origin object", property_key)));
}

// 7.2.3.7 CrossOriginOwnPropertyKeys ( O ), https://html.spec.whatwg.org/multipage/browsers.html#crossoriginownpropertykeys-(-o-)
static GC::RootVector<JS::Value> cross_origin_own_property_keys_impl(Variant<HTML::Location const*, HTML::Window const*> const& object)
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto& vm = event_loop.vm();

    // 1. Let keys be a new empty List.
    GC::RootVector<JS::Value> keys;

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

GC::RootVector<JS::Value> cross_origin_own_property_keys(HTML::Location const& location)
{
    return cross_origin_own_property_keys_impl(Variant<HTML::Location const*, HTML::Window const*> { &location });
}

GC::RootVector<JS::Value> cross_origin_own_property_keys(HTML::Window const& window)
{
    return cross_origin_own_property_keys_impl(Variant<HTML::Location const*, HTML::Window const*> { &window });
}

}
