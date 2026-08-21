/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16FlyString.h>
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

template<typename Implementation, typename Call>
decltype(auto) invoke_first_available(Implementation& implementation, Call&& call)
{
    return call(implementation);
}

template<typename Implementation, typename FirstCall, typename SecondCall, typename... RemainingCalls>
decltype(auto) invoke_first_available(Implementation& implementation, FirstCall&& first_call, SecondCall&& second_call, RemainingCalls&&... remaining_calls)
{
    if constexpr (requires { first_call(implementation); })
        return first_call(implementation);
    else
        return invoke_first_available(implementation, second_call, remaining_calls...);
}

template<typename Implementation, typename Call>
decltype(auto) invoke_first_available_static(Call&& call)
{
    return call.template operator()<Implementation>();
}

template<typename Implementation, typename FirstCall, typename SecondCall, typename... RemainingCalls>
decltype(auto) invoke_first_available_static(FirstCall&& first_call, SecondCall&& second_call, RemainingCalls&&... remaining_calls)
{
    if constexpr (requires { first_call.template operator()<Implementation>(); })
        return first_call.template operator()<Implementation>();
    else
        return invoke_first_available_static<Implementation>(second_call, remaining_calls...);
}

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
WEB_API bool wrapper_is_preserved(PlatformObject const&);

#ifndef WEB_WRAPPABLE
#    define WEB_WRAPPABLE(class_, base_class)                                               \
        GC_CELL(class_, base_class)                                                         \
    public:                                                                                 \
        using Base::initialize;                                                             \
        static constexpr auto s_interface_name = Bindings::InterfaceName::class_;           \
        virtual Bindings::InterfaceName interface_name() const override                     \
        {                                                                                   \
            return s_interface_name;                                                        \
        }                                                                                   \
        virtual bool implements_interface(Bindings::InterfaceName interface) const override \
        {                                                                                   \
            if (interface == s_interface_name)                                              \
                return true;                                                                \
            return Base::implements_interface(interface);                                   \
        }                                                                                   \
        virtual bool implements_interface(                                                  \
            String const& interface) const override                                         \
        {                                                                                   \
            if (interface == #class_)                                                       \
                return true;                                                                \
            return Base::implements_interface(interface);                                   \
        }                                                                                   \
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

    virtual ~Wrappable() override = default;

    [[nodiscard]] virtual Bindings::InterfaceName interface_name() const { VERIFY_NOT_REACHED(); }
    [[nodiscard]] virtual bool implements_interface(Bindings::InterfaceName) const { return false; }
    [[nodiscard]] virtual bool implements_interface(String const&) const { return false; }

    template<typename T>
    requires(IsBaseOf<Wrappable, T> && requires { T::s_interface_name; })
    bool fast_is() const
    {
        return implements_interface(T::s_interface_name);
    }

    template<typename T>
    requires(IsBaseOf<Wrappable, T> && requires { T::s_interface_name; })
    T* fast_as()
    {
        if (!fast_is<T>())
            return nullptr;
        return static_cast<T*>(this);
    }

    template<typename T>
    requires(IsBaseOf<Wrappable, T> && requires { T::s_interface_name; })
    T const* fast_as() const
    {
        if (!fast_is<T>())
            return nullptr;
        return static_cast<T const*>(this);
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin
    // Wrappers forward PlatformObject's extract an origin operation here.
    virtual Optional<URL::Origin> extract_an_origin() const;

    // Wrappers forward legacy platform object enumeration hooks here.
    virtual Vector<Utf16FlyString> supported_property_names() const;
    virtual bool is_supported_property_name(Utf16FlyString const& name) const;

    [[nodiscard]] virtual GC::Ptr<Wrappable> relevant_global_impl() const { return nullptr; }

    static constexpr size_t main_world_wrapper_offset() { return offsetof(Wrappable, m_main_world_wrapper); }

protected:
    Wrappable();

    [[nodiscard]] GC::Ptr<PlatformObject> cached_main_world_wrapper() const;
    [[nodiscard]] GC::Ptr<PlatformObject> cached_main_world_wrapper(WrapperWorld const&) const;

    virtual GC::Ref<PlatformObject> create_wrapper(JS::Realm& wrapper_realm);
    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    friend class GCAllocatedWrappable;
    friend class WrapperWorld;
    friend WEB_API GC::Ref<PlatformObject> create_global_object_wrapper(JS::Realm& wrapper_realm, GC::Ref<Wrappable>);
    friend WEB_API JS::Realm& wrapper_realm_for_wrappable(WrapperWorld const&, JS::Realm&, GC::Ref<Wrappable>);
    friend WEB_API GC::Ref<PlatformObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Wrappable>);
    friend WEB_API JS::ThrowCompletionOr<void> set_prototype_of_cached_main_world_wrapper(Wrappable&, JS::Object&);
    friend WEB_API void preserve_wrapper(Wrappable&, PlatformObject&);
    friend WEB_API bool wrapper_is_preserved(PlatformObject const&);

    void set_cached_main_world_wrapper(PlatformObject&);
    void clear_cached_main_world_wrapper(PlatformObject const&);

    // Main-world preservation intentionally survives document navigation: an
    // adopted node may still be observed through its original wrapper realm,
    // and clearing this edge would make wrapper state depend on bfcache-like
    // resurrection timing. Non-main-world entries are removed by detach().
    GC::Weak<PlatformObject> m_main_world_wrapper;
};

static_assert(sizeof(Wrappable) == 24);

// Current LibWeb implementation objects are GC-owned. Keeping that ownership
// and its preservation flags in a separate base lets classes migrate to other
// ownership models individually.
class WEB_API GCAllocatedWrappable : public Wrappable {
    WEB_NON_IDL_WRAPPABLE(GCAllocatedWrappable, Wrappable);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~GCAllocatedWrappable() override = default;

protected:
    GCAllocatedWrappable() = default;

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

private:
    friend class WrapperWorld;
    friend WEB_API void preserve_wrapper(Wrappable&, PlatformObject&);
    friend WEB_API bool wrapper_is_preserved(PlatformObject const&);

    enum PreservationFlag : u8 {
        MainWorldWrapperIsPreserved = 1 << 0,
        HasNonMainWorldPreservedWrappers = 1 << 1,
    };

    bool main_world_wrapper_is_preserved() const { return m_preservation_flags & MainWorldWrapperIsPreserved; }
    void set_main_world_wrapper_is_preserved(bool value)
    {
        if (value)
            m_preservation_flags |= MainWorldWrapperIsPreserved;
        else
            m_preservation_flags &= ~MainWorldWrapperIsPreserved;
    }
    bool has_non_main_world_preserved_wrappers() const { return m_preservation_flags & HasNonMainWorldPreservedWrappers; }
    void set_has_non_main_world_preserved_wrappers(bool value)
    {
        if (value)
            m_preservation_flags |= HasNonMainWorldPreservedWrappers;
        else
            m_preservation_flags &= ~HasNonMainWorldPreservedWrappers;
    }
    void clear_preserved_wrappers(WrapperWorld const&);

    u8 m_preservation_flags { 0 };
};

static_assert(sizeof(GCAllocatedWrappable) == 32);
// GCAllocatedWrappable's dsize is 25 bytes, allowing derived classes to reuse
// the remaining tail padding where supported by the ABI.

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
