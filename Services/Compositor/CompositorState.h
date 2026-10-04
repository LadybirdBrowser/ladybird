/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DoublyLinkedList.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <Compositor/ContextState.h>
#include <Compositor/VSyncScheduler.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/CanvasSurfaceRegistry.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListPlayerSkia.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Types.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibMedia/Forward.h>
#include <LibMedia/VideoFramePool.h>
#include <LibMedia/VideoSinkHandle.h>

namespace Compositor {

class CompositorStateClient {
public:
    virtual ~CompositorStateClient() = default;

    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) = 0;
    // The stores join the ones allocated last, and leave them again once retired.
    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) = 0;
    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) = 0;
    virtual void did_present_frame(Web::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id) = 0;
    // The compositor performed the event's whole default action, so the UI hears nothing more about it.
    virtual void did_consume_input_event(Web::CompositorContextId, u64 event_id) = 0;
    // No context could take the event; the UI sends it to WebContent itself.
    virtual void did_not_dispatch_input_event(Web::CompositorContextId, u64 event_id) = 0;
};

class CompositorStateWebContentClient {
public:
    virtual ~CompositorStateWebContentClient() = default;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const&) = 0;
    virtual void dispatch_key_event_to_web_content(u64 page_id, Web::KeyEvent const&) = 0;
    virtual void request_rendering_update() = 0;
    virtual void rendering_opportunity(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) = 0;
    virtual void clock_tick(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) = 0;
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) = 0;
    virtual void create_video_edge(Media::VideoSinkHandle) = 0;
    virtual void release_video_edge(Media::VideoSinkHandle) = 0;
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) = 0;
};

class CompositorState final : public RefCounted<CompositorState> {
public:
    static NonnullRefPtr<CompositorState> create(RefPtr<Gfx::SkiaBackendContext>);
    ~CompositorState();

    enum class ContextOwnerCheckResult {
        OwnedByClient,
        ContextUnavailable,
        ConflictingOwner,
    };

    void set_client(CompositorStateClient&);
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, CompositorStateWebContentClient&);
    void destroy_contexts_for_web_content_client(CompositorStateWebContentClient&);

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context() const { return m_skia_backend_context; }
    Compositing::CanvasSurfaceRegistry& canvas_surface_registry() { return m_canvas_surface_registry; }
    Compositing::CanvasSurfaceRegistry const& canvas_surface_registry() const { return m_canvas_surface_registry; }

    void create_context(Web::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&);
    void destroy_context(Web::CompositorContextId);

    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId> parent_context_id);
    void stop_presenting_to_client(Web::CompositorContextId);
    void update_display_list(Web::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, Compositing::ScrollStateSnapshot&&);
    void update_display_list_resources(Web::CompositorContextId, Compositing::DisplayListResourceTransaction&&);
    void update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&);
    void update_scroll_state(Web::CompositorContextId, Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState);
    void add_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void remove_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void set_video_sink_ticking(CompositorStateWebContentClient&, Media::VideoSinkHandle, bool should_tick);
    void on_video_sink_ready(CompositorStateWebContentClient&, Media::VideoSinkHandle, NonnullRefPtr<Media::DisplayingVideoSink> const&);
    void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation);
    void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation);
    bool handle_key_event(Web::CompositorContextId, Web::KeyEvent const&);
    bool dispatch_key_event_to_web_content(Web::CompositorContextId, Web::KeyEvent const&);
    void handle_and_dispatch_mouse_event(Web::CompositorContextId, Web::MouseEvent);
    void handle_pinch_event(Web::CompositorContextId, Web::PinchEvent const&);
    Compositing::AsyncScrollEnqueueResult async_scroll_by(Web::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking);
    Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Web::CompositorContextId, Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator);
    void cancel_smooth_scroll(Web::CompositorContextId, Web::AsyncScrollNodeStableID);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress);
    void request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second);
    void set_paused_debugger_overlay(Web::CompositorContextId, bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<Compositing::PausedDebuggerOverlayAction> hovered_action);
    void set_display_metadata(Web::CompositorContextId, Optional<u64> display_id, double refresh_rate);
    void set_context_visibility(Web::CompositorContextId, Compositing::ContextVisibility);
    void present_frame(Web::CompositorContextId, Gfx::IntRect viewport_rect);
    // Delivers the rendering opportunity a context requested now rather than at the next display tick: a
    // viewport change that arrived while an animation's opportunity was outstanding starts its update at once.
    void hurry_rendering_opportunity(Web::CompositorContextId);
    // Asks for the next display tick the context's maximum rate lets through, for its render clock.
    void request_clock_tick(Web::CompositorContextId, double maximum_frames_per_second);
    bool request_screenshot(Web::CompositorContextId, Gfx::ShareableBitmap&);
    void presented_bitmap_ready_to_paint(Web::CompositorContextId, i32 bitmap_id);
    void set_client_gpu_presentation_capability(bool supported, u64 adapter_luid);
    void set_synthesizes_scroll_momentum(bool);

    struct PlaceholderCanvasAllocation {
        Compositing::CanvasId canvas_id;
        u64 secret { 0 };
    };
    PlaceholderCanvasAllocation allocate_placeholder_canvas(CompositorStateWebContentClient&);
    void release_placeholder_canvas(CompositorStateWebContentClient&, Compositing::CanvasId);
    void commit_placeholder_canvas(Compositing::CanvasId, u64 secret, RefPtr<Gfx::PaintingSurface> source_surface, Gfx::IntSize, bool origin_clean);
    struct PlaceholderCanvasPixels {
        Gfx::ShareableBitmap pixels;
        bool origin_clean { true };
    };
    PlaceholderCanvasPixels read_placeholder_canvas_pixels(CompositorStateWebContentClient&, Compositing::CanvasId, Gfx::IntRect);

private:
    CompositorState(RefPtr<Gfx::SkiaBackendContext>);

    struct PendingAsyncPresent {
        PendingAsyncPresent(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect, Gfx::IntRect damage_rect, i32 bitmap_id)
            : context_id(context_id)
            , viewport_rect(viewport_rect)
            , damage_rect(damage_rect)
            , bitmap_id(bitmap_id)
        {
        }

        Web::CompositorContextId context_id;
        Gfx::IntRect viewport_rect;
        Gfx::IntRect damage_rect;
        i32 bitmap_id { 0 };
        bool was_cancelled { false };
    };

    ContextState* context_if_present(Web::CompositorContextId);
    // Hands the context's async scroll updates to its WebContent process as soon as they exist,
    // so a rendering update reads them locally instead of asking for them over a synchronous call.
    void publish_pending_async_scroll_updates(Web::CompositorContextId, ContextState&);

public:
    void present_pending_frames_for_testing() { present_pending_frames_on_vsync({}, MonotonicTime::now()); }
    void retire_idle_surplus_backing_stores_for_testing(Web::CompositorContextId context_id) { retire_idle_surplus_backing_stores(context_id); }
    size_t pending_async_present_count_for_testing() const { return m_pending_async_presents.size(); }

    // What was not published yet, for a caller that needs the compositor's state as of now.
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId);

private:
    ContextState const* context_if_present(Web::CompositorContextId) const;
    Optional<u64> display_id_for_context(ContextState const&) const;
    ContextState const* root_context_of(ContextState const&) const;
    bool context_is_effectively_visible(ContextState const&) const;
    void resume_presentation_after_becoming_visible(Web::CompositorContextId root_context_id, ContextState& root_context);
    double display_refresh_rate_for_context(ContextState const&) const;
    void clear_parent_context(ContextState&);
    CompositedContextResolver resolver_for(Web::CompositorContextId parent_context_id);
    Compositing::CompositedContextSurface resolve_composited_context(Web::CompositorContextId parent_context_id, Web::CompositorContextId child_context_id, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform);
    void schedule_backing_store_shrink(Web::CompositorContextId, ContextState&);
    void shrink_backing_stores_after_resize(Web::CompositorContextId);
    void resize_backing_stores_if_needed(Web::CompositorContextId, ContextState&);
    void add_backing_store_for_pending_frame_if_needed(Web::CompositorContextId, ContextState&);
    void schedule_surplus_backing_store_retirement(Web::CompositorContextId, ContextState&);
    void retire_idle_surplus_backing_stores(Web::CompositorContextId);
    void present_current_frame(Web::CompositorContextId, ContextState&);
    void resolve_video_sinks(ContextState&);
    enum class VideoSinkUpdateResult : u8 {
        NoUnpaintedSinkRequiresUpdates,
        UnpaintedSinkRequiresUpdates,
    };
    VideoSinkUpdateResult update_all_video_sinks();
    void update_video_sinks_for_display(Optional<u64> display_id);
    void update_unpainted_video_sinks();
    void schedule_unpainted_video_updates();
    int unpainted_video_update_interval_ms() const;
    void present_contexts_drawing_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void present_contexts_drawing_canvas(CompositorStateWebContentClient&, Compositing::CanvasId);
    bool apply_context_update_result(
        Web::CompositorContextId,
        ContextState&,
        ContextState::ContextUpdateResult const&);
    void schedule_animation_frames_if_needed(ContextState&);
    void dispatch_scroll_fling_step(Web::CompositorContextId, ContextState&, Optional<ContextState::ScrollFlingStep>);
    void present_frame(Web::CompositorContextId, ContextState&, ContextState::PendingFrame);
    void schedule_present_frame(Web::CompositorContextId, ContextState&, ContextState::PendingFrame);
    void schedule_present_frame(Web::CompositorContextId, ContextState&, Gfx::IntRect viewport_rect);
    void schedule_pending_present_frame(Web::CompositorContextId, ContextState&);
    void schedule_pending_present_frame_on_vsync(Web::CompositorContextId, ContextState&);
    void schedule_containing_context_present(ContextState&);
    void schedule_pending_present_frame_if_unblocked(Web::CompositorContextId, ContextState&);
    bool try_present_frame_during_resize(Web::CompositorContextId, ContextState&);
    void schedule_caret_repaint(Web::CompositorContextId, Gfx::IntRect damage_rect);
    VSyncScheduler& vsync_scheduler_for_display(Optional<u64> display_id);
    void present_pending_frames_on_vsync(Optional<u64> display_id, MonotonicTime frame_time);
    void publish_backing_stores(Web::CompositorContextId, ContextState&, BackingStoreManager::Publication&&);
    BackingStoreManager::GpuSharing gpu_sharing_for_client() const;
    void did_finish_async_present(PendingAsyncPresent&);
    void cancel_pending_async_presents_for_context(Web::CompositorContextId);
    void schedule_gpu_completion_check();
    void check_gpu_completions();

    HashMap<Web::CompositorContextId, OwnPtr<ContextState>> m_contexts;

    DoublyLinkedList<PendingAsyncPresent> m_pending_async_presents;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Compositing::CanvasSurfaceRegistry m_canvas_surface_registry;
    OwnPtr<Compositing::DisplayListPlayerSkia> m_display_list_player;
    HashMap<Optional<u64>, OwnPtr<VSyncScheduler>> m_vsync_schedulers_by_display;
    RefPtr<Core::Timer> m_gpu_completion_timer;
    CompositorStateClient* m_client { nullptr };

    // LUID of the GPU adapter the client can present shared GPU textures on, if any.
    Optional<u64> m_client_gpu_presentation_adapter_luid;
    bool m_synthesizes_scroll_momentum { false };

    struct VideoSinkState {
        RefPtr<Media::DisplayingVideoSink> sink;
        bool should_tick { true };
        bool requires_updates { false };
    };
    VideoSinkState* video_sink_state(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    static bool video_sink_updates_are_needed(VideoSinkState const&);
    bool video_sink_is_painted_by_any_context(CompositorStateWebContentClient*, Media::VideoSinkHandle) const;
    void update_unpainted_video_update_scheduling();
    HashMap<CompositorStateWebContentClient*, HashMap<Media::VideoSinkHandle, VideoSinkState>> m_video_sink_states;
    RefPtr<Core::Timer> m_unpainted_video_update_timer;

    struct PlaceholderCanvas {
        CompositorStateWebContentClient* owner { nullptr };
        u64 secret { 0 };
        RefPtr<Gfx::PaintingSurface> surface;
        Optional<Gfx::IntSize> size;
        bool origin_clean { true };
    };
    HashMap<Compositing::CanvasId, PlaceholderCanvas> m_placeholder_canvases;
};

}
