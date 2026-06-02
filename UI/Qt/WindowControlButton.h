/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QPoint>
#include <QSize>
#include <QString>
#include <QToolButton>

class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QWidget;

namespace Ladybird {

enum class WindowControlButtonType {
    Minimize,
    Maximize,
    Close,
};

class WindowControlButton final : public QToolButton {
public:
    WindowControlButton(WindowControlButtonType, QString const& tool_tip, QSize icon_size, QSize button_size, QWidget* parent = nullptr);

private:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;

    void reset_transient_state();
    void set_pressed_outside(bool);
    void update_pressed_position(QPoint const&);

    [[maybe_unused]] WindowControlButtonType m_type {};
    bool m_tracking_press { false };
};

struct WindowControlButtons {
    QWidget* container { nullptr };
    WindowControlButton* minimize { nullptr };
    WindowControlButton* maximize { nullptr };
    WindowControlButton* close { nullptr };
};

WindowControlButtons create_window_control_buttons(QWidget& parent, char const* object_name, QSize icon_size, QSize non_macos_button_size);

}
