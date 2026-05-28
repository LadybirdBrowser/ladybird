/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibCore/GeolocationProvider.h>
#include <QGeoPositionInfo>
#include <QGeoPositionInfoSource>
#include <QObject>

namespace Ladybird {

class GeolocationProviderQt final
    : public QObject
    , public Core::GeolocationProvider {
    Q_OBJECT

public:
    static ErrorOr<NonnullOwnPtr<GeolocationProviderQt>> create();
    virtual ~GeolocationProviderQt() override = default;

    virtual ErrorOr<RequestId, Core::GeolocationError> request_current_position(SuccessCallback, ErrorCallback) override;
    virtual void cancel_current_position_request(RequestId) override;
    virtual ErrorOr<WatchId, Core::GeolocationError> start_watching_position(SuccessCallback, ErrorCallback) override;
    virtual void stop_watching_position(WatchId) override;

private:
    struct PositionRequest {
        SuccessCallback on_success;
        ErrorCallback on_error;
    };

    explicit GeolocationProviderQt(QGeoPositionInfoSource&);

    static Core::GeolocationCoordinates coordinates_from_position(QGeoPositionInfo const&);
    static Core::GeolocationError error_from_positioning_error(QGeoPositionInfoSource::Error);

    void did_update_position(QGeoPositionInfo const&);
    void did_error(QGeoPositionInfoSource::Error);
    void update_watching_state();

    QGeoPositionInfoSource& m_source;
    HashMap<RequestId, PositionRequest> m_current_position_requests;
    HashMap<WatchId, PositionRequest> m_watchers;
    RequestId m_next_request_id { 1 };
    WatchId m_next_watch_id { 1 };
};

void install_qt_geolocation_provider();

}
