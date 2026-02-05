/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Forward.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/SyntheticHostDefined.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(Intrinsics);

void Intrinsics::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_namespaces);
    visitor.visit(m_prototypes);
    visitor.visit(m_constructors);
    visitor.visit(m_realm);
    visitor.visit(m_unforgeable_functions);
}

Intrinsics& host_defined_intrinsics(JS::Realm& realm)
{
    ASSERT(realm.host_defined());
    return static_cast<Bindings::HostDefined&>(*realm.host_defined()).intrinsics;
}

GC::Ref<JS::NativeFunction> Intrinsics::ensure_web_unforgeable_function(
    Utf16FlyString const& interface_name,
    Utf16FlyString const& attribute_name,
    Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> behaviour,
    UnforgeableKey::Type type)
{
    UnforgeableKey key { interface_name, attribute_name, type };
    if (auto it = m_unforgeable_functions.find(key); it != m_unforgeable_functions.end())
        return *it->value;

    auto function = JS::NativeFunction::create(*m_realm, move(behaviour), type == UnforgeableKey::Type::Setter ? 1 : 0, attribute_name, m_realm, type == UnforgeableKey::Type::Setter ? "set"sv : "get"sv);
    m_unforgeable_functions.set(key, *function);
    return *function;
}

}

namespace AK {

unsigned Traits<Web::Bindings::UnforgeableKey>::hash(Web::Bindings::UnforgeableKey const& key)
{
    return pair_int_hash(pair_int_hash(key.attribute_name.hash(), key.interface_name.hash()), to_underlying(key.type));
}

}
