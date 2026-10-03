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

[[nodiscard]] GC::Ref<HostDefined> create_principal_host_defined(GC::Ref<HTML::EnvironmentSettingsObject>, GC::Ref<Intrinsics>, GC::Ref<Page>);

class WEB_API PrincipalHostDefined final : public HostDefined {
    GC_CELL(PrincipalHostDefined, HostDefined);
    GC_DECLARE_ALLOCATOR(PrincipalHostDefined);

public:
    virtual ~PrincipalHostDefined() override = default;
    virtual bool is_principal_host_defined() const override { return true; }

    GC::Ref<HTML::EnvironmentSettingsObject> environment_settings_object;
    GC::Ref<Page> page;

private:
    PrincipalHostDefined(GC::Ref<HTML::EnvironmentSettingsObject> eso, GC::Ref<Intrinsics> intrinsics, GC::Ref<WrapperWorld> wrapper_world, GC::Ref<Page> page);

    virtual void visit_edges(Cell::Visitor&) override;
};

[[nodiscard]] inline HTML::EnvironmentSettingsObject& principal_host_defined_environment_settings_object(JS::Realm& realm)
{
    VERIFY(realm.host_defined());
    auto& host_defined = host_defined_of(realm);
    if (auto* principal_host_defined = as_if<PrincipalHostDefined>(host_defined))
        return *principal_host_defined->environment_settings_object;

    return principal_host_defined_environment_settings_object(*host_defined.principal_realm);
}

[[nodiscard]] inline HTML::EnvironmentSettingsObject const& principal_host_defined_environment_settings_object(JS::Realm const& realm)
{
    VERIFY(realm.host_defined());
    auto const& host_defined = host_defined_of(realm);
    if (auto const* principal_host_defined = as_if<PrincipalHostDefined>(host_defined))
        return *principal_host_defined->environment_settings_object;

    return principal_host_defined_environment_settings_object(*host_defined.principal_realm);
}

[[nodiscard]] inline Page& principal_host_defined_page(JS::Realm& realm)
{
    VERIFY(realm.host_defined());
    auto& host_defined = host_defined_of(realm);
    if (auto* principal_host_defined = as_if<PrincipalHostDefined>(host_defined))
        return *principal_host_defined->page;

    return principal_host_defined_page(*host_defined.principal_realm);
}

}

template<>
inline bool Web::Bindings::HostDefined::fast_is<Web::Bindings::PrincipalHostDefined>() const { return is_principal_host_defined(); }
