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
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/ContextState.h>
#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/VSyncScheduler.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
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
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaCodecs/VideoFramePool.h>

namespace Compositor {

class CompositorStateClient {
public:
    virtual ~CompositorStateClient() = default;

    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) = 0;
    // The stores join the ones allocated last, and leave them again once retired.
    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) = 0;
    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) = 0;
    virtual void did_present_frame(Web::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio) = 0;
    // The compositor performed the event's whole default action, so the UI hears nothing more about it.
    virtual void did_consume_input_event(Web::CompositorContextId, u64 event_id) = 0;
    // No context could take the event; the UI sends it to WebContent itself.
    virtual void did_not_dispatch_input_event(Web::CompositorContextId, u64 event_id) = 0;
    // The compositor forwarded the key event to WebContent, after performing its default action by scrolling if it says so.
    virtual void did_dispatch_input_event(Web::CompositorContextId, u64 event_id, bool async_scroll_performed_default_action) = 0;
};

class CompositorStateWebContentClient {
public:
    virtual ~CompositorStateWebContentClient() = default;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const&) = 0;
    virtual void dispatch_key_event_to_web_content(u64 page_id, Web::KeyEvent const&) = 0;
    virtual void request_rendering_update() = 0;
    virtual void rendering_opportunity(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) = 0;
    virtual void clock_tick(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds, Vector<Web::CompositorScrollOffset> const& scroll_offsets) = 0;
    virtual void pointer_moved(Web::CompositorContextId, Web::DevicePixelPoint, u32 /* buttons */, bool /* scrolled_since_frame */, u64 /* input_event_id */) { }
    virtual void pointer_left(Web::CompositorContextId, u64 /* input_event_id */) { }
    virtual void async_scroll_updates(Web::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) = 0;
    virtual void overlay_scrollbar_visibilities_changed(Vector<Web::OverlayScrollbarVisibility> const&) { }
    virtual void create_video_edge(Media::VideoSinkHandle) = 0;
    virtual void release_video_edge(Media::VideoSinkHandle) = 0;
    virtual void video_sink_presented_frame(Media::VideoSinkHandle, RefPtr<Media::VideoFrame>, Media::DisplayingVideoSinkFrameCounts) { }
    virtual void video_sink_counted_frames(Media::VideoSinkHandle, Media::DisplayingVideoSinkFrameCounts) { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) = 0;
    // The owner of this client's canvases, which are the only ones its contexts may draw.
    virtual Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const = 0;
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

    struct PresentationCapabilities {
        bool synthesizes_scroll_momentum { false };
        bool scrolls_elastically { false };
        // Whether a press on a scrollbar track beside the thumb jumps there rather than paging toward it.
        bool scrollbar_track_press_jumps { false };
        // LUID of the GPU adapter the presentation can present shared GPU textures on, if any.
        Optional<u64> gpu_presentation_adapter_luid;
    };

    // Each root context presents to the presentation it is bound to. Unbound contexts keep rendering, but publish nothing.
    void register_presentation(CompositorStateClient&, PresentationCapabilities);
    void unregister_presentation(CompositorStateClient&);
    void bind_presentation(Web::CompositorContextId, CompositorStateClient&);
    CompositorStateClient* presentation_for_context(Web::CompositorContextId) const;
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, CompositorStateWebContentClient&);
    void destroy_contexts_for_web_content_client(CompositorStateWebContentClient&);

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context() const { return m_skia_backend_context; }
    CanvasSurfaceRegistry& canvas_surface_registry() { return m_canvas_surface_registry; }
    CanvasSurfaceRegistry const& canvas_surface_registry() const { return m_canvas_surface_registry; }

    void create_context(Web::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&);
    void destroy_context(Web::CompositorContextId);

    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId> parent_context_id);
    void stop_presenting_to_client(Web::CompositorContextId);
    void update_display_list(Web::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, Compositing::ScrollStateSnapshot&&);
    void update_display_list_resources(Web::CompositorContextId, Compositing::DisplayListResourceTransaction&&);
    void update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&);
    void update_scroll_state(Web::CompositorContextId, Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState);
    void add_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    // For a presentation channel that arrives after the sinks did, or that replaces the one their edges were on.
    void create_video_edges(CompositorStateWebContentClient&);
    void remove_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void set_video_sink_ticking(CompositorStateWebContentClient&, Media::VideoSinkHandle, bool should_tick);
    void on_video_sink_ready(CompositorStateWebContentClient&, Media::VideoSinkHandle, NonnullRefPtr<Media::DisplayingVideoSink> const&);
    void invalidate_wheel_event_listener_state(Web::CompositorContextId, u64 generation);
    void invalidate_keyboard_scroll_state(Web::CompositorContextId, u64 generation);
    void handle_and_dispatch_key_event(Web::CompositorContextId, Web::KeyEvent, bool allow_keyboard_scroll);
    void handle_and_dispatch_mouse_event(Web::CompositorContextId, Web::MouseEvent);
    void handle_pinch_event(Web::CompositorContextId, Web::PinchEvent const&);
    Compositing::AsyncScrollEnqueueResult async_scroll_by(Web::CompositorContextId, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking, Compositing::WheelStepDispatch, u64 input_event_id);
    Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Web::CompositorContextId, Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator, Optional<Compositing::AsyncScrollOperationID> operation_to_continue);
    void cancel_smooth_scroll(Web::CompositorContextId, Web::AsyncScrollNodeStableID);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress);
    void request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second);
    void set_ui_overlays(Web::CompositorContextId, Compositing::UIOverlays);
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
    void set_client_gpu_presentation_capability(CompositorStateClient&, bool supported, u64 adapter_luid);
    void set_scrollbar_track_press_jumps(CompositorStateClient&, bool jumps);
    String scroll_state_for_testing(Web::CompositorContextId, Web::AsyncScrollNodeStableID stretched_scroller) const;
    Optional<Compositing::FirstWheelStepRouteForTesting> route_first_wheel_step_for_testing(Web::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels);
    void begin_input_trace_for_testing(Web::CompositorContextId);
    Vector<Web::InputTraceRecord> take_input_trace_for_testing(Web::CompositorContextId);

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
        PendingAsyncPresent(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio)
            : context_id(context_id)
            , viewport_rect(viewport_rect)
            , damage_rect(damage_rect)
            , bitmap_id(bitmap_id)
            , device_pixel_ratio(device_pixel_ratio)
        {
        }

        Web::CompositorContextId context_id;
        Gfx::IntRect viewport_rect;
        Gfx::IntRect damage_rect;
        i32 bitmap_id { 0 };
        double device_pixel_ratio { 1.0 };
        bool was_cancelled { false };
        bool was_delivered { false };
        // A frame that started rendering before its context was bound has no published buffer to present to.
        bool publish_to_presentation { true };
    };

    ContextState* context_if_present(Web::CompositorContextId);
    // Hands the context's async scroll updates to its WebContent process as soon as they exist,
    // so a rendering update reads them locally instead of asking for them over a synchronous call.
    void publish_pending_async_scroll_updates(Web::CompositorContextId, ContextState&);

public:
    void present_pending_frames_for_testing() { present_pending_frames_on_vsync({}, MonotonicTime::now()); }
    size_t pending_async_present_count_for_testing() const { return m_pending_async_presents.size(); }

    // What was not published yet, for a caller that needs the compositor's state as of now.
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId);

private:
    ContextState const* context_if_present(Web::CompositorContextId) const;
    Optional<u64> display_id_for_context(ContextState const&) const;
    ContextState const* root_context_of(ContextState const&) const;
    bool nested_context_scrolled_since_last_frame(Web::CompositorContextId) const;
    bool context_is_effectively_visible(ContextState const&) const;
    void resume_presentation_after_becoming_visible(Web::CompositorContextId root_context_id, ContextState& root_context);
    double display_refresh_rate_for_context(ContextState const&) const;
    void clear_parent_context(ContextState&);
    CompositedContextResolver resolver_for(Web::CompositorContextId parent_context_id);
    CompositedContextSurface resolve_composited_context(Web::CompositorContextId parent_context_id, Web::CompositorContextId child_context_id, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform);
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
    enum class WheelStepOrigin : u8 {
        UI,
        Fling,
    };
    // A fling step is scrolled at the frame time the fling synthesized it for.
    ContextState::ContextUpdateResult scroll_for_wheel_step(Web::CompositorContextId, ContextState&, Web::MouseEvent const&, WheelStepOrigin, Optional<MonotonicTime> fling_step_time = {});
    struct WheelStepHandedToNestedContext {
        // What is left for the context the step was handed from to present and report.
        ContextState::ContextUpdateResult result;
        bool nested_context_presents_frame { false };
    };
    WheelStepHandedToNestedContext hand_wheel_step_to_nested_context(Web::CompositorContextId, ContextState&, Web::MouseEvent const&, ContextState::NestedContextWheelStep const&, MonotonicTime now);
    struct NestedContextTakingWheelStep {
        Web::CompositorContextId context_id;
        // Null when the step cannot be handed to the context.
        ContextState* context { nullptr };
        Gfx::FloatPoint position {};
        ContextState::FirstWheelStepRoute route;
    };
    // The context that takes the first step of a gesture over the content of a context nested in the context
    // `context_id`, among that context and the contexts nested in it. Empty when each of them passes the step on.
    Optional<NestedContextTakingWheelStep> nested_context_taking_first_wheel_step(Web::CompositorContextId, Compositing::NestedContextWheelHit, Gfx::FloatPoint delta_in_css_pixels, Web::WheelDeltaPrecision);
    bool is_nested_in(Web::CompositorContextId containing_context_id, ContextState const&) const;
    void dispatch_scroll_fling_step(Web::CompositorContextId, ContextState&, Optional<Web::MouseEvent>, MonotonicTime now);
    void present_frame(Web::CompositorContextId, ContextState&, PendingFrame);
    void schedule_present_frame(Web::CompositorContextId, ContextState&, PendingFrame);
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
    BackingStoreManager::GpuSharing gpu_sharing_for_context(Web::CompositorContextId) const;
    void unbind_presentation(Web::CompositorContextId);
    void update_scroll_capabilities();
    void did_finish_async_present(PendingAsyncPresent&);
    void deliver_rendered_frame(PendingAsyncPresent&);
    void cancel_pending_async_presents_for_context(Web::CompositorContextId);
    void schedule_gpu_completion_check();
    void check_gpu_completions();

    HashMap<Web::CompositorContextId, OwnPtr<ContextState>> m_contexts;

    DoublyLinkedList<PendingAsyncPresent> m_pending_async_presents;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CanvasSurfaceRegistry m_canvas_surface_registry;
    OwnPtr<DisplayListPlayerSkia> m_display_list_player;
    HashMap<Optional<u64>, OwnPtr<VSyncScheduler>> m_vsync_schedulers_by_display;
    RefPtr<Core::Timer> m_gpu_completion_timer;

    HashMap<CompositorStateClient*, PresentationCapabilities> m_presentations;
    HashMap<Web::CompositorContextId, CompositorStateClient*> m_context_presentations;

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
