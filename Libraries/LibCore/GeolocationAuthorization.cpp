/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibCore/GeolocationAuthorization.h>

namespace Core {

GeolocationAuthorizationAction action_for_authorization(GeolocationAuthorization authorization)
{
    switch (authorization) {
    case GeolocationAuthorization::NotDetermined:
        // The user hasn't answered the system yet, so a request fails only once they say no.
        return GeolocationAuthorizationAction::Wait;
    case GeolocationAuthorization::Denied:
        return GeolocationAuthorizationAction::Fail;
    case GeolocationAuthorization::Authorized:
    case GeolocationAuthorization::Unknown:
        return GeolocationAuthorizationAction::Proceed;
    }
    VERIFY_NOT_REACHED();
}

}
