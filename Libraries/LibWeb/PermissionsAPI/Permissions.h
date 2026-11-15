/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PermissionStatus.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Permissions/Permissions.h>

namespace Web::PermissionsAPI {

using PermissionDescriptor = Bindings::PermissionDescriptor;
using PermissionState = Bindings::PermissionState;

Optional<Web::Permissions::PermissionName> permission_name_from(Utf16View);

PermissionState permission_state(Web::Permissions::PermissionName, Optional<HTML::EnvironmentSettingsObject&> settings = {});

// Asks the user when the state is "prompt"; the callback runs once a state other than "prompt" is known.
using PermissionDecisionCallback = GC::Ref<GC::Function<void(PermissionState)>>;
void request_permission_with_user_prompt(Web::Permissions::PermissionName, HTML::EnvironmentSettingsObject&, PermissionDecisionCallback);

class WEB_API Permissions : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Permissions, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Permissions);

public:
    static GC::Ref<Permissions> create();

    void query(GC::Ref<JS::Object> permission_desc, GC::Ref<WebIDL::Promise>);

private:
    Permissions();
};

void permission_query_algorithm(PermissionDescriptor const&, PermissionStatus&, HTML::EnvironmentSettingsObject&);

}
