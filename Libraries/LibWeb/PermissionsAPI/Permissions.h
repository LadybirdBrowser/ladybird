/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PermissionStatus.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::PermissionsAPI {

bool is_permission_supported(String const&);

Bindings::PermissionState permission_state(Bindings::PermissionDescriptor descriptor, Optional<HTML::EnvironmentSettingsObject&> settings = {});

Bindings::PermissionState get_current_permission_state(String const& name, Optional<HTML::EnvironmentSettingsObject&> settings = {});

class WEB_API Permissions : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Permissions, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Permissions);

public:
    static GC::Ref<Permissions> create(JS::Realm&);

    GC::Ref<Web::WebIDL::Promise> query(JS::Value permission_desc);

private:
    Permissions(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

void permission_query_algorithm(Bindings::PermissionDescriptor const&, PermissionStatus&);

}
