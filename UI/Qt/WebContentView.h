/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWebView/ViewImplementation.h>

#include <QMenu>
#include <QTimer>
#include <QUrl>
#include <QWidget>

class QKeyEvent;
class QSinglePointEvent;

namespace Ladybird {

struct WebContentViewInitialState {
    double maximum_frames_per_second { 60.0 };
};

class WebContentView final
    : public QWidget
    , public WebView::ViewImplementation {
    Q_OBJECT
public:
    WebContentView(QWidget* window, RefPtr<WebView::WebContentClient> parent_client = nullptr, size_t page_index = 0, WebContentViewInitialState initial_state = {});
    virtual ~WebContentView() override;

    virtual void paintEvent(QPaintEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void leaveEvent(QEvent* event) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void wheelEvent(QWheelEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void dragEnterEvent(QDragEnterEvent*) override;
    virtual void dragMoveEvent(QDragMoveEvent*) override;
    virtual void dragLeaveEvent(QDragLeaveEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual void keyPressEvent(QKeyEvent* event) override;
    virtual void keyReleaseEvent(QKeyEvent* event) override;
    virtual void inputMethodEvent(QInputMethodEvent*) override;
    virtual QVariant inputMethodQuery(Qt::InputMethodQuery) const override;
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;
    virtual void focusOutEvent(QFocusEvent*) override;
    virtual bool event(QEvent*) override;

    void set_viewport_rect(Gfx::IntRect);
    void set_device_pixel_ratio(double);
    void set_maximum_frames_per_second(double);

    enum class PaletteMode {
        Default,
        Dark,
    };
    void update_palette(PaletteMode = PaletteMode::Default);

    using ViewImplementation::client;

    QPoint map_point_to_global_position(Gfx::IntPoint) const;

public slots:
    void select_dropdown_action();

signals:
    void urls_dropped(QList<QUrl> const&);

private:
    // ^WebView::ViewImplementation
    virtual void initialize_client(CreateNewClient) override;
    virtual void update_zoom() override;
    virtual Web::DevicePixelSize viewport_size() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;

    void update_viewport_size();
    void update_cursor(Gfx::Cursor cursor);

    void enqueue_native_event(Web::MouseEvent::Type, QSinglePointEvent const& event);

    void enqueue_native_event(Web::DragEvent::Type, QDropEvent const& event);
    void finish_handling_drag_event(Web::DragEvent const&);

    void enqueue_native_event(Web::KeyEvent::Type, QKeyEvent const& event);
    void finish_handling_key_event(Web::KeyEvent const&);

    void update_screen_rects();

    bool m_tooltip_override { false };
    Optional<ByteString> m_tooltip_text;
    QTimer m_tooltip_hover_timer;

    Gfx::IntSize m_viewport_size;

    QMenu* m_select_dropdown { nullptr };
};

}
