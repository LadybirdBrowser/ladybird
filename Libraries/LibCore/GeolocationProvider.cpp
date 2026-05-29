/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/GeolocationProviderImplementation.h>

namespace Core {

static GeolocationProvider::CreateProvider s_create_provider = create_platform_geolocation_provider;
static GeolocationProvider::IsAvailable s_is_available = platform_geolocation_provider_is_available;

bool GeolocationProvider::is_available()
{
    return s_is_available();
}

ErrorOr<NonnullOwnPtr<GeolocationProvider>> GeolocationProvider::create()
{
    return s_create_provider();
}

void GeolocationProvider::set_provider_functions(CreateProvider create_provider, IsAvailable is_available)
{
    s_create_provider = create_provider;
    s_is_available = is_available;
}

}
