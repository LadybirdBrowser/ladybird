/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/GeolocationProviderImplementation.h>

namespace Core {

bool platform_geolocation_provider_is_available()
{
    return false;
}

ErrorOr<NonnullOwnPtr<GeolocationProvider>> create_platform_geolocation_provider()
{
    return Error::from_string_literal("Geolocation is not available for this platform");
}

}
