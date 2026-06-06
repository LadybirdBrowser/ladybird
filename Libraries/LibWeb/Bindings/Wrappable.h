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

enum class NamedPropertyDeletionResult : u8 {
    // If the named property deleter has an identifier, but does not return a boolean.
    // This is done because we don't know the return type of the deleter outside of the IDL generator.
    NotRelevant,
    DidNotFail,
    DidFail,
};

// The realm passed to these helpers is the preferred realm for wrapper allocation.
// Wrapper identity is keyed by the caller's WrapperWorld.
WEB_API GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<Wrappable>);
WEB_API GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable>);

#ifndef WEB_WRAPPABLE
#    define WEB_WRAPPABLE(class_, base_class)                           \
        GC_CELL(class_, base_class)                                     \
    protected:                                                          \
        virtual GC::Ref<Bindings::PlatformObject> create_wrapper(       \
            JS::Realm& wrapper_realm) override;                         \
                                                                        \
    public:                                                             \
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
        }
#endif

#ifndef WEB_NON_IDL_WRAPPABLE
#    define WEB_NON_IDL_WRAPPABLE(class_, base_class) GC_CELL(class_, base_class)
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

    // Wrappers forward legacy platform object indexed and named property hooks here.
    virtual Optional<JS::Value> item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const;
    virtual JS::Value named_item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, FlyString const& name) const;
    virtual Vector<FlyString> supported_property_names() const;
    virtual bool is_supported_property_name(FlyString const& name) const;
    virtual JS::Realm& wrapper_realm(WrapperWorld const&, JS::Realm& preferred_realm) const;
    virtual WebIDL::ExceptionOr<void> set_value_of_new_named_property(JS::Realm&, String const&, JS::Value);
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_named_property(JS::Realm&, String const&, JS::Value);
    virtual WebIDL::ExceptionOr<void> set_value_of_named_property(JS::Realm&, String const&, JS::Value);
    virtual WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(JS::Realm&, u32, JS::Value);
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_indexed_property(JS::Realm&, u32, JS::Value);
    virtual WebIDL::ExceptionOr<void> set_value_of_indexed_property(JS::Realm&, u32, JS::Value);
    virtual WebIDL::ExceptionOr<NamedPropertyDeletionResult> delete_value(JS::Realm&, String const&);

protected:
    Wrappable();

    [[nodiscard]] GC::Ptr<PlatformObject> cached_main_world_wrapper() const { return m_main_world_wrapper.ptr(); }

    virtual GC::Ref<PlatformObject> create_wrapper(JS::Realm& wrapper_realm) = 0;
    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    friend class WrapperWorld;
    friend GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<Wrappable>);
    friend GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable>);

    void set_cached_main_world_wrapper(PlatformObject&);
    void clear_cached_main_world_wrapper(PlatformObject const&);

    // Wrappables are implementation objects, so they do not get a
    // realm-specific initialization hook. They still upcall JS::Cell while they
    // are allocated on the GC heap.
    virtual void initialize(JS::Realm&) final override;

    GC::Weak<PlatformObject> m_main_world_wrapper;
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
