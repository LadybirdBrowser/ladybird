/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/SharedImageBuffer.h>
#include <UI/Qt/WebContentView.h>

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
    }

    auto* layer = as_layer(m_iosurface_layer);
    if (layer.superlayer != content_layer) {
        [layer removeFromSuperlayer];
        [content_layer addSublayer:layer];
    }
    update_iosurface_layer_frame();
    update_iosurface_layer_background_color();
    // The crash overlay is a child widget that Qt paints into the content layer, underneath this one.
    layer.hidden = m_crash_overlay && !m_crash_overlay->isHidden();
    return true;
}

void WebContentView::present_current_paintable_as_layer_contents()
{
    if (!ensure_iosurface_layer_attached_to_native_view())
        return;

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
}

}
