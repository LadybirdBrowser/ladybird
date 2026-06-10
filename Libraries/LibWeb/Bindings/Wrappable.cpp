/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/CustomElements/CustomElementAlgorithms.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

Wrappable::Wrappable() = default;

Wrappable::~Wrappable() = default;

Optional<URL::Origin> Wrappable::extract_an_origin() const
{
    return {};
}

Vector<FlyString> Wrappable::supported_property_names() const
{
    return {};
}

bool Wrappable::is_supported_property_name(FlyString const& name) const
{
    return supported_property_names().contains_slow(name);
}

void Wrappable::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
}

void Wrappable::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

GC::Ref<PlatformObject> Wrappable::create_wrapper(JS::Realm& wrapper_realm)
{
    return create_wrapper_for_wrappable(wrapper_realm, GC::Ref { *this });
}

GC::Ptr<PlatformObject> Wrappable::cached_main_world_wrapper() const
{
    return m_main_world_wrapper.ptr();
}

void Wrappable::set_cached_main_world_wrapper(PlatformObject& wrapper)
{
    if (m_main_world_wrapper) {
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
    VERIFY(!wrapper_realm.host_defined());
    return wrappable->create_wrapper(wrapper_realm);
}

JS::Realm& wrapper_realm_for_node(WrapperWorld const& wrapper_world, JS::Realm& preferred_realm, DOM::Node& node)
{
    if (!wrapper_world.is_main_world())
        return preferred_realm;

    return HTML::relevant_realm(node);
}

GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable> wrappable)
{
    if (wrapper_world.is_main_world()) {
        if (auto cached_wrapper = wrapper_world.wrapper_for(wrappable, preferred_realm))
            return *cached_wrapper;
    }

    auto& actual_wrapper_realm = wrapper_realm_for_wrappable(wrapper_world, preferred_realm, wrappable);
    if (auto cached_wrapper = wrapper_world.wrapper_for(wrappable, actual_wrapper_realm))
        return *cached_wrapper;

    auto wrapper = wrappable->create_wrapper(actual_wrapper_realm);
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
    if (!this_value.is_object())
        return fallback_realm;

    auto& object = this_value.as_object();
    if (auto const* window_proxy = as_if<HTML::WindowProxy>(&object)) {
        if (auto window = window_proxy->window())
            return HTML::relevant_realm(*window);
    }

    return object.shape().realm();
}

JS::ThrowCompletionOr<void> set_prototype_of_cached_main_world_wrapper(Wrappable& wrappable, JS::Object& prototype)
{
    if (auto wrapper = wrappable.cached_main_world_wrapper())
        TRY(wrapper->internal_set_prototype_of(&prototype));
    return {};
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
