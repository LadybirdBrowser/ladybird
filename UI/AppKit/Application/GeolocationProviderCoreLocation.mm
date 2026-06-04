/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibCore/GeolocationProvider.h>

#import <CoreLocation/CoreLocation.h>

namespace Ladybird {

class GeolocationProviderCoreLocation;
void install_core_location_geolocation_provider();

}

@interface GeolocationDelegate : NSObject <CLLocationManagerDelegate>

@property (nonatomic, assign) Ladybird::GeolocationProviderCoreLocation* provider;

@end

namespace Ladybird {

class GeolocationProviderCoreLocation final : public Core::GeolocationProvider {
public:
    static NonnullOwnPtr<GeolocationProviderCoreLocation> create()
    {
        return adopt_own(*new GeolocationProviderCoreLocation());
    }

    virtual ~GeolocationProviderCoreLocation() override
    {
        [m_location_manager stopUpdatingLocation];
        m_location_manager.delegate = nil;
        m_delegate.provider = nullptr;
    }

    virtual void request_current_position(SuccessCallback, ErrorCallback) override;
    virtual ErrorOr<WatchId, Core::GeolocationError> start_watching_position(SuccessCallback, ErrorCallback) override;
    virtual void stop_watching_position(WatchId) override;

    void did_update_locations(NSArray<CLLocation*>*);
    void did_fail_with_error(NSError*);
    void did_change_authorization();

private:
    struct PositionRequest {
        SuccessCallback on_success;
        ErrorCallback on_error;
    };

    static Core::GeolocationCoordinates coordinates_from_location(CLLocation*);
    static Core::GeolocationError error_from_nserror(NSError*);

    GeolocationProviderCoreLocation()
    {
        m_delegate = [[GeolocationDelegate alloc] init];
        m_delegate.provider = this;
        m_location_manager = [[CLLocationManager alloc] init];
        m_location_manager.delegate = m_delegate;
    }

    bool location_access_denied() const
    {
        auto status = m_location_manager.authorizationStatus;
        return status == kCLAuthorizationStatusDenied || status == kCLAuthorizationStatusRestricted;
    }

    void fail_current_position_request(Core::GeolocationError);
    void fail_all_watchers(Core::GeolocationError);
    void update_watching_state();

    CLLocationManager* m_location_manager;
    GeolocationDelegate* m_delegate;
    Optional<PositionRequest> m_current_position_request;
    HashMap<WatchId, PositionRequest> m_watchers;
    WatchId m_next_watch_id { 1 };
};

}

@implementation GeolocationDelegate

- (void)locationManager:(CLLocationManager*) [[maybe_unused]] manager didUpdateLocations:(NSArray<CLLocation*>*)locations
{
    if (!self.provider)
        return;

    self.provider->did_update_locations(locations);
}

- (void)locationManager:(CLLocationManager*) [[maybe_unused]] manager didFailWithError:(NSError*)error
{
    if (!self.provider)
        return;

    self.provider->did_fail_with_error(error);
}

- (void)locationManagerDidChangeAuthorization:(CLLocationManager*) [[maybe_unused]] manager
{
    if (!self.provider)
        return;

    self.provider->did_change_authorization();
}

@end

namespace Ladybird {

Core::GeolocationCoordinates GeolocationProviderCoreLocation::coordinates_from_location(CLLocation* location)
{
    Core::GeolocationCoordinates coordinates;
    coordinates.latitude = location.coordinate.latitude;
    coordinates.longitude = location.coordinate.longitude;
    coordinates.accuracy = location.horizontalAccuracy;

    if (location.verticalAccuracy >= 0) {
        coordinates.altitude = location.altitude;
        coordinates.altitude_accuracy = location.verticalAccuracy;
    }

    if (location.speed >= 0)
        coordinates.speed = location.speed;

    if (location.course >= 0)
        coordinates.heading = location.course;

    return coordinates;
}

Core::GeolocationError GeolocationProviderCoreLocation::error_from_nserror(NSError* error)
{
    auto message = error.localizedDescription.UTF8String
        ? MUST(String::from_utf8(StringView { error.localizedDescription.UTF8String, strlen(error.localizedDescription.UTF8String) }))
        : "Location acquisition failed"_string;

    if (error.code == kCLErrorDenied) {
        return {
            .type = Core::GeolocationError::Type::PermissionDenied,
            .message = move(message),
        };
    }

    return {
        .type = Core::GeolocationError::Type::PositionUnavailable,
        .message = move(message),
    };
}

void GeolocationProviderCoreLocation::request_current_position(SuccessCallback on_success, ErrorCallback on_error)
{
    m_current_position_request = PositionRequest {
        .on_success = move(on_success),
        .on_error = move(on_error),
    };

    if (location_access_denied()) {
        fail_current_position_request({
            .type = Core::GeolocationError::Type::PermissionDenied,
            .message = "Location services are not authorized"_string,
        });
        return;
    }

    // For both NotDetermined and Authorized: call requestLocation directly. On macOS, CoreLocation
    // handles the authorization prompt automatically when needed.
    [m_location_manager requestLocation];
}

ErrorOr<Core::GeolocationProvider::WatchId, Core::GeolocationError> GeolocationProviderCoreLocation::start_watching_position(SuccessCallback on_success, ErrorCallback on_error)
{
    if (location_access_denied()) {
        return Core::GeolocationError {
            .type = Core::GeolocationError::Type::PermissionDenied,
            .message = "Location services are not authorized"_string,
        };
    }

    auto watch_id = m_next_watch_id++;
    auto should_start_updating_location = m_watchers.is_empty();
    m_watchers.set(watch_id, PositionRequest {
                                 .on_success = move(on_success),
                                 .on_error = move(on_error),
                             });

    if (should_start_updating_location)
        [m_location_manager startUpdatingLocation];
    return watch_id;
}

void GeolocationProviderCoreLocation::stop_watching_position(WatchId watch_id)
{
    m_watchers.remove(watch_id);
    update_watching_state();
}

void GeolocationProviderCoreLocation::did_update_locations(NSArray<CLLocation*>* locations)
{
    if (locations.count == 0)
        return;

    auto coordinates = coordinates_from_location(locations.lastObject);

    if (m_current_position_request.has_value()) {
        auto request = m_current_position_request.release_value();
        request.on_success(coordinates);
    }

    for (auto watch_id : m_watchers.keys()) {
        auto watcher = m_watchers.get(watch_id);
        if (watcher.has_value())
            watcher->on_success(coordinates);
    }
}

void GeolocationProviderCoreLocation::did_fail_with_error(NSError* error)
{
    auto geolocation_error = error_from_nserror(error);

    if (m_current_position_request.has_value())
        fail_current_position_request(geolocation_error);

    if (geolocation_error.type == Core::GeolocationError::Type::PermissionDenied) {
        fail_all_watchers(move(geolocation_error));
        return;
    }

    for (auto watch_id : m_watchers.keys()) {
        auto watcher = m_watchers.get(watch_id);
        if (watcher.has_value())
            watcher->on_error(geolocation_error);
    }
}

void GeolocationProviderCoreLocation::did_change_authorization()
{
    if (location_access_denied()) {
        auto error = Core::GeolocationError {
            .type = Core::GeolocationError::Type::PermissionDenied,
            .message = "Location services are not authorized"_string,
        };
        fail_current_position_request(error);
        fail_all_watchers(move(error));
        return;
    }

    if (m_current_position_request.has_value())
        [m_location_manager requestLocation];
    update_watching_state();
}

void GeolocationProviderCoreLocation::fail_current_position_request(Core::GeolocationError error)
{
    if (!m_current_position_request.has_value())
        return;

    auto request = m_current_position_request.release_value();
    request.on_error(move(error));
}

void GeolocationProviderCoreLocation::fail_all_watchers(Core::GeolocationError error)
{
    auto watchers = move(m_watchers);
    m_watchers.clear();
    update_watching_state();

    for (auto const& watcher : watchers)
        watcher.value.on_error(error);
}

void GeolocationProviderCoreLocation::update_watching_state()
{
    if (m_watchers.is_empty())
        [m_location_manager stopUpdatingLocation];
    else
        [m_location_manager startUpdatingLocation];
}

static ErrorOr<NonnullOwnPtr<Core::GeolocationProvider>> create_geolocation_provider_core_location()
{
    return GeolocationProviderCoreLocation::create();
}

static bool is_core_location_geolocation_available()
{
    return true;
}

void install_core_location_geolocation_provider()
{
    Core::GeolocationProvider::set_provider_functions(create_geolocation_provider_core_location, is_core_location_geolocation_available);
}

}
