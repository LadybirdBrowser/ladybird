/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/ContextPresenter.h>
#include <Compositor/ContextScene.h>
#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/PausedDebuggerOverlay.h>
#include <Compositor/PictureInPictureControls.h>
#include <Compositor/ScrollbarController.h>
#include <LibCompositing/DisplayList/DisplayListDamage.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SkiaUtils.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMediaCodecs/VideoFrame.h>
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

ContextPresenter::ContextPresenter(ContextScene& scene, OwnedCanvasSurfaces canvas_surfaces, bool presents_to_client)
    : m_scene(scene)
    , m_canvas_surfaces(canvas_surfaces)
    , m_presents_to_client(presents_to_client)
{
}

ContextPresenter::~ContextPresenter()
{
    stop_backing_store_shrink_timer();
    stop_surplus_backing_store_retirement_timer();
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

Optional<BackingStoreManager::Publication> ContextPresenter::resize_backing_stores_if_needed(RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, BackingStoreManager::GpuSharing gpu_sharing, BackingStoreManager::PublicationMode publication_mode)
{
    if (m_backing_store_manager.is_rendering())
        return {};
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
    auto should_publish = presents_to_client() && publication_mode == BackingStoreManager::PublicationMode::Published;
    auto allocation = m_backing_store_manager.resize_backing_stores_if_needed(raster_size(), m_window_resize_in_progress, should_publish);
    if (!allocation.has_value())
        return {};
    m_latest_rendered_surface = nullptr;
    m_last_rasterized_frame.clear();
    return m_backing_store_manager.allocate_backing_stores(*allocation, skia_backend_context, should_publish, gpu_sharing);
}

void ContextPresenter::invalidate_backing_stores()
{
    m_backing_store_manager.invalidate();
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

void ContextPresenter::did_finish_gpu_present(i32 bitmap_id)
{
    m_backing_store_manager.complete_rendering(bitmap_id, presents_to_client());
    m_latest_rendered_surface = m_backing_store_manager.latest_rendered_surface();
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
        scale_x = hypotf(transform[0, 0], transform[1, 0]) * destination_rect.width() / m_viewport_size.width();
        scale_y = hypotf(transform[0, 1], transform[1, 1]) * destination_rect.height() / m_viewport_size.height();
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

Optional<PreparedFrame> ContextPresenter::prepare_frame(DisplayListPlayerSkia& display_list_player, PendingFrame pending_frame, FrameContent const& frame_content)
{
    if (is_present_blocked()) {
        queue_present_frame(pending_frame);
        return {};
    }

    if (!can_render_frame()) {
        m_presented_frame = pending_frame.viewport_rect;
        return {};
    }

    auto damage_rect = frame_damage_for(pending_frame, frame_content);
    // NB: A changed content rect must still reach the client even if the pixels are identical.
    if (damage_rect.is_empty() && m_presented_frame == pending_frame.viewport_rect) {
        remember_rasterized_frame(pending_frame.viewport_rect.size(), frame_content.presented_scroll_state_snapshot);
        return {};
    }
    auto render_target = m_backing_store_manager.acquire_render_target(raster_damage_rect(damage_rect));
    if (!render_target.has_value()) {
        queue_present_frame(pending_frame);
        return {};
    }
    auto& back_store = render_target->surface;
    paint_current_display_list(display_list_player, back_store, frame_content, render_target->damage_rect);
    remember_rasterized_frame(pending_frame.viewport_rect.size(), frame_content.presented_scroll_state_snapshot);

    return PreparedFrame {
        .rendered_surface = &back_store,
        .bitmap_id = render_target->bitmap_id,
        .damage_rect = damage_rect,
        .device_pixel_ratio = m_device_pixel_ratio,
    };
}

void ContextPresenter::did_submit_prepared_frame(Gfx::IntRect viewport_rect)
{
    m_presented_frame = viewport_rect;
}

bool ContextPresenter::present_synchronously(DisplayListPlayerSkia& display_list_player, FrameContent const& frame_content)
{
    if (!can_render_frame())
        return false;
    // Don't race an async present already in flight for this context; its own completion will update the output.
    if (is_present_blocked())
        return false;

    Optional<PendingFrame> pending_frame = m_pending_present_frame;
    if (!pending_frame.has_value() && m_presented_frame.has_value())
        pending_frame = PendingFrame::repainting_everything(*m_presented_frame);
    if (!pending_frame.has_value())
        return false;

    auto render_target = m_backing_store_manager.acquire_render_target(raster_damage_rect(frame_damage_for(*pending_frame, frame_content)));
    if (!render_target.has_value())
        return false;
    auto& back_store = render_target->surface;
    paint_current_display_list(display_list_player, back_store, frame_content, render_target->damage_rect);
    remember_rasterized_frame(pending_frame->viewport_rect.size(), frame_content.presented_scroll_state_snapshot);
    display_list_player.flush(back_store);
    m_backing_store_manager.complete_rendering(render_target->bitmap_id, false);
    m_latest_rendered_surface = m_backing_store_manager.latest_rendered_surface();
    m_presented_frame = pending_frame->viewport_rect;
    m_pending_present_frame.clear();
    m_pending_present_frame_scheduled = false;
    return true;
}

void ContextPresenter::paint_screenshot(DisplayListPlayerSkia& display_list_player, Gfx::ShareableBitmap& target_bitmap, FrameContent const& frame_content)
{
    auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*target_bitmap.bitmap());
    paint_current_display_list(display_list_player, *target_surface, frame_content, {}, PaintUIOverlay::No, false);
    display_list_player.flush(*target_surface);
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

bool ContextPresenter::is_present_blocked() const
{
    return m_backing_store_manager.is_rendering()
        || !m_backing_store_manager.has_available_buffer();
}

bool ContextPresenter::can_render_frame() const
{
    return m_scene.display_list() && m_backing_store_manager.is_valid();
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

Gfx::IntRect ContextPresenter::frame_damage_for(PendingFrame const& pending_frame, FrameContent const& frame_content)
{
    Gfx::IntRect viewport_rect { {}, pending_frame.viewport_rect.size() };
    auto forced_damage_rect = pending_frame.forced_damage_rect.intersected(viewport_rect);
    if (forced_damage_rect.contains(viewport_rect))
        return viewport_rect;
    auto damage_rect = damage_since_last_raster(viewport_rect.size(), frame_content);
    damage_rect.unite(forced_damage_rect);
    return damage_rect;
}

Gfx::IntRect ContextPresenter::damage_since_last_raster(Gfx::IntSize viewport_size, FrameContent const& frame_content)
{
    auto const& presented_scroll_state_snapshot = frame_content.presented_scroll_state_snapshot;
    Gfx::IntRect viewport_rect { {}, viewport_size };
    if (!m_last_rasterized_frame.has_value())
        return viewport_rect;
    auto const& last_frame = *m_last_rasterized_frame;
    if (last_frame.viewport_size != viewport_size)
        return viewport_rect;
    if (last_frame.display_list->surface_clear_color() != m_scene.display_list()->surface_clear_color())
        return viewport_rect;
    if (auto index = frame_content.viewport_scroll_node_index; index.has_value()) {
        if (last_frame.scroll_state_snapshot.device_offset_for_index(*index) != presented_scroll_state_snapshot.device_offset_for_index(*index))
            return viewport_rect;
    }

    Vector<Compositing::CanvasId> canvases_with_new_content;
    for (auto const& [canvas_id, content_generation] : last_frame.canvas_content_generations) {
        if (m_canvas_surfaces.canvas_content_generation(canvas_id) != content_generation)
            canvases_with_new_content.append(canvas_id);
    }
    Vector<Compositing::VideoSinkResourceId> video_sinks_with_new_frames;
    auto video_sinks_drawn_by_list = m_scene.display_list()->referenced_resource_ids().video_sinks;
    for (auto const& [video_sink_id, frame_slot] : last_frame.video_frame_slots) {
        if (current_video_frame_slot(video_sink_id) == frame_slot)
            continue;
        // A video drawn only by a nested display list, such as an SVG image's, has no command in
        // this list to damage.
        if (!video_sinks_drawn_by_list.contains_slow(video_sink_id.value()))
            return viewport_rect;
        video_sinks_with_new_frames.append(video_sink_id);
    }
    auto display_list_damage = Compositing::compute_display_list_damage(
        *last_frame.display_list,
        last_frame.visual_context_tree,
        last_frame.scroll_state_snapshot,
        *m_scene.display_list(),
        m_scene.visual_context_tree_for_compositing(),
        presented_scroll_state_snapshot,
        viewport_rect,
        { .canvases = canvases_with_new_content, .video_sinks = video_sinks_with_new_frames });
    if (!display_list_damage.has_value())
        return viewport_rect;

    auto damage_rect = *display_list_damage;
    for (auto const& scrollbar : frame_content.scrollbar_controller.scrollbars()) {
        if (!scrollbar.is_painted_by_compositor)
            continue;
        if (last_frame.scroll_state_snapshot.device_offset_for_index(scrollbar.scroll_node_index) == presented_scroll_state_snapshot.device_offset_for_index(scrollbar.scroll_node_index))
            continue;
        damage_rect.unite(scrollbar.gutter_rect.united(scrollbar.expanded_gutter_rect));
    }
    damage_rect.intersect(viewport_rect);
    return damage_rect;
}

void ContextPresenter::remember_rasterized_frame(Gfx::IntSize viewport_size, Compositing::ScrollStateSnapshot const& presented_scroll_state_snapshot)
{
    HashMap<Compositing::CanvasId, u64> canvas_content_generations;
    for (auto id : m_scene.display_list()->referenced_resource_ids().canvases) {
        Compositing::CanvasId canvas_id { id };
        canvas_content_generations.set(canvas_id, m_canvas_surfaces.canvas_content_generation(canvas_id));
    }
    // Every video the context holds is drawn by the list or by a display list it nests.
    HashMap<Compositing::VideoSinkResourceId, Optional<VideoFrameSlot>> video_frame_slots;
    for (auto const& [id, handle] : m_scene.video_sink_handles()) {
        Compositing::VideoSinkResourceId video_sink_id { id };
        video_frame_slots.set(video_sink_id, current_video_frame_slot(video_sink_id));
    }
    m_last_rasterized_frame = RasterizedFrame {
        .display_list = NonnullRefPtr { *m_scene.display_list() },
        .visual_context_tree = m_scene.visual_context_tree_for_compositing(),
        .scroll_state_snapshot = presented_scroll_state_snapshot,
        .viewport_size = viewport_size,
        .canvas_content_generations = move(canvas_content_generations),
        .video_frame_slots = move(video_frame_slots),
    };
}

Optional<ContextPresenter::VideoFrameSlot> ContextPresenter::current_video_frame_slot(Compositing::VideoSinkResourceId video_sink_id)
{
    auto sink = m_scene.resource_storage().video_sink(video_sink_id);
    if (!sink)
        return {};
    auto frame = sink->current_frame();
    if (!frame)
        return {};
    auto handle = Media::VideoFrameHandle::for_frame(*frame);
    return VideoFrameSlot {
        .pool_id = handle.pool_id,
        .slot_index = handle.slot_index,
        .slot_acquisition_id = handle.slot_acquisition_id,
    };
}

void ContextPresenter::paint_current_display_list(DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface, FrameContent const& frame_content, Optional<Gfx::IntRect> damage_rect, PaintUIOverlay paint_ui_overlay, bool apply_raster_transform)
{
    VERIFY(m_scene.display_list());
    auto surface_clear_color = Gfx::to_skia_color(m_scene.display_list()->surface_clear_color().value_or(Gfx::Color::Transparent));
    auto paint_display_list = [&](Gfx::PaintingSurface& target_surface) {
        display_list_player.execute(
            *m_scene.display_list(),
            m_scene.visual_context_tree_for_compositing(),
            m_scene.resource_storage(),
            m_scene.raster_cache(),
            frame_content.presented_scroll_state_snapshot,
            target_surface,
            &m_canvas_surfaces,
            frame_content.composited_context_resolver,
            &frame_content.scrollbar_controller);
        frame_content.scrollbar_controller.paint(target_surface, display_list_player, frame_content.presented_scroll_state_snapshot);
        if (paint_ui_overlay == PaintUIOverlay::No)
            return;

        if (m_ui_overlays.paused_debugger.has_value())
            paint_paused_debugger_overlay(target_surface, m_viewport_size, m_device_pixel_ratio, *m_ui_overlays.paused_debugger);
        if (auto opacity = m_picture_in_picture_controls_fade.opacity(); m_ui_overlays.picture_in_picture_controls.has_value() && opacity > 0)
            paint_picture_in_picture_controls(target_surface, m_viewport_size, m_device_pixel_ratio, *m_ui_overlays.picture_in_picture_controls, opacity);
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
