/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/Platform.h>
#include <AK/kmalloc.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

#include <QAccessibilityHints>
#include <QApplication>
#include <QStyleHints>
#include <QWidget>

#if defined(AK_OS_LINUX)
// NB: Qt's signals macro clashes with a member of the same name in GLib's D-Bus headers.
#    pragma push_macro("signals")
#    undef signals
#    include <gio/gio.h>
#    pragma pop_macro("signals")
#endif

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
        if (m_interface_theme == WebView::ThemePreference::System) {
            // NB: The chrome takes its appearance from Qt's color scheme, but Qt can deliver the new palette before it
            //     reports the new color scheme, so the chrome restyles again now.
            notify_widgets_of_palette_change();
            emit interface_appearance_changed();
        }
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
#    if defined(AK_OS_LINUX)
// The color scheme the settings portal reports. Unlike Qt's, it stays the system's while the interface theme overrides
// Qt's color scheme.
static Optional<bool> s_portal_prefers_dark;
#    endif

bool SystemAppearance::query_system_prefers_dark() const
{
#    if defined(AK_OS_LINUX)
    if (s_portal_prefers_dark.has_value())
        return *s_portal_prefers_dark;
#    endif
    // FIXME: Ask the platform directly where there is no settings portal. While the interface theme overrides Qt's
    //        color scheme, Qt reports the override, so we keep the appearance we last saw from the system.
    if (m_interface_theme != WebView::ThemePreference::System)
        return m_prefers_dark;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

#    if defined(AK_OS_LINUX)
// https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html
static constexpr auto PORTAL_BUS_NAME = "org.freedesktop.portal.Desktop";
static constexpr auto PORTAL_OBJECT_PATH = "/org/freedesktop/portal/desktop";
static constexpr auto SETTINGS_INTERFACE = "org.freedesktop.portal.Settings";
static constexpr auto APPEARANCE_NAMESPACE = "org.freedesktop.appearance";
static constexpr auto COLOR_SCHEME_KEY = "color-scheme"sv;
static constexpr auto ACCENT_COLOR_KEY = "accent-color"sv;

// The accent color is three sRGB components from 0 to 1. Any component out of that range means that there is none.
static Optional<QColor> accent_color_from_variant(GVariant* value)
{
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(ddd)")))
        return {};
    double red = 0;
    double green = 0;
    double blue = 0;
    g_variant_get(value, "(ddd)", &red, &green, &blue);
    for (auto component : { red, green, blue }) {
        if (component < 0 || component > 1)
            return {};
    }
    return QColor::fromRgbF(red, green, blue);
}

// The color scheme is 1 for dark and 2 for light. 0 means no preference, which portals also report on desktops that
// have no such setting, so Qt's color scheme decides then.
static Optional<bool> prefers_dark_from_variant(GVariant* value)
{
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
        return {};
    switch (g_variant_get_uint32(value)) {
    case 1:
        return true;
    case 2:
        return false;
    default:
        return {};
    }
}

void SystemAppearance::apply_portal_setting(StringView key, GVariant* value)
{
    if (key == COLOR_SCHEME_KEY) {
        s_portal_prefers_dark = prefers_dark_from_variant(value);
        update();
    } else if (key == ACCENT_COLOR_KEY) {
        auto accent = accent_color_from_variant(value);
        if (accent == Theme::desktop_accent())
            return;
        Theme::set_desktop_accent(move(accent));
        notify_widgets_of_palette_change();
    }
}

// NB: Settings.ReadOne is only in version 2 of the portal. Older portals have only Settings.Read, which wraps the
//     value in one more variant.
struct PortalSettingRead {
    AK_ALLOC_WITH_KMALLOC;

    char* key { nullptr };
    bool is_deprecated_read { false };
};

static void read_portal_setting(GDBusConnection*, PortalSettingRead*);

// The settings that changed since we asked for them, whose first read could otherwise undo the change.
static HashTable<ByteString> s_portal_settings_changed;

static void did_read_portal_setting(GObject* source, GAsyncResult* result, gpointer user_data)
{
    auto* read = static_cast<PortalSettingRead*>(user_data);
    auto* connection = G_DBUS_CONNECTION(source);
    auto* reply = g_dbus_connection_call_finish(connection, result, nullptr);
    if (!reply && !read->is_deprecated_read) {
        read->is_deprecated_read = true;
        read_portal_setting(connection, read);
        return;
    }
    if (reply) {
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        if (read->is_deprecated_read && g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
            auto* inner_value = g_variant_get_variant(value);
            g_variant_unref(value);
            value = inner_value;
        }
        StringView key { read->key, strlen(read->key) };
        if (!s_portal_settings_changed.contains(ByteString { key }))
            SystemAppearance::the().apply_portal_setting(key, value);
        g_variant_unref(value);
        g_variant_unref(reply);
    }
    g_free(read->key);
    delete read;
}

static void read_portal_setting(GDBusConnection* connection, PortalSettingRead* read)
{
    g_dbus_connection_call(connection, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, SETTINGS_INTERFACE,
        read->is_deprecated_read ? "Read" : "ReadOne", g_variant_new("(ss)", APPEARANCE_NAMESPACE, read->key),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, did_read_portal_setting, read);
}

static void read_portal_setting(GDBusConnection* connection, StringView key)
{
    read_portal_setting(connection, new PortalSettingRead { .key = g_strndup(key.characters_without_null_termination(), key.length()) });
}

static void portal_setting_changed(GDBusConnection*, char const*, char const*, char const*, char const*, GVariant* parameters, gpointer)
{
    char const* key = nullptr;
    GVariant* value = nullptr;
    g_variant_get(parameters, "(&s&sv)", nullptr, &key, &value);
    StringView key_view { key, strlen(key) };
    s_portal_settings_changed.set(ByteString { key_view });
    SystemAppearance::the().apply_portal_setting(key_view, value);
    g_variant_unref(value);
}

static void did_get_session_bus(GObject*, GAsyncResult* result, gpointer)
{
    // NB: The connection is kept for the lifetime of the process, as the subscription below needs it.
    auto* connection = g_bus_get_finish(result, nullptr);
    if (!connection)
        return;
    g_dbus_connection_signal_subscribe(connection, PORTAL_BUS_NAME, SETTINGS_INTERFACE, "SettingChanged", PORTAL_OBJECT_PATH,
        APPEARANCE_NAMESPACE, G_DBUS_SIGNAL_FLAGS_NONE, portal_setting_changed, nullptr, nullptr);
    read_portal_setting(connection, COLOR_SCHEME_KEY);
    read_portal_setting(connection, ACCENT_COLOR_KEY);
}

// NB: Qt's palette does not follow the accent color of desktops such as GNOME, and Qt's color scheme reports the
//     interface theme's override instead of the system's. The settings portal reports both. Everything here is
//     asynchronous, so a missing portal or session bus never holds up startup.
void SystemAppearance::observe_platform_changes()
{
    g_bus_get(G_BUS_TYPE_SESSION, nullptr, did_get_session_bus, nullptr);
}
#    else
void SystemAppearance::observe_platform_changes()
{
}
#    endif

bool SystemAppearance::prefers_reduced_motion()
{
    // FIXME: Ask the platform.
    return false;
}
#endif

}
