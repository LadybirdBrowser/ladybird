/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

class WEB_API HostDefined : public JS::Cell {
    GC_CELL(HostDefined, JS::Cell);
    GC_DECLARE_ALLOCATOR(HostDefined);

public:
    enum class PrincipalRealmUnderConstruction {
        No,
        Yes,
    };

    virtual ~HostDefined() override;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_principal_host_defined() const { return false; }

    GC::Ref<Intrinsics> intrinsics;
    GC::Ref<WrapperWorld> wrapper_world;
    // Principal realms point this at themselves via PrincipalHostDefined's construction path. Non-principal realms must
    // point at an already-created principal realm with a PrincipalHostDefined.
    GC::Ref<JS::Realm> principal_realm;
    // Created on demand by WebAssembly::Detail::get_cache(). The realm is the only owner that traces it, so the wrapper
    // maps are visited exactly once per collection no matter how many WebAssembly objects are live.
    RefPtr<WebAssembly::Detail::WebAssemblyCache> wasm_cache;

protected:
    HostDefined(GC::Ref<Intrinsics> intrinsics, GC::Ref<WrapperWorld> wrapper_world, GC::Ref<JS::Realm> principal_realm, PrincipalRealmUnderConstruction principal_realm_under_construction = PrincipalRealmUnderConstruction::No);

    virtual void visit_edges(Cell::Visitor&) override;
};

// LibWeb is the only producer of a realm's [[HostDefined]] field, and it always stores a HostDefined there.
[[nodiscard]] inline HostDefined& host_defined_of(JS::Realm& realm)
{
    return static_cast<HostDefined&>(*realm.host_defined());
}

[[nodiscard]] inline HostDefined const& host_defined_of(JS::Realm const& realm)
{
    return static_cast<HostDefined const&>(*realm.host_defined());
}

}
