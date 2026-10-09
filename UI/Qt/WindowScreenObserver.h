/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

#include <QObject>

class QScreen;
class QWidget;
class QWindow;

namespace Ladybird {

// Follows the screen that a window is on, for the device pixel ratio and the display that the views in it present with.
class WindowScreenObserver final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit WindowScreenObserver(QWidget& window);

    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    Optional<u64> display_id() const { return m_display_id; }
    double refresh_rate() const { return m_refresh_rate; }

    Function<void()> on_device_pixel_ratio_change;
    Function<void()> on_display_metadata_change;

private:
    virtual bool eventFilter(QObject*, QEvent*) override;

    bool connect_window_screen_changed_signal();
    void disconnect_window_screen_changed_signal();
    void connect_screen_signals(QScreen*);
    void disconnect_screen_signals(QScreen*);
    void screen_changed(QScreen*);
    void update_device_pixel_ratio();
    void update_display_metadata(Optional<u64> display_id, double refresh_rate);

    QWidget& m_window;
    QScreen* m_current_screen { nullptr };
    QWindow* m_window_screen_changed_signal_window { nullptr };
    Optional<u64> m_display_id;
    double m_device_pixel_ratio { 0 };
    double m_refresh_rate { 60.0 };
};

}
