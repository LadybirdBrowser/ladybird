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

namespace Compositing {

struct KeyEvent;
struct MouseEvent;
struct PinchEvent;

}

namespace Compositor {

class CompositorStateClient {
public:
    virtual ~CompositorStateClient() = default;

    virtual void did_allocate_backing_stores(Compositing::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) = 0;
    virtual void did_present_frame(Compositing::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id) = 0;
};

class CompositorStateWebContentClient {
public:
    virtual ~CompositorStateWebContentClient() = default;

    virtual void dispatch_mouse_event_to_web_content(u64 page_id, Compositing::MouseEvent const&) = 0;
    virtual void dispatch_key_event_to_web_content(u64 page_id, Compositing::KeyEvent const&) = 0;
    virtual void request_rendering_update() = 0;
    virtual void rendering_opportunity(Compositing::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) = 0;
    virtual void async_scroll_updates(Compositing::CompositorContextId, Compositing::PendingAsyncScrollUpdates const&) = 0;
    virtual void create_video_edge(Media::VideoSinkHandle) = 0;
    virtual void release_video_edge(Media::VideoSinkHandle) = 0;
};

class CompositorState final : public RefCounted<CompositorState> {
public:
    static NonnullRefPtr<CompositorState> create(RefPtr<Gfx::SkiaBackendContext>, bool async_scrolling_enabled);
    ~CompositorState();

    enum class ContextOwnerCheckResult {
        OwnedByClient,
        ContextUnavailable,
        ConflictingOwner,
    };

    void set_client(CompositorStateClient&);
    ContextOwnerCheckResult check_context_owner(Compositing::CompositorContextId, CompositorStateWebContentClient&);
    void destroy_contexts_for_web_content_client(CompositorStateWebContentClient&);

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context() const { return m_skia_backend_context; }
    Compositing::CanvasSurfaceRegistry& canvas_surface_registry() { return m_canvas_surface_registry; }
    Compositing::CanvasSurfaceRegistry const& canvas_surface_registry() const { return m_canvas_surface_registry; }

    void create_context(Compositing::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&);
    void destroy_context(Compositing::CompositorContextId);

    void set_parent_context(Compositing::CompositorContextId, Optional<Compositing::CompositorContextId> parent_context_id);
    void stop_presenting_to_client(Compositing::CompositorContextId);
    void update_display_list(Compositing::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, Compositing::ScrollStateSnapshot&&);
    void update_display_list_resources(Compositing::CompositorContextId, Compositing::DisplayListResourceTransaction&&);
    void update_visual_context_tree(Compositing::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&);
    void update_scroll_state(Compositing::CompositorContextId, Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState);
    void add_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void remove_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void set_video_sink_ticking(CompositorStateWebContentClient&, Media::VideoSinkHandle, bool should_tick);
    void on_video_sink_ready(CompositorStateWebContentClient&, Media::VideoSinkHandle, NonnullRefPtr<Media::DisplayingVideoSink> const&);
    void invalidate_wheel_event_listener_state(Compositing::CompositorContextId, u64 generation);
    void invalidate_keyboard_scroll_state(Compositing::CompositorContextId, u64 generation);
    bool handle_key_event(Compositing::CompositorContextId, Compositing::KeyEvent const&);
    bool dispatch_key_event_to_web_content(Compositing::CompositorContextId, Compositing::KeyEvent const&);
    Compositing::MouseEventHandlingResult handle_mouse_event(Compositing::CompositorContextId, Compositing::MouseEvent const&);
    bool dispatch_mouse_event_to_web_content(Compositing::CompositorContextId, Compositing::MouseEvent const&);
    bool handle_pinch_event(Compositing::CompositorContextId, Compositing::PinchEvent const&);
    Compositing::AsyncScrollEnqueueResult async_scroll_by(Compositing::CompositorContextId, Compositing::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers, Compositing::AsyncScrollOperationTracking);
    Compositing::AsyncScrollEnqueueResult smooth_scroll_to(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator);
    void cancel_smooth_scroll(Compositing::CompositorContextId, Compositing::AsyncScrollNodeStableID);
    bool async_scroll_by(Compositing::CompositorContextId, Gfx::FloatPoint position, Gfx::FloatPoint delta, Compositing::WheelDeltaPrecision, Compositing::ScrollGesturePhase, u32 modifiers);
    void viewport_size_updated(Compositing::CompositorContextId, Gfx::IntSize, Compositing::WindowResizingInProgress);
    void request_rendering_opportunity(Compositing::CompositorContextId, double maximum_frames_per_second);
    void set_paused_debugger_overlay(Compositing::CompositorContextId, bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<Compositing::PausedDebuggerOverlayAction> hovered_action);
    void set_display_metadata(Compositing::CompositorContextId, Optional<u64> display_id, double refresh_rate);
    void set_context_visibility(Compositing::CompositorContextId, Compositing::ContextVisibility);
    void present_frame(Compositing::CompositorContextId, Gfx::IntRect viewport_rect);
    // Delivers the rendering opportunity a context requested now rather than at the next display tick: a
    // viewport change that arrived while an animation's opportunity was outstanding starts its update at once.
    void hurry_rendering_opportunity(Compositing::CompositorContextId);
    bool request_screenshot(Compositing::CompositorContextId, Gfx::ShareableBitmap&);
    void presented_bitmap_ready_to_paint(Compositing::CompositorContextId, i32 bitmap_id);
    void set_client_gpu_presentation_capability(bool supported, u64 adapter_luid);

private:
    CompositorState(RefPtr<Gfx::SkiaBackendContext>, bool async_scrolling_enabled);

    struct PendingAsyncPresent {
        PendingAsyncPresent(Compositing::CompositorContextId context_id, Gfx::IntRect viewport_rect, Gfx::IntRect damage_rect, i32 bitmap_id)
            : context_id(context_id)
            , viewport_rect(viewport_rect)
            , damage_rect(damage_rect)
            , bitmap_id(bitmap_id)
        {
        }

        Compositing::CompositorContextId context_id;
        Gfx::IntRect viewport_rect;
        Gfx::IntRect damage_rect;
        i32 bitmap_id { 0 };
        bool was_cancelled { false };
    };

    ContextState* context_if_present(Compositing::CompositorContextId);
    // Hands the context's async scroll updates to its WebContent process as soon as they exist,
    // so a rendering update reads them locally instead of asking for them over a synchronous call.
    void publish_pending_async_scroll_updates(Compositing::CompositorContextId, ContextState&);

public:
    void present_pending_frames_for_testing() { present_pending_frames_on_vsync({}, MonotonicTime::now()); }
    size_t pending_async_present_count_for_testing() const { return m_pending_async_presents.size(); }

    // What was not published yet, for a caller that needs the compositor's state as of now.
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Compositing::CompositorContextId);

private:
    ContextState const* context_if_present(Compositing::CompositorContextId) const;
    Optional<u64> display_id_for_context(ContextState const&) const;
    ContextState const* root_context_of(ContextState const&) const;
    bool context_is_effectively_visible(ContextState const&) const;
    void resume_presentation_after_becoming_visible(Compositing::CompositorContextId root_context_id, ContextState& root_context);
    double display_refresh_rate_for_context(ContextState const&) const;
    void clear_parent_context(ContextState&);
    CompositedContextResolver resolver_for(Compositing::CompositorContextId parent_context_id);
    Compositing::CompositedContextSurface resolve_composited_context(Compositing::CompositorContextId parent_context_id, Compositing::CompositorContextId child_context_id, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform);
    void schedule_backing_store_shrink(Compositing::CompositorContextId, ContextState&);
    void shrink_backing_stores_after_resize(Compositing::CompositorContextId);
    void resize_backing_stores_if_needed(Compositing::CompositorContextId, ContextState&);
    void present_current_frame(Compositing::CompositorContextId, ContextState&);
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
    bool apply_context_update_result(
        Compositing::CompositorContextId,
        ContextState&,
        ContextState::ContextUpdateResult const&);
    void present_frame(Compositing::CompositorContextId, ContextState&, ContextState::PendingFrame);
    void schedule_present_frame(Compositing::CompositorContextId, ContextState&, ContextState::PendingFrame);
    void schedule_present_frame(Compositing::CompositorContextId, ContextState&, Gfx::IntRect viewport_rect);
    void schedule_pending_present_frame(Compositing::CompositorContextId, ContextState&);
    void schedule_pending_present_frame_on_vsync(Compositing::CompositorContextId, ContextState&);
    void schedule_containing_context_present(ContextState&);
    void schedule_pending_present_frame_if_unblocked(Compositing::CompositorContextId, ContextState&);
    bool try_present_frame_during_resize(Compositing::CompositorContextId, ContextState&);
    void schedule_caret_repaint(Compositing::CompositorContextId, Gfx::IntRect damage_rect);
    VSyncScheduler& vsync_scheduler_for_display(Optional<u64> display_id);
    void present_pending_frames_on_vsync(Optional<u64> display_id, MonotonicTime frame_time);
    void publish_backing_stores(Compositing::CompositorContextId, ContextState&, BackingStoreManager::Publication&&);
    BackingStoreManager::GpuSharing gpu_sharing_for_client() const;
    void did_finish_async_present(PendingAsyncPresent&);
    void cancel_pending_async_presents_for_context(Compositing::CompositorContextId);
    void schedule_gpu_completion_check();
    void check_gpu_completions();

    HashMap<Compositing::CompositorContextId, OwnPtr<ContextState>> m_contexts;

    DoublyLinkedList<PendingAsyncPresent> m_pending_async_presents;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Compositing::CanvasSurfaceRegistry m_canvas_surface_registry;
    OwnPtr<Compositing::DisplayListPlayerSkia> m_display_list_player;
    HashMap<Optional<u64>, OwnPtr<VSyncScheduler>> m_vsync_schedulers_by_display;
    RefPtr<Core::Timer> m_gpu_completion_timer;
    CompositorStateClient* m_client { nullptr };
    bool m_async_scrolling_enabled { true };

    // LUID of the GPU adapter the client can present shared GPU textures on, if any.
    Optional<u64> m_client_gpu_presentation_adapter_luid;

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
};

}
