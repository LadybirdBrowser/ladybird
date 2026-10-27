/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

class QPushButton;

namespace Ladybird {

class ScreenshotSelectionOverlay final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr int minimum_selection_size = 8;

    ScreenshotSelectionOverlay(QImage frame, QWidget* parent);

    void select(QRect);
    void use_whole_frame();
    void skip();
    void dismiss();

    Function<void()> on_close;
    Function<void(QImage)> on_finished;

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual bool focusNextPrevChild(bool next) override;
    virtual void changeEvent(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

private:
    void place_instructions();
    QRect selection() const;
    void finish(QImage);
    void update_chrome_style();

    QImage m_frame;
    Optional<QPoint> m_drag_start;
    QPoint m_drag_end;
    QWidget* m_instructions { nullptr };
    Optional<QPoint> m_instructions_position;
    Optional<QPoint> m_instructions_grab_offset;
    Vector<QPushButton*> m_buttons;
};

}
