/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/CustomElements/CustomElementAlgorithms.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

#ifndef NDEBUG
static HashMap<InterfaceName, Vector<FlatPtr>>& hintless_main_world_wrapper_realms_by_interface()
{
    static NeverDestroyed<HashMap<InterfaceName, Vector<FlatPtr>>> realms_by_interface;
    return *realms_by_interface;
}

static void log_hintless_main_world_wrapper_realm_if_new(Wrappable& wrappable, JS::Realm& realm)
{
    auto realm_address = bit_cast<FlatPtr>(&realm);
    auto& seen_realms = hintless_main_world_wrapper_realms_by_interface().ensure(wrappable.interface_name());
    if (seen_realms.contains_slow(realm_address))
        return;

    seen_realms.append(realm_address);
    dbgln("Wrapper diagnostic: hint-less non-Node main-world wrapper for interface #{} was created in previously unseen realm {:p}", static_cast<u16>(wrappable.interface_name()), &realm);
}
#endif

Wrappable::Wrappable() = default;

Wrappable::~Wrappable() = default;

Optional<URL::Origin> Wrappable::extract_an_origin() const
{
    return {};
}

Vector<Utf16FlyString> Wrappable::supported_property_names() const
{
    return {};
}

bool Wrappable::is_supported_property_name(Utf16FlyString const& name) const
{
    return supported_property_names().contains_slow(name);
}

GC::Ref<PlatformObject> Wrappable::create_wrapper(JS::Realm& wrapper_realm)
{
    return create_wrapper_for_wrappable(wrapper_realm, GC::Ref { *this });
}

void Wrappable::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_preserved_wrappers)
        visitor.visit(entry.wrapper);
}

GC::Ptr<PlatformObject> Wrappable::cached_main_world_wrapper(WrapperWorld const& world) const
{
    VERIFY(world.is_main_world());
    if (m_main_world_wrapper)
        VERIFY(&host_defined_wrapper_world(m_main_world_wrapper->realm()) == &world);
    return m_main_world_wrapper.ptr();
}

GC::Ptr<PlatformObject> Wrappable::cached_main_world_wrapper() const
{
    return m_main_world_wrapper.ptr();
}

void Wrappable::set_wrapper_realm_hint(JS::Realm& realm)
{
    m_wrapper_realm_hint = realm;
}

void Wrappable::set_wrapper_realm_hint(JS::Object const& relevant_global_object)
{
    set_wrapper_realm_hint(HTML::relevant_realm(relevant_global_object));
}

void Wrappable::set_wrapper_realm_hint(DOM::Node const& node)
{
    set_wrapper_realm_hint(HTML::relevant_realm(node));
}

void Wrappable::set_wrapper_realm_hint(HTML::EnvironmentSettingsObject& environment)
{
    set_wrapper_realm_hint(environment.realm());
}

void Wrappable::set_wrapper_realm_hint(HTML::Window const& window)
{
    set_wrapper_realm_hint(HTML::relevant_realm(window));
}

GC::Ptr<JS::Realm> Wrappable::wrapper_realm_hint() const
{
    return m_wrapper_realm_hint.ptr();
}

void Wrappable::set_cached_main_world_wrapper(PlatformObject& wrapper)
{
    VERIFY(wrapper.realm().host_defined());
    auto& wrapper_world = host_defined_wrapper_world(wrapper.realm());
    VERIFY(wrapper_world.is_main_world());
    if (m_main_world_wrapper) {
        VERIFY(&host_defined_wrapper_world(m_main_world_wrapper->realm()) == &wrapper_world);
        VERIFY(m_main_world_wrapper.ptr() == &wrapper);
        return;
    }

    m_main_world_wrapper = wrapper;
}

void Wrappable::clear_cached_main_world_wrapper(PlatformObject const& wrapper)
{
    if (!m_main_world_wrapper || m_main_world_wrapper.ptr() == &wrapper)
        m_main_world_wrapper = nullptr;
}

GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<Wrappable> wrappable)
{
    // This helper is for JS::Realm::initialize_host_defined_realm() global
    // object callbacks. At that point HostDefined is not installed yet, so the
    // wrapper is created first and cache_global_object_wrapper() must be called
    // after the caller installs HostDefined/intrinsics for the new realm.
    VERIFY(!wrapper_realm.host_defined());
    return wrappable->create_wrapper(wrapper_realm);
}

JS::Realm& wrapper_realm_for_node(WrapperWorld const& wrapper_world, JS::Realm& preferred_realm, DOM::Node& node)
{
    auto& relevant_realm = HTML::relevant_realm(node);
    if (relevant_realm.host_defined()) {
        auto& relevant_world = host_defined_wrapper_world(relevant_realm);
        VERIFY(!relevant_world.is_main_world() || &relevant_world == &wrapper_world);
        if (&relevant_world == &wrapper_world)
            return relevant_realm;
    }
    return preferred_realm;
}

JS::Realm& wrapper_realm_for_wrappable(WrapperWorld const& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable> wrappable)
{
    if (!wrapper_world.is_main_world())
        return preferred_realm;

    if (auto* node = as_if<DOM::Node>(wrappable.ptr()))
        return wrapper_realm_for_node(wrapper_world, preferred_realm, *node);

    if (auto hint = wrappable->wrapper_realm_hint(); hint && hint->host_defined()) {
        auto& hint_world = host_defined_wrapper_world(*hint);
        if (hint_world.is_main_world() && &hint_world == &wrapper_world)
            return *hint;
    }

    return preferred_realm;
}

GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable> wrappable)
{
    auto& actual_wrapper_realm = wrapper_realm_for_wrappable(wrapper_world, preferred_realm, wrappable);
    if (auto cached_wrapper = wrapper_world.wrapper_for(wrappable, actual_wrapper_realm))
        return *cached_wrapper;

    auto wrapper = wrappable->create_wrapper(actual_wrapper_realm);
    if (wrapper_world.is_main_world() && !is<DOM::Node>(wrappable.ptr()) && !wrappable->m_wrapper_realm_hint) {
#ifndef NDEBUG
        log_hintless_main_world_wrapper_realm_if_new(*wrappable, actual_wrapper_realm);
#endif
        wrappable->m_wrapper_realm_hint = actual_wrapper_realm;
    }
    if (auto* element = as_if<DOM::Element>(wrappable.ptr()))
        set_prototype_from_custom_element_definition_if_needed(*element, wrapper);
    wrapper_world.set_wrapper(wrappable, wrapper);
    return wrapper;
}

GC::Ptr<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ptr<Wrappable> wrappable)
{
    if (!wrappable)
        return nullptr;
    return wrap(wrapper_world, preferred_realm, GC::Ref { *wrappable });
}

JS::Realm& this_value_realm(JS::Realm& fallback_realm, JS::Value this_value)
{
    // Generated native-function prologues normally validate and unwrap their
    // receiver before asking for its realm. Keep this helper total for callers
    // that pass an arbitrary JS::Value; the fallback is their explicit realm.
    if (!this_value.is_object())
        return fallback_realm;

    auto& object = this_value.as_object();
    if (auto* window_proxy = as_if<HTML::WindowProxy>(&object)) {
        auto& proxy_realm = object.shape().realm();
        if (!proxy_realm.host_defined())
            return fallback_realm;

        // A main-world WindowProxy is created once, in the initial about:blank
        // realm, then reused as globalThis across later navigations. Do not use
        // that stale shape realm for receiver-realm decisions: HTML defines a
        // WindowProxy's relevant realm as the realm of its current [[Window]].
        //
        // Isolated-world WindowProxy objects are created in their own world
        // realm; keep those on the proxy's realm so cross-world wrapping stays
        // isolated instead of leaking through the principal Window realm.
        if (host_defined_wrapper_world(proxy_realm).is_main_world()) {
            if (auto window = window_proxy->window())
                return window->principal_realm();
        }
    }

    return object.shape().realm();
}

JS::ThrowCompletionOr<void> set_prototype_of_cached_main_world_wrapper(Wrappable& wrappable, JS::Object& prototype)
{
    if (auto wrapper = wrappable.m_main_world_wrapper.ptr()) {
        // Custom-element construction in an isolated world must not install
        // an isolated-world prototype on any main-world wrapper. The agent
        // ownership invariant guarantees that two main-world cells cannot
        // legitimately meet here.
        if (!wrapper->realm().host_defined() || !prototype.shape().realm().host_defined())
            return {};
        auto& wrapper_world = host_defined_wrapper_world(wrapper->realm());
        auto& prototype_world = host_defined_wrapper_world(prototype.shape().realm());
        if (!wrapper_world.is_main_world() || !prototype_world.is_main_world())
            return {};
        TRY(wrapper->internal_set_prototype_of(&prototype));
    }
    return {};
}

void preserve_wrapper(Wrappable& wrappable, PlatformObject& wrapper)
{
    auto& world = host_defined_wrapper_world(wrapper.realm());

    // Script can still run against a detached world's realm (e.g. a pending microtask after
    // WrapperWorld::detach()). Nothing in a detached world needs to survive GC, so preservation
    // is a no-op rather than a crash.
    if (world.is_detached())
        return;

    for (auto const& entry : wrappable.m_preserved_wrappers) {
        if (entry.wrapper.ptr() == &wrapper && entry.world.ptr() == &world)
            return;
    }

    wrappable.m_preserved_wrappers.append({ world, wrapper });
    if (!world.is_main_world())
        world.register_preserved_wrappable(wrappable);
}

bool wrapper_is_preserved(PlatformObject const& wrapper)
{
    auto const* wrappable = wrappable_impl_from(&wrapper);
    if (!wrappable)
        return false;

    for (auto const& entry : wrappable->m_preserved_wrappers) {
        if (entry.wrapper.ptr() == &wrapper)
            return true;
    }

    return false;
}

void Wrappable::clear_preserved_wrappers(WrapperWorld const& world)
{
    m_preserved_wrappers.remove_all_matching([&](auto const& entry) { return entry.world.ptr() == &world; });
}

void cache_global_object_wrapper(JS::Realm& realm)
{
    auto* platform_object = as_if<PlatformObject>(&realm.global_object());
    VERIFY(platform_object);

    auto* wrappable = platform_object->wrappable_impl();
    VERIFY(wrappable);

    host_defined_wrapper_world(realm).set_wrapper(*wrappable, *platform_object);
}

Wrappable* wrappable_impl_from(JS::Object* object)
{
    auto* platform_object = as_if<PlatformObject>(object);
    if (!platform_object)
        return nullptr;
    return platform_object->wrappable_impl();
}

Wrappable const* wrappable_impl_from(JS::Object const* object)
{
    auto const* platform_object = as_if<PlatformObject>(object);
    if (!platform_object)
        return nullptr;
    return platform_object->wrappable_impl();
}

}
