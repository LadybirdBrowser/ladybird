/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/GeolocationProvider.h>

namespace Core {

bool platform_geolocation_provider_is_available();
ErrorOr<NonnullOwnPtr<GeolocationProvider>> create_platform_geolocation_provider();

}
