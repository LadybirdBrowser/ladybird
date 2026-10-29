/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#ifdef AK_OS_MACOS
#    include <UI/Qt/NativeTextEditingBindings.h>
#endif
#include <LibGfx/Cursor.h>
#include <LibGfx/Forward.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibGfx/Rect.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/AccessibilityTreeManager.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebCompatReport.h>
#include <UI/Qt/BackingStoreRelease.h>
#include <UI/Qt/ScreenshotSelectionOverlay.h>

#include <LibWebCommon/Page/QueuedInputEvent.h>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <memory>

#ifdef AK_OS_MACOS
#    define LADYBIRD_QT_USE_IOSURFACE_LAYER 1
#elif defined(USE_DIRECTX)
#    define LADYBIRD_QT_USE_D3D_RHI_WIDGET 1
#    define LADYBIRD_QT_USE_RHI_WIDGET 1
#elif defined(USE_VULKAN_DMABUF_IMAGES)
#    define LADYBIRD_QT_USE_VULKAN_WINDOW 1
#endif

#ifdef LADYBIRD_QT_USE_RHI_WIDGET
#    include <QRhiWidget>
#else
#    include <QWidget>
#endif

#if !defined(Q_OS_WIN)
#    include <QAccessible>
#endif
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
#    include "AccessibilityInterface.h"
#endif

class QKeyEvent;
class QLabel;
class QPushButton;
class QScreen;
class QShortcut;
class QSinglePointEvent;
class QCursor;

#ifdef LADYBIRD_QT_USE_D3D_RHI_WIDGET
struct ID3D11Texture2D;

namespace Gfx {

struct WindowsD3DHandle;

}
#endif

namespace Ladybird {

class WaylandPageSurface;

}

namespace Ladybird {

#ifdef LADYBIRD_QT_USE_RHI_WIDGET
using WebContentViewBase = QRhiWidget;
#else
using WebContentViewBase = QWidget;
#endif

class CrashOverlayUrlLabel;
class CrashReportReviewWidget;
class PageOverlay;
class ScreenshotViewer;
class SelectDropdown;

struct WebCompatReportCapture {
    WebView::WebCompatReport::Page page;
    QImage screenshot;
    NonnullRefPtr<Core::Promise<String>> page_dumps;
};

struct WebContentViewInitialState {
    WebView::IsPrivate is_private { WebView::IsPrivate::No };
    double maximum_frames_per_second { 60.0 };
    Optional<u64> display_id;
    WebView::ViewImplementation const* owner_view { nullptr };
};

class WebContentView final
    : public WebContentViewBase
    , public WebView::ViewImplementation
#if !defined(Q_OS_WIN)
    , public QAccessible::ActivationObserver
#endif
{
    Q_OBJECT
public:
    AK_ALLOC_WITH_KMALLOC;

    WebContentView(QWidget* window, Optional<WebView::CanonicalTraversable&> traversable = {}, WebContentViewInitialState initial_state = {});
    virtual ~WebContentView() override;

#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    // The GPU that the views present on, when the window system says which one it presents on.
    static Optional<Gfx::GpuDeviceIdentity> presentation_gpu_device();
    static Vector<u64> presentation_drm_format_modifiers();
#endif

    // Shows an image over the page, at a position in the view, where the window system can stack it above the page
    // itself. Returns false where it cannot, and Qt has to show the image some other way.
    bool show_image_over_page(QImage const&, QPoint position);
    void hide_image_over_page();

#if !defined(LADYBIRD_QT_USE_RHI_WIDGET)
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
    void update_page_focus();
    virtual bool event(QEvent*) override;

    void set_viewport_rect(Gfx::IntRect);
    void push_viewport_size();
    void set_device_pixel_ratio(double);
    void set_zoom_level(double);
    void set_vertical_tab_overlay_insets(int left, int right);
    void set_bookmarks_bar_overlay_height(int);
    // How much the page is dimmed under a page dialog, from 0 to 1, where the page is presented above anything Qt paints
    // over it.
    void set_page_dim(qreal);
    // A card that floats over the page, such as the find bar. Qt paints it above the page, so whatever presents the
    // page above Qt's painting steps aside while it shows.
    void add_floating_surface(QWidget&);
    void prepare_for_window_move();
    void finish_window_move();
    void close_select_dropdown_after_crash();
    void cancel_mouse_press();
    void review_hidden_crash_report(ByteString const& report_name);
    void leave_report_review();
    void review_web_compat_report(WebCompatReportCapture);
    void select_web_compat_report_screenshot(Function<void(QImage)> on_selected, Function<void()> on_close = {});

    Function<void(WebCompatReportCapture)> on_request_web_compat_report_review;
    Function<void(Function<void(QImage)> on_selected)> on_request_web_compat_report_screenshot;
    Function<void()> on_close_web_compat_report_tab;

    enum class PaletteMode {
        Default,
        Dark,
    };
    void update_palette(PaletteMode = PaletteMode::Default);
    Optional<QPixmap> tab_preview_pixmap(QSize const& maximum_size) const;

    WebView::AccessibilityTreeManager const* accessibility_tree_manager() const { return m_accessibility_manager.ptr(); }

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    QAccessibleInterface* accessibility_interface_for_node(i64 node_id);
#endif

    QPoint map_point_to_global_position(Gfx::IntPoint) const;

    // On macOS, Qt ignores mouse movement while Ladybird is inactive, so a view that should still follow it opts in.
    bool follows_mouse_while_inactive() const { return m_follows_mouse_while_inactive; }
    void set_follows_mouse_while_inactive(bool follows) { m_follows_mouse_while_inactive = follows; }

signals:
    void ready_to_paint();
    void crash_overlay_changed();
    void image_overlay_unavailable();
    void urls_dropped(QList<QUrl> const&);

private:
    // ^WebView::ViewImplementation
    virtual void prepare_page_for_navigable(WebView::WebContentPage&) override;
    virtual Core::AnonymousBuffer system_theme() override;
    virtual Optional<ScreenRects> screen_rects() const override;
    virtual void update_zoom() override;
    virtual Web::DevicePixelSize viewport_size() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;
    virtual void did_accept_presented_backing_store(i32, Gfx::IntRect) override;

#ifdef LADYBIRD_QT_USE_RHI_WIDGET
    // ^QRhiWidget
    virtual void initialize(QRhiCommandBuffer*) override;
    virtual void render(QRhiCommandBuffer*) override;
    virtual void releaseResources() override;
#endif

    struct Paintable {
        Gfx::SharedImageBuffer const* shared_image_buffer { nullptr };
        Gfx::IntSize bitmap_size;
        // The backing store's ID, or -1 for a frame kept from backing stores that are gone.
        i32 bitmap_id { -1 };
        // The page that the backing store belongs to.
        u64 page_id { 0 };
    };

    Optional<Paintable> current_paintable() const;
    // The pixels of a frame, which a frame that lives only in GPU memory has once they are read back from there.
    RefPtr<Gfx::Bitmap const> cpu_bitmap_of(Paintable const&) const;
    // The pixels of a frame for Qt to paint, which there is no need to read back while the page's window covers them.
    RefPtr<Gfx::Bitmap const> cpu_bitmap_to_paint(Paintable const&) const;
    QImage visible_snapshot() const;

    void update_viewport_size();
    void update_cursor(Gfx::Cursor cursor);
    void apply_web_content_cursor(QCursor const&);
    void schedule_repaint();
#ifdef LADYBIRD_QT_USE_RHI_WIDGET
    void schedule_frame_damage_repaint();
#endif
    Web::DevicePixelPoint node_picker_position_for(QSinglePointEvent const&) const;

    // The platform may report a phase of a wheel event's gesture that Qt does not pass on.
    void enqueue_native_event(Web::MouseEvent::Type, QSinglePointEvent const& event, Optional<Web::ScrollGesturePhase> scroll_gesture_phase_the_platform_reports = {});
    void handle_pointer_leave();

    void enqueue_native_event(Web::DragEvent::Type, QDropEvent const& event);
    void finish_handling_drag_event(Web::DragEvent const&);

    void enqueue_native_event(Web::KeyEvent::Type, QKeyEvent const& event);
    void finish_handling_key_event(Web::KeyEvent const&);

    void watch_screen_geometry(QScreen&);

    void set_crash_overlay_visible(bool);
    void show_crash_report_review(Optional<ByteString> const& hidden_crash_report_name = {});

    void start_web_compat_report();
    ScreenshotSelectionOverlay& select_screenshot(QImage const& frame, ScreenshotSelectionOverlay::Purpose, Function<void()> on_close = {});
    void start_screenshot();
    QPointer<QWidget> show_transient_overlay(QWidget&);
    void close_transient_overlay(QWidget&, QPointer<QWidget> const& focus_to_restore);

    Array<QWidget*, 4> overlays() const;
    bool is_page_covered_by_overlay() const;
    bool has_floating_surface_over_page() const;

#if !defined(Q_OS_WIN)
    virtual void accessibilityActiveChanged(bool active) override;
    void fetch_accessibility_tree();
#endif

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    void notify_accessibility_focus_on_document_root();
    void announce_accessibility_focus();
#endif

    friend void install_accessibility(WebContentView*);
    friend void post_accessibility_focus_changed(WebContentView*, i64);

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    void deregister_accessibility_interfaces();
    QHash<i64, AccessibilityInterface*> m_accessibility_elements;
    friend class WebContentViewAccessible;
#endif

    OwnPtr<WebView::AccessibilityTreeManager> m_accessibility_manager;
    bool m_posted_initial_accessibility_focus { false };
    // Set once this view has asked WebContent for the tree; from then on, WebContent pushes every change on its own.
    bool m_requested_accessibility_tree { false };
#if !defined(Q_OS_WIN)
    // Set when an assistive technology turns up mid-page: The next tree to arrive reports the focus to it.
    bool m_report_accessibility_focus_with_next_tree { false };
#endif

    bool m_follows_mouse_while_inactive { false };

    bool m_tooltip_override { false };
    Optional<ByteString> m_tooltip_text;
    QTimer m_tooltip_hover_timer;

    Gfx::IntSize m_viewport_size;
    bool m_viewport_push_pending { false };

    u64 m_last_click_timestamp { 0 };
    QPointF m_last_click_position;
    int m_click_count { 0 };

    SelectDropdown* m_select_dropdown { nullptr };

    PageOverlay* m_crash_overlay { nullptr };
    CrashOverlayUrlLabel* m_crash_overlay_url { nullptr };
    QLabel* m_crash_overlay_message { nullptr };
    QPushButton* m_crash_overlay_reload_button { nullptr };
    QShortcut* m_crash_overlay_reload_shortcut { nullptr };
    QWidget* m_crash_report_container { nullptr };
    CrashReportReviewWidget* m_crash_report_review { nullptr };

    QPointer<ScreenshotSelectionOverlay> m_screenshot_selection_overlay;
    QPointer<ScreenshotViewer> m_screenshot_viewer;
    Vector<QPointer<QWidget>> m_floating_surfaces;
    PageOverlay* m_web_compat_report_overlay { nullptr };

#ifdef AK_OS_MACOS
    NativeTextEditingBindings m_native_text_editing_bindings;

    // The end of a flick's fingers, held back while the momentum AppKit said would follow it has yet to begin.
    std::unique_ptr<QWheelEvent> m_wheel_end_awaiting_momentum;
    Qt::ScrollPhase m_last_wheel_event_phase { Qt::NoScrollPhase };
    quint64 m_last_wheel_event_timestamp { 0 };
#endif

#if defined(LADYBIRD_QT_USE_IOSURFACE_LAYER) || defined(LADYBIRD_HAS_WAYLAND_GPU_QUERY)
    virtual bool eventFilter(QObject*, QEvent*) override;
    void watch_ancestors_for_moves();
    void place_native_view_again();
    Vector<QPointer<QWidget>> m_watched_ancestors;
#endif

#ifdef LADYBIRD_QT_USE_IOSURFACE_LAYER

    bool ensure_iosurface_layer_attached_to_native_view();
    void present_current_paintable_as_layer_contents();
    void update_iosurface_layer_frame();
    void update_iosurface_layer_background_color();
    void detach_iosurface_layer_from_native_view();
    void destroy_iosurface_layer();

    void* m_iosurface_layer { nullptr };
    void* m_page_dim_layer { nullptr };
#endif

#ifdef LADYBIRD_QT_USE_RHI_WIDGET
    bool m_force_full_repaint { true };
    bool m_has_pending_frame_damage { false };
    Gfx::IntRect m_pending_frame_damage;
#endif

#ifdef LADYBIRD_QT_USE_D3D_RHI_WIDGET
    // The front and back backing stores alternate every frame, so imported textures are cached
    // per shared image buffer to avoid re-opening the shared handle on every present. Entries
    // with a null qrhi_texture record failed imports so they are not retried every frame.
    struct ImportedD3DTexture {
        Gfx::SharedImageBuffer const* shared_image_buffer { nullptr };
        int handle_value { -1 };
        ID3D11Texture2D* d3d11_texture { nullptr };
        QRhiTexture* qrhi_texture { nullptr };
    };

    QRhiTexture* imported_d3d_texture_for(Gfx::SharedImageBuffer const&, Gfx::WindowsD3DHandle const&);
    void release_imported_d3d_textures();

    Vector<ImportedD3DTexture> m_imported_d3d_textures;
#endif

#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    struct VulkanRenderer;
    struct VulkanWindow;
    struct VulkanWindowRenderer;
    friend struct VulkanWindow;
    friend struct VulkanWindowRenderer;

    void create_vulkan_window();
    void destroy_vulkan_window();
    void update_vulkan_window_mask();
    void update_vulkan_alpha_blending_support();
    bool current_paintable_can_use_vulkan_window() const;
    void schedule_vulkan_window_update();
    void update_vulkan_window_geometry();
    void set_vulkan_window_cursor(QCursor const&);
    bool handle_vulkan_window_event(QEvent*);
    bool vulkan_window_has_native_focus() const;
    void set_vulkan_window_container_visible(bool);
    void fall_back_to_bitmap_rendering();
    void vulkan_window_exposure_changed();
    QRect rect_covered_by_page_window() const;
    bool vulkan_window_update_would_only_resize() const;
    void remap_unexposed_vulkan_window();

    virtual void release_backing_store(i32 bitmap_id) override;

#    ifdef LADYBIRD_HAS_WAYLAND_GPU_QUERY
    bool create_page_surface();
    void update_page_surface();
    void hide_page_surface();
    bool page_surface_can_show_frames() const;
    void fall_back_from_page_surface();
    void destroy_page_surface();
    void release_retired_backing_stores_no_longer_painted();
    void release_all_retired_backing_stores();
    Vector<Gfx::SharedImageBuffer const*> backing_stores() const;
    QRect rect_shown_in_window() const;

    // Shows the page's frames on Wayland by handing their dma-bufs to the compositor of the desktop. Without it, the
    // page's window draws them in a swap chain of its own.
    OwnPtr<WaylandPageSurface> m_page_surface;
    bool m_page_surface_failed { false };
    // Where the page surface shows a frame, in device pixels of the view.
    QRect m_page_surface_rect;
    // Where the page surface last showed a frame, in the logical coordinates of the browser window.
    QPoint m_page_surface_position;
    // The backing stores the view is done with that the compositor of the desktop may still read. The page they belong
    // to can stop displaying the tab before the compositor is done.
    struct BackingStoreWaitingForRelease {
        NonnullRefPtr<WebView::WebContentPage> page;
        i32 bitmap_id { -1 };
    };
    Vector<BackingStoreWaitingForRelease> m_backing_stores_waiting_for_release;
    struct RetiredBackingStore {
        u64 page_id;
        i32 bitmap_id;
        u64 device;
        u64 inode;
        NonnullRefPtr<BackingStoreRelease> release;
    };
    Vector<RetiredBackingStore> m_retired_backing_stores;
#    endif

    VulkanWindow* m_vulkan_window { nullptr };
    QWidget* m_vulkan_window_container { nullptr };
    Optional<bool> m_vulkan_window_supports_alpha_blending;
    // Whether frames come in a format or layout that the page's window can never import.
    bool m_vulkan_window_cannot_import_frames { false };
    bool m_remapped_unexposed_vulkan_window { false };
    QTimer m_unexposed_vulkan_window_timer;
    // Waits for the browser window to be exposed again, before the page's window is mapped again.
    QObject* m_top_level_expose_watcher { nullptr };
    // The size of the page's last frame on screen, in device pixels.
    QSize m_presented_page_window_size;
    // Counts the changes to what the page's window shows, so that a present that would only resize it can be skipped.
    u64 m_page_window_content_serial { 0 };
    u64 m_presented_page_window_content_serial { 0 };

    // The pixels of the frame on screen, read back from GPU memory once for each frame that Qt paints itself.
    struct FrameReadFromGpu {
        Gfx::SharedImageBuffer const* shared_image_buffer { nullptr };
        RefPtr<Gfx::Bitmap const> bitmap;
    };
    mutable FrameReadFromGpu m_frame_read_from_gpu;

    int m_vertical_tab_overlay_left { 0 };
    int m_vertical_tab_overlay_right { 0 };
    int m_bookmarks_bar_overlay_height { 0 };
#endif
};

bool wheel_event_scrolls_continuously(QWheelEvent const&);

}
