/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

struct HostDefined : public JS::Realm::HostDefined {
    explicit HostDefined(GC::Ref<Intrinsics> intrinsics)
        : intrinsics(intrinsics)
    {
    }
    virtual ~HostDefined() override = default;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    GC::Ref<Intrinsics> intrinsics;
};

}
