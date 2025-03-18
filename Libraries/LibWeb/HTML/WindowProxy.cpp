/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CrossOrigin/AbstractOperations.h>
#include <LibWeb/HTML/CrossOrigin/Reporting.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WindowProxy);

// 7.4 The WindowProxy exotic object, https://html.spec.whatwg.org/multipage/window-object.html#the-windowproxy-exotic-object
WindowProxy::WindowProxy(JS::Realm& realm)
    : JS::Object(realm, nullptr, MayInterfereWithIndexedPropertyAccess::Yes)
{
}

// 7.4.1 [[GetPrototypeOf]] ( ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-getprototypeof
JS::ThrowCompletionOr<JS::Object*> WindowProxy::internal_get_prototype_of() const
{
    // 1. Let W be the value of the [[Window]] internal slot of this.

    // 2. If IsPlatformObjectSameOrigin(W) is true, then return ! OrdinaryGetPrototypeOf(W).
    if (is_platform_object_same_origin(*m_window))
        return MUST(m_window->internal_get_prototype_of());

    // 3. Return null.
    return nullptr;
}

// 7.4.2 [[SetPrototypeOf]] ( V ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-setprototypeof
JS::ThrowCompletionOr<bool> WindowProxy::internal_set_prototype_of(Object* prototype)
{
    // 1. Return ! SetImmutablePrototype(this, V).
    return MUST(set_immutable_prototype(prototype));
}

// 7.4.3 [[IsExtensible]] ( ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-isextensible
JS::ThrowCompletionOr<bool> WindowProxy::internal_is_extensible() const
{
    // 1. Return true.
    return true;
}

// 7.4.4 [[PreventExtensions]] ( ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-preventextensions
JS::ThrowCompletionOr<bool> WindowProxy::internal_prevent_extensions()
{
    // 1. Return false.
    return false;
}

// 7.4.5 [[GetOwnProperty]] ( P ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-getownproperty
JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> WindowProxy::internal_get_own_property(JS::PropertyKey const& property_key) const
{
    auto& vm = this->vm();

    // 1. Let W be the value of the [[Window]] internal slot of this.

    // 2. If P is an array index property name, then:
    if (property_key.is_number()) {
        // 1. Let index be ! ToUint32(P).
        auto index = property_key.as_number();

        // 2. Let children be the document-tree child navigables of W's associated Document.
        auto children = m_window->associated_document().document_tree_child_navigables();

        // 3. Let value be undefined.
        Optional<JS::Value> value;

        // 4. If index is less than children's size, then:
        if (index < children.size()) {
            // 1. Sort children in ascending order, with navigableA being less than navigableB if navigableA's container was inserted into W's associated Document earlier than navigableB's container was.
            // NOTE: children are coming sorted in required order from document_tree_child_navigables()

            // 2. Set value to children[index]'s active WindowProxy.
            value = children[index]->active_window_proxy();
        }

        // 5. If value is undefined, then:
        if (!value.has_value()) {
            // 1. If IsPlatformObjectSameOrigin(W) is true, then return undefined.
            if (is_platform_object_same_origin(*m_window))
                return Optional<JS::PropertyDescriptor> {};

            // 2. Throw a "SecurityError" DOMException.
            return throw_completion(WebIDL::SecurityError::create(m_window->realm(), MUST(String::formatted("Can't access property '{}' on cross-origin object", property_key))));
        }

        // 6. Return PropertyDescriptor{ [[Value]]: value, [[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: true }.
        return JS::PropertyDescriptor { .value = move(value), .writable = false, .enumerable = true, .configurable = true };
    }

    // 3. If IsPlatformObjectSameOrigin(W) is true, then return ! OrdinaryGetOwnProperty(W, P).
    // NOTE: This is a willful violation of the JavaScript specification's invariants of the essential internal methods to maintain compatibility with existing web content. See tc39/ecma262 issue #672 for more information.
    if (is_platform_object_same_origin(*m_window))
        return m_window->internal_get_own_property(property_key);

    // 4. Let property be CrossOriginGetOwnPropertyHelper(W, P).
    auto property = cross_origin_get_own_property_helper(const_cast<Window*>(m_window.ptr()), property_key);

    // 5. If property is not undefined, then return property.
    if (property.has_value())
        return property;

    // 6. If property is undefined and P is in W's document-tree child navigable target name property set, then:
    auto navigable_property_set = m_window->document_tree_child_navigable_target_name_property_set();
    auto property_key_string = property_key.to_string();

    if (auto navigable = navigable_property_set.get(property_key_string); navigable.has_value()) {
        // 1. Let value be the active WindowProxy of the named object of W with the name P.
        auto value = navigable.value()->active_window_proxy();

        // 2. Return PropertyDescriptor{ [[Value]]: value, [[Enumerable]]: false, [[Writable]]: false, [[Configurable]]: true }.
        // NOTE: The reason the property descriptors are non-enumerable, despite this mismatching the same-origin behavior, is for compatibility with existing web content. See issue #3183 for details.
        return JS::PropertyDescriptor { .value = value, .writable = false, .enumerable = false, .configurable = true };
    }

    // 7. Return ? CrossOriginPropertyFallback(P).
    return TRY(cross_origin_property_fallback(vm, property_key));
}

// 7.4.6 [[DefineOwnProperty]] ( P, Desc ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-defineownproperty
JS::ThrowCompletionOr<bool> WindowProxy::internal_define_own_property(JS::PropertyKey const& property_key, JS::PropertyDescriptor const& descriptor, Optional<JS::PropertyDescriptor>*)
{
    // 1. Let W be the value of the [[Window]] internal slot of this.

    // 2. If IsPlatformObjectSameOrigin(W) is true, then:
    if (is_platform_object_same_origin(*m_window)) {
        // 1. If P is an array index property name, return false.
        if (property_key.is_number())
            return false;

        // 2. Return ? OrdinaryDefineOwnProperty(W, P, Desc).
        // NOTE: This is a willful violation of the JavaScript specification's invariants of the essential internal methods to maintain compatibility with existing web content. See tc39/ecma262 issue #672 for more information.
        return m_window->internal_define_own_property(property_key, descriptor);
    }

    // 3. Throw a "SecurityError" DOMException.
    return throw_completion(WebIDL::SecurityError::create(m_window->realm(), MUST(String::formatted("Can't define property '{}' on cross-origin object", property_key))));
}

// 7.4.7 [[Get]] ( P, Receiver ), https://html.spec.whatwg.org/multipage/nav-history-apis.html#windowproxy-get
// https://html.spec.whatwg.org/multipage/nav-history-apis.html#windowproxy-get
JS::ThrowCompletionOr<JS::Value> WindowProxy::internal_get(JS::PropertyKey const& property_key, JS::Value receiver, JS::CacheablePropertyMetadata*, PropertyLookupPhase) const
{
    auto& vm = this->vm();

    // 1. Let W be the value of the [[Window]] internal slot of this.

    // 2. Check if an access between two browsing contexts should be reported, given the current principal global object's browsing context, W's browsing context, P, and the current principal settings object.
    check_if_access_between_two_browsing_contexts_should_be_reported(*as<Window>(current_principal_global_object()).browsing_context(), m_window->browsing_context(), property_key, current_principal_settings_object());

    // 3. If IsPlatformObjectSameOrigin(W) is true, then return ? OrdinaryGet(this, P, Receiver).
    // NOTE: this is passed rather than W as OrdinaryGet and CrossOriginGet will invoke the [[GetOwnProperty]] internal method.
    if (is_platform_object_same_origin(*m_window))
        return JS::Object::internal_get(property_key, receiver);

    // 4. Return ? CrossOriginGet(this, P, Receiver).
    // NOTE: this is passed rather than W as OrdinaryGet and CrossOriginGet will invoke the [[GetOwnProperty]] internal method.
    return cross_origin_get(vm, *this, property_key, receiver);
}

// 7.4.8 [[Set]] ( P, V, Receiver ), https://html.spec.whatwg.org/multipage/nav-history-apis.html#windowproxy-set
// https://html.spec.whatwg.org/multipage/nav-history-apis.html#windowproxy-set
JS::ThrowCompletionOr<bool> WindowProxy::internal_set(JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver, JS::CacheablePropertyMetadata*)
{
    auto& vm = this->vm();

    // 1. Let W be the value of the [[Window]] internal slot of this.

    // 2. Check if an access between two browsing contexts should be reported, given the current principal global object's browsing context, W's browsing context, P, and the current principal settings object.
    check_if_access_between_two_browsing_contexts_should_be_reported(*as<Window>(current_principal_global_object()).browsing_context(), m_window->browsing_context(), property_key, current_principal_settings_object());

    // 3. If IsPlatformObjectSameOrigin(W) is true, then:
    if (is_platform_object_same_origin(*m_window)) {
        // 1. If P is an array index property name, then return false.
        if (property_key.is_number())
            return false;

        // 2. Return ? OrdinarySet(W, P, V, Receiver).
        return m_window->internal_set(property_key, value, receiver);
    }

    // 4. Return ? CrossOriginSet(this, P, V, Receiver).
    // NOTE: this is passed rather than W as CrossOriginSet will invoke the [[GetOwnProperty]] internal method.
    return cross_origin_set(vm, *this, property_key, value, receiver);
}

// 7.4.9 [[Delete]] ( P ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-delete
JS::ThrowCompletionOr<bool> WindowProxy::internal_delete(JS::PropertyKey const& property_key)
{
    // 1. Let W be the value of the [[Window]] internal slot of this.

    // 2. If IsPlatformObjectSameOrigin(W) is true, then:
    if (is_platform_object_same_origin(*m_window)) {
        // 1. If P is an array index property name, then:
        if (property_key.is_number()) {
            // 2. Let desc be ! this.[[GetOwnProperty]](P).
            auto descriptor = MUST(internal_get_own_property(property_key));

            // 2. If desc is undefined, then return true.
            if (!descriptor.has_value())
                return true;

            // 3. Return false.
            return false;
        }

        // 2. Return ? OrdinaryDelete(W, P).
        return m_window->internal_delete(property_key);
    }

    // 3. Throw a "SecurityError" DOMException.
    return throw_completion(WebIDL::SecurityError::create(m_window->realm(), MUST(String::formatted("Can't delete property '{}' on cross-origin object", property_key))));
}

// 7.4.10 [[OwnPropertyKeys]] ( ), https://html.spec.whatwg.org/multipage/window-object.html#windowproxy-ownpropertykeys
JS::ThrowCompletionOr<GC::RootVector<JS::Value>> WindowProxy::internal_own_property_keys() const
{
    auto& event_loop = main_thread_event_loop();
    auto& vm = event_loop.vm();

    // 1. Let W be the value of the [[Window]] internal slot of this.

    // 2. Let keys be a new empty List.
    auto keys = GC::RootVector<JS::Value> { vm.heap() };

    // 3. Let maxProperties be W's associated Document's document-tree child navigables's size.
    auto max_properties = m_window->associated_document().document_tree_child_navigables().size();

    // 4. Let index be 0.
    // 5. Repeat while index < maxProperties,
    for (size_t i = 0; i < max_properties; ++i) {
        // 1. Add ! ToString(index) as the last element of keys.
        keys.append(JS::PrimitiveString::create(vm, ByteString::number(i)));

        // 2. Increment index by 1.
    }

    // 6. If IsPlatformObjectSameOrigin(W) is true, then return the concatenation of keys and OrdinaryOwnPropertyKeys(W).
    if (is_platform_object_same_origin(*m_window)) {
        keys.extend(MUST(m_window->internal_own_property_keys()));
        return keys;
    }

    // 7. Return the concatenation of keys and ! CrossOriginOwnPropertyKeys(W).
    keys.extend(cross_origin_own_property_keys(m_window.ptr()));
    return keys;
}

void WindowProxy::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

void WindowProxy::set_window(GC::Ref<Window> window)
{
    m_window = move(window);
}

GC::Ref<BrowsingContext> WindowProxy::associated_browsing_context() const
{
    return *m_window->associated_document().browsing_context();
}

}
