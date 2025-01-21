/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/TypeCasts.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

struct PrincipalHostDefined : public HostDefined {
    PrincipalHostDefined(GC::Ref<HTML::EnvironmentSettingsObject> eso, GC::Ref<Intrinsics> intrinsics, GC::Ref<Page> page)
        : HostDefined(intrinsics)
        , environment_settings_object(eso)
        , page(page)
    {
    }
    virtual ~PrincipalHostDefined() override = default;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    GC::Ref<HTML::EnvironmentSettingsObject> environment_settings_object;
    GC::Ref<Page> page;
};

[[nodiscard]] inline HTML::EnvironmentSettingsObject& principal_host_defined_environment_settings_object(JS::Realm& realm)
{
    return *as<PrincipalHostDefined>(realm.host_defined())->environment_settings_object;
}

[[nodiscard]] inline HTML::EnvironmentSettingsObject const& principal_host_defined_environment_settings_object(JS::Realm const& realm)
{
    return *as<PrincipalHostDefined>(realm.host_defined())->environment_settings_object;
}

[[nodiscard]] inline Page& principal_host_defined_page(JS::Realm& realm)
{
    return *as<PrincipalHostDefined>(realm.host_defined())->page;
}

}
