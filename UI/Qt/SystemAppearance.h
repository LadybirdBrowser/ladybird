/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Settings.h>

#include <QObject>

namespace Ladybird {

// The light or dark appearance chosen in the system's settings. The interface theme setting overrides the appearance
// of our own windows, so this tracks the system separately: websites follow it while their color scheme setting is
// left on System.
class SystemAppearance final : public QObject {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    static SystemAppearance& the();

    bool prefers_dark() const { return m_prefers_dark; }
    // Whether the system asks for less motion. The chrome then moves nothing, though it still fades.
    static bool prefers_reduced_motion();

    void apply_interface_theme(WebView::ThemePreference);

signals:
    void changed();

private:
    SystemAppearance();

    void update();
    bool query_system_prefers_dark() const;
    void observe_platform_changes();

    bool m_prefers_dark { false };
    WebView::ThemePreference m_interface_theme { WebView::ThemePreference::System };
};

}
