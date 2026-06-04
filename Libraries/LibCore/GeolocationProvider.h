/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibCore/Export.h>

namespace Core {

struct GeolocationCoordinates {
    double latitude { 0.0 };
    double longitude { 0.0 };
    double accuracy { 0.0 };
    double altitude { 0.0 };
    double altitude_accuracy { 0.0 };
    double heading { 0.0 };
    double speed { 0.0 };
};

struct GeolocationError {
    enum class Type : u8 {
        PermissionDenied,
        PositionUnavailable,
        Timeout,
    };

    Type type;
    String message;
};

class CORE_API GeolocationProvider {
public:
    using WatchId = u64;
    using SuccessCallback = Function<void(GeolocationCoordinates)>;
    using ErrorCallback = Function<void(GeolocationError)>;
    using IsAvailable = bool (*)();
    using CreateProvider = ErrorOr<NonnullOwnPtr<GeolocationProvider>> (*)();

    static bool is_available();
    static ErrorOr<NonnullOwnPtr<GeolocationProvider>> create();
    static void set_provider_functions(CreateProvider, IsAvailable);

    virtual ~GeolocationProvider() = default;

    virtual void request_current_position(SuccessCallback on_success, ErrorCallback on_error) = 0;
    virtual ErrorOr<WatchId, GeolocationError> start_watching_position(SuccessCallback on_success, ErrorCallback on_error) = 0;
    virtual void stop_watching_position(WatchId) = 0;
};

}
