/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;
use crate::permissions::permissions::PermissionName;
use crate::permissions_api::permission_status::PermissionState;
use crate::permissions_api::permissions::permission_state;

// https://notifications.spec.whatwg.org/#permissions-integration
pub(crate) fn get_the_notifications_permission_state(settings: &CppEnvironmentSettingsObject) -> PermissionState {
    // To get the notifications permission state, run these steps:

    // 1. Let permissionState be the result of getting the current permission state with "notifications".
    permission_state(PermissionName::Notifications, settings)

    // NB: Steps 2 and 3, which turn "prompt" into "default", belong to the permission getter: Every other caller wants
    //     the raw state.
}
