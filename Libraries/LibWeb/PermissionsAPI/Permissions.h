/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibWeb/Bindings/PermissionStatus.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::PermissionsAPI {

using PermissionDescriptor = Bindings::PermissionDescriptor;
using PermissionState = Bindings::PermissionState;

bool is_permission_supported(String const&);

PermissionState permission_state(PermissionDescriptor descriptor, Optional<HTML::EnvironmentSettingsObject&> settings = {});

PermissionState get_current_permission_state(String const& name, Optional<HTML::EnvironmentSettingsObject&> settings = {});

PermissionState request_permission(PermissionDescriptor const& descriptor);

class WEB_API Permissions : public Bindings::Wrappable {
    WEB_WRAPPABLE(Permissions, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Permissions);

public:
    static GC::Ref<Permissions> create();

    void query(JS::Realm&, GC::Ref<JS::Object> permission_desc, GC::Ref<WebIDL::Promise>);

private:
    Permissions();
};

void permission_query_algorithm(PermissionDescriptor const&, PermissionStatus&);

}
