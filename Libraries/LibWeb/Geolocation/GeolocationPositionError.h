/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationpositionerror
class GeolocationPositionError : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GeolocationPositionError, Bindings::PlatformObject);
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
    GeolocationPositionError(JS::Realm&, ErrorCode);

    virtual void initialize(JS::Realm&) override;

    ErrorCode m_code { 0 };
};

}
