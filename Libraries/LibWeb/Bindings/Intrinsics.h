/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <LibGC/Heap.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/VM.h>

#define WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(interface_class, interface_name)                  \
    do {                                                                                                   \
        static auto name = #interface_name##_fly_string;                                                   \
        set_prototype(&Bindings::ensure_web_prototype<Bindings::interface_class##Prototype>(realm, name)); \
    } while (0)

#define WEB_SET_PROTOTYPE_FOR_INTERFACE(interface_name) WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(interface_name, interface_name)

namespace Web::Bindings {

class Intrinsics final : public JS::Cell {
    GC_CELL(Intrinsics, JS::Cell);
    GC_DECLARE_ALLOCATOR(Intrinsics);

public:
    Intrinsics(JS::Realm& realm)
        : m_realm(realm)
    {
    }

    template<typename NamespaceType>
    JS::Object& ensure_web_namespace(FlyString const& namespace_name)
    {
        if (auto it = m_namespaces.find(namespace_name); it != m_namespaces.end())
            return *it->value;

        create_web_namespace<NamespaceType>(*m_realm);
        return *m_namespaces.find(namespace_name)->value;
    }

    template<typename PrototypeType>
    JS::Object& ensure_web_prototype(FlyString const& class_name)
    {
        if (auto it = m_prototypes.find(class_name); it != m_prototypes.end())
            return *it->value;

        create_web_prototype_and_constructor<PrototypeType>(*m_realm);
        return *m_prototypes.find(class_name)->value;
    }

    template<typename PrototypeType>
    JS::NativeFunction& ensure_web_constructor(FlyString const& class_name)
    {
        if (auto it = m_constructors.find(class_name); it != m_constructors.end())
            return *it->value;

        create_web_prototype_and_constructor<PrototypeType>(*m_realm);
        return *m_constructors.find(class_name)->value;
    }

    bool is_exposed(StringView name) const;

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    template<typename NamespaceType>
    void create_web_namespace(JS::Realm& realm);

    template<typename PrototypeType>
    void create_web_prototype_and_constructor(JS::Realm& realm);

    HashMap<FlyString, GC::Ref<JS::Object>> m_namespaces;
    HashMap<FlyString, GC::Ref<JS::Object>> m_prototypes;
    HashMap<FlyString, GC::Ptr<JS::NativeFunction>> m_constructors;
    GC::Ref<JS::Realm> m_realm;
};

Intrinsics& host_defined_intrinsics(JS::Realm& realm);

template<typename T>
[[nodiscard]] JS::Object& ensure_web_namespace(JS::Realm& realm, FlyString const& namespace_name)
{
    return host_defined_intrinsics(realm).ensure_web_namespace<T>(namespace_name);
}

template<typename T>
[[nodiscard]] JS::Object& ensure_web_prototype(JS::Realm& realm, FlyString const& class_name)
{
    return host_defined_intrinsics(realm).ensure_web_prototype<T>(class_name);
}

template<typename T>
[[nodiscard]] JS::NativeFunction& ensure_web_constructor(JS::Realm& realm, FlyString const& class_name)
{
    return host_defined_intrinsics(realm).ensure_web_constructor<T>(class_name);
}

}
