/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <QGuiApplication>
#include <QMetaObject>
#include <QTimer>
#include <UI/Qt/ExternalURLActivationToken.h>
#include <UI/Qt/StringUtils.h>
#include <qpa/qplatformwindow_p.h>

namespace Ladybird {

class ActivationTokenRequest final : public RefCounted<ActivationTokenRequest> {
public:
    explicit ActivationTokenRequest(Function<void(Optional<String>)> callback)
        : m_callback(move(callback))
    {
    }

    void set_connection(QMetaObject::Connection connection)
    {
        m_connection = move(connection);
    }

    void complete(Optional<String> token)
    {
        if (!m_callback)
            return;

        QObject::disconnect(m_connection);
        auto callback = move(m_callback);
        callback(move(token));
    }

private:
    Function<void(Optional<String>)> m_callback;
    QMetaObject::Connection m_connection;
};

void request_external_url_activation_token(Function<void(Optional<String>)> callback)
{
    auto* application = as_if<QGuiApplication>(QCoreApplication::instance());
    if (!application) {
        callback({});
        return;
    }

    auto* window = QGuiApplication::focusWindow();
    auto* wayland_application = application->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto* wayland_window = window
        ? window->nativeInterface<QNativeInterface::Private::QWaylandWindow>()
        : nullptr;
    if (!wayland_application || !wayland_window) {
        callback({});
        return;
    }

    auto request = adopt_ref(*new ActivationTokenRequest(move(callback)));
    request->set_connection(QObject::connect(
        wayland_window,
        &QNativeInterface::Private::QWaylandWindow::xdgActivationTokenCreated,
        application,
        [request](QString const& token) {
            request->complete(ak_string_from_qstring(token));
        },
        Qt::SingleShotConnection));

    wayland_window->requestXdgActivationToken(wayland_application->lastInputSerial());

    QTimer::singleShot(1000, application, [request] {
        request->complete({});
    });
}

}
