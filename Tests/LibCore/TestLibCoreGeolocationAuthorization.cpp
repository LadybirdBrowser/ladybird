/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/GeolocationAuthorization.h>
#include <LibTest/TestCase.h>

using namespace Core;

TEST_CASE(not_determined_keeps_requests_waiting)
{
    EXPECT(action_for_authorization(GeolocationAuthorization::NotDetermined) == GeolocationAuthorizationAction::Wait);
}

TEST_CASE(denied_fails_requests)
{
    EXPECT(action_for_authorization(GeolocationAuthorization::Denied) == GeolocationAuthorizationAction::Fail);
}

TEST_CASE(authorized_proceeds)
{
    EXPECT(action_for_authorization(GeolocationAuthorization::Authorized) == GeolocationAuthorizationAction::Proceed);
}

TEST_CASE(unknown_proceeds)
{
    EXPECT(action_for_authorization(GeolocationAuthorization::Unknown) == GeolocationAuthorizationAction::Proceed);
}
