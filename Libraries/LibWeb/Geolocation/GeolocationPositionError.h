/*
 * Copyright (c) 2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#position_error_interface
class GeolocationPositionError final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GeolocationPositionError, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GeolocationPositionError);

public:
    [[nodiscard]] static GC::Ref<GeolocationPositionError> create(JS::Realm&, WebIDL::UnsignedShort code);
    [[nodiscard]] static GC::Ref<GeolocationPositionError> create(JS::Realm&, WebIDL::UnsignedShort code, String message);

    WebIDL::UnsignedShort code() const { return m_code; }
    String message() const { return m_message; }

    // FIXME: Generate these consts from the IDL.
    static WebIDL::UnsignedShort const PERMISSION_DENIED = 1;
    static WebIDL::UnsignedShort const POSITION_UNAVAILABLE = 2;
    static WebIDL::UnsignedShort const TIMEOUT = 3;

private:
    GeolocationPositionError(JS::Realm&, WebIDL::UnsignedShort code, String message);
    virtual ~GeolocationPositionError() override;

    virtual void initialize(JS::Realm&) override;

    WebIDL::UnsignedShort m_code;
    String m_message;
};

}
