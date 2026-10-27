/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WindowControlButton.h>

#include <AK/Assertions.h>

#include <QBoxLayout>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QWidget>

#if defined(AK_OS_LINUX)
#    include <AK/StringView.h>
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
// Its glyph is drawn in a 10 px box, with a heavier line than the hairlines of the square buttons.
static constexpr qreal ROUND_BUTTON_GLYPH_SIZE = 10;
static constexpr qreal ROUND_BUTTON_GLYPH_STROKE_WIDTH = 1.5;

static constexpr auto pressed_outside_property = "pressedOutside";

static char const* object_name_for_type(WindowControlButtonType type)
{
    switch (type) {
    case WindowControlButtonType::Minimize:
    case WindowControlButtonType::Maximize:
        return "LadybirdWindowButton";
    case WindowControlButtonType::Close:
        return "LadybirdCloseWindowButton";
    }
    VERIFY_NOT_REACHED();
}

WindowControlButton::WindowControlButton(WindowControlButtonType type, QString const& tool_tip, QSize icon_size, QSize button_size, QWidget* parent)
    : QToolButton(parent)
    , m_type(type)
{
    setObjectName(object_name_for_type(type));
    setToolTip(tool_tip);
    setIconSize(icon_size);
    setFixedSize(button_size);
    setFocusPolicy(Qt::NoFocus);
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
    painter.setPen(Qt::NoPen);
    painter.setBrush(chrome.field);
    painter.drawEllipse(circle);
    if (isDown() || underMouse()) {
        painter.setBrush(isDown() ? Theme::panel().pressed : chrome.hover);
        painter.drawEllipse(circle);
    }

    QPainterPath glyph;
    switch (m_type) {
    case WindowControlButtonType::Minimize:
        glyph.moveTo(1.5, 5);
        glyph.lineTo(8.5, 5);
        break;
    case WindowControlButtonType::Maximize:
        glyph.addRoundedRect(QRectF(1.5, 1.5, 7, 7), 1.5, 1.5);
        break;
    case WindowControlButtonType::Close:
        glyph.moveTo(1.5, 1.5);
        glyph.lineTo(8.5, 8.5);
        glyph.moveTo(8.5, 1.5);
        glyph.lineTo(1.5, 8.5);
        break;
    }
    auto inactive = ChromeStyle::window_activity(*this) == ChromeStyle::WindowActivity::Inactive;
    painter.translate(circle.center() - QPointF(ROUND_BUTTON_GLYPH_SIZE / 2, ROUND_BUTTON_GLYPH_SIZE / 2));
    painter.setPen(QPen(inactive ? chrome.muted : chrome.icon, ROUND_BUTTON_GLYPH_STROKE_WIDTH, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(glyph);
}

struct DesktopWindowButtons {
    bool minimize { true };
    bool maximize { true };
    bool close { true };
    bool round { false };
};

// The window controls follow the desktop's: on GNOME, the buttons its settings list, as circles.
// FIXME: Place the window controls on the leading side when the desktop puts them there.
static DesktopWindowButtons desktop_window_buttons()
{
    DesktopWindowButtons buttons;
#if defined(AK_OS_LINUX)
    auto const* desktop = getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || !StringView { desktop, strlen(desktop) }.contains("GNOME"sv))
        return buttons;
    buttons.round = true;

    auto* source = g_settings_schema_source_get_default();
    auto* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.wm.preferences", TRUE) : nullptr;
    if (!schema)
        return buttons;
    g_settings_schema_unref(schema);

    auto* settings = g_settings_new("org.gnome.desktop.wm.preferences");
    auto* layout = g_settings_get_string(settings, "button-layout");
    // The layout lists the buttons on the left of a colon and those on the right after it, such as "appmenu:close".
    auto layout_view = StringView { layout, strlen(layout) };
    buttons.minimize = layout_view.contains("minimize"sv);
    buttons.maximize = layout_view.contains("maximize"sv);
    buttons.close = layout_view.contains("close"sv);
    g_free(layout);
    g_object_unref(settings);
#endif
    return buttons;
}

WindowControlButtons create_window_control_buttons(QWidget& parent, char const* object_name, QSize icon_size, QSize button_size)
{
    auto* container = new QWidget(&parent);
    container->setObjectName(object_name);

    auto* layout = new QHBoxLayout(container);
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* minimize = new WindowControlButton(WindowControlButtonType::Minimize, "Minimize", icon_size, button_size, container);
    auto* maximize = new WindowControlButton(WindowControlButtonType::Maximize, "Maximize", icon_size, button_size, container);
    auto* close = new WindowControlButton(WindowControlButtonType::Close, "Close", icon_size, button_size, container);
    layout->addWidget(minimize);
    layout->addWidget(maximize);
    layout->addWidget(close);

    static auto const desktop_buttons = desktop_window_buttons();
    minimize->setVisible(desktop_buttons.minimize);
    maximize->setVisible(desktop_buttons.maximize);
    close->setVisible(desktop_buttons.close);
    for (auto* button : { minimize, maximize, close })
        button->set_round(desktop_buttons.round);

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
