/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

struct WEB_API HostDefined : public JS::Realm::HostDefined {
    enum class PrincipalRealmUnderConstruction {
        No,
        Yes,
    };

    HostDefined(GC::Ref<Intrinsics> intrinsics, GC::Ref<WrapperWorld> wrapper_world, GC::Ref<JS::Realm> principal_realm, PrincipalRealmUnderConstruction principal_realm_under_construction = PrincipalRealmUnderConstruction::No)
        : intrinsics(intrinsics)
        , wrapper_world(wrapper_world)
        , principal_realm(principal_realm)
    {
        if (principal_realm_under_construction == PrincipalRealmUnderConstruction::No) {
            VERIFY(principal_realm->host_defined());
            VERIFY(principal_realm->host_defined()->is_principal_host_defined());
        }
    }
    virtual ~HostDefined() override = default;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    GC::Ref<Intrinsics> intrinsics;
    GC::Ref<WrapperWorld> wrapper_world;
    // Principal realms point this at themselves via PrincipalHostDefined's
    // construction path. Non-principal realms must point at an already-created
    // principal realm with a PrincipalHostDefined.
    GC::Ref<JS::Realm> principal_realm;
};

}
