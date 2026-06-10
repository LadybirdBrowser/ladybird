/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationpositionerror
class GeolocationPositionError : public Bindings::Wrappable {
    WEB_WRAPPABLE(GeolocationPositionError, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(GeolocationPositionError);

public:
    enum class ErrorCode : WebIDL::UnsignedShort {
        PermissionDenied = 1,
        PositionUnavailable = 2,
        Timeout = 3,
    };

    ErrorCode code() const { return m_code; }
    String message() const;

private:
    explicit GeolocationPositionError(ErrorCode);

    ErrorCode m_code { 0 };
};

}
