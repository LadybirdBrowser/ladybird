/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/GeolocationProvider.h>

namespace Core {

static GeolocationProvider::CreateProvider s_create_provider;
static GeolocationProvider::IsAvailable s_is_available;

bool GeolocationProvider::is_available()
{
    return s_create_provider && s_is_available && s_is_available();
}

ErrorOr<NonnullOwnPtr<GeolocationProvider>> GeolocationProvider::create()
{
    if (s_create_provider)
        return s_create_provider();
    return Error::from_string_literal("Geolocation is not available for this platform");
}

void GeolocationProvider::set_provider_functions(CreateProvider create_provider, IsAvailable is_available)
{
    s_create_provider = create_provider;
    s_is_available = is_available;
}

}
