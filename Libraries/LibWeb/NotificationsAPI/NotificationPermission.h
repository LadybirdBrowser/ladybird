/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::NotificationsAPI {

// https://notifications.spec.whatwg.org/#permissions-integration
WEB_API PermissionsAPI::PermissionState get_the_notifications_permission_state(HTML::EnvironmentSettingsObject&);

}
