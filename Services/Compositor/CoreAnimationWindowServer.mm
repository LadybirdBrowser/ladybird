/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/HashTable.h>
#include <AK/Math.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <Compositor/CoreAnimationWindowServer.h>
#include <LibCompositing/FrameFitting.h>
#include <LibCore/Timer.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibMediaCodecs/VideoSurface.h>

#include <core/SkPath.h>
#include <core/SkPoint.h>
#include <dlfcn.h>
#include <objc/runtime.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
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

// A CABackdropLayer applies CAFilters to what the window server shows under it. Neither is public API either.
@protocol LadybirdCoreAnimationFilterClass <NSObject>
+ (id)filterWithType:(NSString*)type;
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

static Class core_animation_backdrop_layer_class()
{
    static Class backdrop_layer_class = []() -> Class {
        Class backdrop_layer_class = NSClassFromString(@"CABackdropLayer");
        if (!backdrop_layer_class || ![backdrop_layer_class isSubclassOfClass:[CALayer class]])
            return nil;
        return backdrop_layer_class;
    }();
    return backdrop_layer_class;
}

static Class core_animation_filter_class()
{
    static Class filter_class = []() -> Class {
        Class filter_class = NSClassFromString(@"CAFilter");
        if (!filter_class || ![filter_class respondsToSelector:@selector(filterWithType:)])
            return nil;
        return filter_class;
    }();
    return filter_class;
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
void CoreAnimationWindowServer::hold_changes_on_screen()
{
    [CATransaction begin];
    s_changes_are_held_off_screen = true;
}

void CoreAnimationWindowServer::release_changes_held_off_screen()
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

RefPtr<CoreAnimationWindowServer> CoreAnimationWindowServer::try_create()
{
    if (!main_window_server_connection_id_function() || !core_animation_context_class())
        return nullptr;
    return adopt_ref(*new CoreAnimationWindowServer);
}

OwnPtr<RemoteLayer> CoreAnimationWindowServer::try_create_remote_layer(u64)
{
    return CoreAnimationRemoteLayer::try_create();
}

bool CoreAnimationWindowServer::shows_tiles_of_painted_layers() const
{
    return !showing_tiles_of_painted_layers_is_switched_off();
}

bool CoreAnimationWindowServer::filters_what_is_below_layers() const
{
    return core_animation_backdrop_layer_class() && core_animation_filter_class();
}

CoreAnimationSurface::CoreAnimationSurface(Gfx::SharedImageBuffer const& shared_image_buffer)
    : m_iosurface(Core::IOSurfaceHandle::from_ref(shared_image_buffer.iosurface_handle().core_foundation_pointer()))
{
}

CoreAnimationSurface::CoreAnimationSurface(NonnullOwnPtr<Gfx::SharedImageBuffer> shared_image_buffer)
    : m_owned_shared_image_buffer(move(shared_image_buffer))
    , m_iosurface(Core::IOSurfaceHandle::from_ref(m_owned_shared_image_buffer->iosurface_handle().core_foundation_pointer()))
{
}

CoreAnimationSurface::~CoreAnimationSurface() = default;

ErrorOr<NonnullRefPtr<PresentableSurface>> CoreAnimationWindowServer::try_create_presentable_surface_over_new_iosurface(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto shared_image_buffer = make<Gfx::SharedImageBuffer>(TRY(Gfx::SharedImageBuffer::try_create(size)));
    auto painting_surface = Gfx::PaintingSurface::create_from_shared_image_buffer(*shared_image_buffer, skia_backend_context);
    return PresentableSurface::create(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft, make<CoreAnimationSurface>(move(shared_image_buffer)));
}

ErrorOr<NonnullRefPtr<PresentableSurface>> CoreAnimationWindowServer::try_create_presentable_surface(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    if (skia_backend_context)
        return try_create_presentable_surface_over_new_iosurface(size, *skia_backend_context);
    auto shared_image_buffer = make<Gfx::SharedImageBuffer>(TRY(Gfx::SharedImageBuffer::try_create(size)));
    auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*shared_image_buffer->bitmap());
    return PresentableSurface::create(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft, make<CoreAnimationSurface>(move(shared_image_buffer)));
}

OwnPtr<CoreAnimationRemoteLayer> CoreAnimationRemoteLayer::try_create()
{
    auto* connection_id_function = main_window_server_connection_id_function();
    Class context_class = core_animation_context_class();
    if (!connection_id_function || !context_class)
        return nullptr;

    auto remote_layer = adopt_own(*new CoreAnimationRemoteLayer);
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

CoreAnimationRemoteLayer::~CoreAnimationRemoteLayer()
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

Compositing::RemoteLayerHostHandle CoreAnimationRemoteLayer::host_handle() const
{
    return { .kind = Compositing::RemoteLayerHostKind::CoreAnimationContext, .id = m_core_animation_context_id };
}

bool CoreAnimationRemoteLayer::can_show(WindowServerPlacement const&, Content const& content, double) const
{
    return content.visit(
        [](NonnullRefPtr<PresentableSurface> const& surface) {
            return surface->window_server_surface() != nullptr;
        },
        [](NonnullRefPtr<Media::VideoFrame> const& frame) {
            // A frame that a decoder left in memory has no surface that the window server could read.
            if (frame->surface() == nullptr)
                return false;
            // The window server converts a surface from YCbCr by the matrix it is tagged with, and no tag names the
            // identity matrix that an RGB surface needs.
            return frame->cicp().matrix_coefficients() != Media::MatrixCoefficients::Identity;
        });
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
void CoreAnimationRemoteLayer::lay_out_frame(Optional<Gfx::IntSize> fit_within)
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

void CoreAnimationRemoteLayer::place_frame(Optional<Gfx::IntSize> fit_within)
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

static Core::IOSurfaceHandle const& iosurface_of(PresentableSurface const& surface)
{
    auto const* window_server_surface = surface.window_server_surface();
    VERIFY(window_server_surface);
    return static_cast<CoreAnimationSurface const*>(window_server_surface)->iosurface();
}

bool CoreAnimationRemoteLayer::show_frame(Frame frame)
{
    Core::IOSurfaceHandle const* buffer = nullptr;
    if (frame.store.has_value()) {
        auto& store = frame.store->surface();
        buffer = &iosurface_of(store);
        store.did_hand_to_window_server();
    }
    auto painted_size = frame.painted_size;
    auto device_pixel_ratio = frame.device_pixel_ratio;
    auto fit_within = frame.fit_within;
    auto& sublayers = frame.sublayers;
    Gfx::IntSize surface_size;
    if (buffer) {
        auto const& iosurface = *buffer;
        surface_size = { static_cast<int>(iosurface.width()), static_cast<int>(iosurface.height()) };
        // A store can be larger than the frame painted into it, as while the window is resized.
        painted_size = { min(painted_size.width(), surface_size.width()), min(painted_size.height(), surface_size.height()) };
        if (painted_size.is_empty())
            return true;
        m_painted_size = painted_size;
    }
    if (frame.shows_no_store) {
        if (painted_size.is_empty())
            return true;
        m_painted_size = painted_size;
    }
    if (m_painted_size.is_empty())
        return true;
    m_device_pixel_ratio = device_pixel_ratio;

    commit_layer_changes([&] {
        auto* content_layer = as_layer(m_content_layer);
        if (buffer) {
            content_layer.contentsRect = unit_rect_of_surface_part(content_layer, m_painted_size, surface_size);
            content_layer.contentsScale = device_pixel_ratio;
        }
        lay_out_frame(fit_within);
        if (buffer)
            content_layer.contents = (__bridge id)buffer->core_foundation_pointer();
        else if (frame.shows_no_store && content_layer.contents)
            content_layer.contents = nil;
        show_sublayers(sublayers);
    });
    release_video_frames_window_server_let_go_of();
    return true;
}

static CoreAnimationRemoteLayer::SublayerKind kind_of(RemoteSublayer const& sublayer)
{
    using Kind = CoreAnimationRemoteLayer::SublayerKind;
    return sublayer.content.visit(
        [](RemoteSublayer::Group const&) { return Kind::Group; },
        [](RemoteSublayer::Tile const&) { return Kind::Tile; },
        [](Gfx::Color const&) { return Kind::Color; },
        [](RemoteSublayer::Backdrop const&) { return Kind::Backdrop; },
        [](auto const&) { return Kind::PlacedContent; });
}

CoreAnimationRemoteLayer::ShownSublayer& CoreAnimationRemoteLayer::shown_sublayer_of_kind(u64 identity, SublayerKind kind)
{
    if (auto shown_sublayer = m_shown_sublayers.find(identity); shown_sublayer != m_shown_sublayers.end()) {
        if (shown_sublayer->value.kind == kind)
            return shown_sublayer->value;
        stop_showing_sublayer(shown_sublayer->value);
        m_shown_sublayers.remove(shown_sublayer);
        m_sublayer_got_another_layer = true;
    }
    ShownSublayer shown_sublayer;
    shown_sublayer.kind = kind;
    Class class_of_outermost_layer = kind == SublayerKind::Backdrop ? core_animation_backdrop_layer_class() : [CALayer class];
    VERIFY(class_of_outermost_layer);
    CALayer* outermost_layer = [[class_of_outermost_layer alloc] init];
    outermost_layer.anchorPoint = CGPointZero;
    shown_sublayer.outermost_layer = outermost_layer;
    // A group fades what its sublayers show together, not each of them over the ones below it.
    if (kind == SublayerKind::Group)
        outermost_layer.allowsGroupOpacity = YES;
    if (kind == SublayerKind::PlacedContent) {
        outermost_layer.masksToBounds = YES;
        auto* content_layer = [[CALayer alloc] init];
        content_layer.anchorPoint = CGPointZero;
        [outermost_layer addSublayer:content_layer];
        shown_sublayer.content_layer = content_layer;
    }
    m_shown_sublayers.set(identity, move(shown_sublayer));
    return *m_shown_sublayers.get(identity);
}

void CoreAnimationRemoteLayer::show_sublayers(Vector<RemoteSublayer>& sublayers)
{
    HashTable<u64> shown_identities;
    // Per sublayer, by its index, the identities of the sublayers in it where it is a group, and after those, the
    // identities of the sublayers of the frame itself.
    Vector<Vector<u64>> identities_of_sublayers_by_group_index;
    identities_of_sublayers_by_group_index.resize(sublayers.size() + 1);
    auto index_of_frame = sublayers.size();
    for (size_t index = 0; index < sublayers.size(); ++index) {
        auto& sublayer = sublayers[index];
        auto& shown_sublayer = shown_sublayer_of_kind(sublayer.identity, kind_of(sublayer));
        sublayer.content.visit(
            [&](RemoteSublayer::Group& group) { update_shown_group(shown_sublayer, group); },
            [&](RemoteSublayer::Tile& tile) { update_shown_tile(shown_sublayer, tile, sublayer.content_rect); },
            [&](Gfx::Color& color) { update_shown_color(shown_sublayer, color, sublayer.content_rect); },
            [&](RemoteSublayer::Backdrop& backdrop) { update_shown_backdrop(shown_sublayer, backdrop); },
            [&](auto&) { update_shown_sublayer(shown_sublayer, sublayer); });
        shown_sublayer.shown_at_device_pixel_ratio = m_device_pixel_ratio;
        auto group_index = sublayer.group_index.value_or(index_of_frame);
        // A group comes before the sublayers in it.
        if (group_index != index_of_frame && (group_index >= index || !sublayers[group_index].content.has<RemoteSublayer::Group>()))
            group_index = index_of_frame;
        identities_of_sublayers_by_group_index[group_index].append(sublayer.identity);
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

    auto layers_of = [&](Vector<u64> const& identities) {
        auto* layers = [NSMutableArray arrayWithCapacity:identities.size() + 1];
        for (auto identity : identities)
            [layers addObject:as_layer(m_shown_sublayers.get(identity)->outermost_layer)];
        return layers;
    };
    for (size_t index = 0; index < sublayers.size(); ++index) {
        auto& identities_of_sublayers = identities_of_sublayers_by_group_index[index];
        auto& shown_group = *m_shown_sublayers.get(sublayers[index].identity);
        if (shown_group.kind != SublayerKind::Group)
            continue;
        if (!m_sublayer_got_another_layer && shown_group.identities_of_sublayers == identities_of_sublayers)
            continue;
        as_layer(shown_group.outermost_layer).sublayers = layers_of(identities_of_sublayers);
        shown_group.identities_of_sublayers = move(identities_of_sublayers);
    }
    auto& identities_of_sublayers_of_frame = identities_of_sublayers_by_group_index[index_of_frame];
    if (m_sublayer_got_another_layer || m_identities_of_sublayers_of_frame != identities_of_sublayers_of_frame) {
        auto* layers_of_frame = layers_of(identities_of_sublayers_of_frame);
        [layers_of_frame insertObject:as_layer(m_content_layer) atIndex:0];
        as_layer(m_frame_layer).sublayers = layers_of_frame;
        m_identities_of_sublayers_of_frame = move(identities_of_sublayers_of_frame);
    }
    m_sublayer_got_another_layer = false;
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

static bool holds_high_dynamic_range(Media::CodingIndependentCodePoints cicp)
{
    return cicp.transfer_characteristics() == Media::TransferCharacteristics::SMPTE2084 || cicp.transfer_characteristics() == Media::TransferCharacteristics::HLG;
}

// AVFoundation tone maps PQ and HLG video differently from how the window server maps the same surface as a layer's
// contents, and it is how other browsers and the system's own players show such video, so it shows HDR video here too.
static bool is_shown_through_sample_buffers(RemoteSublayer const& sublayer)
{
    auto const* frame = sublayer.content.get_pointer<NonnullRefPtr<Media::VideoFrame>>();
    return frame && holds_high_dynamic_range((*frame)->cicp());
}

static AVSampleBufferDisplayLayer* as_sample_buffer_layer(CALayer* layer)
{
    if (![layer isKindOfClass:[AVSampleBufferDisplayLayer class]])
        return nil;
    return static_cast<AVSampleBufferDisplayLayer*>(layer);
}

// Extended dynamic range costs power, so a layer asks for it only while it shows content brighter than SDR white. The
// window server then tone maps the content to the headroom of the display it is on.
static void let_layer_show_high_dynamic_range(CALayer* layer, bool shows_high_dynamic_range)
{
    if (@available(macOS 26.0, *)) {
        layer.preferredDynamicRange = shows_high_dynamic_range ? CADynamicRangeHigh : CADynamicRangeStandard;
    } else {
        layer.wantsExtendedDynamicRangeContent = shows_high_dynamic_range;
    }
}

void CoreAnimationRemoteLayer::use_content_layer_of_kind_for(ShownSublayer& shown_sublayer, RemoteSublayer const& sublayer)
{
    auto* content_layer = as_layer(shown_sublayer.content_layer);
    auto wants_sample_buffers = is_shown_through_sample_buffers(sublayer);
    if ((as_sample_buffer_layer(content_layer) != nil) == wants_sample_buffers)
        return;

    CALayer* replacement = nil;
    if (wants_sample_buffers) {
        auto* sample_buffer_layer = [[AVSampleBufferDisplayLayer alloc] init];
        sample_buffer_layer.videoGravity = AVLayerVideoGravityResize;
        sample_buffer_layer.opaque = YES;
        let_layer_show_high_dynamic_range(sample_buffer_layer, true);
        // Other browsers tag HDR video with the 4 stops of headroom of an XDR display.
        if (@available(macOS 26.0, *))
            sample_buffer_layer.contentsHeadroom = 16;
        replacement = sample_buffer_layer;
    } else {
        replacement = [[CALayer alloc] init];
    }
    replacement.anchorPoint = CGPointZero;

    stop_showing_content(shown_sublayer);
    [content_layer.superlayer replaceSublayer:content_layer with:replacement];
    [content_layer release];
    shown_sublayer.content_layer = replacement;
}

void CoreAnimationRemoteLayer::update_shown_sublayer(ShownSublayer& shown_sublayer, RemoteSublayer& sublayer)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    auto* clip_layer = as_layer(shown_sublayer.outermost_layer);
    use_content_layer_of_kind_for(shown_sublayer, sublayer);
    auto* content_layer = as_layer(shown_sublayer.content_layer);
    clip_layer.frame = in_points(sublayer.clip_rect, device_pixel_ratio);
    update_path_clip_mask(shown_sublayer, sublayer);

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

// CoreGraphics has no conic curves, so a conic is drawn as quadratic curves that stray from it by at most this, in the
// units of the space of its path.
static constexpr float conic_tolerance_in_units_of_path = 0.1f;
static constexpr int most_halvings_of_conic_into_quadratic_curves = 5;

// How many times a conic is halved into quadratic curves for them to stay within the tolerance of it, by the bound on
// how far a single quadratic curve strays from it. Each halving quarters how far the curves stray.
static int halvings_of_conic_into_quadratic_curves(SkPoint start, SkPoint control, SkPoint end, float weight)
{
    auto weight_past_parabola = weight - 1;
    auto factor = weight_past_parabola / (4 * (2 + weight_past_parabola));
    auto x = factor * (start.x() - 2 * control.x() + end.x());
    auto y = factor * (start.y() - 2 * control.y() + end.y());
    auto distance_strayed = AK::sqrt(x * x + y * y);
    int halvings = 0;
    while (halvings < most_halvings_of_conic_into_quadratic_curves && distance_strayed > conic_tolerance_in_units_of_path) {
        distance_strayed *= 0.25f;
        ++halvings;
    }
    return halvings;
}

static void append_conic_as_quadratic_curves(CGMutablePathRef outline, Gfx::FloatPoint control, Gfx::FloatPoint end, float weight)
{
    auto current_point = CGPathGetCurrentPoint(outline);
    auto start = SkPoint::Make(current_point.x, current_point.y);
    auto skia_control = SkPoint::Make(control.x(), control.y());
    auto skia_end = SkPoint::Make(end.x(), end.y());
    auto halvings = halvings_of_conic_into_quadratic_curves(start, skia_control, skia_end, weight);
    Array<SkPoint, 1 + 2 * (1 << most_halvings_of_conic_into_quadratic_curves)> quadratic_curve_points;
    auto quadratic_curve_count = SkPath::ConvertConicToQuads(start, skia_control, skia_end, weight, quadratic_curve_points.data(), halvings);
    for (int index = 0; index < quadratic_curve_count; ++index) {
        auto const& curve_control = quadratic_curve_points[1 + 2 * index];
        auto const& curve_end = quadratic_curve_points[2 + 2 * index];
        CGPathAddQuadCurveToPoint(outline, nullptr, curve_control.x(), curve_control.y(), curve_end.x(), curve_end.y());
    }
}

// The outline of the path in its own space.
static CGPathRef create_outline_of_path(RemoteSublayer::PathClip const& path_clip)
{
    auto* outline = CGPathCreateMutable();
    auto const& points = path_clip.points;
    size_t index_of_next_point = 0;
    size_t index_of_next_conic_weight = 0;
    auto next_point = [&] { return points[index_of_next_point++]; };
    for (auto verb : path_clip.verbs) {
        switch (verb) {
        case Gfx::PathVerb::Move: {
            auto point = next_point();
            CGPathMoveToPoint(outline, nullptr, point.x(), point.y());
            break;
        }
        case Gfx::PathVerb::Line: {
            auto point = next_point();
            CGPathAddLineToPoint(outline, nullptr, point.x(), point.y());
            break;
        }
        case Gfx::PathVerb::Quad: {
            auto control = next_point();
            auto end = next_point();
            CGPathAddQuadCurveToPoint(outline, nullptr, control.x(), control.y(), end.x(), end.y());
            break;
        }
        case Gfx::PathVerb::Conic: {
            auto control = next_point();
            auto end = next_point();
            append_conic_as_quadratic_curves(outline, control, end, path_clip.conic_weights[index_of_next_conic_weight++]);
            break;
        }
        case Gfx::PathVerb::Cubic: {
            auto first_control = next_point();
            auto second_control = next_point();
            auto end = next_point();
            CGPathAddCurveToPoint(outline, nullptr, first_control.x(), first_control.y(), second_control.x(), second_control.y(), end.x(), end.y());
            break;
        }
        case Gfx::PathVerb::Close:
            CGPathCloseSubpath(outline);
            break;
        }
    }
    return outline;
}

// The mask stays in the space of the path, and its transform maps that space into the clip layer, whose points have
// their origin at the origin of the clip rect: from the path's space to the frame's pixels, then to pixels from the
// origin of the clip rect, then to points. With its anchor point and position at the origin, the mask is placed by
// that transform alone.
void CoreAnimationRemoteLayer::update_path_clip_mask(ShownSublayer& shown_sublayer, RemoteSublayer const& sublayer)
{
    auto* clip_layer = as_layer(shown_sublayer.outermost_layer);
    if (!sublayer.path_clip.has_value()) {
        if (shown_sublayer.path_clip_mask_layer) {
            clip_layer.mask = nil;
            [as_layer(shown_sublayer.path_clip_mask_layer) release];
            shown_sublayer.path_clip_mask_layer = nullptr;
            shown_sublayer.identity_of_path_in_clip_mask = {};
        }
        return;
    }
    auto const& path_clip = *sublayer.path_clip;
    if (!shown_sublayer.path_clip_mask_layer) {
        auto* mask_layer = [[CAShapeLayer alloc] init];
        mask_layer.anchorPoint = CGPointZero;
        mask_layer.position = CGPointZero;
        clip_layer.mask = mask_layer;
        shown_sublayer.path_clip_mask_layer = mask_layer;
    }
    auto* mask_layer = static_cast<CAShapeLayer*>(shown_sublayer.path_clip_mask_layer);
    if (shown_sublayer.identity_of_path_in_clip_mask != path_clip.path_identity) {
        auto* outline = create_outline_of_path(path_clip);
        mask_layer.path = outline;
        CGPathRelease(outline);
        shown_sublayer.identity_of_path_in_clip_mask = path_clip.path_identity;
    }
    auto* fill_rule = path_clip.winding_rule == Gfx::WindingRule::EvenOdd ? kCAFillRuleEvenOdd : kCAFillRuleNonZero;
    if (![mask_layer.fillRule isEqualToString:fill_rule])
        mask_layer.fillRule = fill_rule;

    auto device_pixel_ratio = m_device_pixel_ratio;
    auto origin_of_clip_rect = sublayer.clip_rect.location();
    auto path_to_clip_layer = to_core_graphics(path_clip.path_to_frame);
    path_to_clip_layer = CGAffineTransformConcat(path_to_clip_layer, CGAffineTransformMakeTranslation(-origin_of_clip_rect.x(), -origin_of_clip_rect.y()));
    path_to_clip_layer = CGAffineTransformConcat(path_to_clip_layer, CGAffineTransformMakeScale(1 / device_pixel_ratio, 1 / device_pixel_ratio));
    mask_layer.affineTransform = path_to_clip_layer;
}

static CGPoint in_points(Gfx::FloatPoint point, double device_pixel_ratio)
{
    return CGPointMake(point.x() / device_pixel_ratio, point.y() / device_pixel_ratio);
}

static bool places_alike(RemoteSublayer::Group const& a, RemoteSublayer::Group const& b)
{
    auto const& transform = a.to_parent;
    auto const& other_transform = b.to_parent;
    return transform.a() == other_transform.a() && transform.b() == other_transform.b() && transform.c() == other_transform.c()
        && transform.d() == other_transform.d() && transform.e() == other_transform.e() && transform.f() == other_transform.f()
        && a.clip_rect == b.clip_rect;
}

static bool rounds_corners_alike(RemoteSublayer::Group const& a, RemoteSublayer::Group const& b)
{
    auto size_clipped_to = [](RemoteSublayer::Group const& group) { return group.clip_rect.map([](auto const& clip_rect) { return clip_rect.size(); }); };
    return a.corner_radii == b.corner_radii && size_clipped_to(a) == size_clipped_to(b);
}

// The one radius that every rounded corner has, or none where two rounded corners differ or one is elliptical.
static Optional<int> single_radius_of_rounded_corners(Gfx::CornerRadii const& corner_radii)
{
    Optional<int> single_radius;
    for (auto const& corner : { corner_radii.top_left, corner_radii.top_right, corner_radii.bottom_right, corner_radii.bottom_left }) {
        if (!corner)
            continue;
        if (corner.horizontal_radius != corner.vertical_radius || single_radius.value_or(corner.horizontal_radius) != corner.horizontal_radius)
            return {};
        single_radius = corner.horizontal_radius;
    }
    return single_radius.value_or(0);
}

// The outline of a rect of the size whose corners are each rounded by a quarter of an ellipse, in the coordinates of a
// layer whose y-axis points down.
static CGPathRef create_outline_of_rect_with_rounded_corners(CGSize size, Gfx::CornerRadii const& corner_radii, double device_pixel_ratio)
{
    // How far along a radius the control points of the cubic curve nearest a quarter of an ellipse are.
    static constexpr CGFloat control_point_distance_along_radius = 0.5522847498;
    auto radii_in_points = [&](Gfx::CornerRadius const& corner) {
        if (!corner)
            return CGSizeZero;
        return CGSizeMake(min(corner.horizontal_radius / device_pixel_ratio, size.width), min(corner.vertical_radius / device_pixel_ratio, size.height));
    };
    auto top_left = radii_in_points(corner_radii.top_left);
    auto top_right = radii_in_points(corner_radii.top_right);
    auto bottom_right = radii_in_points(corner_radii.bottom_right);
    auto bottom_left = radii_in_points(corner_radii.bottom_left);
    auto distance_to_control_point = [](CGFloat radius) { return radius * (1 - control_point_distance_along_radius); };
    auto* outline = CGPathCreateMutable();
    CGPathMoveToPoint(outline, nullptr, top_left.width, 0);
    CGPathAddLineToPoint(outline, nullptr, size.width - top_right.width, 0);
    CGPathAddCurveToPoint(outline, nullptr, size.width - distance_to_control_point(top_right.width), 0, size.width, distance_to_control_point(top_right.height), size.width, top_right.height);
    CGPathAddLineToPoint(outline, nullptr, size.width, size.height - bottom_right.height);
    CGPathAddCurveToPoint(outline, nullptr, size.width, size.height - distance_to_control_point(bottom_right.height), size.width - distance_to_control_point(bottom_right.width), size.height, size.width - bottom_right.width, size.height);
    CGPathAddLineToPoint(outline, nullptr, bottom_left.width, size.height);
    CGPathAddCurveToPoint(outline, nullptr, distance_to_control_point(bottom_left.width), size.height, 0, size.height - distance_to_control_point(bottom_left.height), 0, size.height - bottom_left.height);
    CGPathAddLineToPoint(outline, nullptr, 0, top_left.height);
    CGPathAddCurveToPoint(outline, nullptr, 0, distance_to_control_point(top_left.height), distance_to_control_point(top_left.width), 0, top_left.width, 0);
    CGPathCloseSubpath(outline);
    return outline;
}

// A group layer has the rect the group clips to as its bounds, or no bounds at its origin, so that its sublayers are
// laid out in the pixels of the group. It is placed by where the origin of those bounds goes, with the rest of the
// group's transform around that point. A frame that only moves the group, as a scroll does, sets nothing else.
void CoreAnimationRemoteLayer::update_shown_group(ShownSublayer& shown_sublayer, RemoteSublayer::Group const& group)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    auto const* shown_group = shown_sublayer.shown_group.has_value() && shown_sublayer.shown_at_device_pixel_ratio == device_pixel_ratio ? &shown_sublayer.shown_group.value() : nullptr;
    auto* group_layer = as_layer(shown_sublayer.outermost_layer);
    auto bounds = in_points(group.clip_rect.value_or({}), device_pixel_ratio);
    if (!shown_group || !places_alike(*shown_group, group)) {
        auto const& to_parent = group.to_parent;
        group_layer.bounds = bounds;
        group_layer.position = in_points(to_parent.map(group.clip_rect.value_or({}).location()), device_pixel_ratio);
        group_layer.affineTransform = CGAffineTransformMake(to_parent.a(), to_parent.b(), to_parent.c(), to_parent.d(), 0, 0);
        group_layer.masksToBounds = group.clip_rect.has_value();
    }
    if (!shown_group || shown_group->opacity != group.opacity)
        group_layer.opacity = group.opacity;
    if (!shown_group || !rounds_corners_alike(*shown_group, group)) {
        // The window server rounds the corners of a layer by one radius. Corners that differ, or that are elliptical,
        // are cut by a mask of their outline instead, which costs it a pass of its own.
        auto const& corner_radii = group.corner_radii;
        auto single_radius = single_radius_of_rounded_corners(corner_radii);
        group_layer.cornerRadius = single_radius.value_or(0) / device_pixel_ratio;
        // The corners are named in the layer's own coordinates, whose y-axis points down here like the frame's.
        CACornerMask masked_corners = 0;
        if (corner_radii.top_left)
            masked_corners |= kCALayerMinXMinYCorner;
        if (corner_radii.top_right)
            masked_corners |= kCALayerMaxXMinYCorner;
        if (corner_radii.bottom_right)
            masked_corners |= kCALayerMaxXMaxYCorner;
        if (corner_radii.bottom_left)
            masked_corners |= kCALayerMinXMaxYCorner;
        group_layer.maskedCorners = masked_corners;
        if (single_radius.has_value() || !group.clip_rect.has_value()) {
            group_layer.mask = nil;
        } else {
            auto* outline_layer = [CAShapeLayer layer];
            outline_layer.anchorPoint = CGPointZero;
            auto* outline = create_outline_of_rect_with_rounded_corners(bounds.size, corner_radii, device_pixel_ratio);
            outline_layer.path = outline;
            CGPathRelease(outline);
            group_layer.mask = outline_layer;
        }
    }
    // The outline is laid out in the pixels of the group, as its sublayers are.
    if (group_layer.mask)
        group_layer.mask.frame = bounds;
    shown_sublayer.shown_group = group;
}

void CoreAnimationRemoteLayer::update_shown_tile(ShownSublayer& shown_sublayer, RemoteSublayer::Tile& tile, Gfx::IntRect rect)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    auto* tile_layer = as_layer(shown_sublayer.outermost_layer);
    if (shown_sublayer.shown_rect != rect || shown_sublayer.shown_at_device_pixel_ratio != device_pixel_ratio) {
        tile_layer.frame = in_points(rect, device_pixel_ratio);
        shown_sublayer.shown_rect = rect;
    }
    if (shown_sublayer.shown_as_opaque != tile.every_pixel_is_opaque) {
        tile_layer.opaque = tile.every_pixel_is_opaque;
        shown_sublayer.shown_as_opaque = tile.every_pixel_is_opaque;
    }
    auto const* shown_lease = shown_sublayer.content.get_pointer<PresentableSurface::Lease>();
    if (shown_lease && &shown_lease->surface() == &tile.surface.surface())
        return;
    auto& surface = tile.surface.surface();
    surface.did_hand_to_window_server();
    tile_layer.contents = (__bridge id)iosurface_of(surface).core_foundation_pointer();
    shown_sublayer.content = move(tile.surface);
}

void CoreAnimationRemoteLayer::update_shown_color(ShownSublayer& shown_sublayer, Gfx::Color color, Gfx::IntRect rect)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    auto* color_layer = as_layer(shown_sublayer.outermost_layer);
    if (shown_sublayer.shown_rect != rect || shown_sublayer.shown_at_device_pixel_ratio != device_pixel_ratio) {
        color_layer.frame = in_points(rect, device_pixel_ratio);
        shown_sublayer.shown_rect = rect;
    }
    if (shown_sublayer.shown_color == color)
        return;
    auto* core_graphics_color = CGColorCreateSRGB(color.red() / 255.0, color.green() / 255.0, color.blue() / 255.0, color.alpha() / 255.0);
    color_layer.backgroundColor = core_graphics_color;
    CGColorRelease(core_graphics_color);
    color_layer.opaque = color.alpha() == 255;
    shown_sublayer.shown_color = color;
}

// The window server blurs by a standard deviation in points, and fills in what lies past the edge of a backdrop by
// normalizing, which comes closest to mirroring at the edge. A CAColorMatrix holds the same 20 floats in the same
// order, and the window server applies it to unpremultiplied colours as well.
static NSArray* core_animation_filters_of(RemoteSublayer::Backdrop const& backdrop, double device_pixel_ratio)
{
    auto filter_class = (Class<LadybirdCoreAnimationFilterClass>)core_animation_filter_class();
    auto* core_animation_filters = [NSMutableArray arrayWithCapacity:backdrop.filters.size()];
    for (auto const& filter : backdrop.filters) {
        switch (filter.kind) {
        case RemoteSublayer::Backdrop::Filter::Kind::Blur: {
            id blur = [filter_class filterWithType:@"gaussianBlur"];
            [blur setValue:@(filter.blur_standard_deviation / device_pixel_ratio) forKey:@"inputRadius"];
            [blur setValue:@YES forKey:@"inputNormalizeEdges"];
            [core_animation_filters addObject:blur];
            break;
        }
        case RemoteSublayer::Backdrop::Filter::Kind::ColorMatrix: {
            id color_matrix = [filter_class filterWithType:@"colorMatrix"];
            [color_matrix setValue:[NSValue valueWithBytes:filter.color_matrix.data() objCType:"{CAColorMatrix=ffffffffffffffffffff}"] forKey:@"inputColorMatrix"];
            [core_animation_filters addObject:color_matrix];
            break;
        }
        }
    }
    return core_animation_filters;
}

void CoreAnimationRemoteLayer::update_shown_backdrop(ShownSublayer& shown_sublayer, RemoteSublayer::Backdrop const& backdrop)
{
    auto device_pixel_ratio = m_device_pixel_ratio;
    if (shown_sublayer.shown_backdrop == backdrop && shown_sublayer.shown_at_device_pixel_ratio == device_pixel_ratio)
        return;
    auto* backdrop_layer = as_layer(shown_sublayer.outermost_layer);
    backdrop_layer.frame = in_points(backdrop.region, device_pixel_ratio);
    backdrop_layer.filters = core_animation_filters_of(backdrop, device_pixel_ratio);
    shown_sublayer.shown_backdrop = backdrop;
}

// The window server converts a YCbCr surface to RGB by the matrix it is tagged with. It decodes the transfer of SDR
// video by a curve of its own whatever the transfer is tagged as, but the colour space of the surface overrides that,
// and naming it after sRGB draws the video as the frame draws it. PQ and HLG video is tagged as what it is.
static void describe_colours_of_video_surface(IOSurfaceRef surface, Media::CodingIndependentCodePoints cicp)
{
    auto matrix = CVYCbCrMatrixGetStringForIntegerCodePoint(to_underlying(cicp.matrix_coefficients()));
    auto primaries = CVColorPrimariesGetStringForIntegerCodePoint(to_underlying(cicp.color_primaries()));
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceYCbCrMatrix"), matrix ? matrix : kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceColorPrimaries"), primaries ? primaries : kCVImageBufferColorPrimaries_ITU_R_709_2);

    CFStringRef transfer_function = kCVImageBufferTransferFunction_sRGB;
    CFStringRef color_space_name = kCGColorSpaceSRGB;
    // The colour spaces named after ITU-R BT.2100 have BT.2020 primaries, so Display P3 video has names of its own.
    auto has_display_p3_primaries = cicp.color_primaries() == Media::ColorPrimaries::SMPTE432;
    if (cicp.transfer_characteristics() == Media::TransferCharacteristics::SMPTE2084) {
        transfer_function = kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
        color_space_name = has_display_p3_primaries ? kCGColorSpaceDisplayP3_PQ : kCGColorSpaceITUR_2100_PQ;
    } else if (cicp.transfer_characteristics() == Media::TransferCharacteristics::HLG) {
        transfer_function = kCVImageBufferTransferFunction_ITU_R_2100_HLG;
        color_space_name = has_display_p3_primaries ? kCGColorSpaceDisplayP3_HLG : kCGColorSpaceITUR_2100_HLG;
    } else if (cicp.color_primaries() == Media::ColorPrimaries::SMPTE432) {
        color_space_name = kCGColorSpaceDisplayP3;
    } else if (cicp.color_primaries() == Media::ColorPrimaries::BT2020) {
        color_space_name = kCGColorSpaceITUR_2020_sRGBGamma;
    }
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceTransferFunction"), transfer_function);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceColorSpace"), color_space_name);
}

static void set_pixel_buffer_attachment(CVPixelBufferRef pixel_buffer, CFStringRef key, CFTypeRef value)
{
    if (value)
        CVBufferSetAttachment(pixel_buffer, key, value, kCVAttachmentMode_ShouldPropagate);
}

// Chromium gives HLG video an ambient viewing environment, whose values it found not to change how AVFoundation shows
// the video. This is the one that iPhone HDR video carries.
// https://developer.apple.com/documentation/technotes/tn3145-hdr-video-metadata
static CFDataRef create_ambient_viewing_environment()
{
    static constexpr u32 illuminance_in_ten_thousandths_of_lux = 3'140'000;
    static constexpr u16 white_point_x = 15635;
    static constexpr u16 white_point_y = 16450;
    u8 const bytes[] = {
        illuminance_in_ten_thousandths_of_lux >> 24, (illuminance_in_ten_thousandths_of_lux >> 16) & 0xff,
        (illuminance_in_ten_thousandths_of_lux >> 8) & 0xff, illuminance_in_ten_thousandths_of_lux & 0xff,
        white_point_x >> 8, white_point_x & 0xff,
        white_point_y >> 8, white_point_y & 0xff
    };
    return CFDataCreate(kCFAllocatorDefault, bytes, sizeof(bytes));
}

template<Unsigned T>
static void append_big_endian(Vector<u8, 24>& bytes, T value)
{
    for (size_t i = sizeof(T); i > 0; i--)
        bytes.append((value >> ((i - 1) * 8)) & 0xff);
}

// The payload of an HEVC mastering display colour volume SEI message, which lists the primaries as green, blue, red.
static CFDataRef create_mastering_display_color_volume(Media::MasteringDisplayColorVolume const& volume)
{
    constexpr float chromaticity_units_per_unit = 50'000;
    constexpr float luminance_units_per_candela_per_square_metre = 10'000;
    Vector<u8, 24> bytes;
    for (auto const& chromaticity : { volume.green, volume.blue, volume.red, volume.white_point }) {
        append_big_endian(bytes, round_to<u16>(chromaticity.x * chromaticity_units_per_unit));
        append_big_endian(bytes, round_to<u16>(chromaticity.y * chromaticity_units_per_unit));
    }
    append_big_endian(bytes, round_to<u32>(volume.maximum_luminance * luminance_units_per_candela_per_square_metre));
    append_big_endian(bytes, round_to<u32>(volume.minimum_luminance * luminance_units_per_candela_per_square_metre));
    VERIFY(bytes.size() == 24);
    return CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size()));
}

static CFDataRef create_content_light_level_information(Media::ContentLightLevelInformation const& information)
{
    Vector<u8, 24> bytes;
    append_big_endian(bytes, information.maximum_content_light_level);
    append_big_endian(bytes, information.maximum_frame_average_light_level);
    return CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size()));
}

static void enqueue_video_surface(AVSampleBufferDisplayLayer* layer, IOSurfaceRef surface, Media::ColorMetadata const& color_metadata, Gfx::IntSize shown_size)
{
    auto const& cicp = color_metadata.cicp;
    CVPixelBufferRef pixel_buffer = nullptr;
    if (auto result = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, nullptr, &pixel_buffer); result != kCVReturnSuccess) {
        dbgln("Failed to wrap a video surface in a pixel buffer: {}", result);
        return;
    }
    ScopeGuard release_pixel_buffer = [&] { CVPixelBufferRelease(pixel_buffer); };

    set_pixel_buffer_attachment(pixel_buffer, kCVImageBufferColorPrimariesKey, CVColorPrimariesGetStringForIntegerCodePoint(to_underlying(cicp.color_primaries())));
    set_pixel_buffer_attachment(pixel_buffer, kCVImageBufferTransferFunctionKey, CVTransferFunctionGetStringForIntegerCodePoint(to_underlying(cicp.transfer_characteristics())));
    set_pixel_buffer_attachment(pixel_buffer, kCVImageBufferYCbCrMatrixKey, CVYCbCrMatrixGetStringForIntegerCodePoint(to_underlying(cicp.matrix_coefficients())));
    if (cicp.transfer_characteristics() == Media::TransferCharacteristics::HLG) {
        if (auto const* ambient_viewing_environment = create_ambient_viewing_environment()) {
            set_pixel_buffer_attachment(pixel_buffer, kCVImageBufferAmbientViewingEnvironmentKey, ambient_viewing_environment);
            CFRelease(ambient_viewing_environment);
        }
    }
    if (auto const& volume = color_metadata.mastering_display_color_volume; volume.has_value()) {
        if (auto const* mastering_display_color_volume = create_mastering_display_color_volume(volume.value())) {
            set_pixel_buffer_attachment(pixel_buffer, kCVImageBufferMasteringDisplayColorVolumeKey, mastering_display_color_volume);
            CFRelease(mastering_display_color_volume);
        }
    }
    if (auto const& information = color_metadata.content_light_level_information; information.has_value() && information->maximum_content_light_level != 0 && information->maximum_frame_average_light_level != 0) {
        if (auto const* content_light_level_information = create_content_light_level_information(information.value())) {
            set_pixel_buffer_attachment(pixel_buffer, kCVImageBufferContentLightLevelInfoKey, content_light_level_information);
            CFRelease(content_light_level_information);
        }
    }

    // A decoder's surface can be larger than its picture, and the clean aperture is centred in the surface.
    Gfx::IntSize surface_size { static_cast<int>(IOSurfaceGetWidth(surface)), static_cast<int>(IOSurfaceGetHeight(surface)) };
    if (shown_size != surface_size) {
        NSDictionary* clean_aperture = @{
            (id)kCVImageBufferCleanApertureWidthKey : @(shown_size.width()),
            (id)kCVImageBufferCleanApertureHeightKey : @(shown_size.height()),
            (id)kCVImageBufferCleanApertureHorizontalOffsetKey : @((shown_size.width() - surface_size.width()) / 2.0),
            (id)kCVImageBufferCleanApertureVerticalOffsetKey : @((shown_size.height() - surface_size.height()) / 2.0),
        };
        set_pixel_buffer_attachment(pixel_buffer, kCVImageBufferCleanApertureKey, (__bridge CFDictionaryRef)clean_aperture);
    }

    CMVideoFormatDescriptionRef format_description = nullptr;
    if (auto result = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel_buffer, &format_description); result != noErr) {
        dbgln("Failed to describe the format of a video surface: {}", result);
        return;
    }
    ScopeGuard release_format_description = [&] { CFRelease(format_description); };

    // The time is unused, as the sample is shown as soon as it is enqueued.
    CMSampleTimingInfo timing { .duration = kCMTimeInvalid, .presentationTimeStamp = kCMTimeZero, .decodeTimeStamp = kCMTimeInvalid };
    CMSampleBufferRef sample_buffer = nullptr;
    if (auto result = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pixel_buffer, format_description, &timing, &sample_buffer); result != noErr) {
        dbgln("Failed to make a sample buffer of a video surface: {}", result);
        return;
    }
    ScopeGuard release_sample_buffer = [&] { CFRelease(sample_buffer); };
    auto const* attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer, true);
    VERIFY(attachments && CFArrayGetCount(attachments) > 0);
    CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0))), kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);

    auto* renderer = layer.sampleBufferRenderer;
    // The renderer stops after failing, or after the system took away its decoding resources, until it is flushed.
    if (renderer.status == AVQueuedSampleBufferRenderingStatusFailed || renderer.requiresFlushToResumeDecoding)
        [renderer flush];
    [renderer enqueueSampleBuffer:sample_buffer];
    if (renderer.status == AVQueuedSampleBufferRenderingStatusFailed)
        dbgln("AVFoundation failed to show a video surface: {}", renderer.error.description.UTF8String);
}

void CoreAnimationRemoteLayer::show_content(ShownSublayer& shown_sublayer, RemoteSublayer& sublayer)
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
            auto* surface = static_cast<IOSurfaceRef>(iosurface.core_foundation_pointer());
            describe_colours_of_video_surface(surface, frame->cicp());
            Gfx::IntSize surface_size { static_cast<int>(iosurface.width()), static_cast<int>(iosurface.height()) };
            auto visible_size = frame->size().to_type<int>();
            Gfx::IntSize shown_size { min(visible_size.width(), surface_size.width()), min(visible_size.height(), surface_size.height()) };
            if (auto* sample_buffer_layer = as_sample_buffer_layer(content_layer)) {
                enqueue_video_surface(sample_buffer_layer, surface, frame->color_metadata(), shown_size);
            } else {
                content_layer.contentsRect = unit_rect_of_surface_part(content_layer, shown_size, surface_size);
                content_layer.opaque = YES;
                content_layer.contents = (__bridge id)surface;
            }
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
            auto const& iosurface = iosurface_of(surface);
            surface.did_hand_to_window_server();
            content_layer.contentsRect = CGRectMake(0, 0, 1, 1);
            content_layer.opaque = NO;
            content_layer.contents = (__bridge id)iosurface.core_foundation_pointer();
            shown_sublayer.content = move(lease);
        },
        [](auto&) {
            VERIFY_NOT_REACHED();
        });
}

void CoreAnimationRemoteLayer::stop_showing_content(ShownSublayer& shown_sublayer)
{
    if (auto const* shown_frame = shown_sublayer.content.get_pointer<NonnullRefPtr<Media::VideoFrame>>())
        keep_video_frame_until_window_server_lets_go(*shown_frame);
    shown_sublayer.content = Empty {};
    if (auto* sample_buffer_layer = as_sample_buffer_layer(as_layer(shown_sublayer.content_layer)))
        [sample_buffer_layer.sampleBufferRenderer flushWithRemovalOfDisplayedImage:YES completionHandler:nil];
}

void CoreAnimationRemoteLayer::stop_showing_sublayer(ShownSublayer& shown_sublayer)
{
    if (shown_sublayer.kind == SublayerKind::PlacedContent)
        stop_showing_content(shown_sublayer);
    shown_sublayer.content = Empty {};
    [as_layer(shown_sublayer.outermost_layer) removeFromSuperlayer];
    if (shown_sublayer.content_layer)
        [as_layer(shown_sublayer.content_layer) release];
    if (shown_sublayer.rounded_clip_layer)
        [as_layer(shown_sublayer.rounded_clip_layer) release];
    if (shown_sublayer.path_clip_mask_layer)
        [as_layer(shown_sublayer.path_clip_mask_layer) release];
    [as_layer(shown_sublayer.outermost_layer) release];
}

void CoreAnimationRemoteLayer::keep_video_frame_until_window_server_lets_go(NonnullRefPtr<Media::VideoFrame> frame)
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

void CoreAnimationRemoteLayer::release_video_frames_window_server_let_go_of()
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
