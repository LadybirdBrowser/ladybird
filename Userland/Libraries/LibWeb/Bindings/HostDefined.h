/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/TypeCasts.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

struct HostDefined : public JS::Realm::HostDefined {
    HostDefined(GC::Ref<HTML::EnvironmentSettingsObject> eso, GC::Ref<Intrinsics> intrinsics, GC::Ref<Page> page)
        : environment_settings_object(eso)
        , intrinsics(intrinsics)
        , page(page)
    {
    }
    virtual ~HostDefined() override = default;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    GC::Ref<HTML::EnvironmentSettingsObject> environment_settings_object;
    GC::Ref<Intrinsics> intrinsics;
    GC::Ref<Page> page;
};

[[nodiscard]] inline HTML::EnvironmentSettingsObject& host_defined_environment_settings_object(JS::Realm& realm)
{
    return *verify_cast<HostDefined>(realm.host_defined())->environment_settings_object;
}

[[nodiscard]] inline Page& host_defined_page(JS::Realm& realm)
{
    return *verify_cast<HostDefined>(realm.host_defined())->page;
}

}
