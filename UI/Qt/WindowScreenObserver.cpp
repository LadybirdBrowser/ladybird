/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WindowScreenObserver.h>

#include <QEvent>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#else
#    include <AK/HashMap.h>
#endif

namespace Ladybird {

static bool should_use_screen_signal_for_dpi_changes()
{
    return QGuiApplication::platformName() != "wayland";
}

#if !defined(AK_OS_MACOS)
static Optional<u64> display_id_for_screen(QScreen* screen)
{
    if (!screen)
        return {};

    // Qt does not expose a portable physical display identifier. Away from macOS the compositor only needs a
    // stable per-process grouping key for Qt-backed windows.
    static u64 next_display_id = 1;
    static HashMap<QScreen*, u64> display_ids;
    return display_ids.ensure(screen, [] {
        return next_display_id++;
    });
}
#endif

static Optional<u64> display_id_for_window([[maybe_unused]] QWidget& window, [[maybe_unused]] QScreen* screen)
{
#if defined(AK_OS_MACOS)
    // The compositor drives a CVDisplayLink per display, which needs the CGDirectDisplayID of the window's screen.
    return appkit_display_id_for_window(window);
#else
    return display_id_for_screen(screen);
#endif
}

WindowScreenObserver::WindowScreenObserver(QWidget& window)
    : QObject(&window)
    , m_window(window)
    , m_current_screen(window.screen())
    , m_display_id(display_id_for_window(window, m_current_screen))
    , m_device_pixel_ratio(window.devicePixelRatio())
{
    if (m_current_screen)
        m_refresh_rate = m_current_screen->refreshRate();

    if (should_use_screen_signal_for_dpi_changes()) {
        window.setAttribute(Qt::WA_NativeWindow);
        window.setAttribute(Qt::WA_DontCreateNativeAncestors);
    }
    connect_screen_signals(m_current_screen);
    connect_window_screen_changed_signal();
    window.installEventFilter(this);
}

bool WindowScreenObserver::eventFilter(QObject* object, QEvent* event)
{
    if (object != &m_window)
        return false;

    switch (event->type()) {
    case QEvent::DevicePixelRatioChange:
        update_device_pixel_ratio();
        break;
    case QEvent::WinIdChange:
        connect_window_screen_changed_signal();
        break;
    case QEvent::PlatformSurface:
        switch (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()) {
        case QPlatformSurfaceEvent::SurfaceCreated:
            connect_window_screen_changed_signal();
            break;
        case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
            disconnect_window_screen_changed_signal();
            break;
        }
        break;
    case QEvent::ScreenChangeInternal:
    // The native window exists by the time the window is shown, and knows the screen it settled on.
    case QEvent::Show:
        screen_changed(m_window.screen());
        break;
    default:
        break;
    }
    return false;
}

bool WindowScreenObserver::connect_window_screen_changed_signal()
{
    auto* window = m_window.windowHandle();
    if (!window)
        return false;
    if (m_window_screen_changed_signal_window == window)
        return true;

    disconnect_window_screen_changed_signal();

    m_window_screen_changed_signal_window = window;
    QObject::connect(window, &QWindow::screenChanged, this, [this](QScreen* screen) {
        screen_changed(screen);
    });
    screen_changed(window->screen());
    return true;
}

void WindowScreenObserver::disconnect_window_screen_changed_signal()
{
    if (!m_window_screen_changed_signal_window)
        return;

    QObject::disconnect(m_window_screen_changed_signal_window, &QWindow::screenChanged, this, nullptr);
    m_window_screen_changed_signal_window = nullptr;
}

void WindowScreenObserver::connect_screen_signals(QScreen* screen)
{
    if (!screen)
        return;

    if (should_use_screen_signal_for_dpi_changes()) {
        QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] {
            update_device_pixel_ratio();
        });
    }
    QObject::connect(screen, &QScreen::refreshRateChanged, this, [this](qreal refresh_rate) {
        update_display_metadata(m_display_id, refresh_rate);
    });
}

void WindowScreenObserver::disconnect_screen_signals(QScreen* screen)
{
    if (screen)
        QObject::disconnect(screen, nullptr, this, nullptr);
}

void WindowScreenObserver::screen_changed(QScreen* screen)
{
    if (m_current_screen != screen) {
        disconnect_screen_signals(m_current_screen);
        m_current_screen = screen;
        connect_screen_signals(m_current_screen);
    }

    update_device_pixel_ratio();

    auto refresh_rate = m_current_screen ? m_current_screen->refreshRate() : m_refresh_rate;
    update_display_metadata(display_id_for_window(m_window, m_current_screen), refresh_rate);
}

void WindowScreenObserver::update_device_pixel_ratio()
{
    auto device_pixel_ratio = m_window.devicePixelRatio();
    if (m_device_pixel_ratio == device_pixel_ratio)
        return;

    m_device_pixel_ratio = device_pixel_ratio;
    if (on_device_pixel_ratio_change)
        on_device_pixel_ratio_change();
}

void WindowScreenObserver::update_display_metadata(Optional<u64> display_id, double refresh_rate)
{
    if (m_display_id == display_id && m_refresh_rate == refresh_rate)
        return;

    m_display_id = display_id;
    m_refresh_rate = refresh_rate;
    if (on_display_metadata_change)
        on_display_metadata_change();
}

}
