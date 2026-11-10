/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/StringView.h>
#include <LibWebView/Settings.h>

#include <QObject>

#if defined(AK_OS_LINUX)
typedef struct _GVariant GVariant;
#endif

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
    void apply_color_theme(WebView::ColorTheme);

#if defined(AK_OS_LINUX)
    // Takes in a setting that the desktop's settings portal reported.
    void apply_portal_setting(StringView key, GVariant* value);
#endif

signals:
    void changed();
    // Our windows switched between the light and dark appearance, whether the system or the interface theme switched
    // them.
    void interface_appearance_changed();

private:
    SystemAppearance();

    void update();
    void update_interface_appearance(bool was_dark);
    bool query_system_prefers_dark() const;
    void observe_platform_changes();

    bool m_prefers_dark { false };
    WebView::ThemePreference m_interface_theme { WebView::ThemePreference::System };
    WebView::ColorTheme m_color_theme { WebView::ColorTheme::Neutral };
};

}
