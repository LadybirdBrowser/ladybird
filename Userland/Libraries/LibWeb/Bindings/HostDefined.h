/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/TypeCasts.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

struct HostDefined : public JS::Realm::HostDefined {
    HostDefined(JS::NonnullGCPtr<HTML::EnvironmentSettingsObject> eso, JS::NonnullGCPtr<Intrinsics> intrinsics, JS::NonnullGCPtr<Page> page)
        : environment_settings_object(eso)
        , intrinsics(intrinsics)
        , page(page)
    {
    }
    virtual ~HostDefined() override = default;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    JS::NonnullGCPtr<HTML::EnvironmentSettingsObject> environment_settings_object;
    JS::NonnullGCPtr<Intrinsics> intrinsics;
    JS::NonnullGCPtr<Page> page;
};

[[nodiscard]] inline HTML::EnvironmentSettingsObject& host_defined_environment_settings_object(JS::Realm& realm)
{
    return *verify_cast<HostDefined>(realm.host_defined())->environment_settings_object;
}

[[nodiscard]] inline HTML::EnvironmentSettingsObject const& host_defined_environment_settings_object(JS::Realm const& realm)
{
    return *verify_cast<HostDefined>(realm.host_defined())->environment_settings_object;
}

[[nodiscard]] inline Page& host_defined_page(JS::Realm& realm)
{
    return *verify_cast<HostDefined>(realm.host_defined())->page;
}

}
