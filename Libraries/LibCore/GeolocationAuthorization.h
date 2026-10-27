/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibCore/Export.h>

namespace Core {

// Whether the operating system lets the browser itself find the user's location.
enum class GeolocationAuthorization : u8 {
    // The system hasn't asked the user yet, or is asking right now.
    NotDetermined,
    Authorized,
    Denied,
    // The provider can't tell.
    Unknown,
};

// What a provider does with the requests it holds when it learns the system's authorization.
enum class GeolocationAuthorizationAction : u8 {
    Wait,
    Proceed,
    Fail,
};

CORE_API GeolocationAuthorizationAction action_for_authorization(GeolocationAuthorization);

}
