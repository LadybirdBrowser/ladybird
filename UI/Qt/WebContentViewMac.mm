/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGfx/SharedImageBuffer.h>
#include <UI/Qt/WebContentView.h>

#include <QChildEvent>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

namespace Ladybird {

static CALayer* as_layer(void* layer)
{
    return static_cast<CALayer*>(layer);
}

bool WebContentView::ensure_iosurface_layer_attached_to_native_view()
{
    auto* window_handle = windowHandle();
    auto* cocoa_window = window_handle ? window_handle->nativeInterface<QNativeInterface::Private::QCocoaWindow>() : nullptr;
    CALayer* content_layer = cocoa_window ? cocoa_window->contentLayer() : nil;
    if (!content_layer)
        return false;

    // NB: Qt restores the native view's exposed state from the content layer's display callback after it is unhidden.
    //     Presenting our own sublayer may leave that layer clean, which keeps Qt from routing mouse events to the view.
    if (isVisible() && !window_handle->isExposed())
        [content_layer setNeedsDisplay];

    if (!m_iosurface_layer) {
        auto* layer = [[CALayer alloc] init];
        layer.opaque = YES;
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
    ScopeGuard update_painting_for_new_contents = [this] { update_whether_qt_paints_view(); };

    auto* layer = as_layer(m_iosurface_layer);
    layer.contentsScale = m_device_pixel_ratio;

    auto paintable = current_paintable();
    if (!paintable.has_value()) {
        layer.contents = nil;
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
    layer.contentsGravity = m_scales_frames_to_fit ? kCAGravityResizeAspect : top_left_gravity;
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
    auto color = page_background_color();
    auto* background_color = CGColorCreateSRGB(color.red() / 255.0, color.green() / 255.0, color.blue() / 255.0, 1.0);
    as_layer(m_iosurface_layer).backgroundColor = background_color;
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
// on every step of a window resize it is a fill and a copy of the whole view into freshly allocated surfaces. Qt paints
// the view before the layer has a frame, and while an overlay, which Qt paints into the view, hides the layer.
void WebContentView::update_whether_qt_paints_view()
{
    auto* layer = as_layer(m_iosurface_layer);
    bool page_layer_covers_view = layer.superlayer != nil && !layer.hidden && layer.contents != nil;
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

    if (m_page_dim_layer) {
        [as_layer(m_page_dim_layer) removeFromSuperlayer];
        [as_layer(m_page_dim_layer) release];
        m_page_dim_layer = nullptr;
    }
}

}
