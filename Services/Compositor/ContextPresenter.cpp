/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Math.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/ContextPresenter.h>
#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/PausedDebuggerOverlay.h>
#include <Compositor/PictureInPictureControls.h>
#include <Compositor/SkiaUtils.h>
#include <LibCompositing/PictureInPictureControls.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoSurface.h>
#include <core/SkCanvas.h>
#include <core/SkImage.h>
#include <math.h>

namespace Compositor {

static Gfx::FloatSize bounded_composited_raster_scale(Gfx::FloatSize scale, Gfx::IntSize viewport_size)
{
    if (viewport_size.is_empty())
        return { 1, 1 };

    scale = { clamp(scale.width(), 0.25f, 5.0f), clamp(scale.height(), 0.25f, 5.0f) };
    // Reserve a pixel on each axis for fractional placement. The two backing stores together use at most
    // 128 MiB, even when an embedding transform magnifies a large iframe.
    constexpr float maximum_dimension = 16384;
    constexpr float maximum_pixels = 16 * 1024 * 1024;
    scale.set_width(min(scale.width(), (maximum_dimension - 1) / viewport_size.width()));
    scale.set_height(min(scale.height(), (maximum_dimension - 1) / viewport_size.height()));
    auto pixel_count = (viewport_size.width() * scale.width()) * (viewport_size.height() * scale.height());
    constexpr float maximum_content_pixels = maximum_pixels - 2 * maximum_dimension - 1;
    if (pixel_count > maximum_content_pixels)
        scale.scale_by(sqrtf(maximum_content_pixels / pixel_count));
    return scale;
}

static bool video_frame_holds_high_dynamic_range(Media::VideoFrame const& frame)
{
    auto transfer_characteristics = frame.cicp().transfer_characteristics();
    return transfer_characteristics == Media::TransferCharacteristics::SMPTE2084 || transfer_characteristics == Media::TransferCharacteristics::HLG;
}

ContextPresenter::ContextPresenter(OwnedCanvasSurfaces canvas_surfaces, bool presents_to_client)
    : m_canvas_surfaces(canvas_surfaces)
    , m_presents_to_client(presents_to_client)
{
}

ContextPresenter::~ContextPresenter()
{
    stop_backing_store_shrink_timer();
    stop_surplus_backing_store_retirement_timer();
    stop_layer_drop_timer();
}

void ContextPresenter::set_display_list(NonnullRefPtr<Compositing::DisplayList const> display_list)
{
    m_display_list = move(display_list);
}

void ContextPresenter::apply_resource_transaction(Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    m_raster_cache.evict(m_resource_storage.apply_transaction(move(resource_transaction)));
}

void ContextPresenter::set_viewport_size(Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    m_viewport_size = viewport_size;
    m_device_pixel_ratio = device_pixel_ratio;
    m_window_resize_in_progress = window_resize_in_progress;
}

bool ContextPresenter::should_shrink_backing_stores_after_resize() const
{
    return m_window_resize_in_progress == Compositing::WindowResizingInProgress::Yes;
}

void ContextPresenter::schedule_backing_store_shrink(Function<void()> on_timeout)
{
    if (!m_backing_store_shrink_timer)
        m_backing_store_shrink_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(3000), move(on_timeout));
    m_backing_store_shrink_timer->restart();
}

void ContextPresenter::finish_window_resize()
{
    m_window_resize_in_progress = Compositing::WindowResizingInProgress::No;
}

void ContextPresenter::set_ui_overlays(Compositing::UIOverlays overlays)
{
    m_ui_overlays = move(overlays);

    auto const& picture_in_picture_controls = m_ui_overlays.picture_in_picture_controls;
    auto shows_picture_in_picture_controls = picture_in_picture_controls.has_value() && picture_in_picture_controls->shown;
    m_picture_in_picture_controls_fade.fade_to(shows_picture_in_picture_controls ? 1 : 0, MonotonicTime::now());
}

Optional<PendingFrame> ContextPresenter::advance_ui_overlay_fades(MonotonicTime frame_time)
{
    if (!m_picture_in_picture_controls_fade.advance_to(frame_time))
        return {};

    auto viewport_rect = viewport_rect_for_ui_overlay();
    if (!viewport_rect.has_value())
        return {};
    auto geometry = Compositing::picture_in_picture_controls_geometry(m_viewport_size, m_device_pixel_ratio);
    return PendingFrame { *viewport_rect, Compositing::overlay_buttons_bounds(geometry.buttons()) };
}

Optional<Gfx::IntRect> ContextPresenter::viewport_rect_for_ui_overlay() const
{
    if (m_viewport_size.is_empty())
        return {};

    auto viewport_rect = m_pending_present_frame.has_value()
        ? m_pending_present_frame->viewport_rect
        : m_presented_frame.value_or(Gfx::IntRect {});
    viewport_rect.set_size(m_viewport_size);
    return viewport_rect;
}

ErrorOr<Optional<BackingStoreManager::Publication>> ContextPresenter::resize_backing_stores_if_needed(RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, BackingStoreManager::GpuSharing gpu_sharing, BackingStoreManager::PublicationMode publication_mode, RefPtr<WindowServer> window_server_showing_stores)
{
    if (m_backing_store_manager.is_rendering())
        return Optional<BackingStoreManager::Publication> {};
    if (!presents_to_client()) {
        // A resize can arrive before the parent's next replay supplies the new embedding transform.
        // Bound the existing embedding's scale for the new viewport before allocating backing stores. Bounding the
        // already bounded scale instead can round it differently, which would discard the raster on every replay.
        auto scale = bounded_composited_raster_scale(m_unbounded_raster_scale_of_embedding, m_viewport_size);
        if (m_raster_scale != scale) {
            m_raster_scale = scale;
            m_latest_rendered_surface = nullptr;
            m_last_rasterized_frame.clear();
        }
    }
    if (!presents_to_client())
        publication_mode = BackingStoreManager::PublicationMode::Unpublished;
    auto allocation = m_backing_store_manager.resize_backing_stores_if_needed(raster_size(), m_window_resize_in_progress, publication_mode);
    if (!allocation.has_value())
        return Optional<BackingStoreManager::Publication> {};
    m_latest_rendered_surface = nullptr;
    m_last_rasterized_frame.clear();
    return m_backing_store_manager.allocate_backing_stores(*allocation, skia_backend_context, publication_mode, gpu_sharing, move(window_server_showing_stores));
}

void ContextPresenter::invalidate_backing_stores()
{
    m_backing_store_manager.invalidate();
    m_frame_without_store_in_flight = false;
}

Optional<BackingStoreManager::Publication> ContextPresenter::add_backing_store_for_pending_frame_if_needed(RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    if (!presents_to_client() || !m_pending_present_frame.has_value() || !can_render_frame())
        return {};
    return m_backing_store_manager.add_backing_store_if_window_server_still_reads_every_released_store(skia_backend_context);
}

void ContextPresenter::schedule_surplus_backing_store_retirement(Function<void()> on_timeout)
{
    if (!m_surplus_backing_store_retirement_timer)
        m_surplus_backing_store_retirement_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(3000), move(on_timeout));
    if (!m_surplus_backing_store_retirement_timer->is_active())
        m_surplus_backing_store_retirement_timer->start();
}

bool ContextPresenter::acknowledge_presented_bitmap(i32 bitmap_id)
{
    return m_backing_store_manager.release_buffer(bitmap_id);
}

void ContextPresenter::did_finish_gpu_present(i32 bitmap_id, Gfx::IntSize frame_size)
{
    m_backing_store_manager.complete_rendering(bitmap_id);
    m_latest_rendered_surface = m_backing_store_manager.latest_rendered_surface();
    m_size_of_latest_rendered_frame = frame_size;
}

bool ContextPresenter::update_composited_raster_transform(Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& transform)
{
    if (presents_to_client() || m_backing_store_manager.is_rendering() || m_viewport_size.is_empty() || destination_rect.is_empty())
        return false;

    // Display list coordinates already include device scale and page zoom. Only the embedding transform and
    // the viewport-to-destination mapping belong here; neither changes the child's layout or scroll geometry.
    auto has_perspective = transform[3, 0] != 0 || transform[3, 1] != 0 || transform[3, 3] != 1;
    float scale_x = 1;
    float scale_y = 1;
    if (!has_perspective) {
        scale_x = AK::hypot<float>(transform[0, 0], transform[1, 0]) * destination_rect.width() / m_viewport_size.width();
        scale_y = AK::hypot<float>(transform[0, 1], transform[1, 1]) * destination_rect.height() / m_viewport_size.height();
    }
    if (!isfinite(scale_x) || !isfinite(scale_y))
        return false;
    m_unbounded_raster_scale_of_embedding = { scale_x, scale_y };

    // Perspective keeps the native resolution until we can choose a scale from the visible projected region.
    auto scale = bounded_composited_raster_scale(m_unbounded_raster_scale_of_embedding, m_viewport_size);

    Gfx::FloatPoint translation;
    if (!has_perspective && transform[0, 1] == 0 && transform[1, 0] == 0 && scale.width() == scale_x && scale.height() == scale_y) {
        auto x = transform[0, 0] * destination_rect.x() + transform[0, 3];
        auto y = transform[1, 1] * destination_rect.y() + transform[1, 3];
        if (!isfinite(x) || !isfinite(y))
            return false;
        // Mirrored placements need the opposite fractional phase so the remaining image transform still
        // lands on integer device pixels.
        if (transform[0, 0] < 0)
            x = -x;
        if (transform[1, 1] < 0)
            y = -y;
        translation = { x - floorf(x), y - floorf(y) };
    }

    if (m_raster_scale == scale && m_raster_translation == translation)
        return false;
    m_raster_scale = scale;
    m_raster_translation = translation;
    m_latest_rendered_surface = nullptr;
    m_last_rasterized_frame.clear();
    return true;
}

CompositedContextSurface ContextPresenter::composited_surface() const
{
    if (!m_latest_rendered_surface || !m_last_rasterized_frame.has_value())
        return {};
    auto size = m_last_rasterized_frame->viewport_size.to_type<float>();
    size.scale_by(m_raster_scale.width(), m_raster_scale.height());
    return { m_latest_rendered_surface, { m_raster_translation, size } };
}

void ContextPresenter::queue_present_frame(PendingFrame pending_frame)
{
    if (!m_pending_present_frame.has_value()) {
        m_pending_present_frame = pending_frame;
        return;
    }

    if (m_pending_present_frame->viewport_rect.size() != pending_frame.viewport_rect.size()) {
        m_pending_present_frame = PendingFrame::repainting_everything(pending_frame.viewport_rect);
        return;
    }

    m_pending_present_frame->viewport_rect = pending_frame.viewport_rect;
    m_pending_present_frame->forced_damage_rect.unite(pending_frame.forced_damage_rect);
}

void ContextPresenter::mark_pending_present_frame_scheduled()
{
    m_pending_present_frame_scheduled = true;
}

bool ContextPresenter::can_schedule_pending_present_frame_if_unblocked() const
{
    if (is_present_blocked())
        return false;
    if (!m_pending_present_frame.has_value())
        return false;
    if (m_pending_present_frame_scheduled)
        return false;
    return true;
}

Optional<PendingFrame> ContextPresenter::take_pending_present_frame_if_unblocked()
{
    if (is_present_blocked())
        return {};
    m_pending_present_frame_scheduled = false;
    if (!m_pending_present_frame.has_value())
        return {};
    return m_pending_present_frame.release_value();
}

bool ContextPresenter::needs_rasterization() const
{
    return m_pending_present_frame.has_value()
        || (!m_latest_rendered_surface && m_presented_frame.has_value());
}

Optional<Gfx::IntRect> ContextPresenter::frame_rect_to_repaint() const
{
    if (m_pending_present_frame.has_value())
        return m_pending_present_frame->viewport_rect;
    return m_presented_frame;
}

Optional<Gfx::IntRect> ContextPresenter::self_present_rect() const
{
    if (m_presented_frame.has_value())
        return m_presented_frame;
    if (!m_viewport_size.is_empty())
        return Gfx::IntRect { {}, m_viewport_size };
    return {};
}

bool ContextPresenter::draws_canvas(Compositing::CanvasId canvas_id) const
{
    return m_last_rasterized_frame.has_value() && m_last_rasterized_frame->canvas_content_generations.contains(canvas_id);
}

Optional<PreparedFrame> ContextPresenter::prepare_frame(DisplayListPlayerSkia& display_list_player, PendingFrame pending_frame, Optional<FrameInputs>&& frame_inputs, CompositedContextResolver const* composited_context_resolver)
{
    if (is_present_blocked()) {
        queue_present_frame(pending_frame);
        return {};
    }

    if (!can_render_frame()) {
        m_presented_frame = pending_frame.viewport_rect;
        return {};
    }
    VERIFY(frame_inputs.has_value());

    auto frame_damage = frame_damage_for(pending_frame, *frame_inputs);
    auto damage_rect = frame_damage.rect;
    // NB: A changed content rect must still reach the client even if the pixels are identical.
    if (damage_rect.is_empty() && m_presented_frame == pending_frame.viewport_rect) {
        auto only_media_shown_by_window_server_changed = frame_damage.layers.shown_by_window_server && frame_damage.live_media_changed;
        commit_layer_update(move(frame_damage.layers));
        remember_rasterized_frame(pending_frame.viewport_rect.size(), frame_inputs.release_value());
        if (!only_media_shown_by_window_server_changed)
            return {};
        m_frame_without_store_in_flight = true;
        return PreparedFrame {
            .rendered_surface = nullptr,
            .bitmap_id = -1,
            .damage_rect = {},
            .device_pixel_ratio = m_device_pixel_ratio,
            .sublayers = sublayers_shown_by_window_server(),
        };
    }
    auto render_target = m_backing_store_manager.acquire_render_target(raster_damage_rect(damage_rect));
    if (!render_target.has_value()) {
        queue_present_frame(pending_frame);
        return {};
    }
    auto& back_store = render_target->surface;
    auto painted_rect = paint_frame(display_list_player, back_store, *frame_inputs, composited_context_resolver, render_target->damage_rect, frame_damage.layers);
    if (painted_rect != render_target->damage_rect)
        damage_rect = { {}, pending_frame.viewport_rect.size() };
    commit_layer_update(move(frame_damage.layers));
    remember_rasterized_frame(pending_frame.viewport_rect.size(), frame_inputs.release_value());

    return PreparedFrame {
        .rendered_surface = &back_store,
        .bitmap_id = render_target->bitmap_id,
        .damage_rect = damage_rect,
        .device_pixel_ratio = m_device_pixel_ratio,
        .sublayers = sublayers_shown_by_window_server(),
    };
}

void ContextPresenter::did_submit_prepared_frame(Gfx::IntRect viewport_rect)
{
    m_presented_frame = viewport_rect;
}

bool ContextPresenter::present_synchronously(DisplayListPlayerSkia& display_list_player, Optional<FrameInputs>&& frame_inputs, CompositedContextResolver const* composited_context_resolver)
{
    if (!can_render_frame())
        return false;
    VERIFY(frame_inputs.has_value());
    // Don't race an async present already in flight for this context; its own completion will update the output.
    if (is_present_blocked())
        return false;

    Optional<PendingFrame> pending_frame = m_pending_present_frame;
    if (!pending_frame.has_value() && m_presented_frame.has_value())
        pending_frame = PendingFrame::repainting_everything(*m_presented_frame);
    if (!pending_frame.has_value())
        return false;

    auto frame_damage = frame_damage_for(*pending_frame, *frame_inputs);
    auto render_target = m_backing_store_manager.acquire_render_target(raster_damage_rect(frame_damage.rect));
    if (!render_target.has_value())
        return false;
    auto& back_store = render_target->surface;
    (void)paint_frame(display_list_player, back_store, *frame_inputs, composited_context_resolver, render_target->damage_rect, frame_damage.layers);
    commit_layer_update(move(frame_damage.layers));
    remember_rasterized_frame(pending_frame->viewport_rect.size(), frame_inputs.release_value());
    display_list_player.flush(back_store);
    m_backing_store_manager.complete_rendering(render_target->bitmap_id);
    m_latest_rendered_surface = m_backing_store_manager.latest_rendered_surface();
    m_size_of_latest_rendered_frame = pending_frame->viewport_rect.size();
    m_presented_frame = pending_frame->viewport_rect;
    m_pending_present_frame.clear();
    m_pending_present_frame_scheduled = false;
    return true;
}

void ContextPresenter::paint_screenshot(DisplayListPlayerSkia& display_list_player, Gfx::ShareableBitmap& target_bitmap, FrameInputs const& frame_inputs, CompositedContextResolver const* composited_context_resolver)
{
    auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*target_bitmap.bitmap());
    paint_current_display_list(display_list_player, *target_surface, frame_inputs, composited_context_resolver, {}, PaintUIOverlay::No, false);
    display_list_player.flush(*target_surface);
}

static Gfx::IntSize size_fitting_within(Gfx::IntSize size, Gfx::IntSize maximum_size)
{
    if (size.width() <= maximum_size.width() && size.height() <= maximum_size.height())
        return size;
    auto scale = min(static_cast<double>(maximum_size.width()) / size.width(), static_cast<double>(maximum_size.height()) / size.height());
    return { max(1, static_cast<int>(round(size.width() * scale))), max(1, static_cast<int>(round(size.height() * scale))) };
}

Optional<Gfx::ShareableBitmap> ContextPresenter::snapshot_of_presented_frame(DisplayListPlayerSkia& display_list_player, Optional<Gfx::IntSize> maximum_size)
{
    if (!m_latest_rendered_surface)
        return {};
    NonnullRefPtr<Gfx::PaintingSurface> source_surface = *m_latest_rendered_surface;
    // The store then holds only what is under the canvases, videos and overlays that the window server shows above it.
    if (m_window_server_shows_media && m_layer_plan && m_last_rasterized_frame.has_value()) {
        auto composited_or_error = Gfx::PaintingSurface::try_create_with_size(source_surface->size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, source_surface->skia_backend_context());
        if (composited_or_error.is_error())
            return {};
        auto composited = composited_or_error.release_value();
        auto store_image = source_surface->sk_image_snapshot<sk_sp<SkImage>>();
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        composited->canvas().drawImage(store_image.get(), 0, 0, SkSamplingOptions {}, &paint);
        auto const& frame_inputs = m_last_rasterized_frame->frame_inputs;
        composite_layers_above_base(display_list_player, *composited, frame_inputs, nullptr, *m_layer_plan, composited->rect());
        paint_compositor_overlays(display_list_player, *composited, frame_inputs, PaintUIOverlay::Yes);
        source_surface = composited;
    }
    auto const& source = *source_surface;
    Gfx::IntSize frame_size { min(m_size_of_latest_rendered_frame.width(), source.size().width()), min(m_size_of_latest_rendered_frame.height(), source.size().height()) };
    if (frame_size.is_empty())
        return {};
    auto snapshot_size = frame_size;
    if (maximum_size.has_value() && !maximum_size->is_empty())
        snapshot_size = size_fitting_within(frame_size, *maximum_size);

    auto bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, snapshot_size);
    if (bitmap_or_error.is_error())
        return {};
    auto bitmap = bitmap_or_error.release_value();

    if (snapshot_size == frame_size) {
        source.read_into_bitmap(*bitmap);
        return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
    }

    // A linear filter reads only the pixels around each one it writes, so a frame scaled down to a fraction of its size
    // in one step would skip most of them. Halving it first lets every pixel count.
    auto image = source.sk_image_snapshot<sk_sp<SkImage>>();
    auto image_size = frame_size;
    SkSamplingOptions const sampling { SkFilterMode::kLinear };
    auto draw_scaled = [&](Gfx::IntSize size) -> RefPtr<Gfx::PaintingSurface> {
        auto surface_or_error = Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, source.skia_backend_context());
        if (surface_or_error.is_error())
            return nullptr;
        auto surface = surface_or_error.release_value();
        surface->canvas().drawImageRect(image.get(), SkRect::MakeIWH(image_size.width(), image_size.height()), SkRect::MakeIWH(size.width(), size.height()), sampling, nullptr, SkCanvas::kStrict_SrcRectConstraint);
        return surface;
    };
    while (image_size.width() / 2 >= snapshot_size.width() && image_size.height() / 2 >= snapshot_size.height()) {
        Gfx::IntSize half_size { image_size.width() / 2, image_size.height() / 2 };
        auto half = draw_scaled(half_size);
        if (!half)
            return {};
        image = half->sk_image_snapshot<sk_sp<SkImage>>();
        image_size = half_size;
    }
    auto snapshot_surface = draw_scaled(snapshot_size);
    if (!snapshot_surface)
        return {};
    snapshot_surface->read_into_bitmap(*bitmap);
    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

void ContextPresenter::stop_backing_store_shrink_timer()
{
    if (!m_backing_store_shrink_timer)
        return;
    m_backing_store_shrink_timer->on_timeout = {};
    m_backing_store_shrink_timer->stop();
}

void ContextPresenter::stop_surplus_backing_store_retirement_timer()
{
    if (!m_surplus_backing_store_retirement_timer)
        return;
    m_surplus_backing_store_retirement_timer->on_timeout = {};
    m_surplus_backing_store_retirement_timer->stop();
}

void ContextPresenter::stop_layer_drop_timer()
{
    if (!m_layer_drop_timer)
        return;
    m_layer_drop_timer->on_timeout = {};
    m_layer_drop_timer->stop();
}

bool ContextPresenter::is_present_blocked() const
{
    return m_backing_store_manager.is_rendering()
        || m_frame_without_store_in_flight
        || !m_backing_store_manager.has_available_buffer();
}

bool ContextPresenter::can_render_frame() const
{
    return m_display_list && m_backing_store_manager.is_valid();
}

Gfx::IntSize ContextPresenter::raster_size() const
{
    if (m_viewport_size.is_empty())
        return {};
    if (presents_to_client())
        return m_viewport_size;
    return {
        static_cast<int>(ceilf(m_viewport_size.width() * m_raster_scale.width() + m_raster_translation.x())),
        static_cast<int>(ceilf(m_viewport_size.height() * m_raster_scale.height() + m_raster_translation.y())),
    };
}

Gfx::IntRect ContextPresenter::raster_damage_rect(Gfx::IntRect damage_rect) const
{
    if (damage_rect.is_empty())
        return {};
    if (damage_rect.contains(Gfx::IntRect { {}, m_viewport_size }))
        return { {}, raster_size() };
    auto rect = damage_rect.to_type<float>();
    rect.scale_by(m_raster_scale.width(), m_raster_scale.height());
    rect.translate_by(m_raster_translation);
    return Gfx::enclosing_int_rect(rect).intersected(Gfx::IntRect { {}, raster_size() });
}

ContextPresenter::FrameDamage ContextPresenter::frame_damage_for(PendingFrame const& pending_frame, FrameInputs const& frame_inputs)
{
    Gfx::IntRect viewport_rect { {}, pending_frame.viewport_rect.size() };
    auto forced_damage_rect = pending_frame.forced_damage_rect.intersected(viewport_rect);
    auto changes = live_content_changes_since_last_raster(viewport_rect.size());
    auto now = MonotonicTime::now();
    m_media_activity.note_updates(changes.media_updates, now);

    auto layers = layer_update_for(frame_inputs, viewport_rect, now);
    auto const* plan = layers.keeps_plan ? m_layer_plan.ptr() : layers.new_plan.ptr();
    layers.shown_by_window_server = plan && window_server_can_show_media_layers_of(*plan);
    auto layers_hold_last_frame = m_layer_plan && m_last_rasterized_frame.has_value();
    auto keeps_layer_rasters = plan && layers_hold_last_frame && plan->has_same_layers_as(*m_layer_plan);
    auto repaints_everything = changes.repaints_everything || forced_damage_rect.contains(viewport_rect);
    auto damage = repaints_everything
        ? DisplayListDamage {}
        : damage_since_last_raster(viewport_rect.size(), frame_inputs, forced_damage_rect, changes,
              plan ? plan->media_record_offsets() : ReadonlySpan<u32> {},
              keeps_layer_rasters ? m_layer_plan.ptr() : nullptr,
              keeps_layer_rasters ? plan : nullptr);
    layers.painted_damage = damage.rect.value_or(viewport_rect).intersected(viewport_rect);

    if (!plan) {
        layers.repaints_everything = layers_hold_last_frame || m_repaint_everything_after_dropping_layers;
    } else if (!keeps_layer_rasters) {
        layers.fully_repainted_layer_bits = NumericLimits<u32>::max();
        layers.repaints_everything = true;
    } else {
        for (size_t index = 0; index < plan->layers().size(); ++index) {
            auto layer_damage = damage.layer_rects.is_empty() ? Optional<Gfx::IntRect> { layers.painted_damage } : damage.layer_rects[index];
            if (!layer_damage.has_value())
                layers.fully_repainted_layer_bits |= 1u << index;
            layers.damage_of_layers.append(layer_damage.value_or(viewport_rect).intersected(viewport_rect));
        }
    }

    // A frame holds the canvases and videos only while the window server does not show them, so a switch either way
    // repaints all of it.
    if (m_last_rasterized_frame.has_value() && layers.shown_by_window_server != m_window_server_shows_media) {
        layers.repaints_everything = true;
        layers.fully_repainted_layer_bits = NumericLimits<u32>::max();
    }

    auto live_media_changed = !changes.canvases.is_empty() || !changes.video_sinks.is_empty();
    if (layers.repaints_everything)
        return { viewport_rect, move(layers), live_media_changed };
    auto damage_rect = layers.painted_damage;
    if (plan && !layers.shown_by_window_server)
        damage_rect.unite(plan->rect_of_media_layers_showing(changes.canvases, changes.video_sinks).intersected(viewport_rect));
    return { damage_rect, move(layers), live_media_changed };
}

ContextPresenter::LiveContentChangesSinceLastRaster ContextPresenter::live_content_changes_since_last_raster(Gfx::IntSize viewport_size)
{
    LiveContentChangesSinceLastRaster changes;
    if (!m_last_rasterized_frame.has_value()) {
        changes.repaints_everything = true;
        return changes;
    }
    auto const& last_frame = *m_last_rasterized_frame;
    if (last_frame.viewport_size != viewport_size || last_frame.display_list->surface_clear_color() != m_display_list->surface_clear_color())
        changes.repaints_everything = true;

    for (auto const& [canvas_id, content_generation] : last_frame.canvas_content_generations) {
        auto current_content_generation = m_canvas_surfaces.canvas_content_generation(canvas_id);
        if (current_content_generation == content_generation)
            continue;
        changes.canvases.append(canvas_id);
        changes.media_updates.append({ { RustFFI::FfiLiveMediaKind::Canvas, canvas_id.value() }, current_content_generation });
    }
    auto video_sinks_drawn_by_list = m_display_list->referenced_resource_ids().video_sinks;
    for (auto const& [video_sink_id, frame_id] : last_frame.video_frame_ids) {
        auto current_frame_id = current_video_frame_id(video_sink_id);
        if (current_frame_id == frame_id)
            continue;
        changes.media_updates.append({ { RustFFI::FfiLiveMediaKind::Video, video_sink_id.value() }, current_frame_id.value_or(0) });
        // A video drawn only by a nested display list, such as an SVG image's, has no command in
        // this list to damage.
        if (!video_sinks_drawn_by_list.contains_slow(video_sink_id.value()))
            changes.repaints_everything = true;
        changes.video_sinks.append(video_sink_id);
    }
    auto image_frames_drawn_by_list = m_display_list->referenced_resource_ids().image_frames;
    for (auto const& [image_frame_id, generation] : m_resource_storage.image_frame_pixel_generations()) {
        if (last_frame.image_frame_pixel_generations.get(image_frame_id) == generation)
            continue;
        // An image drawn only by a nested display list, such as an SVG image's, has no command in this list to damage.
        if (!image_frames_drawn_by_list.contains_slow(image_frame_id))
            changes.repaints_everything = true;
        changes.image_frames.append(Compositing::ImageFrameResourceId { image_frame_id });
    }
    return changes;
}

DisplayListDamage ContextPresenter::damage_since_last_raster(Gfx::IntSize viewport_size, FrameInputs const& frame_inputs, Gfx::IntRect forced_damage_rect, LiveContentChangesSinceLastRaster const& changes, ReadonlySpan<u32> offsets_of_records_composited_as_media_layers, LayerPlan const* last_layer_plan, LayerPlan const* layer_plan)
{
    Gfx::IntRect viewport_rect { {}, viewport_size };
    auto const& last_frame = *m_last_rasterized_frame;
    auto damage = frame_inputs.damage_since(
        *m_display_list,
        last_frame.frame_inputs,
        *last_frame.display_list,
        viewport_rect,
        { .canvases = changes.canvases, .video_sinks = changes.video_sinks, .image_frames = changes.image_frames, .forced_damage_rect = forced_damage_rect, .offsets_of_records_composited_as_media_layers = offsets_of_records_composited_as_media_layers },
        last_layer_plan ? last_layer_plan->rust_plan() : nullptr,
        layer_plan ? layer_plan->rust_plan() : nullptr);
    if (damage.rect.has_value())
        damage.rect = damage.rect->intersected(viewport_rect);
    return damage;
}

ContextPresenter::LayerUpdate ContextPresenter::layer_update_for(FrameInputs const& frame_inputs, Gfx::IntRect viewport_rect, MonotonicTime now)
{
    LayerUpdate layers;
    if (!presents_to_client())
        return layers;
    layers.active_media = m_media_activity.active_media(now);
    // An HDR video is a layer from its first frame for as long as it is drawn, even while paused, so that the window
    // server keeps showing it brighter than SDR white instead of it going back into the frame, which holds SDR.
    if (m_remote_layer_that_may_show_media && m_remote_layer_that_may_show_media->can_show_high_dynamic_range() && m_window_server_may_show_high_dynamic_range) {
        for (auto id : m_display_list->referenced_resource_ids().video_sinks) {
            LiveMedia media { RustFFI::FfiLiveMediaKind::Video, id };
            auto is_active = any_of(layers.active_media, [&](auto const& active) { return active.kind == media.kind && active.id == media.id; });
            if (is_active)
                continue;
            auto frame = current_video_frame(Compositing::VideoSinkResourceId { id });
            if (frame && video_frame_holds_high_dynamic_range(*frame))
                layers.active_media.append(media);
        }
    }
    if (layers.active_media.is_empty())
        return layers;
    auto plan_holds_for_frame = m_layer_plan
        && m_last_rasterized_frame.has_value()
        && m_last_rasterized_frame->display_list.ptr() == m_display_list.ptr()
        && m_last_rasterized_frame->viewport_size == viewport_rect.size()
        && same_media(m_layer_plan_active_media, layers.active_media)
        && frame_inputs.places_content_like(m_last_rasterized_frame->frame_inputs);
    if (plan_holds_for_frame)
        layers.keeps_plan = true;
    else
        layers.new_plan = LayerPlan::build(frame_inputs, *m_display_list, viewport_rect, layers.active_media);
    return layers;
}

void ContextPresenter::commit_layer_update(LayerUpdate&& layers)
{
    m_window_server_shows_media = layers.has_plan() && layers.shown_by_window_server;
    if (layers.repaints_everything)
        m_repaint_everything_after_dropping_layers = false;
    if (!layers.has_plan()) {
        drop_layers();
        return;
    }
    if (layers.new_plan)
        m_layer_plan = move(layers.new_plan);
    m_layer_plan_active_media = move(layers.active_media);

    if (!m_layer_drop_timer) {
        m_layer_drop_timer = Core::Timer::create_single_shot(MediaActivity::time_media_stays_active_after_an_update, [this] {
            // While the window server shows the media, the layers cost nothing, and the frame on screen still needs
            // their plan, as for a snapshot. The next frame without a plan puts the media back into the store.
            if (!m_layer_plan || m_window_server_shows_media)
                return;
            drop_layers();
            m_repaint_everything_after_dropping_layers = true;
        });
    }
    m_layer_drop_timer->restart();
}

void ContextPresenter::drop_layers()
{
    m_layer_plan = nullptr;
    m_layer_plan_active_media.clear();
    m_layer_surfaces.clear();
    m_painted_layer_presentations.clear();
    if (m_layer_drop_timer)
        m_layer_drop_timer->stop();
}

void ContextPresenter::remember_rasterized_frame(Gfx::IntSize viewport_size, FrameInputs&& frame_inputs)
{
    HashMap<Compositing::CanvasId, u64> canvas_content_generations;
    for (auto id : m_display_list->referenced_resource_ids().canvases) {
        Compositing::CanvasId canvas_id { id };
        canvas_content_generations.set(canvas_id, m_canvas_surfaces.canvas_content_generation(canvas_id));
    }
    // Every video the context holds is drawn by the list or by a display list it nests.
    HashMap<Compositing::VideoSinkResourceId, Optional<u64>> video_frame_ids;
    for (auto const& [id, handle] : m_resource_storage.video_sink_handles()) {
        Compositing::VideoSinkResourceId video_sink_id { id };
        video_frame_ids.set(video_sink_id, current_video_frame_id(video_sink_id));
    }
    m_last_rasterized_frame = RasterizedFrame {
        .display_list = NonnullRefPtr { *m_display_list },
        .frame_inputs = move(frame_inputs),
        .viewport_size = viewport_size,
        .canvas_content_generations = move(canvas_content_generations),
        .video_frame_ids = move(video_frame_ids),
        .image_frame_pixel_generations = m_resource_storage.image_frame_pixel_generations(),
    };
}

RefPtr<Media::VideoFrame> ContextPresenter::current_video_frame(Compositing::VideoSinkResourceId video_sink_id)
{
    auto sink = m_resource_storage.video_sink(video_sink_id);
    if (!sink)
        return {};
    return sink->current_frame();
}

Optional<u64> ContextPresenter::current_video_frame_id(Compositing::VideoSinkResourceId video_sink_id)
{
    auto frame = current_video_frame(video_sink_id);
    if (!frame)
        return {};
    return frame->id();
}

void ContextPresenter::paint_compositor_overlays(DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& target_surface, FrameInputs const& frame_inputs, PaintUIOverlay paint_ui_overlay)
{
    display_list_player.paint_scrollbars_into(target_surface, frame_inputs.scrollbars_painted_by_compositor());
    if (paint_ui_overlay == PaintUIOverlay::No)
        return;

    if (m_ui_overlays.paused_debugger.has_value())
        paint_paused_debugger_overlay(target_surface, m_viewport_size, m_device_pixel_ratio, *m_ui_overlays.paused_debugger);
    if (auto opacity = m_picture_in_picture_controls_fade.opacity(); m_ui_overlays.picture_in_picture_controls.has_value() && opacity > 0)
        paint_picture_in_picture_controls(target_surface, m_viewport_size, m_device_pixel_ratio, *m_ui_overlays.picture_in_picture_controls, opacity);
}

Gfx::IntRect ContextPresenter::paint_frame(DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface, FrameInputs const& frame_inputs, CompositedContextResolver const* composited_context_resolver, Gfx::IntRect damage_rect, LayerUpdate& layers)
{
    if (layers.has_plan()) {
        if (paint_layered_frame(display_list_player, surface, frame_inputs, composited_context_resolver, damage_rect, layers))
            return damage_rect;
        // The sublayers that the window server was to show go away with this frame, so the store has to hold all
        // that they showed.
        if (layers.shown_by_window_server)
            damage_rect = { {}, raster_size() };
        layers.new_plan = nullptr;
        layers.keeps_plan = false;
        layers.shown_by_window_server = false;
        drop_layers();
        m_repaint_everything_after_dropping_layers = true;
    }
    paint_current_display_list(display_list_player, surface, frame_inputs, composited_context_resolver, damage_rect);
    return damage_rect;
}

bool ContextPresenter::paint_layered_frame(DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface, FrameInputs const& frame_inputs, CompositedContextResolver const* composited_context_resolver, Gfx::IntRect damage_rect, LayerUpdate const& layer_update)
{
    VERIFY(m_display_list);
    auto const& plan = layer_update.keeps_plan ? *m_layer_plan : *layer_update.new_plan;
    auto layers = plan.layers();
    auto skia_backend_context = surface.skia_backend_context();
    auto surface_clear_color = Gfx::to_skia_color(m_display_list->surface_clear_color().value_or(Gfx::Color::Transparent));
    m_raster_cache.drop_what_was_not_used_since_last_frame();

    m_layer_surfaces.resize(layers.size());
    for (size_t index = 0; index < layers.size(); ++index) {
        auto const& layer = layers[index];
        auto& layer_surface = m_layer_surfaces[index];
        auto rect = layer.kind == LayerKind::Base ? Gfx::IntRect { {}, m_viewport_size } : layer.rect;
        if (layer.kind == LayerKind::Media || rect.is_empty()) {
            layer_surface = {};
            continue;
        }
        auto repaints_whole_layer = (layer_update.fully_repainted_layer_bits & (1u << index)) != 0;
        auto surface_fits = layer.kind == LayerKind::Base ? layer_surface.rect == rect : layer_surface.rect.contains(rect);
        if (!layer_surface.surface || !surface_fits || layer_surface.surface->skia_backend_context() != skia_backend_context) {
            auto new_surface = Gfx::PaintingSurface::try_create_with_size(rect.size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context);
            if (new_surface.is_error())
                return false;
            layer_surface = { new_surface.release_value(), rect };
            repaints_whole_layer = true;
        }
        auto repaint_rect = repaints_whole_layer ? layer_surface.rect : layer_update.damage_of_layers[index].intersected(layer_surface.rect);
        if (repaint_rect.is_empty())
            continue;
        auto& layer_canvas = layer_surface.surface->canvas();
        layer_canvas.save();
        layer_canvas.translate(-layer_surface.rect.x(), -layer_surface.rect.y());
        layer_canvas.clipIRect(SkIRect::MakeXYWH(repaint_rect.x(), repaint_rect.y(), repaint_rect.width(), repaint_rect.height()));
        layer_canvas.clear(layer.kind == LayerKind::Base ? surface_clear_color : SK_ColorTRANSPARENT);
        replay_display_list(display_list_player, *layer_surface.surface, frame_inputs, composited_context_resolver, replaying_only(layer.record_spans));
        layer_canvas.restore();
        layer_surface.content_version = ++m_last_layer_content_version;
    }

    auto& canvas = surface.canvas();
    auto save_count = canvas.save();
    canvas.clipIRect(SkIRect::MakeXYWH(damage_rect.x(), damage_rect.y(), damage_rect.width(), damage_rect.height()));
    if (auto const& base_surface = m_layer_surfaces[0]; base_surface.surface && base_surface.rect.intersects(damage_rect)) {
        auto image = base_surface.surface->sk_image_snapshot<sk_sp<SkImage>>();
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        canvas.drawImage(image.get(), base_surface.rect.x(), base_surface.rect.y(), SkSamplingOptions {}, &paint);
    }
    if (!layer_update.shown_by_window_server) {
        composite_layers_above_base(display_list_player, surface, frame_inputs, composited_context_resolver, plan, damage_rect);
        paint_compositor_overlays(display_list_player, surface, frame_inputs, PaintUIOverlay::Yes);
    }
    canvas.restoreToCount(save_count);

    // The window server shows everything above the base layer from surfaces of its own.
    if (layer_update.shown_by_window_server) {
        if (!present_painted_layers_to_window_server(plan, skia_backend_context))
            return false;
        if (!present_compositor_overlays_to_window_server(display_list_player, frame_inputs, skia_backend_context))
            return false;
    }
    return true;
}

void ContextPresenter::composite_layers_above_base(DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface, FrameInputs const& frame_inputs, CompositedContextResolver const* composited_context_resolver, LayerPlan const& plan, Gfx::IntRect damage_rect)
{
    auto layers = plan.layers();
    auto& canvas = surface.canvas();
    for (size_t index = 1; index < layers.size(); ++index) {
        auto const& layer = layers[index];
        if (layer.kind == LayerKind::Media) {
            if (layer.rect.intersects(damage_rect))
                replay_display_list(display_list_player, surface, frame_inputs, composited_context_resolver, replaying_only(layer.record_spans, true));
            continue;
        }
        if (index >= m_layer_surfaces.size())
            continue;
        auto const& layer_surface = m_layer_surfaces[index];
        if (!layer_surface.surface || !layer_surface.rect.intersects(damage_rect))
            continue;
        auto image = layer_surface.surface->sk_image_snapshot<sk_sp<SkImage>>();
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrcOver);
        canvas.drawImage(image.get(), layer_surface.rect.x(), layer_surface.rect.y(), SkSamplingOptions {}, &paint);
    }
}

bool ContextPresenter::window_server_can_show_media_layers_of(LayerPlan const& plan)
{
    if (!m_remote_layer_that_may_show_media)
        return false;
    for (auto const& layer : plan.layers()) {
        if (layer.kind != LayerKind::Media)
            continue;
        if (!layer.window_server_placement.has_value())
            return false;
        Optional<RemoteLayer::Content> content;
        if (layer.media.kind == RustFFI::FfiLiveMediaKind::Canvas) {
            if (auto surface = m_canvas_surfaces.presentable_canvas_surface(Compositing::CanvasId { layer.media.id }))
                content = surface.release_nonnull();
        } else if (auto frame = current_video_frame(Compositing::VideoSinkResourceId { layer.media.id })) {
            if (!m_window_server_may_show_high_dynamic_range && video_frame_holds_high_dynamic_range(*frame))
                return false;
            content = frame.release_nonnull();
        }
        if (!content.has_value() || !m_remote_layer_that_may_show_media->can_show(*layer.window_server_placement, *content, m_device_pixel_ratio))
            return false;
    }
    return true;
}

// Each painted layer goes to the window server as its own surface, which is copied from its raster when the raster
// changes. The window server may still show the surface of the frame before, so the copy goes into another one.
bool ContextPresenter::present_painted_layers_to_window_server(LayerPlan const& plan, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto layers = plan.layers();
    m_painted_layer_presentations.resize(layers.size());
    for (size_t index = 0; index < layers.size(); ++index) {
        auto const& layer = layers[index];
        auto& presentation = m_painted_layer_presentations[index];
        auto const* layer_surface = index < m_layer_surfaces.size() ? &m_layer_surfaces[index] : nullptr;
        if (layer.kind != LayerKind::Painted || layer.rect.is_empty() || !layer_surface || !layer_surface->surface) {
            presentation.latest_surface = nullptr;
            continue;
        }
        if (presentation.latest_surface && presentation.rect == layer.rect && presentation.content_version == layer_surface->content_version)
            continue;
        auto surface_or_error = presentation.surfaces.surface_for_next_content(layer.rect.size(), skia_backend_context);
        if (surface_or_error.is_error())
            return false;
        auto surface = surface_or_error.release_value();
        auto image = layer_surface->surface->sk_image_snapshot<sk_sp<SkImage>>();
        auto source_rect = layer.rect.translated(-layer_surface->rect.location());
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        surface->painting_surface().canvas().drawImageRect(image.get(), Gfx::to_skia_rect(source_rect), SkRect::MakeIWH(layer.rect.width(), layer.rect.height()), SkSamplingOptions {}, &paint, SkCanvas::kStrict_SrcRectConstraint);
        presentation.latest_surface = move(surface);
        presentation.rect = layer.rect;
        presentation.content_version = layer_surface->content_version;
    }
    return true;
}

Gfx::IntRect ContextPresenter::compositor_overlay_bounds(FrameInputs const& frame_inputs) const
{
    Gfx::IntRect viewport_rect { {}, m_viewport_size };
    if (m_ui_overlays.paused_debugger.has_value())
        return viewport_rect;
    if (m_ui_overlays.picture_in_picture_controls.has_value() && m_picture_in_picture_controls_fade.opacity() > 0)
        return viewport_rect;
    Gfx::IntRect bounds;
    for (auto const& scrollbar : frame_inputs.scrollbars()) {
        if (!scrollbar.is_painted_by_compositor || scrollbar.opacity <= 0)
            continue;
        // The thumb's outline is drawn around the gutter.
        bounds.unite(Gfx::enclosing_int_rect(scrollbar.gutter_rect).united(scrollbar.thin_and_expanded_gutter_rect).inflated(2, 2));
    }
    return bounds.intersected(viewport_rect);
}

bool ContextPresenter::present_compositor_overlays_to_window_server(DisplayListPlayerSkia& display_list_player, FrameInputs const& frame_inputs, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto bounds = compositor_overlay_bounds(frame_inputs);
    m_compositor_overlay_rect = bounds;
    if (bounds.is_empty()) {
        m_latest_compositor_overlay_surface = nullptr;
        return true;
    }
    auto surface_or_error = m_compositor_overlay_surfaces.surface_for_next_content(bounds.size(), skia_backend_context);
    if (surface_or_error.is_error())
        return false;
    auto surface = surface_or_error.release_value();
    auto& canvas = surface->painting_surface().canvas();
    canvas.clear(SK_ColorTRANSPARENT);
    auto save_count = canvas.save();
    canvas.translate(-bounds.x(), -bounds.y());
    paint_compositor_overlays(display_list_player, surface->painting_surface(), frame_inputs, PaintUIOverlay::Yes);
    canvas.restoreToCount(save_count);
    m_latest_compositor_overlay_surface = move(surface);
    return true;
}

// Sublayers keep their identity from one frame to the next by what they show.
static constexpr u64 canvas_sublayer_identity_tag = u64 { 1 } << 60;
static constexpr u64 video_sublayer_identity_tag = u64 { 2 } << 60;
static constexpr u64 painted_layer_sublayer_identity_tag = u64 { 3 } << 60;
static constexpr u64 compositor_overlay_sublayer_identity = u64 { 4 } << 60;
// Canvas and video IDs count up from 1, and stay far below this.
static constexpr u64 media_drawing_index_shift = 44;

Vector<RemoteSublayer> ContextPresenter::sublayers_shown_by_window_server()
{
    Vector<RemoteSublayer> sublayers;
    if (!m_window_server_shows_media || !m_layer_plan)
        return sublayers;
    // The same canvas or video can be drawn by more than one record, and each drawing is a sublayer of its own.
    HashMap<u64, u64> drawings_of_media;
    auto layers = m_layer_plan->layers();
    for (size_t index = 1; index < layers.size(); ++index) {
        auto const& layer = layers[index];
        if (layer.kind == LayerKind::Media) {
            auto const& placement = *layer.window_server_placement;
            Optional<Variant<NonnullRefPtr<Media::VideoFrame>, PresentableSurface::Lease>> content;
            u64 identity = 0;
            if (layer.media.kind == RustFFI::FfiLiveMediaKind::Canvas) {
                if (auto surface = m_canvas_surfaces.presentable_canvas_surface(Compositing::CanvasId { layer.media.id }))
                    content = surface->lease();
                identity = canvas_sublayer_identity_tag | layer.media.id;
            } else {
                if (auto frame = current_video_frame(Compositing::VideoSinkResourceId { layer.media.id }); frame && frame->surface())
                    content = frame.release_nonnull();
                identity = video_sublayer_identity_tag | layer.media.id;
            }
            if (!content.has_value())
                continue;
            auto& earlier_drawings = drawings_of_media.ensure(identity, [] { return 0; });
            identity |= earlier_drawings++ << media_drawing_index_shift;
            sublayers.append({
                .identity = identity,
                .content = content.release_value(),
                .content_rect = placement.content_rect,
                .content_to_frame = placement.content_to_viewport,
                .clip_rect = placement.clip_rect,
                .rounded_clip = placement.rounded_clip,
                .scaling_mode = placement.scaling_mode,
            });
            continue;
        }
        if (index >= m_painted_layer_presentations.size() || !m_painted_layer_presentations[index].latest_surface)
            continue;
        auto const& presentation = m_painted_layer_presentations[index];
        sublayers.append({
            .identity = painted_layer_sublayer_identity_tag | index,
            .content = presentation.latest_surface->lease(),
            .content_rect = presentation.rect,
            .content_to_frame = {},
            .clip_rect = presentation.rect.to_type<float>(),
            .rounded_clip = {},
            .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
        });
    }
    if (m_latest_compositor_overlay_surface) {
        sublayers.append({
            .identity = compositor_overlay_sublayer_identity,
            .content = m_latest_compositor_overlay_surface->lease(),
            .content_rect = m_compositor_overlay_rect,
            .content_to_frame = {},
            .clip_rect = m_compositor_overlay_rect.to_type<float>(),
            .rounded_clip = {},
            .scaling_mode = Gfx::ScalingMode::NearestNeighbor,
        });
    }
    return sublayers;
}

void ContextPresenter::replay_display_list(DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface, FrameInputs const& frame_inputs, CompositedContextResolver const* composited_context_resolver, ReplaySelection const& selection)
{
    auto painter = display_list_player.begin_painting(surface, m_resource_storage, m_raster_cache, &m_canvas_surfaces, composited_context_resolver);
    frame_inputs.replay(*m_display_list, painter, selection);
    display_list_player.end_painting();
}

void ContextPresenter::paint_current_display_list(DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface, FrameInputs const& frame_inputs, CompositedContextResolver const* composited_context_resolver, Optional<Gfx::IntRect> damage_rect, PaintUIOverlay paint_ui_overlay, bool apply_raster_transform)
{
    VERIFY(m_display_list);
    auto surface_clear_color = Gfx::to_skia_color(m_display_list->surface_clear_color().value_or(Gfx::Color::Transparent));
    auto paint_display_list = [&](Gfx::PaintingSurface& target_surface) {
        m_raster_cache.drop_what_was_not_used_since_last_frame();
        replay_display_list(display_list_player, target_surface, frame_inputs, composited_context_resolver);
        paint_compositor_overlays(display_list_player, target_surface, frame_inputs, paint_ui_overlay);
    };

    if (damage_rect.has_value() && !damage_rect->is_empty() && presents_to_client() && damage_rect->size() != surface.size() && surface.skia_backend_context()) {
        if (!m_damage_surface || m_damage_surface->size() != damage_rect->size()) {
            m_damage_surface = Gfx::PaintingSurface::create_with_size(
                damage_rect->size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, surface.skia_backend_context());
        }

        auto& damage_canvas = m_damage_surface->canvas();
        damage_canvas.clear(surface_clear_color);
        damage_canvas.save();
        damage_canvas.translate(-damage_rect->x(), -damage_rect->y());
        paint_display_list(*m_damage_surface);
        damage_canvas.restore();

        auto image = m_damage_surface->sk_image_snapshot<sk_sp<SkImage>>();
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        surface.canvas().drawImageRect(
            image.get(),
            SkRect::MakeWH(damage_rect->width(), damage_rect->height()),
            SkRect::MakeXYWH(damage_rect->x(), damage_rect->y(), damage_rect->width(), damage_rect->height()),
            SkSamplingOptions {},
            &paint,
            SkCanvas::kStrict_SrcRectConstraint);
        return;
    }

    auto& canvas = surface.canvas();
    auto save_count = canvas.save();
    if (damage_rect.has_value()) {
        canvas.clipIRect(SkIRect::MakeXYWH(damage_rect->x(), damage_rect->y(), damage_rect->width(), damage_rect->height()));
        canvas.clear(surface_clear_color);
    }
    if (apply_raster_transform) {
        canvas.translate(m_raster_translation.x(), m_raster_translation.y());
        canvas.scale(m_raster_scale.width(), m_raster_scale.height());
    }
    paint_display_list(surface);
    canvas.restoreToCount(save_count);
}

}
