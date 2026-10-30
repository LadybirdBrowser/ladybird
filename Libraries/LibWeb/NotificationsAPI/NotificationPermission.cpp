/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/NotificationsAPI/NotificationPermission.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::NotificationsAPI {

// https://notifications.spec.whatwg.org/#permissions-integration
PermissionsAPI::PermissionState get_the_notifications_permission_state(HTML::EnvironmentSettingsObject& settings)
{
    // To get the notifications permission state, run these steps:

    // 1. Let permissionState be the result of getting the current permission state with "notifications".
    return PermissionsAPI::permission_state({ "notifications"_utf16 }, settings);

    // NB: Steps 2 and 3, which turn "prompt" into "default", belong to the permission getter: Every other caller wants
    //     the raw state.
}

}
