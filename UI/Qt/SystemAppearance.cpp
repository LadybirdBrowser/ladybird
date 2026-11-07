/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

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

// Widgets restyle themselves on palette changes, so a change to the chrome's colors that leaves the application palette
// as it is tells them about one.
static void notify_widgets_of_palette_change()
{
    QEvent application_event(QEvent::ApplicationPaletteChange);
    QCoreApplication::sendEvent(qApp, &application_event);
    for (auto* widget : QApplication::allWidgets()) {
        QEvent event(QEvent::PaletteChange);
        QCoreApplication::sendEvent(widget, &event);
    }
}

SystemAppearance::SystemAppearance()
{
    m_prefers_dark = query_system_prefers_dark();

    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        update();
        // While the interface theme is Light or Dark, our windows keep their appearance whatever Qt reports.
        if (m_interface_theme == WebView::ThemePreference::System)
            emit interface_appearance_changed();
    });
    // The chrome's colors depend on the contrast preference, but changing it does not change the application palette.
    QObject::connect(QGuiApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this, [] {
        notify_widgets_of_palette_change();
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
    auto was_dark = Theme::is_dark();
    m_interface_theme = theme;
    Theme::set_interface_theme(theme);

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

    // NB: Some platform themes, such as KDE's, ignore requests for a color scheme. Qt then neither changes its color
    //     scheme nor tells anyone, so the chrome learns of the switch from us instead.
    update_interface_appearance(was_dark);
}

void SystemAppearance::update_interface_appearance(bool was_dark)
{
    if (Theme::is_dark() == was_dark)
        return;

    // NB: Qt's color scheme can't tell whether Qt restyles the widgets: it is unchanged where the request was ignored
    //     but the system already had the requested scheme, and elsewhere it only changes once the theme change has
    //     been delivered.
    notify_widgets_of_palette_change();
    emit interface_appearance_changed();
}

void SystemAppearance::apply_color_theme(WebView::ColorTheme theme)
{
    if (theme == m_color_theme)
        return;
    m_color_theme = theme;
    Theme::set_color_theme(theme);
    notify_widgets_of_palette_change();
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
