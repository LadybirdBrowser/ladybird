/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGfx/SharedImageBuffer.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/MacWindow.h>
#include <UI/Qt/WebContentView.h>

#include <QChildEvent>
#include <QPointer>
#include <QScreen>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

// A CALayerHost shows the layer tree that another process commits into a CoreAnimation context, given the context's ID.
// It is not public API, so the class is looked up and checked before use.
@protocol LadybirdCoreAnimationLayerHost <NSObject>
@property uint32_t contextId;
@end

// CoreAnimation calls these handlers while it commits a transaction: before the new layer tree goes to the window
// server, and after it has gone but before the window shows it. Neither is public API.
typedef NS_ENUM(unsigned int, LadybirdTransactionPhase) {
    LadybirdTransactionPhasePreLayout,
    LadybirdTransactionPhasePreCommit,
    LadybirdTransactionPhasePostCommit,
};

@interface CATransaction (LadybirdCommitHandlers)
+ (void)addCommitHandler:(void (^)(void))block forPhase:(LadybirdTransactionPhase)phase;
@end

namespace Ladybird {

static CALayer* as_layer(void* layer)
{
    return static_cast<CALayer*>(layer);
}

static Class core_animation_layer_host_class()
{
    static Class layer_host_class = []() -> Class {
        Class layer_host_class = NSClassFromString(@"CALayerHost");
        if (!layer_host_class || ![layer_host_class isSubclassOfClass:CALayer.class])
            return nil;
        if (![layer_host_class instancesRespondToSelector:@selector(contextId)] || ![layer_host_class instancesRespondToSelector:@selector(setContextId:)])
            return nil;
        return layer_host_class;
    }();
    return layer_host_class;
}

bool WebContentView::can_host_remote_layers()
{
    return core_animation_layer_host_class() != nil;
}

// A resize commits the window's new size before the page has a frame of that size, unless the commit waits for it. The
// Compositor holds its layers' changes from the first resize of a transaction, the commit waits for a frame that the
// page laid out at the new size before it sends the window's layer tree, and the Compositor lets its changes go after
// that, when the window server shows the two together.
//
// The commit waits no longer than a frame of the display, as the window could not resize any faster than the page lays
// out otherwise. A page that misses that frame is not waited for again until the resizing pauses, and lags behind it
// until then, as without the wait.
static constexpr auto HELD_FRAME_RELEASE_WAIT_LIMIT = AK::Duration::from_milliseconds(50);
static constexpr int FRAMES_WITHOUT_RESIZE_THAT_END_A_RESIZE = 4;

static AK::Duration display_frame_interval(QWidget const& widget)
{
    auto refresh_rate = widget.screen() ? widget.screen()->refreshRate() : 60.0;
    if (!(refresh_rate > 0))
        refresh_rate = 60.0;
    return AK::Duration::from_nanoseconds(static_cast<i64>(1'000'000'000.0 / refresh_rate));
}

void WebContentView::synchronize_resize_with_remote_layer()
{
    if (m_remote_layer_context_id == 0 || m_synchronized_resize_token.has_value() || !isVisible())
        return;

    auto now = MonotonicTime::now();
    auto frame_interval = display_frame_interval(*this);
    auto resizing_paused = now - m_last_resize_time >= AK::Duration::from_nanoseconds(frame_interval.to_nanoseconds() * FRAMES_WITHOUT_RESIZE_THAT_END_A_RESIZE);
    m_last_resize_time = now;
    if (resizing_paused)
        m_page_lags_behind_resize = false;
    if (m_page_lags_behind_resize)
        return;

    if (![CATransaction respondsToSelector:@selector(addCommitHandler:forPhase:)])
        return;

    static u64 s_next_token = 1;
    auto token = s_next_token++;
    m_synchronized_resize_token = token;
    begin_synchronized_resize(token);

    QPointer<WebContentView> view { this };
    [CATransaction
        addCommitHandler:^{
            if (view)
                view->await_resized_frame_before_commit(token);
        }
                forPhase:LadybirdTransactionPhasePreCommit];
    [CATransaction
        addCommitHandler:^{
            if (view)
                view->release_resized_frame_with_commit(token);
        }
                forPhase:LadybirdTransactionPhasePostCommit];
}

void WebContentView::await_resized_frame_before_commit(u64 token)
{
    if (m_viewport_push_pending) {
        m_viewport_push_pending = false;
        update_viewport_size();
    }
    await_synchronized_resize_frame(token, m_viewport_size);

    auto deadline = MonotonicTime::now() + display_frame_interval(*this);
    for (;;) {
        auto message = HubConnection::the().wait_for_specific_endpoint_message_until<Messages::HubClient::ViewDidHoldSynchronizedResizeFrame>(deadline);
        if (!message) {
            m_page_lags_behind_resize = true;
            return;
        }
        if (message->view_id() == view_id() && message->token() == token)
            return;
    }
}

void WebContentView::release_resized_frame_with_commit(u64 token)
{
    m_synchronized_resize_token = {};
    commit_synchronized_resize(token);

    auto deadline = MonotonicTime::now() + HELD_FRAME_RELEASE_WAIT_LIMIT;
    for (;;) {
        auto message = HubConnection::the().wait_for_specific_endpoint_message_until<Messages::HubClient::ViewDidCommitSynchronizedResize>(deadline);
        if (!message || (message->view_id() == view_id() && message->token() == token))
            return;
    }
}

void WebContentView::remote_layer_changed(Compositing::RemoteLayerHostHandle host_handle)
{
    m_remote_layer_context_id = host_handle.kind == Compositing::RemoteLayerHostKind::CoreAnimationContext ? static_cast<u32>(host_handle.id) : 0;
    update_remote_layer_host();
    schedule_repaint();
}

// The host shows the remote layer once a frame has been shown in it. Until then, the page layer goes on showing the
// last frame it was given itself.
void WebContentView::update_remote_layer_host()
{
    if (!m_iosurface_layer)
        return;
    auto* page_layer = as_layer(m_iosurface_layer);
    if (m_remote_layer_context_id == 0) {
        if (m_remote_layer_host) {
            [as_layer(m_remote_layer_host) removeFromSuperlayer];
            [as_layer(m_remote_layer_host) release];
            m_remote_layer_host = nullptr;
        }
        return;
    }

    auto* host = static_cast<CALayer<LadybirdCoreAnimationLayerHost>*>(m_remote_layer_host);
    if (!host || host.contextId != m_remote_layer_context_id) {
        auto* new_host = static_cast<CALayer<LadybirdCoreAnimationLayerHost>*>([[core_animation_layer_host_class() alloc] init]);
        new_host.anchorPoint = CGPointZero;
        new_host.contextId = m_remote_layer_context_id;
        new_host.actions = @ {
            @"bounds" : NSNull.null,
            @"position" : NSNull.null,
            @"hidden" : NSNull.null,
        };
        [page_layer addSublayer:new_host];
        if (host) {
            [host removeFromSuperlayer];
            [host release];
        }
        host = new_host;
        m_remote_layer_host = host;
    }
    host.frame = page_layer.bounds;
}

bool WebContentView::ensure_iosurface_layer_attached_to_native_view()
{
    auto* content_layer = as_layer(appkit_content_layer(*this));
    if (!content_layer)
        return false;

    // NB: Qt restores the native view's exposed state from the content layer's display callback after it is unhidden.
    //     Presenting our own sublayer may leave that layer clean, which keeps Qt from routing mouse events to the view.
    if (isVisible() && !windowHandle()->isExposed())
        [content_layer setNeedsDisplay];

    if (!m_iosurface_layer) {
        auto* layer = [[CALayer alloc] init];
        layer.opaque = YES;
        // A remote layer can hold a frame larger than the view, as while the window shrinks.
        layer.masksToBounds = YES;
        // Without this every new frame would cross-fade in and geometry changes would slide.
        layer.actions = @ {
            @"contents" : NSNull.null,
            @"contentsRect" : NSNull.null,
            @"contentsScale" : NSNull.null,
            @"contentsGravity" : NSNull.null,
            @"bounds" : NSNull.null,
            @"position" : NSNull.null,
            @"hidden" : NSNull.null,
            @"backgroundColor" : NSNull.null,
        };
        m_iosurface_layer = layer;
        update_remote_layer_host();
    }

    if (!m_asked_for_remote_layer && can_host_remote_layers()) {
        m_asked_for_remote_layer = true;
        set_hosts_remote_layer(true);
    }

    auto* layer = as_layer(m_iosurface_layer);
    if (layer.superlayer != content_layer) {
        [layer removeFromSuperlayer];
        [content_layer addSublayer:layer];
    }
    update_iosurface_layer_frame();
    update_iosurface_layer_background_color();
    // Overlays are child widgets that Qt paints into the content layer, underneath this one.
    layer.hidden = is_page_covered_by_overlay();
    update_whether_qt_paints_view();
    return true;
}

void WebContentView::present_current_paintable_as_layer_contents()
{
    if (!ensure_iosurface_layer_attached_to_native_view())
        return;
    auto* layer = as_layer(m_iosurface_layer);
    ScopeGuard update_for_new_contents = [this, layer, had_contents = layer.contents != nil] {
        if ((layer.contents != nil) != had_contents)
            update_iosurface_layer_background_color();
        update_whether_qt_paints_view();
    };

    layer.contentsScale = device_pixel_ratio();

    auto paintable = current_paintable();
    if (!paintable.has_value()) {
        layer.contents = nil;
        return;
    }
    // The Compositor showed the frame in the remote layer, which the host shows over this layer.
    if (!paintable->shared_image_buffer) {
        if (layer.contents != nil) {
            layer.contents = nil;
            reinterpret_cast<NSView*>(winId()).window.viewsNeedDisplay = YES;
        }
        return;
    }

    auto const& iosurface_handle = paintable->shared_image_buffer->iosurface_handle();
    auto surface_width = static_cast<int>(iosurface_handle.width());
    auto surface_height = static_cast<int>(iosurface_handle.height());
    auto painted_width = min(paintable->bitmap_size.width(), surface_width);
    auto painted_height = min(paintable->bitmap_size.height(), surface_height);
    if (painted_width <= 0 || painted_height <= 0) {
        layer.contents = nil;
        return;
    }
    auto painted_unit_width = static_cast<CGFloat>(painted_width) / surface_width;
    auto painted_unit_height = static_cast<CGFloat>(painted_height) / surface_height;

    // The surface can be larger than what was painted into it (a live resize pads it), so only the painted part is
    // shown: unless frames are scaled to fit, one surface pixel per device pixel, anchored at the visual top-left.
    // Inside Qt's flipped view the y-axis of this layer points down, which turns both the top-left gravity constant
    // and the unit rectangle upside down.
    bool y_axis_points_down = [layer contentsAreFlipped];
    auto top_left_gravity = y_axis_points_down ? kCAGravityBottomLeft : kCAGravityTopLeft;
    layer.contentsGravity = scales_frames_to_fit() ? kCAGravityResizeAspect : top_left_gravity;
    layer.contentsRect = y_axis_points_down
        ? CGRectMake(0, 0, painted_unit_width, painted_unit_height)
        : CGRectMake(0, 1 - painted_unit_height, painted_unit_width, painted_unit_height);
    layer.contents = (id)iosurface_handle.core_foundation_pointer();

    // Committing along with the window's display cycle keeps the page in the same transaction as any chrome Qt paints
    // in this turn of the event loop, the way Qt's own backing store flushes do.
    reinterpret_cast<NSView*>(winId()).window.viewsNeedDisplay = YES;
}

void WebContentView::update_iosurface_layer_frame()
{
    if (!m_iosurface_layer)
        return;
    as_layer(m_iosurface_layer).frame = CGRectMake(0, 0, width(), height());
    if (m_remote_layer_host)
        as_layer(m_remote_layer_host).frame = as_layer(m_iosurface_layer).bounds;
    if (m_page_dim_layer)
        as_layer(m_page_dim_layer).frame = CGRectMake(0, 0, width(), height());
}

// The dim is a layer of its own right above the page, under the native views of the dialog and the other floating
// surfaces.
void WebContentView::set_page_dim(qreal dim)
{
    if (!m_page_dim_layer && dim <= 0)
        return;
    if (!ensure_iosurface_layer_attached_to_native_view())
        return;

    auto* page_layer = as_layer(m_iosurface_layer);
    if (!m_page_dim_layer) {
        auto* layer = [[CALayer alloc] init];
        layer.actions = @ {
            @"bounds" : NSNull.null,
            @"position" : NSNull.null,
            @"opacity" : NSNull.null,
            @"hidden" : NSNull.null,
        };
        auto* black = CGColorCreateSRGB(0, 0, 0, 0.22);
        layer.backgroundColor = black;
        CGColorRelease(black);
        m_page_dim_layer = layer;
    }
    auto* layer = as_layer(m_page_dim_layer);
    if (layer.superlayer != page_layer.superlayer || [page_layer.superlayer.sublayers indexOfObject:layer] != [page_layer.superlayer.sublayers indexOfObject:page_layer] + 1) {
        [layer removeFromSuperlayer];
        [page_layer.superlayer insertSublayer:layer above:page_layer];
    }
    layer.frame = CGRectMake(0, 0, width(), height());
    layer.opacity = dim;
    layer.hidden = dim <= 0;
    reinterpret_cast<NSView*>(winId()).window.viewsNeedDisplay = YES;
}

void WebContentView::update_iosurface_layer_background_color()
{
    if (!m_iosurface_layer)
        return;
    auto* layer = as_layer(m_iosurface_layer);
    // Before its first frame, the view shows the chrome's color.
    auto page_color = page_background_color();
    bool shows_frame = layer.contents != nil || (m_remote_layer_host && current_paintable().has_value());
    auto color = shows_frame ? QColor(page_color.red(), page_color.green(), page_color.blue()) : ChromeStyle::chrome_background(window()->palette());
    auto* background_color = CGColorCreateSRGB(color.redF(), color.greenF(), color.blueF(), 1.0);
    layer.backgroundColor = background_color;
    CGColorRelease(background_color);
}

void WebContentView::detach_iosurface_layer_from_native_view()
{
    if (m_iosurface_layer)
        [as_layer(m_iosurface_layer) removeFromSuperlayer];
    update_whether_qt_paints_view();
}

// While the page layer covers the view, the view paints on screen, so Qt neither paints it into the window's backing
// store nor copies it into a backing store of the view's own native view. All of that would lie under the layer, and
// on every step of a window resize it is a fill and a copy of the whole view into freshly allocated surfaces. Once made,
// that backing store of the native view stays for as long as the view, so Qt must not paint the view before it has a
// frame either: until then the layer shows the color Qt would paint. Qt paints the view only while the layer is not
// attached yet, and while an overlay, which Qt paints into the view, hides the layer.
void WebContentView::update_whether_qt_paints_view()
{
    auto* layer = as_layer(m_iosurface_layer);
    bool page_layer_covers_view = layer.superlayer != nil && !layer.hidden;
    if (testAttribute(Qt::WA_PaintOnScreen) == page_layer_covers_view)
        return;
    setAttribute(Qt::WA_PaintOnScreen, page_layer_covers_view);
    // What Qt last painted of the view is from before the layer covered it. Painting now puts the view as it is into
    // the same window update in which the layer stops covering it.
    if (!page_layer_covers_view)
        repaint();
}

// The view paints nothing on screen. QWidget's own version returns no engine either, but warns.
QPaintEngine* WebContentView::paintEngine() const
{
    return nullptr;
}

// Qt gives each child of a widget that paints on screen a native view of its own, which would stack the overlays and
// the dialog above the page layer, showing whatever lies under them in the window's backing store. They stay painted
// into this view's native view instead, as while Qt paints the view. A child that wants a native view asks for one in
// its own constructor, after this.
void WebContentView::childEvent(QChildEvent* event)
{
    WebContentViewBase::childEvent(event);
    if (!event->added() || !testAttribute(Qt::WA_PaintOnScreen) || !event->child()->isWidgetType())
        return;
    auto* child = static_cast<QWidget*>(event->child());
    if (!child->isWindow() && !child->testAttribute(Qt::WA_WState_Created))
        child->setAttribute(Qt::WA_NativeWindow, false);
}

void WebContentView::destroy_iosurface_layer()
{
    if (!m_iosurface_layer)
        return;
    auto* layer = as_layer(m_iosurface_layer);
    layer.contents = nil;
    [layer removeFromSuperlayer];
    [layer release];
    m_iosurface_layer = nullptr;
    if (m_remote_layer_host) {
        [as_layer(m_remote_layer_host) release];
        m_remote_layer_host = nullptr;
    }

    if (m_page_dim_layer) {
        [as_layer(m_page_dim_layer) removeFromSuperlayer];
        [as_layer(m_page_dim_layer) release];
        m_page_dim_layer = nullptr;
    }
}

}
