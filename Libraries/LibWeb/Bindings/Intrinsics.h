/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Heap.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>

namespace Web::Bindings {

template<typename T>
[[nodiscard]] JS::Object& ensure_web_prototype(JS::Realm&, Utf16FlyString const&);

namespace Detail {

template<typename PrototypeType, typename Object>
void set_prototype_for_interface_on(JS::Realm& realm, Object& object_to_initialize, Utf16FlyString const& name)
{
    if constexpr (requires { object_to_initialize.shape(); object_to_initialize.set_prototype(static_cast<JS::Object*>(nullptr)); }) {
        if (!object_to_initialize.shape().prototype())
            object_to_initialize.set_prototype(&Bindings::ensure_web_prototype<PrototypeType>(realm, name));
    }
}

}

}

#define WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME_ON(object, interface_class, interface_name) \
    do {                                                                                             \
        static NeverDestroyed<Utf16FlyString> name { #interface_name##_utf16_fly_string };           \
        auto& object_to_initialize = (object);                                                       \
        Bindings::Detail::set_prototype_for_interface_on<Bindings::interface_class##Prototype>(      \
            realm, object_to_initialize, *name);                                                     \
    } while (0)

#define WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(interface_class, interface_name) \
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME_ON(*this, interface_class, interface_name)

#define WEB_SET_PROTOTYPE_FOR_INTERFACE_ON(object, interface_name) \
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME_ON(object, interface_name, interface_name)

#define WEB_SET_PROTOTYPE_FOR_INTERFACE(interface_name) \
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(interface_name, interface_name)

namespace Web::Bindings {

struct InterfaceObjectMetadata;

struct UnforgeableKey {
    enum class Type {
        Setter,
        Getter,
    };
    Utf16FlyString interface_name;
    Utf16FlyString attribute_name;
    Type type {};

    bool operator==(UnforgeableKey const&) const = default;
};

class WEB_API Intrinsics final : public JS::Cell {
    GC_CELL(Intrinsics, JS::Cell);
    GC_DECLARE_ALLOCATOR(Intrinsics);

public:
    Intrinsics(JS::Realm& realm)
        : m_realm(realm)
    {
    }

    template<typename NamespaceType>
    JS::Object& ensure_web_namespace(Utf16FlyString const& namespace_name)
    {
        if (auto it = m_namespaces.find(namespace_name); it != m_namespaces.end())
            return *it->value;

        create_web_namespace<NamespaceType>(*m_realm);
        return *m_namespaces.find(namespace_name)->value;
    }

    template<typename PrototypeType>
    JS::Object& ensure_web_prototype(Utf16FlyString const& class_name)
    {
        if (auto it = m_prototypes.find(class_name); it != m_prototypes.end())
            return *it->value;

        create_web_prototype_and_constructor<PrototypeType>(*m_realm);
        return *m_prototypes.find(class_name)->value;
    }

    template<typename PrototypeType>
    JS::NativeFunction& ensure_web_constructor(Utf16FlyString const& class_name)
    {
        if (auto it = m_constructors.find(class_name); it != m_constructors.end())
            return *it->value;

        create_web_prototype_and_constructor<PrototypeType>(*m_realm);
        return *m_constructors.find(class_name)->value;
    }

    GC::Ref<JS::NativeFunction> ensure_web_unforgeable_function(
        Utf16FlyString const& interface_name,
        Utf16FlyString const& attribute_name,
        Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> behaviour,
        UnforgeableKey::Type);

    JS::Object& existing_web_prototype(Utf16FlyString const&);

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    template<typename NamespaceType>
    void create_web_namespace(JS::Realm& realm);

    template<typename PrototypeType>
    void create_web_prototype_and_constructor(JS::Realm& realm);
    void create_web_prototype_and_constructor(JS::Realm& realm, InterfaceObjectMetadata const&);
    void create_web_constructor(JS::Realm& realm, InterfaceObjectMetadata const&, JS::Object& prototype);

    HashMap<Utf16FlyString, GC::Ref<JS::Object>> m_namespaces;
    HashMap<Utf16FlyString, GC::Ref<JS::Object>> m_prototypes;
    HashMap<Utf16FlyString, GC::Ptr<JS::NativeFunction>> m_constructors;
    HashMap<UnforgeableKey, GC::Ref<JS::NativeFunction>> m_unforgeable_functions;
    GC::Ref<JS::Realm> m_realm;
};

WEB_API Intrinsics& host_defined_intrinsics(JS::Realm& realm);

template<typename T>
[[nodiscard]] JS::Object& ensure_web_namespace(JS::Realm& realm, Utf16FlyString const& namespace_name)
{
    return host_defined_intrinsics(realm).ensure_web_namespace<T>(namespace_name);
}

template<typename T>
[[nodiscard]] JS::Object& ensure_web_prototype(JS::Realm& realm, Utf16FlyString const& class_name)
{
    return host_defined_intrinsics(realm).ensure_web_prototype<T>(class_name);
}

// https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
// Steps from "internally create a new object implementing the interface"
template<typename PrototypeType>
JS::ThrowCompletionOr<void> set_prototype_from_new_target(JS::Realm& target_realm, JS::Value prototype, Utf16FlyString const& interface_name, JS::Object& object)
{
    // 3.3. If Type(prototype) is not Object, then:
    if (!prototype.is_object()) {
        // 2. Set prototype to the interface prototype object for interface in targetRealm.
        prototype = &ensure_web_prototype<PrototypeType>(target_realm, interface_name);
    }

    // 9. Set instance.[[Prototype]] to prototype.
    VERIFY(prototype.is_object());
    TRY(object.internal_set_prototype_of(&prototype.as_object()));

    // If the installed prototype differs from this wrapper's own-realm interface
    // prototype, the wrapper carries realm-specific identity that must stay stable
    // for the lifetime of the implementation object. This covers a subclass
    // constructor installing a custom prototype (and private elements), and also
    // the cross-realm case where new.target resolves to a different realm than the
    // wrapper's own (e.g. Reflect.construct with a bound function from another
    // realm), so target_realm's default prototype is not the wrapper realm's.
    if (auto* wrapper = as_if<PlatformObject>(&object)) {
        auto& interface_prototype = ensure_web_prototype<PrototypeType>(wrapper->realm(), interface_name);
        if (&prototype.as_object() != &interface_prototype) {
            if (auto* wrappable = wrappable_impl_from(wrapper))
                preserve_wrapper(*wrappable, *wrapper);
        }
    }
    return {};
}

template<typename PrototypeType>
JS::ThrowCompletionOr<void> set_prototype_from_new_target(JS::VM& vm, JS::FunctionObject& new_target, Utf16FlyString const& interface_name, JS::Object& object)
{
    // 3.2. Let prototype be ? Get(newTarget, "prototype").
    auto prototype = TRY(new_target.get(vm.names.prototype));

    JS::Realm* realm_for_default_prototype = nullptr;

    // 3.3. If Type(prototype) is not Object, then:
    if (!prototype.is_object()) {
        // 1. Let targetRealm be ? GetFunctionRealm(newTarget).
        realm_for_default_prototype = TRY(JS::get_function_realm(vm, new_target));
        VERIFY(realm_for_default_prototype);
    } else {
        // The shared helper only needs a realm when it must recover the default
        // interface prototype. Passing the explicit prototype's realm keeps the
        // call equivalent without another overload.
        realm_for_default_prototype = &prototype.as_object().shape().realm();
    }

    return set_prototype_from_new_target<PrototypeType>(*realm_for_default_prototype, prototype, interface_name, object);
}

template<typename T>
[[nodiscard]] JS::NativeFunction& ensure_web_constructor(JS::Realm& realm, Utf16FlyString const& class_name)
{
    return host_defined_intrinsics(realm).ensure_web_constructor<T>(class_name);
}

}

namespace AK {

template<>
struct Traits<Web::Bindings::UnforgeableKey> : public DefaultTraits<Web::Bindings::UnforgeableKey> {
    static unsigned hash(Web::Bindings::UnforgeableKey const&);
};

} // namespace AK
