/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWebCommon/Geolocation/GeolocationPositionErrorCode.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-geolocationpositionerror
class GeolocationPositionError : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(GeolocationPositionError, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(GeolocationPositionError);

public:
    using ErrorCode = GeolocationPositionErrorCode;

    ErrorCode code() const { return m_code; }
    Utf16String message() const;

private:
    explicit GeolocationPositionError(ErrorCode);

    ErrorCode m_code { 0 };
};

}
