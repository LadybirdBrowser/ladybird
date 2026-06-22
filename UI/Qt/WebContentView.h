/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWebView/ViewImplementation.h>

#include <QMenu>
#include <QPixmap>
#include <QTimer>
#include <QUrl>

#ifdef AK_OS_MACOS
#    define LADYBIRD_QT_USE_METAL_RHI_WIDGET 1
#    define LADYBIRD_QT_USE_RHI_WIDGET 1
#elif defined(USE_VULKAN_DMABUF_IMAGES) && QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#    define LADYBIRD_QT_USE_VULKAN_WINDOW 1
#endif

#ifdef LADYBIRD_QT_USE_RHI_WIDGET
#    include <QRhiWidget>
#else
#    include <QWidget>
#endif

class QKeyEvent;
class QSinglePointEvent;
class QCursor;

namespace Ladybird {

#ifdef LADYBIRD_QT_USE_RHI_WIDGET
using WebContentViewBase = QRhiWidget;
#else
using WebContentViewBase = QWidget;
#endif

struct WebContentViewInitialState {
    double maximum_frames_per_second { 60.0 };
    Optional<u64> display_id;
};

class WebContentView final
    : public WebContentViewBase
    , public WebView::ViewImplementation {
    Q_OBJECT
public:
    WebContentView(QWidget* window, RefPtr<WebView::WebContentClient> parent_client = nullptr, size_t page_index = 0, WebContentViewInitialState initial_state = {});
    virtual ~WebContentView() override;

#ifndef LADYBIRD_QT_USE_RHI_WIDGET
    virtual void paintEvent(QPaintEvent*) override;
#endif
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
    void set_zoom_level(double);
    void set_maximum_frames_per_second(double);
    void set_display_metadata(Optional<u64> display_id, double maximum_frames_per_second);

    enum class PaletteMode {
        Default,
        Dark,
    };
    void update_palette(PaletteMode = PaletteMode::Default);
    Optional<QPixmap> tab_preview_pixmap(QSize const& maximum_size) const;

    using ViewImplementation::client;

    QPoint map_point_to_global_position(Gfx::IntPoint) const;

public slots:
    void select_dropdown_action();

signals:
    void urls_dropped(QList<QUrl> const&);

    void native_window_pointer_event();

private:
    // ^WebView::ViewImplementation
    virtual void initialize_client(CreateNewClient) override;
    virtual void update_zoom() override;
    virtual Web::DevicePixelSize viewport_size() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;

#ifdef LADYBIRD_QT_USE_RHI_WIDGET
    // ^QRhiWidget
    virtual void initialize(QRhiCommandBuffer*) override;
    virtual void render(QRhiCommandBuffer*) override;
    virtual void releaseResources() override;
#endif

    struct Paintable {
        Gfx::SharedImageBuffer const* shared_image_buffer { nullptr };
        Gfx::IntSize bitmap_size;
    };

    Optional<Paintable> current_paintable() const;

    void update_viewport_size();
    void update_cursor(Gfx::Cursor cursor);
    void apply_web_content_cursor(QCursor const&);
    void schedule_repaint();
    void update_compositor_display_metadata();

    Web::DevicePixelPoint node_picker_position_for(QSinglePointEvent const&) const;

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

    u64 m_last_click_timestamp { 0 };
    QPointF m_last_click_position;
    int m_click_count { 0 };

    QMenu* m_select_dropdown { nullptr };

#ifdef AK_OS_MACOS
    bool prepare_metal_renderer(unsigned long render_target_pixel_format);
    bool update_imported_iosurface_texture(Gfx::SharedImageBuffer const&);
    void release_metal_resources();
    void release_imported_iosurface_texture();

    void* m_metal_device { nullptr };
    void* m_metal_library { nullptr };
    void* m_metal_pipeline_state { nullptr };
    void* m_metal_sampler_state { nullptr };
    void* m_imported_iosurface_texture { nullptr };
    Gfx::SharedImageBuffer const* m_imported_shared_image_buffer { nullptr };
    unsigned long m_render_target_pixel_format { 0 };
#endif

#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    struct VulkanRenderer;
    struct VulkanWindow;
    struct VulkanWindowRenderer;
    friend struct VulkanWindow;
    friend struct VulkanWindowRenderer;

    void create_vulkan_window();
    void destroy_vulkan_window();
    bool current_paintable_can_use_vulkan_window() const;
    void schedule_vulkan_window_update();
    void update_vulkan_window_geometry();
    void set_vulkan_window_cursor(QCursor const&);
    bool handle_vulkan_window_event(QEvent*);

    VulkanWindow* m_vulkan_window { nullptr };
    QWidget* m_vulkan_window_container { nullptr };
#endif
};

}
