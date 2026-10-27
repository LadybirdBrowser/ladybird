/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWebView/BrowsingSession.h>

#include <QToolButton>

class QVariantAnimation;

namespace Ladybird {

// The Ladybird button opens the browser's menu. It shows the logomark in the profile's color and a menu chevron, on a soft
// fill that deepens under the pointer and while the menu is open.
class LadybirdButton final : public QToolButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    LadybirdButton(WebView::IsPrivate, QWidget* parent);

    virtual QSize sizeHint() const override;

    // The part of the button its tile covers, which popovers hang from.
    QRect tile_rect() const;
    void set_tile_vertical_offset(int);

private:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;

    void show_menu();
    void animate_fill(qreal);

    WebView::IsPrivate m_is_private;
    bool m_has_keyboard_focus { false };
    int m_tile_vertical_offset { 0 };
    // How deep the tile's fill is, from 0 at rest to 1 while pressed.
    qreal m_fill { 0 };
    QVariantAnimation* m_fill_animation { nullptr };
};

}
