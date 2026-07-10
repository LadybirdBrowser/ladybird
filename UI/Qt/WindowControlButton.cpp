/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WindowControlButton.h>

#include <AK/Assertions.h>

#include <QBoxLayout>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QWidget>

namespace Ladybird {

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
{
    setObjectName(object_name_for_type(type));
    setToolTip(tool_tip);
    setIconSize(icon_size);
    setFixedSize(button_size);
    setFocusPolicy(Qt::NoFocus);
    setProperty(pressed_outside_property, false);
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
