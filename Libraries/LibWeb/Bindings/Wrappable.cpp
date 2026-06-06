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

Wrappable::Wrappable(JS::Realm& realm)
    : m_realm(realm)
{
}

Wrappable::~Wrappable() = default;

Optional<URL::Origin> Wrappable::extract_an_origin() const
{
    return {};
}

Optional<JS::Value> Wrappable::item_value(JS::Realm&, size_t) const
{
    return {};
}

JS::Value Wrappable::named_item_value(JS::Realm&, FlyString const&) const
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

WebIDL::ExceptionOr<void> Wrappable::set_value_of_new_named_property(String const&, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_existing_named_property(String const&, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_named_property(String const&, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_new_indexed_property(u32, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_existing_indexed_property(u32, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> Wrappable::set_value_of_indexed_property(u32, JS::Value)
{
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<NamedPropertyDeletionResult> Wrappable::delete_value(String const&)
{
    VERIFY_NOT_REACHED();
}

void Wrappable::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
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

GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& realm, GC::Ref<Wrappable> wrappable, WrapperWorldType wrapper_world_type)
{
    VERIFY(!realm.host_defined());
    if (wrapper_world_type == WrapperWorldType::Main)
        VERIFY(&wrappable->realm() == &realm);
    return wrappable->create_wrapper(realm);
}

GC::Ref<PlatformObject> wrap(JS::Realm& realm, GC::Ref<Wrappable> wrappable)
{
    auto& requested_wrapper_world = host_defined_wrapper_world(realm);
    auto& wrapper_realm = requested_wrapper_world.is_main_world() ? wrappable->realm() : realm;
    auto& wrapper_world = host_defined_wrapper_world(wrapper_realm);
    if (auto cached_wrapper = wrapper_world.wrapper_for(wrappable))
        return *cached_wrapper;

    auto wrapper = wrappable->create_wrapper(wrapper_realm);
    wrapper_world.set_wrapper(wrappable, wrapper);
    return wrapper;
}

GC::Ptr<PlatformObject> wrap(JS::Realm& realm, GC::Ptr<Wrappable> wrappable)
{
    if (!wrappable)
        return nullptr;
    return wrap(realm, GC::Ref { *wrappable });
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
