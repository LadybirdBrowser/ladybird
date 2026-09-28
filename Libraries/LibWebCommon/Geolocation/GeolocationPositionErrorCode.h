/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebCommon/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationpositionerror-code
enum class GeolocationPositionErrorCode : WebIDL::UnsignedShort {
    PermissionDenied = 1,
    PositionUnavailable = 2,
    Timeout = 3,
};

}
