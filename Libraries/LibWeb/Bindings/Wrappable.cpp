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
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

Wrappable::Wrappable() = default;

Wrappable::~Wrappable() = default;

Optional<URL::Origin> Wrappable::extract_an_origin() const
{
    return {};
}

Optional<JS::Value> Wrappable::item_value(Bindings::WrapperWorld&, JS::Realm&, size_t) const
{
    return {};
}

JS::Value Wrappable::named_item_value(Bindings::WrapperWorld&, JS::Realm&, FlyString const&) const
{
    return JS::js_undefined();
}

Vector<FlyString> Wrappable::supported_property_names() const
{
    return {};
}

bool Wrappable::is_supported_property_name(FlyString const& name) const
{
    return supported_property_names().contains_slow(name);
}

JS::Realm& Wrappable::wrapper_realm(WrapperWorld const&, JS::Realm& preferred_realm) const
{
    return preferred_realm;
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_new_named_property(JS::Realm&, String const&, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_existing_named_property(JS::Realm&, String const&, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_named_property(JS::Realm&, String const&, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_new_indexed_property(JS::Realm&, u32, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_existing_indexed_property(JS::Realm&, u32, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_indexed_property(JS::Realm&, u32, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<NamedPropertyDeletionResult> Wrappable::delete_value(JS::Realm&, String const&)
{
    VERIFY_NOT_REACHED();
}

void Wrappable::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
}

void Wrappable::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

void Wrappable::set_cached_main_world_wrapper(PlatformObject& wrapper)
{
    VERIFY(!m_main_world_wrapper || m_main_world_wrapper.ptr() == &wrapper);
    m_main_world_wrapper = wrapper;
}

void Wrappable::clear_cached_main_world_wrapper(PlatformObject const& wrapper)
{
    if (m_main_world_wrapper.ptr() == &wrapper)
        m_main_world_wrapper = nullptr;
}

GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<Wrappable> wrappable)
{
    VERIFY(!wrapper_realm.host_defined());
    return wrappable->create_wrapper(wrapper_realm);
}

GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable> wrappable)
{
    if (auto cached_wrapper = wrapper_world.wrapper_for(wrappable))
        return *cached_wrapper;

    auto& actual_wrapper_realm = wrappable->wrapper_realm(wrapper_world, preferred_realm);
    auto wrapper = wrappable->create_wrapper(actual_wrapper_realm);
    wrapper_world.set_wrapper(wrappable, wrapper);
    return wrapper;
}

GC::Ptr<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ptr<Wrappable> wrappable)
{
    if (!wrappable)
        return nullptr;
    return wrap(wrapper_world, preferred_realm, GC::Ref { *wrappable });
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
