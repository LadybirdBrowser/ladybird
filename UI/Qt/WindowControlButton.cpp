/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WindowControlButton.h>

#include <AK/Assertions.h>
#include <AK/Vector.h>

#include <QBoxLayout>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QWidget>

#if defined(AK_OS_LINUX)
#    include <AK/ByteString.h>
#    include <AK/Optional.h>
#    include <AK/StringView.h>
#    include <LibCore/Environment.h>
#    include <LibFileSystem/FileSystem.h>
#    include <UI/Qt/BrowserWindow.h>

#    include <QApplication>
#    include <QSettings>
#    include <QStandardPaths>
// NB: GLib uses "signals" as a name, which Qt defines as a keyword.
#    pragma push_macro("signals")
#    undef signals
#    include <gio/gio.h>
#    pragma pop_macro("signals")
#endif

namespace Ladybird {

// A round button is a 24 px circle at the end of its button, with a little room before it.
static constexpr int ROUND_BUTTON_SIZE = 24;
static constexpr int ROUND_BUTTON_LEADING_GAP = 6;
// Square buttons run to the window's edge, but a circle keeps clear of it.
static constexpr int ROUND_BUTTONS_TRAILING_GAP = 8;
// Its glyph is drawn in a 10 px box, with a heavier line than the hairlines of the square buttons.
static constexpr qreal ROUND_BUTTON_GLYPH_SIZE = 10;
static constexpr qreal ROUND_BUTTON_GLYPH_STROKE_WIDTH = 1.5;

static constexpr auto pressed_outside_property = "pressedOutside";

static char const* object_name_for_type(ControlButton type)
{
    switch (type) {
    case ControlButton::Minimize:
    case ControlButton::Maximize:
        return "LadybirdWindowButton";
    case ControlButton::Close:
        return "LadybirdCloseWindowButton";
    }
    VERIFY_NOT_REACHED();
}

WindowControlButton::WindowControlButton(ControlButton type, QString const& tool_tip, QSize icon_size, QSize button_size, QWidget* parent)
    : QToolButton(parent)
    , m_type(type)
{
    setObjectName(object_name_for_type(type));
    setToolTip(tool_tip);
    setIconSize(icon_size);
    setFixedSize(button_size);
    setFocusPolicy(Qt::NoFocus);
    // NB: The window follows the pointer across a button to show its resize cursor over the edge of the window.
    setMouseTracking(true);
    setProperty(pressed_outside_property, false);
}

void WindowControlButton::set_round(bool is_round)
{
    m_is_round = is_round;
    if (m_is_round)
        setFixedWidth(ROUND_BUTTON_SIZE + ROUND_BUTTON_LEADING_GAP);
    update();
}

void WindowControlButton::paintEvent(QPaintEvent* event)
{
    if (!m_is_round) {
        QToolButton::paintEvent(event);
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QRectF circle { static_cast<qreal>(width() - ROUND_BUTTON_SIZE), (height() - ROUND_BUTTON_SIZE) / 2.0, ROUND_BUTTON_SIZE, ROUND_BUTTON_SIZE };
    auto const& chrome = Theme::chrome(*this);
    auto is_hovered = isDown() || underMouse();
    auto is_close_hovered = m_type == ControlButton::Close && is_hovered;
    painter.setPen(Qt::NoPen);
    painter.setBrush(chrome.field);
    painter.drawEllipse(circle);
    if (is_hovered) {
        painter.setBrush(is_close_hovered ? ChromeStyle::chrome_destructive_hover() : isDown() ? Theme::panel().pressed
                                                                                               : chrome.hover);
        painter.drawEllipse(circle);
    }

    QPainterPath glyph;
    switch (m_type) {
    case ControlButton::Minimize:
        glyph.moveTo(1.5, 5);
        glyph.lineTo(8.5, 5);
        break;
    case ControlButton::Maximize:
        glyph.addRoundedRect(QRectF(1.5, 1.5, 7, 7), 1.5, 1.5);
        break;
    case ControlButton::Close:
        glyph.moveTo(1.5, 1.5);
        glyph.lineTo(8.5, 8.5);
        glyph.moveTo(8.5, 1.5);
        glyph.lineTo(1.5, 8.5);
        break;
    }
    auto inactive = ChromeStyle::window_activity(*this) == ChromeStyle::WindowActivity::Inactive;
    auto glyph_color = inactive ? chrome.muted : chrome.icon;
    if (is_close_hovered)
        glyph_color = ChromeStyle::chrome_destructive_text();
    painter.translate(circle.center() - QPointF(ROUND_BUTTON_GLYPH_SIZE / 2, ROUND_BUTTON_GLYPH_SIZE / 2));
    painter.setPen(QPen(glyph_color, ROUND_BUTTON_GLYPH_STROKE_WIDTH, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(glyph);
}

struct DesktopWindowButtons {
    bool shows(ControlButton button) const
    {
        switch (button) {
        case ControlButton::Minimize:
            return minimize;
        case ControlButton::Maximize:
            return maximize;
        case ControlButton::Close:
            return close;
        }
        VERIFY_NOT_REACHED();
    }

    bool operator==(DesktopWindowButtons const&) const = default;

    bool minimize { true };
    bool maximize { true };
    bool close { true };
    bool round { true };
    bool on_left { false };
    Vector<ControlButton, 3> order { ControlButton::Minimize, ControlButton::Maximize, ControlButton::Close };
};

#if defined(AK_OS_LINUX)
static constexpr auto WM_PREFERENCES_SCHEMA = "org.gnome.desktop.wm.preferences";
static constexpr auto BUTTON_LAYOUT_KEY = "button-layout";

// https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html
static constexpr auto PORTAL_BUS_NAME = "org.freedesktop.portal.Desktop";
static constexpr auto PORTAL_OBJECT_PATH = "/org/freedesktop/portal/desktop";
static constexpr auto SETTINGS_INTERFACE = "org.freedesktop.portal.Settings";

static bool is_inside_flatpak()
{
    return FileSystem::exists("/.flatpak-info"sv);
}

static DesktopWindowButtons& desktop_window_buttons();

static void set_gnome_button_layout(DesktopWindowButtons& buttons, StringView layout)
{
    // The layout lists the buttons on the left of a colon and those on the right after it, such as "appmenu:close".
    buttons.minimize = layout.contains("minimize"sv);
    buttons.maximize = layout.contains("maximize"sv);
    buttons.close = layout.contains("close"sv);
}

static void apply_portal_button_layout(GVariant* reply)
{
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    g_variant_unref(reply);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        auto* inner_value = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner_value;
    }
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        g_variant_unref(value);
        return;
    }

    auto& buttons = desktop_window_buttons();
    auto previous_buttons = buttons;
    gsize layout_length = 0;
    auto const* layout = g_variant_get_string(value, &layout_length);
    set_gnome_button_layout(buttons, StringView { layout, layout_length });
    g_variant_unref(value);
    if (buttons == previous_buttons)
        return;

    for (auto* widget : QApplication::allWidgets()) {
        if (auto* button = as_if<WindowControlButton>(widget))
            button->setVisible(buttons.shows(button->type()));
    }
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->window_control_buttons_changed();
    }
}

static void call_settings_method(GDBusConnection* connection, char const* method, GAsyncReadyCallback callback)
{
    g_dbus_connection_call(connection, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, SETTINGS_INTERFACE, method,
        g_variant_new("(ss)", WM_PREFERENCES_SCHEMA, BUTTON_LAYOUT_KEY), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
        nullptr, callback, nullptr);
}

static void did_read_portal_button_layout(GObject* source, GAsyncResult* result, gpointer)
{
    if (auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, nullptr))
        apply_portal_button_layout(reply);
}

static void did_read_one_portal_button_layout(GObject* source, GAsyncResult* result, gpointer)
{
    auto* connection = G_DBUS_CONNECTION(source);
    GError* error = nullptr;
    auto* reply = g_dbus_connection_call_finish(connection, result, &error);
    // Portals before version 2 have only Settings.Read, which wraps the value in one more variant.
    if (!reply && g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        call_settings_method(connection, "Read", did_read_portal_button_layout);
    g_clear_error(&error);
    if (reply)
        apply_portal_button_layout(reply);
}

static void read_portal_button_layout()
{
    // Connecting synchronously sends the read while the first window is being built, so its answer is usually in
    // before that window is painted.
    auto* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection)
        return;
    call_settings_method(connection, "ReadOne", did_read_one_portal_button_layout);
    g_object_unref(connection);
}

static Optional<ByteString> read_gsettings_button_layout()
{
    auto* source = g_settings_schema_source_get_default();
    auto* schema = source ? g_settings_schema_source_lookup(source, WM_PREFERENCES_SCHEMA, TRUE) : nullptr;
    if (!schema)
        return {};
    g_settings_schema_unref(schema);

    auto* settings = g_settings_new(WM_PREFERENCES_SCHEMA);
    auto* layout = g_settings_get_string(settings, BUTTON_LAYOUT_KEY);
    ByteString result { layout };
    g_free(layout);
    g_object_unref(settings);
    return result;
}

static Optional<ByteString> read_gtk_decoration_layout()
{
    for (auto const& path : QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, "gtk-3.0/settings.ini")) {
        auto* key_file = g_key_file_new();
        Optional<ByteString> layout;
        if (g_key_file_load_from_file(key_file, path.toUtf8().constData(), G_KEY_FILE_NONE, nullptr)) {
            if (auto* value = g_key_file_get_string(key_file, "Settings", "gtk-decoration-layout", nullptr)) {
                layout = ByteString { value };
                g_free(value);
            }
        }
        g_key_file_unref(key_file);
        if (layout.has_value())
            return layout;
    }
    return {};
}

static Vector<ControlButton, 3> kwin_layout_buttons(QString const& letters)
{
    Vector<ControlButton, 3> buttons;
    for (auto letter : letters) {
        switch (letter.unicode()) {
        case 'I':
            buttons.append(ControlButton::Minimize);
            break;
        case 'A':
            buttons.append(ControlButton::Maximize);
            break;
        case 'X':
            buttons.append(ControlButton::Close);
            break;
        default:
            break;
        }
    }
    return buttons;
}

static Vector<ControlButton, 3> gtk_layout_buttons(StringView names)
{
    Vector<ControlButton, 3> buttons;
    names.for_each_split_view(',', SplitBehavior::Nothing, [&](StringView name) {
        if (name == "minimize"sv) {
            buttons.append(ControlButton::Minimize);
        } else if (name == "maximize"sv) {
            buttons.append(ControlButton::Maximize);
        } else if (name == "close"sv) {
            buttons.append(ControlButton::Close);
        }
    });
    return buttons;
}
#endif

// The window controls follow the desktop's settings; GNOME's buttons are drawn as circles.
static DesktopWindowButtons read_desktop_window_buttons()
{
    DesktopWindowButtons buttons;

#if defined(AK_OS_LINUX)
    auto desktop = Core::Environment::get("XDG_CURRENT_DESKTOP"sv);
    if (!desktop.has_value())
        return buttons;

    if (auto desktops = desktop->split_view(Core::Environment::PATH_SEPARATOR); desktops.contains_slow("KDE"sv)) {
        Vector<ControlButton, 3> left;
        Vector<ControlButton, 3> right;
        if (auto config_file = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, "kwinrc"); !config_file.isEmpty()) {
            QSettings settings(config_file, QSettings::IniFormat);
            settings.beginGroup("org.kde.kdecoration2");
            left = kwin_layout_buttons(settings.value("ButtonsOnLeft", "MS").toString());
            right = kwin_layout_buttons(settings.value("ButtonsOnRight", "HIAX").toString());
        } else if (is_inside_flatpak()) {
            // Plasma copies the KWin layout into the GTK settings, which a Flatpak can read.
            auto layout = read_gtk_decoration_layout();
            if (!layout.has_value())
                return buttons;
            auto colon = layout->find(':');
            left = gtk_layout_buttons(colon.has_value() ? layout->substring_view(0, *colon) : layout->view());
            if (colon.has_value())
                right = gtk_layout_buttons(layout->substring_view(*colon + 1));
        } else {
            return buttons;
        }

        // Keep the controls together on the side containing the close button.
        buttons.on_left = left.contains_slow(ControlButton::Close);
        buttons.order = move(left);
        buttons.order.extend(move(right));

        buttons.minimize = buttons.order.contains_slow(ControlButton::Minimize);
        buttons.maximize = buttons.order.contains_slow(ControlButton::Maximize);
        buttons.close = buttons.order.contains_slow(ControlButton::Close);

        return buttons;
    } else if (!desktops.contains_slow("GNOME"sv) && Core::Environment::get("XDG_SESSION_TYPE"sv) != "wayland"sv) {
        // NB: On Wayland, GTK reads the GNOME layout on other desktops too, so we follow it there as well. On X11, GTK
        //     takes the layout from the desktop's XSettings daemon instead.
        return buttons;
    }

    // Inside a Flatpak, GSettings only sees the defaults of the runtime's own schemas.
    if (is_inside_flatpak()) {
        read_portal_button_layout();
    } else if (auto layout = read_gsettings_button_layout(); layout.has_value()) {
        set_gnome_button_layout(buttons, *layout);
    }
#elif defined(AK_OS_WINDOWS)
    buttons.round = false;
#endif

    return buttons;
}

static DesktopWindowButtons& desktop_window_buttons()
{
    static auto buttons = read_desktop_window_buttons();
    return buttons;
}

bool window_controls_on_left()
{
    return use_native_macos_window_controls() || desktop_window_buttons().on_left;
}

void set_window_control_button_order(QWidget& container, bool on_left)
{
    // Mirror the desktop's button order when moving the controls to the other side.
    auto* layout = static_cast<QBoxLayout*>(container.layout());
    layout->setDirection(on_left != window_controls_on_left() ? QBoxLayout::RightToLeft : QBoxLayout::LeftToRight);
    layout->activate();
}

WindowControlButtons create_window_control_buttons(QWidget& parent, char const* object_name, QSize icon_size, QSize button_size)
{
    auto* container = new QWidget(&parent);
    container->setObjectName(object_name);

    auto* layout = new QHBoxLayout(container);
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* minimize = new WindowControlButton(ControlButton::Minimize, "Minimize", icon_size, button_size, container);
    auto* maximize = new WindowControlButton(ControlButton::Maximize, "Maximize", icon_size, button_size, container);
    auto* close = new WindowControlButton(ControlButton::Close, "Close", icon_size, button_size, container);
    auto const& desktop_buttons = desktop_window_buttons();
    for (auto button : desktop_buttons.order) {
        switch (button) {
        case ControlButton::Minimize:
            layout->addWidget(minimize);
            break;
        case ControlButton::Maximize:
            layout->addWidget(maximize);
            break;
        case ControlButton::Close:
            layout->addWidget(close);
            break;
        }
    }
    for (auto* button : { minimize, maximize, close }) {
        button->setVisible(desktop_buttons.shows(button->type()));
        button->set_round(desktop_buttons.round);
    }
    if (desktop_buttons.round)
        layout->setContentsMargins(0, 0, ROUND_BUTTONS_TRAILING_GAP, 0);

    return { container, minimize, maximize, close };
}

QWidget* create_window_controls_spacer(QWidget& parent, char const* object_name, QSize size)
{
    auto* spacer = new QWidget(&parent);
    spacer->setObjectName(object_name);
    spacer->setFixedSize(size);
    spacer->setAttribute(Qt::WA_TransparentForMouseEvents);
    return spacer;
}

void WindowControlButton::enterEvent(QEnterEvent* event)
{
    QToolButton::enterEvent(event);

    if (!m_tracking_press)
        return;

    setDown(true);
    set_pressed_outside(false);
}

void WindowControlButton::leaveEvent(QEvent* event)
{
    QToolButton::leaveEvent(event);

    if (!m_tracking_press)
        return;

    setDown(false);
    set_pressed_outside(true);
}

void WindowControlButton::mouseMoveEvent(QMouseEvent* event)
{
    QToolButton::mouseMoveEvent(event);

    if (m_tracking_press)
        update_pressed_position(event->position().toPoint());
}

void WindowControlButton::mouseDoubleClickEvent(QMouseEvent* event)
{
    QToolButton::mouseDoubleClickEvent(event);

    reset_transient_state();
}

void WindowControlButton::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_tracking_press = true;
        set_pressed_outside(false);
    }

    QToolButton::mousePressEvent(event);
}

void WindowControlButton::mouseReleaseEvent(QMouseEvent* event)
{
    QToolButton::mouseReleaseEvent(event);

    reset_transient_state();
}

void WindowControlButton::reset_transient_state()
{
    m_tracking_press = false;
    setDown(false);
    set_pressed_outside(false);
}

void WindowControlButton::set_pressed_outside(bool pressed_outside)
{
    if (property(pressed_outside_property).toBool() == pressed_outside)
        return;

    setProperty(pressed_outside_property, pressed_outside);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void WindowControlButton::update_pressed_position(QPoint const& position)
{
    auto is_inside = rect().contains(position);
    setDown(is_inside);
    set_pressed_outside(!is_inside);
}

}
