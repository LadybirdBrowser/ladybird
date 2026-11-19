/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ExternalURLActivationToken.h>

#if defined(LADYBIRD_HAS_WAYLAND_ACTIVATION)
#    include <AK/ByteString.h>
#    include <AK/kmalloc.h>
#    include <UI/Qt/WaylandRegistry.h>
#    include <UI/Qt/WaylandWindow.h>

#    include <QGuiApplication>
#    include <QTimer>
#    include <QWindow>

// The generated listener functions cast the const away from the listeners they pass to libwayland.
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-qual"
#    include <xdg-activation-v1-client-protocol.h>
#    pragma GCC diagnostic pop
#endif

namespace Ladybird {

#if defined(LADYBIRD_HAS_WAYLAND_ACTIVATION)
class ActivationTokenRequest final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    ActivationTokenRequest(QGuiApplication& application, xdg_activation_token_v1& token, Function<void(Optional<String>)> callback)
        : QObject(&application)
        , m_token(&token)
        , m_callback(move(callback))
    {
        static xdg_activation_token_v1_listener const listener {
            .done = [](void* data, xdg_activation_token_v1*, char const* token) {
                auto string = String::from_utf8({ token, strlen(token) });
                static_cast<ActivationTokenRequest*>(data)->complete(string.is_error() ? Optional<String> {} : string.release_value());
            },
        };

        xdg_activation_token_v1_add_listener(m_token, &listener, this);

        QTimer::singleShot(1000, this, [this] { complete({}); });

        QObject::connect(&application, &QCoreApplication::aboutToQuit, this, [this] {
            m_callback = nullptr;
            complete({});
        });
    }

private:
    void complete(Optional<String> token)
    {
        if (!m_token)
            return;

        xdg_activation_token_v1_destroy(exchange(m_token, nullptr));
        deleteLater();

        if (auto callback = move(m_callback))
            callback(move(token));
    }

    xdg_activation_token_v1* m_token { nullptr };
    Function<void(Optional<String>)> m_callback;
};

static xdg_activation_v1* wayland_activation()
{
    static auto* activation = []() -> xdg_activation_v1* {
        auto registry = WaylandRegistry::create();
        if (!registry)
            return nullptr;

        auto* activation = registry->bind<xdg_activation_v1>(xdg_activation_v1_interface, 1);
        if (activation)
            WaylandRegistry::move_to_qt_queue(activation);

        return activation;
    }();

    return activation;
}
#endif

void request_external_url_activation_token([[maybe_unused]] StringView desktop_id, Function<void(Optional<String>)> callback)
{
#if defined(LADYBIRD_HAS_WAYLAND_ACTIVATION)
    auto* application = as_if<QGuiApplication>(QCoreApplication::instance());
    auto* window = application ? QGuiApplication::focusWindow() : nullptr;
    auto* surface = window ? wayland_surface_for_window(*window) : nullptr;

    if (surface) {
        auto* wayland = application->nativeInterface<QNativeInterface::QWaylandApplication>();

        if (auto* activation = wayland_activation()) {
            auto* token = xdg_activation_v1_get_activation_token(activation);
            xdg_activation_token_v1_set_surface(token, surface);

            auto serial = wayland->lastInputSerial();
            if (auto* seat = wayland->lastInputSeat(); seat && serial)
                xdg_activation_token_v1_set_serial(token, serial, seat);

            static constexpr auto desktop_suffix = ".desktop"sv;
            if (desktop_id.ends_with(desktop_suffix) && desktop_id.length() > desktop_suffix.length()) {
                auto app_id = desktop_id.substring_view(0, desktop_id.length() - desktop_suffix.length()).to_byte_string();
                xdg_activation_token_v1_set_app_id(token, app_id.characters());
            }

            new ActivationTokenRequest(*application, *token, move(callback));
            xdg_activation_token_v1_commit(token);
            return;
        }
    }
#endif

    callback({});
}

}
