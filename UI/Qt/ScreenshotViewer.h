/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/kmalloc.h>

#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QWidget>

class QLabel;
class QPushButton;

namespace Ladybird {

class ScreenshotViewer final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    ScreenshotViewer(QImage screenshot, QWidget* parent);

    bool fits_at_actual_size() const;
    bool is_at_actual_size() const { return m_is_at_actual_size; }
    void set_actual_size(bool, Optional<QPointF> anchor = {});
    QRectF image_rect() const;

    void close_viewer();

    Function<void()> on_closed;

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void wheelEvent(QWheelEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;

private:
    QSizeF actual_size() const;
    QRectF available_rect() const;
    QRectF centered_rect(QSizeF) const;
    QPixmap const& pixmap_for(QSizeF);
    void move_image_by(QPointF);
    void update_cursor(QPointF);
    void update_caption();

    QImage m_screenshot;
    QPixmap m_pixmap;
    bool m_is_at_actual_size { false };
    QPointF m_offset;

    Optional<QPointF> m_press_position;
    QPointF m_press_offset;
    bool m_is_dragging { false };

    QPushButton* m_close_button { nullptr };
    QLabel* m_caption { nullptr };
};

}
