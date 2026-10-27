/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <UI/Qt/SystemAppearance.h>

#include <QAccessibilityHints>
#include <QApplication>
#include <QStyleHints>
#include <QWidget>

namespace Ladybird {

SystemAppearance& SystemAppearance::the()
{
    static auto* appearance = new SystemAppearance;
    return *appearance;
}

SystemAppearance::SystemAppearance()
{
    m_prefers_dark = query_system_prefers_dark();

    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        update();
    });
    // The chrome's colors depend on the contrast preference, but changing it does not change the application palette.
    // Widgets restyle themselves on palette changes, so tell them about one.
    QObject::connect(QGuiApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this, [] {
        QEvent application_event(QEvent::ApplicationPaletteChange);
        QCoreApplication::sendEvent(qApp, &application_event);
        for (auto* widget : QApplication::allWidgets()) {
            QEvent event(QEvent::PaletteChange);
            QCoreApplication::sendEvent(widget, &event);
        }
    });
    observe_platform_changes();
}

void SystemAppearance::update()
{
    auto prefers_dark = query_system_prefers_dark();
    if (prefers_dark == m_prefers_dark)
        return;

    m_prefers_dark = prefers_dark;
    emit changed();
}

void SystemAppearance::apply_interface_theme(WebView::ThemePreference theme)
{
    if (theme == m_interface_theme)
        return;
    m_interface_theme = theme;

    switch (theme) {
    case WebView::ThemePreference::System:
        QGuiApplication::styleHints()->unsetColorScheme();
        break;
    case WebView::ThemePreference::Light:
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        break;
    case WebView::ThemePreference::Dark:
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        break;
    }
}

#if !defined(AK_OS_MACOS)
bool SystemAppearance::query_system_prefers_dark() const
{
    // FIXME: Ask the platform directly. While the interface theme overrides Qt's color scheme, Qt reports the override,
    //        so we keep the appearance we last saw from the system.
    if (m_interface_theme != WebView::ThemePreference::System)
        return m_prefers_dark;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void SystemAppearance::observe_platform_changes()
{
}

bool SystemAppearance::prefers_reduced_motion()
{
    // FIXME: Ask the platform.
    return false;
}
#endif

}
