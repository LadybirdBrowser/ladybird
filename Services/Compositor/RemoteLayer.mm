/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <Compositor/RemoteLayer.h>
#include <LibCompositing/FrameFitting.h>
#include <LibGfx/SharedImageBuffer.h>

#include <dlfcn.h>
#include <objc/runtime.h>

#import <QuartzCore/QuartzCore.h>

// A CAContext carries a layer tree to the window server, where a CALayerHost in another process shows it by the
// context's ID. Neither is public API, so the context is reached through the runtime and checked before use.
@protocol LadybirdCoreAnimationContext <NSObject>
@property (readonly) uint32_t contextId;
@property (retain) CALayer* layer;
@end

@protocol LadybirdCoreAnimationContextClass <NSObject>
+ (id<LadybirdCoreAnimationContext>)contextWithCGSConnection:(uint32_t)connection options:(NSDictionary*)options;
@end

namespace Compositor {

using WindowServerConnectionIDFunction = uint32_t (*)();

static WindowServerConnectionIDFunction main_window_server_connection_id_function()
{
    static auto function = reinterpret_cast<WindowServerConnectionIDFunction>(dlsym(RTLD_DEFAULT, "CGSMainConnectionID"));
    return function;
}

static Class core_animation_context_class()
{
    static Class context_class = []() -> Class {
        Class context_class = NSClassFromString(@"CAContext");
        if (!context_class || ![context_class respondsToSelector:@selector(contextWithCGSConnection:options:)])
            return nil;
        // The properties are generated at run time, so instances do not answer to their selectors before one exists.
        if (!class_getProperty(context_class, "contextId") || !class_getProperty(context_class, "layer"))
            return nil;
        return context_class;
    }();
    return context_class;
}

// Changes made outside an explicit transaction open an implicit one, which only a run loop commits, and while it is
// open it holds back the explicit ones nested in it as well. This process has no run loop, so every change is made in
// an explicit transaction, and any implicit one that something opened anyway is flushed after it.
template<typename Callback>
static void commit_layer_changes(Callback callback)
{
    @autoreleasepool {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        callback();
        [CATransaction commit];
        [CATransaction flush];
    }
}

// The transactions of each change nest in this one until it is committed, so none of them reaches the window server
// before then.
void RemoteLayer::hold_changes_on_screen()
{
    [CATransaction begin];
}

void RemoteLayer::release_changes_held_off_screen()
{
    [CATransaction commit];
    [CATransaction flush];
}

static CALayer* as_layer(void* layer)
{
    return static_cast<CALayer*>(layer);
}

OwnPtr<RemoteLayer> RemoteLayer::try_create()
{
    auto* connection_id_function = main_window_server_connection_id_function();
    Class context_class = core_animation_context_class();
    if (!connection_id_function || !context_class)
        return nullptr;

    auto remote_layer = adopt_own(*new RemoteLayer);
    commit_layer_changes([&] {
        auto* root_layer = [[CALayer alloc] init];
        root_layer.anchorPoint = CGPointZero;
        // Frames are laid out from the top left, as the view that hosts the tree is.
        root_layer.geometryFlipped = YES;

        auto* content_layer = [[CALayer alloc] init];
        content_layer.anchorPoint = CGPointZero;
        [root_layer addSublayer:content_layer];

        id<LadybirdCoreAnimationContext> context = [(Class<LadybirdCoreAnimationContextClass>)context_class contextWithCGSConnection:connection_id_function() options:@ {}];
        context.layer = root_layer;

        remote_layer->m_context = [context retain];
        remote_layer->m_root_layer = root_layer;
        remote_layer->m_content_layer = content_layer;
        remote_layer->m_core_animation_context_id = context.contextId;
    });
    if (remote_layer->m_core_animation_context_id == 0)
        return nullptr;
    return remote_layer;
}

RemoteLayer::~RemoteLayer()
{
    commit_layer_changes([&] {
        auto context = static_cast<id<LadybirdCoreAnimationContext>>(m_context);
        context.layer = nil;
        [context release];
        [as_layer(m_content_layer) release];
        [as_layer(m_root_layer) release];
    });
}

static CGRect in_points(Gfx::IntRect rect, double device_pixel_ratio)
{
    return CGRectMake(rect.x() / device_pixel_ratio, rect.y() / device_pixel_ratio, rect.width() / device_pixel_ratio, rect.height() / device_pixel_ratio);
}

// The root layer spans the view a frame is fitted into, as its y-axis is flipped within its own height.
void RemoteLayer::lay_out_frame(Optional<Gfx::IntSize> fit_within)
{
    Gfx::IntRect root_rect { {}, fit_within.value_or(m_painted_size) };
    auto content_rect = fit_within.has_value() ? Compositing::rect_fitting_frame(m_painted_size, *fit_within) : root_rect;
    as_layer(m_root_layer).frame = in_points(root_rect, m_device_pixel_ratio);
    as_layer(m_content_layer).frame = in_points(content_rect, m_device_pixel_ratio);
}

void RemoteLayer::place_frame(Optional<Gfx::IntSize> fit_within)
{
    if (m_painted_size.is_empty())
        return;
    commit_layer_changes([&] {
        lay_out_frame(fit_within);
    });
}

void RemoteLayer::show_frame(Gfx::SharedImageBuffer const& buffer, Gfx::IntSize painted_size, double device_pixel_ratio, Optional<Gfx::IntSize> fit_within)
{
    auto const& iosurface = buffer.iosurface_handle();
    auto surface_width = static_cast<int>(iosurface.width());
    auto surface_height = static_cast<int>(iosurface.height());
    auto painted_width = min(painted_size.width(), surface_width);
    auto painted_height = min(painted_size.height(), surface_height);
    if (painted_width <= 0 || painted_height <= 0)
        return;
    m_painted_size = { painted_width, painted_height };
    m_device_pixel_ratio = device_pixel_ratio;

    commit_layer_changes([&] {
        auto* content_layer = as_layer(m_content_layer);
        auto painted_unit_width = static_cast<CGFloat>(painted_width) / surface_width;
        auto painted_unit_height = static_cast<CGFloat>(painted_height) / surface_height;
        // A store can be larger than the frame painted into it, as while the window is resized. The unit rectangle
        // that picks the painted part turns upside down along with the layer's y-axis.
        content_layer.contentsRect = content_layer.contentsAreFlipped
            ? CGRectMake(0, 0, painted_unit_width, painted_unit_height)
            : CGRectMake(0, 1 - painted_unit_height, painted_unit_width, painted_unit_height);
        content_layer.contentsScale = device_pixel_ratio;
        lay_out_frame(fit_within);
        content_layer.contents = (__bridge id)iosurface.core_foundation_pointer();
    });
}

}
