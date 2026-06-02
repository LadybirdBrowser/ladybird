/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WindowControlButton.h>

#include <AK/Assertions.h>
#include <AK/Platform.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif

#include <QBoxLayout>
#include <QCursor>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QWidget>

namespace Ladybird {

static constexpr auto pressed_outside_property = "pressedOutside";

#if defined(AK_OS_MACOS)
static void update_window_control_group(QWidget* widget)
{
    if (!widget)
        return;

    for (auto* child : widget->children()) {
        if (auto* button = dynamic_cast<WindowControlButton*>(child))
            button->update();
    }
}

static void window_control_group_hover_changed(QWidget* widget)
{
    update_window_control_group(widget);
}

class WindowControlGroupEventFilter final : public QObject {
public:
    using QObject::QObject;

private:
    virtual bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget)
            return false;

        if (event->type() == QEvent::Show || event->type() == QEvent::Move || event->type() == QEvent::Resize)
            install_always_active_window_control_hover_tracking(*widget, window_control_group_hover_changed);
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
            update_window_control_group(widget);

        return false;
    }
};

static bool window_control_group_hovered(QWidget const* widget)
{
    if (!widget)
        return false;

    auto position = widget->mapFromGlobal(QCursor::pos());
    return widget->rect().contains(position);
}
#endif

static QSize default_window_control_button_size(QSize non_macos_size)
{
#if defined(AK_OS_MACOS)
    Q_UNUSED(non_macos_size);
    return { 16, 38 };
#else
    return non_macos_size;
#endif
}

static int default_window_control_button_spacing()
{
#if defined(AK_OS_MACOS)
    return 7;
#else
    return 0;
#endif
}

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

#if defined(AK_OS_MACOS)
    setStyleSheet(QString {
        "min-width: %1px; max-width: %1px; "
        "min-height: %2px; max-height: %2px; "
        "background: transparent; border: 0; padding: 0;" }
            .arg(button_size.width())
            .arg(button_size.height()));
#endif
}

static void add_window_control_buttons_to_layout(QBoxLayout& layout, QToolButton& minimize_button, QToolButton& maximize_button, QToolButton& close_button)
{
#if defined(AK_OS_MACOS)
    layout.addWidget(&close_button);
    layout.addWidget(&minimize_button);
    layout.addWidget(&maximize_button);
#else
    layout.addWidget(&minimize_button);
    layout.addWidget(&maximize_button);
    layout.addWidget(&close_button);
#endif
}

WindowControlButtons create_window_control_buttons(QWidget& parent, char const* object_name, QSize icon_size, QSize non_macos_button_size)
{
    auto* container = new QWidget(&parent);
    container->setObjectName(object_name);

    auto* layout = new QHBoxLayout(container);
    layout->setSpacing(default_window_control_button_spacing());
    layout->setContentsMargins(0, 0, 0, 0);

    auto const button_size = default_window_control_button_size(non_macos_button_size);
    auto* minimize = new WindowControlButton(WindowControlButtonType::Minimize, "Minimize", icon_size, button_size, container);
    auto* maximize = new WindowControlButton(WindowControlButtonType::Maximize, "Maximize", icon_size, button_size, container);
    auto* close = new WindowControlButton(WindowControlButtonType::Close, "Close", icon_size, button_size, container);
    add_window_control_buttons_to_layout(*layout, *minimize, *maximize, *close);

#if defined(AK_OS_MACOS)
    auto* event_filter = new WindowControlGroupEventFilter(container);
    container->installEventFilter(event_filter);
#endif

    return { container, minimize, maximize, close };
}

void WindowControlButton::paintEvent(QPaintEvent* event)
{
#if !defined(AK_OS_MACOS)
    QToolButton::paintEvent(event);
#else
    Q_UNUSED(event);

    static constexpr qreal diameter = 13.0;
    auto bounds = QRectF(0, 0, diameter, diameter);
    bounds.moveCenter(rect().center());

    auto hovered = !property(pressed_outside_property).toBool() && window_control_group_hovered(parentWidget());
    auto active = window() && window()->isActiveWindow();
    auto colorized = active || hovered;
    QColor fill;
    QColor symbol;
    switch (m_type) {
    case WindowControlButtonType::Close:
        fill = colorized ? QColor(255, 95, 86) : QColor(198, 198, 198);
        symbol = QColor(128, 32, 29);
        break;
    case WindowControlButtonType::Minimize:
        fill = colorized ? QColor(255, 189, 46) : QColor(198, 198, 198);
        symbol = QColor(153, 101, 13);
        break;
    case WindowControlButtonType::Maximize:
        fill = colorized ? QColor(39, 201, 63) : QColor(198, 198, 198);
        symbol = QColor(25, 99, 30);
        break;
    }

    if (isDown() && !property(pressed_outside_property).toBool())
        fill = fill.darker(112);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(fill.darker(active ? 118 : 106), 0.5));
    painter.setBrush(fill);
    painter.drawEllipse(bounds);

    if (!hovered)
        return;

    painter.setPen(QPen(symbol, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    auto center = bounds.center();
    switch (m_type) {
    case WindowControlButtonType::Close:
        painter.drawLine(center + QPointF(-2.8, -2.8), center + QPointF(2.8, 2.8));
        painter.drawLine(center + QPointF(2.8, -2.8), center + QPointF(-2.8, 2.8));
        break;
    case WindowControlButtonType::Minimize:
        painter.drawLine(center + QPointF(-3.2, 0), center + QPointF(3.2, 0));
        break;
    case WindowControlButtonType::Maximize:
        painter.drawLine(center + QPointF(-3.0, 0), center + QPointF(3.0, 0));
        painter.drawLine(center + QPointF(0, -3.0), center + QPointF(0, 3.0));
        break;
    }
#endif
}

void WindowControlButton::enterEvent(QEnterEvent* event)
{
    QToolButton::enterEvent(event);

#if defined(AK_OS_MACOS)
    update_window_control_group(parentWidget());
#endif

    if (!m_tracking_press)
        return;

    setDown(true);
    set_pressed_outside(false);
}

void WindowControlButton::leaveEvent(QEvent* event)
{
    QToolButton::leaveEvent(event);

#if defined(AK_OS_MACOS)
    update_window_control_group(parentWidget());
#endif

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
