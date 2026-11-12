/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/StdLibExtras.h>
#include <Compositor/RemoteLayer.h>
#include <LibCompositing/FrameFitting.h>
#include <LibCore/Timer.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibMediaCodecs/VideoSurface.h>

#include <dlfcn.h>
#include <objc/runtime.h>

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurfaceRef.h>
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

static bool s_changes_are_held_off_screen = false;
static MonotonicTime s_changes_last_released_on_screen;

// The transactions of each change nest in this one until it is committed, so none of them reaches the window server
// before then.
void RemoteLayer::hold_changes_on_screen()
{
    [CATransaction begin];
    s_changes_are_held_off_screen = true;
}

void RemoteLayer::release_changes_held_off_screen()
{
    [CATransaction commit];
    [CATransaction flush];
    s_changes_are_held_off_screen = false;
    s_changes_last_released_on_screen = MonotonicTime::now();
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

        // The frame and the sublayers over it are laid out in the frame's own pixels, and scaled together to fit.
        auto* frame_layer = [[CALayer alloc] init];
        frame_layer.anchorPoint = CGPointZero;
        [root_layer addSublayer:frame_layer];

        auto* content_layer = [[CALayer alloc] init];
        content_layer.anchorPoint = CGPointZero;
        [frame_layer addSublayer:content_layer];

        id<LadybirdCoreAnimationContext> context = [(Class<LadybirdCoreAnimationContextClass>)context_class contextWithCGSConnection:connection_id_function() options:@ {}];
        context.layer = root_layer;

        remote_layer->m_context = [context retain];
        remote_layer->m_root_layer = root_layer;
        remote_layer->m_frame_layer = frame_layer;
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
        for (auto& it : m_shown_sublayers)
            stop_showing_sublayer(it.value);
        auto context = static_cast<id<LadybirdCoreAnimationContext>>(m_context);
        context.layer = nil;
        [context release];
        [as_layer(m_content_layer) release];
        [as_layer(m_frame_layer) release];
        [as_layer(m_root_layer) release];
    });
    if (m_video_frame_release_timer)
        m_video_frame_release_timer->stop();
}

static CGRect in_points(Gfx::FloatRect rect, double device_pixel_ratio)
{
    return CGRectMake(rect.x() / device_pixel_ratio, rect.y() / device_pixel_ratio, rect.width() / device_pixel_ratio, rect.height() / device_pixel_ratio);
}

static CGRect in_points(Gfx::IntRect rect, double device_pixel_ratio)
{
    return in_points(rect.to_type<float>(), device_pixel_ratio);
}

// The root layer spans the view a frame is fitted into, as its y-axis is flipped within its own height.
void RemoteLayer::lay_out_frame(Optional<Gfx::IntSize> fit_within)
{
    Gfx::IntRect frame_rect { {}, m_painted_size };
    Gfx::IntRect root_rect { {}, fit_within.value_or(m_painted_size) };
    auto fitted_rect = fit_within.has_value() ? Compositing::rect_fitting_frame(m_painted_size, *fit_within) : root_rect;
    as_layer(m_root_layer).frame = in_points(root_rect, m_device_pixel_ratio);

    auto* frame_layer = as_layer(m_frame_layer);
    frame_layer.bounds = in_points(frame_rect, m_device_pixel_ratio);
    frame_layer.position = in_points(fitted_rect, m_device_pixel_ratio).origin;
    frame_layer.affineTransform = CGAffineTransformMakeScale(
        static_cast<CGFloat>(fitted_rect.width()) / max(frame_rect.width(), 1),
        static_cast<CGFloat>(fitted_rect.height()) / max(frame_rect.height(), 1));
    as_layer(m_content_layer).frame = in_points(frame_rect, m_device_pixel_ratio);
}

void RemoteLayer::place_frame(Optional<Gfx::IntSize> fit_within)
{
    if (m_painted_size.is_empty())
        return;
    commit_layer_changes([&] {
        lay_out_frame(fit_within);
    });
}

// The part of a surface that a layer shows, as a unit rectangle, which turns upside down along with the layer's y-axis.
static CGRect unit_rect_of_surface_part(CALayer* layer, Gfx::IntSize part_size, Gfx::IntSize surface_size)
{
    auto unit_width = static_cast<CGFloat>(part_size.width()) / surface_size.width();
    auto unit_height = static_cast<CGFloat>(part_size.height()) / surface_size.height();
    return layer.contentsAreFlipped
        ? CGRectMake(0, 0, unit_width, unit_height)
        : CGRectMake(0, 1 - unit_height, unit_width, unit_height);
}

void RemoteLayer::show_frame(Gfx::SharedImageBuffer const* buffer, Gfx::IntSize painted_size, double device_pixel_ratio, Optional<Gfx::IntSize> fit_within, Vector<RemoteSublayer> sublayers)
{
    Gfx::IntSize surface_size;
    if (buffer) {
        auto const& iosurface = buffer->iosurface_handle();
        surface_size = { static_cast<int>(iosurface.width()), static_cast<int>(iosurface.height()) };
        // A store can be larger than the frame painted into it, as while the window is resized.
        painted_size = { min(painted_size.width(), surface_size.width()), min(painted_size.height(), surface_size.height()) };
        if (painted_size.is_empty())
            return;
        m_painted_size = painted_size;
    }
    if (m_painted_size.is_empty())
        return;
    m_device_pixel_ratio = device_pixel_ratio;

    commit_layer_changes([&] {
        auto* content_layer = as_layer(m_content_layer);
        if (buffer) {
            content_layer.contentsRect = unit_rect_of_surface_part(content_layer, m_painted_size, surface_size);
            content_layer.contentsScale = device_pixel_ratio;
        }
        lay_out_frame(fit_within);
        if (buffer)
            content_layer.contents = (__bridge id)buffer->iosurface_handle().core_foundation_pointer();
        show_sublayers(sublayers);
    });
    release_video_frames_window_server_let_go_of();
}

void RemoteLayer::show_sublayers(Vector<RemoteSublayer>& sublayers)
{
    HashTable<u64> shown_identities;
    auto* ordered_layers = [NSMutableArray arrayWithObject:as_layer(m_content_layer)];
    for (auto& sublayer : sublayers) {
        auto& shown_sublayer = m_shown_sublayers.ensure(sublayer.identity, [] {
            ShownSublayer shown_sublayer;
            auto* clip_layer = [[CALayer alloc] init];
            clip_layer.anchorPoint = CGPointZero;
            clip_layer.masksToBounds = YES;
            auto* content_layer = [[CALayer alloc] init];
            content_layer.anchorPoint = CGPointZero;
            [clip_layer addSublayer:content_layer];
            shown_sublayer.clip_layer = clip_layer;
            shown_sublayer.content_layer = content_layer;
            return shown_sublayer;
        });
        update_shown_sublayer(shown_sublayer, sublayer);
        [ordered_layers addObject:as_layer(shown_sublayer.clip_layer)];
        shown_identities.set(sublayer.identity);
    }

    Vector<u64> identities_no_longer_shown;
    for (auto& it : m_shown_sublayers) {
        if (!shown_identities.contains(it.key))
            identities_no_longer_shown.append(it.key);
    }
    for (auto identity : identities_no_longer_shown) {
        auto shown_sublayer = m_shown_sublayers.take(identity).release_value();
        stop_showing_sublayer(shown_sublayer);
    }

    as_layer(m_frame_layer).sublayers = ordered_layers;
}

static CGAffineTransform to_core_graphics(Gfx::AffineTransform const& transform)
{
    return CGAffineTransformMake(transform.a(), transform.b(), transform.c(), transform.d(), transform.e(), transform.f());
}

static NSString* filter_for_scaling_mode(Gfx::ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case Gfx::ScalingMode::None:
    case Gfx::ScalingMode::NearestNeighbor:
        return kCAFilterNearest;
    case Gfx::ScalingMode::Bilinear:
    case Gfx::ScalingMode::BilinearMipmap:
        return kCAFilterLinear;
    }
    VERIFY_NOT_REACHED();
}

void RemoteLayer::update_shown_sublayer(ShownSublayer& shown_sublayer, RemoteSublayer& sublayer)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    auto* clip_layer = as_layer(shown_sublayer.clip_layer);
    auto* content_layer = as_layer(shown_sublayer.content_layer);
    clip_layer.frame = in_points(sublayer.clip_rect, device_pixel_ratio);

    // The content layer is placed relative to the innermost layer that clips it.
    auto origin_of_content_parent = sublayer.clip_rect.location();
    if (sublayer.rounded_clip.has_value()) {
        auto const& rounded_clip = *sublayer.rounded_clip;
        if (!shown_sublayer.rounded_clip_layer) {
            auto* rounded_clip_layer = [[CALayer alloc] init];
            rounded_clip_layer.anchorPoint = CGPointZero;
            rounded_clip_layer.masksToBounds = YES;
            [clip_layer addSublayer:rounded_clip_layer];
            [rounded_clip_layer addSublayer:content_layer];
            shown_sublayer.rounded_clip_layer = rounded_clip_layer;
        }
        auto* rounded_clip_layer = as_layer(shown_sublayer.rounded_clip_layer);
        rounded_clip_layer.frame = in_points(rounded_clip.rect.translated(-origin_of_content_parent), device_pixel_ratio);
        rounded_clip_layer.cornerRadius = rounded_clip.radius / device_pixel_ratio;
        // The corners are named in the layer's own coordinates, whose y-axis points down here like the frame's.
        CACornerMask masked_corners = 0;
        if (rounded_clip.rounds_top_left)
            masked_corners |= kCALayerMinXMinYCorner;
        if (rounded_clip.rounds_top_right)
            masked_corners |= kCALayerMaxXMinYCorner;
        if (rounded_clip.rounds_bottom_right)
            masked_corners |= kCALayerMaxXMaxYCorner;
        if (rounded_clip.rounds_bottom_left)
            masked_corners |= kCALayerMinXMaxYCorner;
        rounded_clip_layer.maskedCorners = masked_corners;
        origin_of_content_parent = rounded_clip.rect.location();
    } else if (shown_sublayer.rounded_clip_layer) {
        [clip_layer addSublayer:content_layer];
        [as_layer(shown_sublayer.rounded_clip_layer) removeFromSuperlayer];
        [as_layer(shown_sublayer.rounded_clip_layer) release];
        shown_sublayer.rounded_clip_layer = nullptr;
    }

    // Maps the layer's points to the content's pixels, through the frame's pixels, to the points of its parent.
    auto content_rect = sublayer.content_rect;
    auto content_width_in_points = content_rect.width() / device_pixel_ratio;
    auto content_height_in_points = content_rect.height() / device_pixel_ratio;
    auto transform = CGAffineTransformMakeScale(device_pixel_ratio, device_pixel_ratio);
    transform = CGAffineTransformConcat(transform, CGAffineTransformMakeTranslation(content_rect.x(), content_rect.y()));
    transform = CGAffineTransformConcat(transform, to_core_graphics(sublayer.content_to_frame));
    transform = CGAffineTransformConcat(transform, CGAffineTransformMakeScale(1 / device_pixel_ratio, 1 / device_pixel_ratio));
    transform = CGAffineTransformConcat(transform, CGAffineTransformMakeTranslation(-origin_of_content_parent.x() / device_pixel_ratio, -origin_of_content_parent.y() / device_pixel_ratio));
    auto const* lease = sublayer.content.get_pointer<PresentableSurface::Lease>();
    if (lease && lease->surface().content_is_upside_down())
        transform = CGAffineTransformConcat(CGAffineTransformMake(1, 0, 0, -1, 0, content_height_in_points), transform);
    content_layer.bounds = CGRectMake(0, 0, content_width_in_points, content_height_in_points);
    content_layer.position = CGPointZero;
    content_layer.affineTransform = transform;
    content_layer.allowsEdgeAntialiasing = !sublayer.content_to_frame.is_identity_or_translation_or_scale();
    content_layer.contentsScale = device_pixel_ratio;
    content_layer.magnificationFilter = filter_for_scaling_mode(sublayer.scaling_mode);
    content_layer.minificationFilter = sublayer.scaling_mode == Gfx::ScalingMode::BilinearMipmap ? kCAFilterTrilinear : filter_for_scaling_mode(sublayer.scaling_mode);

    show_content(shown_sublayer, sublayer);
}

// The window server converts a YCbCr surface to RGB by the matrix it is tagged with. It decodes the transfer of SDR
// video by a curve of its own whatever the transfer is tagged as, but the colour space of the surface overrides that,
// and naming it after sRGB draws the video as the frame draws it.
static void describe_colours_of_video_surface(IOSurfaceRef surface, Media::CodingIndependentCodePoints cicp)
{
    auto matrix = CVYCbCrMatrixGetStringForIntegerCodePoint(to_underlying(cicp.matrix_coefficients()));
    auto primaries = CVColorPrimariesGetStringForIntegerCodePoint(to_underlying(cicp.color_primaries()));
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceYCbCrMatrix"), matrix ? matrix : kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceColorPrimaries"), primaries ? primaries : kCVImageBufferColorPrimaries_ITU_R_709_2);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceTransferFunction"), kCVImageBufferTransferFunction_sRGB);
    CFStringRef color_space_name = kCGColorSpaceSRGB;
    if (cicp.color_primaries() == Media::ColorPrimaries::SMPTE432)
        color_space_name = kCGColorSpaceDisplayP3;
    else if (cicp.color_primaries() == Media::ColorPrimaries::BT2020)
        color_space_name = kCGColorSpaceITUR_2020_sRGBGamma;
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceColorSpace"), color_space_name);
}

void RemoteLayer::show_content(ShownSublayer& shown_sublayer, RemoteSublayer& sublayer)
{
    auto* content_layer = as_layer(shown_sublayer.content_layer);
    sublayer.content.visit(
        [&](NonnullRefPtr<Media::VideoFrame>& frame) {
            auto const* shown_frame = shown_sublayer.content.get_pointer<NonnullRefPtr<Media::VideoFrame>>();
            if (shown_frame && shown_frame->ptr() == frame.ptr())
                return;
            auto video_surface = frame->surface();
            VERIFY(video_surface);
            auto const& iosurface = video_surface->io_surface();
            describe_colours_of_video_surface(static_cast<IOSurfaceRef>(iosurface.core_foundation_pointer()), frame->cicp());
            Gfx::IntSize surface_size { static_cast<int>(iosurface.width()), static_cast<int>(iosurface.height()) };
            auto visible_size = frame->size().to_type<int>();
            Gfx::IntSize shown_size { min(visible_size.width(), surface_size.width()), min(visible_size.height(), surface_size.height()) };
            content_layer.contentsRect = unit_rect_of_surface_part(content_layer, shown_size, surface_size);
            content_layer.opaque = YES;
            content_layer.contents = (__bridge id)iosurface.core_foundation_pointer();
            if (shown_frame)
                keep_video_frame_until_window_server_lets_go(*shown_frame);
            shown_sublayer.content = frame;
        },
        [&](PresentableSurface::Lease& lease) {
            if (auto const* shown_frame = shown_sublayer.content.get_pointer<NonnullRefPtr<Media::VideoFrame>>())
                keep_video_frame_until_window_server_lets_go(*shown_frame);
            auto const* shown_lease = shown_sublayer.content.get_pointer<PresentableSurface::Lease>();
            if (shown_lease && &shown_lease->surface() == &lease.surface())
                return;
            auto& surface = lease.surface();
            auto const* iosurface = surface.iosurface();
            VERIFY(iosurface);
            surface.did_hand_to_window_server();
            content_layer.contentsRect = CGRectMake(0, 0, 1, 1);
            content_layer.opaque = NO;
            content_layer.contents = (__bridge id)iosurface->core_foundation_pointer();
            shown_sublayer.content = move(lease);
        });
}

void RemoteLayer::stop_showing_sublayer(ShownSublayer& shown_sublayer)
{
    if (auto const* shown_frame = shown_sublayer.content.get_pointer<NonnullRefPtr<Media::VideoFrame>>())
        keep_video_frame_until_window_server_lets_go(*shown_frame);
    shown_sublayer.content = Empty {};
    [as_layer(shown_sublayer.clip_layer) removeFromSuperlayer];
    [as_layer(shown_sublayer.content_layer) release];
    if (shown_sublayer.rounded_clip_layer)
        [as_layer(shown_sublayer.rounded_clip_layer) release];
    [as_layer(shown_sublayer.clip_layer) release];
}

void RemoteLayer::keep_video_frame_until_window_server_lets_go(NonnullRefPtr<Media::VideoFrame> frame)
{
    m_replaced_video_frames.append({ move(frame), MonotonicTime::now() });
    if (!m_video_frame_release_timer) {
        m_video_frame_release_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(25), [this] {
            release_video_frames_window_server_let_go_of();
        });
    }
    if (!m_video_frame_release_timer->is_active())
        m_video_frame_release_timer->start();
}

// The window server shows a replacement from its next display refresh on, and a composite that started before may
// still be reading the frame replaced. A few refreshes later, even at 60 Hz, it has let go of it.
static constexpr AK::Duration time_window_server_may_read_a_replaced_surface = AK::Duration::from_milliseconds(50);

void RemoteLayer::release_video_frames_window_server_let_go_of()
{
    // A replacement made while changes are held off screen reaches the window server only once they are released.
    if (s_changes_are_held_off_screen)
        return;
    auto now = MonotonicTime::now();
    m_replaced_video_frames.remove_all_matching([&](auto const& replaced) {
        auto shown_on_screen_since = max(replaced.replaced_at, s_changes_last_released_on_screen);
        return now - shown_on_screen_since >= time_window_server_may_read_a_replaced_surface;
    });
    if (m_replaced_video_frames.is_empty() && m_video_frame_release_timer)
        m_video_frame_release_timer->stop();
}

}
