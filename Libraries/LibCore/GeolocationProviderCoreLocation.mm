/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Platform.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/GeolocationProviderImplementation.h>

#import <CoreLocation/CoreLocation.h>

#if !__has_feature(objc_arc)
#    error "This file requires ARC"
#endif

namespace Core {

class GeolocationProviderCoreLocation;

}

@interface GeolocationDelegate : NSObject <CLLocationManagerDelegate>

@property (nonatomic, assign) Core::GeolocationProviderCoreLocation* provider;

@end

namespace Core {

class GeolocationProviderCoreLocation final : public GeolocationProvider {
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

    virtual ErrorOr<RequestId, GeolocationError> request_current_position(SuccessCallback, ErrorCallback) override;
    virtual void cancel_current_position_request(RequestId) override;
    virtual ErrorOr<WatchId, GeolocationError> start_watching_position(SuccessCallback, ErrorCallback) override;
    virtual void stop_watching_position(WatchId) override;

    void did_update_locations(NSArray<CLLocation*>*);
    void did_fail_with_error(NSError*);
    void did_change_authorization();

private:
    struct PositionRequest {
        SuccessCallback on_success;
        ErrorCallback on_error;
    };

    static GeolocationCoordinates coordinates_from_location(CLLocation*);
    static GeolocationError error_from_nserror(NSError*);
    static GeolocationError location_access_not_authorized_error();

    GeolocationProviderCoreLocation()
    {
        m_delegate = [[GeolocationDelegate alloc] init];
        m_delegate.provider = this;
        m_location_manager = [[CLLocationManager alloc] init];
        m_location_manager.delegate = m_delegate;
    }

    bool location_access_authorized() const;

    void fail_current_position_requests(GeolocationError);
    void fail_all_watchers(GeolocationError);
    void update_watching_state();

    CLLocationManager* m_location_manager;
    GeolocationDelegate* m_delegate;
    HashMap<RequestId, PositionRequest> m_current_position_requests;
    HashMap<WatchId, PositionRequest> m_watchers;
    RequestId m_next_request_id { 1 };
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

namespace Core {

GeolocationCoordinates GeolocationProviderCoreLocation::coordinates_from_location(CLLocation* location)
{
    GeolocationCoordinates coordinates;
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

GeolocationError GeolocationProviderCoreLocation::error_from_nserror(NSError* error)
{
    auto message = error.localizedDescription.UTF8String
        ? MUST(String::from_utf8(StringView { error.localizedDescription.UTF8String, strlen(error.localizedDescription.UTF8String) }))
        : "Location acquisition failed"_string;

    if (error.code == kCLErrorDenied) {
        return {
            .type = GeolocationError::Type::PermissionDenied,
            .message = move(message),
        };
    }

    return {
        .type = GeolocationError::Type::PositionUnavailable,
        .message = move(message),
    };
}

GeolocationError GeolocationProviderCoreLocation::location_access_not_authorized_error()
{
    return {
        .type = GeolocationError::Type::PermissionDenied,
        .message = "Location services are not authorized"_string,
    };
}

bool GeolocationProviderCoreLocation::location_access_authorized() const
{
    if (![CLLocationManager locationServicesEnabled])
        return false;

    auto authorization_status = m_location_manager.authorizationStatus;
#if defined(AK_OS_IOS)
    return authorization_status == kCLAuthorizationStatusAuthorizedAlways
        || authorization_status == kCLAuthorizationStatusAuthorizedWhenInUse;
#else
    return authorization_status == kCLAuthorizationStatusAuthorizedAlways;
#endif
}

ErrorOr<GeolocationProvider::RequestId, GeolocationError> GeolocationProviderCoreLocation::request_current_position(SuccessCallback on_success, ErrorCallback on_error)
{
    if (![CLLocationManager locationServicesEnabled])
        return location_access_not_authorized_error();

    if (m_location_manager.authorizationStatus != kCLAuthorizationStatusNotDetermined && !location_access_authorized())
        return location_access_not_authorized_error();

    auto request_id = m_next_request_id++;
    auto should_request_location = m_current_position_requests.is_empty();
    m_current_position_requests.set(request_id, PositionRequest { move(on_success), move(on_error) });

    if (m_location_manager.authorizationStatus == kCLAuthorizationStatusNotDetermined)
        [m_location_manager requestWhenInUseAuthorization];
    else if (should_request_location)
        [m_location_manager requestLocation];

    return request_id;
}

void GeolocationProviderCoreLocation::cancel_current_position_request(RequestId request_id)
{
    if (!m_current_position_requests.remove(request_id) || !m_current_position_requests.is_empty())
        return;

    [m_location_manager stopUpdatingLocation];
    update_watching_state();
}

ErrorOr<GeolocationProvider::WatchId, GeolocationError> GeolocationProviderCoreLocation::start_watching_position(SuccessCallback on_success, ErrorCallback on_error)
{
    if (![CLLocationManager locationServicesEnabled])
        return location_access_not_authorized_error();

    auto authorization_status = m_location_manager.authorizationStatus;
    if (authorization_status != kCLAuthorizationStatusNotDetermined && !location_access_authorized())
        return location_access_not_authorized_error();

    auto watch_id = m_next_watch_id++;
    auto should_start_updating_location = m_watchers.is_empty();
    m_watchers.set(watch_id, PositionRequest { move(on_success), move(on_error) });

    if (authorization_status == kCLAuthorizationStatusNotDetermined)
        [m_location_manager requestWhenInUseAuthorization];
    else if (should_start_updating_location)
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

    auto current_position_requests = move(m_current_position_requests);
    for (auto& request : current_position_requests)
        request.value.on_success(coordinates);

    for (auto watch_id : m_watchers.keys()) {
        auto watcher = m_watchers.get(watch_id);
        if (watcher.has_value())
            watcher->on_success(coordinates);
    }
}

void GeolocationProviderCoreLocation::did_fail_with_error(NSError* error)
{
    auto geolocation_error = error_from_nserror(error);

    if (!m_current_position_requests.is_empty())
        fail_current_position_requests(geolocation_error);

    if (geolocation_error.type == GeolocationError::Type::PermissionDenied) {
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
    if (!location_access_authorized()) {
        auto error = location_access_not_authorized_error();
        fail_current_position_requests(error);
        fail_all_watchers(move(error));
        return;
    }

    if (!m_current_position_requests.is_empty())
        [m_location_manager requestLocation];
    update_watching_state();
}

void GeolocationProviderCoreLocation::fail_current_position_requests(GeolocationError error)
{
    if (m_current_position_requests.is_empty())
        return;

    auto current_position_requests = move(m_current_position_requests);

    for (auto& request : current_position_requests)
        request.value.on_error(error);
}

void GeolocationProviderCoreLocation::fail_all_watchers(GeolocationError error)
{
    auto watchers = move(m_watchers);
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

ErrorOr<NonnullOwnPtr<GeolocationProvider>> create_platform_geolocation_provider()
{
    return GeolocationProviderCoreLocation::create();
}

bool platform_geolocation_provider_is_available()
{
    return true;
}

}
