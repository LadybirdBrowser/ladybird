/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>
#include <LibURL/Origin.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

// The realm passed to these helpers is the preferred realm for wrapper allocation.
// Wrapper identity is keyed by the caller's WrapperWorld.
WEB_API GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<Wrappable>);
WEB_API GC::Ref<PlatformObject> create_wrapper_for_wrappable(JS::Realm& wrapper_realm, GC::Ref<Wrappable>);
WEB_API JS::Realm& wrapper_realm_for_wrappable(WrapperWorld const& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable>);
WEB_API JS::Realm& wrapper_realm_for_node(WrapperWorld const& wrapper_world, JS::Realm& preferred_realm, DOM::Node&);
WEB_API GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable>);
WEB_API JS::Realm& this_value_realm(JS::Realm& fallback_realm, JS::Value);
WEB_API JS::ThrowCompletionOr<void> set_prototype_of_cached_main_world_wrapper(Wrappable&, JS::Object&);
WEB_API void preserve_wrapper(Wrappable&, PlatformObject&);

#ifndef WEB_WRAPPABLE
#    define WEB_WRAPPABLE(class_, base_class)                           \
        GC_CELL(class_, base_class)                                     \
    public:                                                             \
        using Base::initialize;                                         \
        virtual Bindings::InterfaceName interface_name() const override \
        {                                                               \
            return Bindings::InterfaceName::class_;                     \
        }                                                               \
        virtual bool implements_interface(                              \
            String const& interface) const override                     \
        {                                                               \
            if (interface == #class_)                                   \
                return true;                                            \
            return Base::implements_interface(interface);               \
        }                                                               \
                                                                        \
    public:
#endif

#ifndef WEB_NON_IDL_WRAPPABLE
#    define WEB_NON_IDL_WRAPPABLE(class_, base_class) \
        GC_CELL(class_, base_class)                   \
    public:                                           \
        using Base::initialize;
#endif

// Base class for internal implementation objects that can be reflected into JS
// by a PlatformObject wrapper.
class WEB_API Wrappable : public JS::Cell {
    GC_CELL(Wrappable, JS::Cell);

public:
    using JSValueConversionIsForbidden = void;

    virtual ~Wrappable() override;

    [[nodiscard]] virtual Bindings::InterfaceName interface_name() const { VERIFY_NOT_REACHED(); }
    [[nodiscard]] virtual bool implements_interface(String const&) const { return false; }

    // https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin
    // Wrappers forward PlatformObject's extract an origin operation here.
    virtual Optional<URL::Origin> extract_an_origin() const;

    // Wrappers forward legacy platform object enumeration hooks here.
    virtual Vector<FlyString> supported_property_names() const;
    virtual bool is_supported_property_name(FlyString const& name) const;

    // Wrappables are implementation objects, so they do not get a
    // realm-specific initialization hook. They still upcall JS::Cell while they
    // are allocated on the GC heap.
    virtual void initialize(JS::Realm&) final override;

protected:
    Wrappable();

    [[nodiscard]] GC::Ptr<PlatformObject> cached_main_world_wrapper() const;

    virtual GC::Ref<PlatformObject> create_wrapper(JS::Realm& wrapper_realm);
    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    friend class WrapperWorld;
    friend WEB_API GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<Wrappable>);
    friend WEB_API GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable>);
    friend WEB_API JS::ThrowCompletionOr<void> set_prototype_of_cached_main_world_wrapper(Wrappable&, JS::Object&);
    friend WEB_API void preserve_wrapper(Wrappable&, PlatformObject&);

    void set_cached_main_world_wrapper(PlatformObject&);
    void clear_cached_main_world_wrapper(PlatformObject const&);

    GC::Weak<PlatformObject> m_main_world_wrapper;
    Vector<GC::Ptr<PlatformObject>> m_preserved_wrappers;
};

static_assert(!IsConstructible<JS::Value, Wrappable*>);
static_assert(!IsConstructible<JS::Value, Wrappable const*>);
static_assert(!IsConstructible<JS::Value, GC::Ptr<Wrappable>>);
static_assert(!IsConstructible<JS::Value, GC::Ref<Wrappable>>);
static_assert(!IsConstructible<JS::Value, GC::Root<Wrappable> const&>);

template<typename T>
requires(IsBaseOf<Wrappable, T> && !IsSameIgnoringCV<T, Wrappable>)
[[nodiscard]] GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<T> wrappable)
{
    return create_global_object_wrapper(wrapper_realm, GC::Ref<Wrappable> { wrappable });
}

WEB_API GC::Ptr<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ptr<Wrappable>);

template<typename T>
requires(IsBaseOf<Wrappable, T> && !IsSameIgnoringCV<T, Wrappable>)
[[nodiscard]] GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<T> wrappable)
{
    return wrap(wrapper_world, preferred_realm, GC::Ref<Wrappable> { wrappable });
}

template<typename T>
requires(IsBaseOf<Wrappable, T> && !IsSameIgnoringCV<T, Wrappable>)
[[nodiscard]] GC::Ptr<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ptr<T> wrappable)
{
    return wrap(wrapper_world, preferred_realm, GC::Ptr<Wrappable> { wrappable });
}

WEB_API void cache_global_object_wrapper(JS::Realm&);
WEB_API Wrappable* wrappable_impl_from(JS::Object*);
WEB_API Wrappable const* wrappable_impl_from(JS::Object const*);

template<typename Impl, typename Object>
[[nodiscard]] CopyConst<Object, Impl>* impl_from(Object* object)
{
    static_assert(IsBaseOf<Wrappable, Impl>);

    if (!object)
        return nullptr;

    if (auto* wrappable = wrappable_impl_from(object))
        return as_if<Impl>(wrappable);

    return nullptr;
}

template<typename Impl, typename Object>
[[nodiscard]] CopyConst<Object, Impl>& impl_from(Object& object)
{
    auto* impl = impl_from<Impl>(&object);
    VERIFY(impl);
    return *impl;
}

}
