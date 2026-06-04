/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/GeolocationProviderQt.h>

#include <QGeoCoordinate>

namespace Ladybird {

ErrorOr<NonnullOwnPtr<GeolocationProviderQt>> GeolocationProviderQt::create()
{
    auto source = QGeoPositionInfoSource::createDefaultSource(nullptr);
    if (!source)
        return Error::from_string_literal("Qt Positioning does not have an available geolocation source");
    return adopt_own(*new GeolocationProviderQt(*source));
}

GeolocationProviderQt::GeolocationProviderQt(QGeoPositionInfoSource& source)
    : m_source(source)
{
    m_source.setParent(this);
    connect(&m_source, &QGeoPositionInfoSource::positionUpdated, this, &GeolocationProviderQt::did_update_position);
    connect(&m_source, &QGeoPositionInfoSource::errorOccurred, this, &GeolocationProviderQt::did_error);
}

Core::GeolocationCoordinates GeolocationProviderQt::coordinates_from_position(QGeoPositionInfo const& position)
{
    auto coordinate = position.coordinate();

    Core::GeolocationCoordinates coordinates;
    coordinates.latitude = coordinate.latitude();
    coordinates.longitude = coordinate.longitude();
    coordinates.accuracy = position.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)
        ? position.attribute(QGeoPositionInfo::HorizontalAccuracy)
        : 0.0;

    if (coordinate.type() == QGeoCoordinate::Coordinate3D)
        coordinates.altitude = coordinate.altitude();
    if (position.hasAttribute(QGeoPositionInfo::VerticalAccuracy))
        coordinates.altitude_accuracy = position.attribute(QGeoPositionInfo::VerticalAccuracy);
    if (position.hasAttribute(QGeoPositionInfo::GroundSpeed))
        coordinates.speed = position.attribute(QGeoPositionInfo::GroundSpeed);
    if (position.hasAttribute(QGeoPositionInfo::Direction))
        coordinates.heading = position.attribute(QGeoPositionInfo::Direction);

    return coordinates;
}

Core::GeolocationError GeolocationProviderQt::error_from_positioning_error(QGeoPositionInfoSource::Error error)
{
    switch (error) {
    case QGeoPositionInfoSource::AccessError:
        return {
            .type = Core::GeolocationError::Type::PermissionDenied,
            .message = "Location services are not authorized"_string,
        };
    case QGeoPositionInfoSource::UpdateTimeoutError:
        return {
            .type = Core::GeolocationError::Type::Timeout,
            .message = "Location acquisition timed out"_string,
        };
    case QGeoPositionInfoSource::ClosedError:
    case QGeoPositionInfoSource::UnknownSourceError:
    case QGeoPositionInfoSource::NoError:
        return {
            .type = Core::GeolocationError::Type::PositionUnavailable,
            .message = "Location acquisition failed"_string,
        };
    }

    VERIFY_NOT_REACHED();
}

void GeolocationProviderQt::request_current_position(SuccessCallback on_success, ErrorCallback on_error)
{
    m_current_position_request = PositionRequest {
        .on_success = move(on_success),
        .on_error = move(on_error),
    };
    m_source.requestUpdate();
}

ErrorOr<Core::GeolocationProvider::WatchId, Core::GeolocationError> GeolocationProviderQt::start_watching_position(SuccessCallback on_success, ErrorCallback on_error)
{
    auto watch_id = m_next_watch_id++;
    auto should_start_updating_location = m_watchers.is_empty();
    m_watchers.set(watch_id, PositionRequest {
                                 .on_success = move(on_success),
                                 .on_error = move(on_error),
                             });

    if (should_start_updating_location)
        m_source.startUpdates();
    return watch_id;
}

void GeolocationProviderQt::stop_watching_position(WatchId watch_id)
{
    m_watchers.remove(watch_id);
    update_watching_state();
}

void GeolocationProviderQt::did_update_position(QGeoPositionInfo const& position)
{
    if (!position.isValid())
        return;

    auto coordinates = coordinates_from_position(position);

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

void GeolocationProviderQt::did_error(QGeoPositionInfoSource::Error error)
{
    if (error == QGeoPositionInfoSource::NoError)
        return;

    auto geolocation_error = error_from_positioning_error(error);

    if (m_current_position_request.has_value()) {
        auto request = m_current_position_request.release_value();
        request.on_error(geolocation_error);
    }

    if (geolocation_error.type == Core::GeolocationError::Type::PermissionDenied) {
        auto watchers = move(m_watchers);
        m_watchers.clear();
        update_watching_state();

        for (auto const& watcher : watchers)
            watcher.value.on_error(geolocation_error);
        return;
    }

    for (auto watch_id : m_watchers.keys()) {
        auto watcher = m_watchers.get(watch_id);
        if (watcher.has_value())
            watcher->on_error(geolocation_error);
    }
}

void GeolocationProviderQt::update_watching_state()
{
    if (m_watchers.is_empty())
        m_source.stopUpdates();
    else
        m_source.startUpdates();
}

static ErrorOr<NonnullOwnPtr<Core::GeolocationProvider>> create_geolocation_provider_qt()
{
    return GeolocationProviderQt::create();
}

static bool is_qt_geolocation_available()
{
    return !QGeoPositionInfoSource::availableSources().isEmpty();
}

void install_qt_geolocation_provider()
{
    Core::GeolocationProvider::set_provider_functions(create_geolocation_provider_qt, is_qt_geolocation_available);
}

}
