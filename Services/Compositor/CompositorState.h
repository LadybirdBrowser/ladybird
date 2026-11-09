/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/DoublyLinkedList.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <Compositor/AnimatedImage.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/CompositorCore.h>
#include <Compositor/ContextPresenter.h>
#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/FramePacer.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/VSyncScheduler.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Types.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaCodecs/VideoFramePool.h>

namespace Compositor {

class CompositorStateClient {
public:
    virtual ~CompositorStateClient() = default;

    virtual PresentationId presentation_id() const = 0;
    virtual void post_message_to_presentation(IPC::MessageBuffer&) = 0;
    virtual void did_allocate_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) = 0;
    // The stores join the ones allocated last, and leave them again once retired.
    virtual void did_add_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>&& backing_stores) = 0;
    virtual void did_retire_backing_stores(Web::CompositorContextId, Vector<i32> bitmap_ids) = 0;
    virtual void did_present_frame(Web::CompositorContextId, Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio) = 0;
};

class CompositorStateWebContentClient {
public:
    virtual ~CompositorStateWebContentClient() = default;

    virtual WebContentClientId web_content_client_id() const = 0;
    virtual void post_message_to_web_content(IPC::MessageBuffer&) = 0;
    virtual void post_message_to_render_clock(IPC::MessageBuffer&) = 0;
    virtual void rendering_opportunity(Web::CompositorContextId, i64 frame_time_nanoseconds, double frame_interval_milliseconds) = 0;
    virtual void create_video_edge(Media::VideoSinkHandle) = 0;
    virtual void release_video_edge(Media::VideoSinkHandle) = 0;
    virtual void video_sink_presented_frame(Media::VideoSinkHandle, RefPtr<Media::VideoFrame>, Media::DisplayingVideoSinkFrameCounts) { }
    virtual void video_sink_counted_frames(Media::VideoSinkHandle, Media::DisplayingVideoSinkFrameCounts) { }
    virtual void request_animated_image_frames(AnimatedImage::FrameRequest const&) { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) = 0;
    // The owner of this client's canvases, which are the only ones its contexts may draw.
    virtual Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const = 0;
};

class CompositorState final
    : public RefCounted<CompositorState>
    , public CompositorCoreHost {
public:
    static NonnullRefPtr<CompositorState> create(RefPtr<Gfx::SkiaBackendContext>, bool scrollbars_always_shown = false);
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
        // Whether the presentation found it cannot present the shared GPU images it was given.
        bool cannot_present_gpu_images { false };
    };

    // Each root context presents to the presentation it is bound to. Unbound contexts keep rendering, but publish nothing.
    PresentationId register_presentation(CompositorStateClient&, PresentationCapabilities);
    void unregister_presentation(CompositorStateClient&);
    void bind_presentation(Web::CompositorContextId, CompositorStateClient&);
    CompositorStateClient* presentation_for_context(Web::CompositorContextId) const;
    WebContentClientId register_web_content_client(CompositorStateWebContentClient&);
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, WebContentClientId);
    void destroy_contexts_for_web_content_client(CompositorStateWebContentClient&);

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context() const { return m_skia_backend_context; }
    CanvasSurfaceRegistry& canvas_surface_registry() { return m_canvas_surface_registry; }
    CanvasSurfaceRegistry const& canvas_surface_registry() const { return m_canvas_surface_registry; }

    void create_context(Web::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&);
    void destroy_context(Web::CompositorContextId);

    bool context_can_be_nested_in(Web::CompositorContextId, Web::CompositorContextId parent_context_id) const;
    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId> parent_context_id);
    void stop_presenting_to_client(Web::CompositorContextId);
    // Answers false, changing nothing, for async scrolling metadata or a scroll state that do not decode.
    bool update_display_list(Web::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, IPC::Encoded<Optional<Compositing::DisplayList::AsyncScrollingMetadata>> const&, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, ReadonlyBytes encoded_scroll_state);
    void update_display_list_resources(Web::CompositorContextId, Compositing::DisplayListResourceTransaction&&);
    // Answers false, changing nothing, for a scroll state or keyboard scroll state that do not decode.
    bool update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&, ReadonlyBytes encoded_scroll_state, IPC::Encoded<Compositing::KeyboardScrollState> const&);
    void add_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    // For a presentation channel that arrives after the sinks did, or that replaces the one their edges were on.
    void create_video_edges(CompositorStateWebContentClient&);
    void remove_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void set_video_sink_ticking(CompositorStateWebContentClient&, Media::VideoSinkHandle, bool should_tick);
    void on_video_sink_ready(CompositorStateWebContentClient&, Media::VideoSinkHandle, NonnullRefPtr<Media::DisplayingVideoSink> const&);
    void add_animated_image(CompositorStateWebContentClient&, Compositing::ImageFrameResourceId, AnimatedImage::Description);
    void remove_animated_image(CompositorStateWebContentClient&, Compositing::ImageFrameResourceId);
    void start_animated_image(CompositorStateWebContentClient&, Compositing::ImageFrameResourceId);
    void stop_animated_image(CompositorStateWebContentClient&, Compositing::ImageFrameResourceId);
    void animated_image_frames_decoded(CompositorStateWebContentClient&, i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>>);
    void animated_image_decode_failed(CompositorStateWebContentClient&, i64 session_id);
    // The client's images lost the decoder their sessions were on. Each keeps the frame it shows.
    void animated_image_decoder_lost(CompositorStateWebContentClient&);
    AnimatedImage const* animated_image(CompositorStateWebContentClient&, Compositing::ImageFrameResourceId) const;
    // The messages of WebContent and of a presentation that the scroll and input logic takes undecoded.
    IPC::RawMessage::OutcomeInRust handle_message_from_web_content(WebContentClientId, IPC::RawMessage&);
    IPC::RawMessage::OutcomeInRust handle_message_from_presentation(PresentationId, IPC::RawMessage&);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress);
    void request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second);
    void set_ui_overlays(Web::CompositorContextId, Compositing::UIOverlays);
    void set_display_metadata(Web::CompositorContextId, Optional<u64> display_id, double refresh_rate);
    void set_context_visibility(Web::CompositorContextId, Compositing::ContextVisibility);
    void set_context_presents_from_cpu_memory(Web::CompositorContextId, bool);
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
    CompositorState(RefPtr<Gfx::SkiaBackendContext>, bool scrollbars_always_shown);

    CompositorStateWebContentClient* web_content_client_if_present(WebContentClientId) const;
    virtual void post_message_to_web_content(WebContentClientId, IPC::MessageBuffer&) override;
    virtual void post_message_to_render_clock_of_web_content(WebContentClientId, IPC::MessageBuffer&) override;
    virtual void post_message_to_presentation(PresentationId, IPC::MessageBuffer&) override;
    virtual void request_display_ticks(Web::CompositorContextId) override;
    virtual void set_timer(Web::CompositorContextId, ContextTimer, AK::Duration delay) override;
    virtual void cancel_timer(Web::CompositorContextId, ContextTimer) override;
    void context_timer_fired(Web::CompositorContextId, ContextTimer);

    // What the compositor keeps of a context to present it.
    struct PresentedContext {
        AK_MAKE_NONCOPYABLE(PresentedContext);
        AK_MAKE_NONMOVABLE(PresentedContext);

    public:
        AK_ALLOC_WITH_KMALLOC;

        PresentedContext(Optional<u64> page_id, CompositorStateWebContentClient&, OwnedCanvasSurfaces);

        Optional<u64> page_id;
        CompositorStateWebContentClient& web_content_client;
        ContextPresenter presenter;
        Optional<Web::CompositorContextId> parent_context_id;
        Compositing::ContextVisibility visibility { Compositing::ContextVisibility::Visible };
        Optional<u64> display_id;
        double display_refresh_rate { 60.0 };
        // Whether the presentation shows this context's frames from CPU memory for now, as when the UI paints it itself.
        bool presents_from_cpu_memory { false };
        PacedTickRequest rendering_opportunity_request;
        PacedTickRequest clock_tick_request;
    };

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

public:
    void present_pending_frames_for_testing() { present_pending_frames_on_vsync({}, MonotonicTime::now()); }
    size_t pending_async_present_count_for_testing() const { return m_pending_async_presents.size(); }

private:
    PresentedContext* presented_context_if_present(Web::CompositorContextId);
    PresentedContext const* presented_context_if_present(Web::CompositorContextId) const;
    Optional<u64> display_id_for_context(PresentedContext const&) const;
    PresentedContext const* root_context_of(PresentedContext const&) const;
    Web::CompositorContextId root_context_id_of(Web::CompositorContextId) const;
    bool context_is_effectively_visible(PresentedContext const&) const;
    void resume_presentation_after_becoming_visible(Web::CompositorContextId root_context_id, PresentedContext& root_context);
    double display_refresh_rate_for_context(PresentedContext const&) const;
    void clear_parent_context(Web::CompositorContextId, PresentedContext&);
    CompositedContextResolver resolver_for(Web::CompositorContextId parent_context_id);
    CompositedContextSurface resolve_composited_context(Web::CompositorContextId parent_context_id, Web::CompositorContextId child_context_id, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform);
    void schedule_backing_store_shrink(Web::CompositorContextId, PresentedContext&);
    void shrink_backing_stores_after_resize(Web::CompositorContextId);
    void resize_backing_stores_if_needed(Web::CompositorContextId, PresentedContext&);
    void add_backing_store_for_pending_frame_if_needed(Web::CompositorContextId, PresentedContext&);
    void schedule_surplus_backing_store_retirement(Web::CompositorContextId, PresentedContext&);
    void retire_idle_surplus_backing_stores(Web::CompositorContextId);
    void present_current_frame(Web::CompositorContextId, PresentedContext&);
    void resolve_video_sinks(PresentedContext&);
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
    bool needs_animation_frames(Web::CompositorContextId, PresentedContext const&, MonotonicTime now);
    static bool has_pending_present_frame_scheduled_on(PresentedContext const&, Optional<u64> display_id);
    void schedule_animation_frames_if_needed(Web::CompositorContextId, PresentedContext const&);
    void present_frame(Web::CompositorContextId, PresentedContext&, PendingFrame);
    void schedule_requested_presents();
    void schedule_present_frame(Web::CompositorContextId, PresentedContext&, PendingFrame);
    void schedule_present_frame(Web::CompositorContextId, PresentedContext&, Gfx::IntRect viewport_rect);
    void schedule_pending_present_frame(Web::CompositorContextId, PresentedContext&);
    void schedule_pending_present_frame_on_vsync(Web::CompositorContextId, PresentedContext&);
    void schedule_containing_context_present(PresentedContext&);
    void schedule_pending_present_frame_if_unblocked(Web::CompositorContextId, PresentedContext&);
    bool try_present_frame_during_resize(Web::CompositorContextId, PresentedContext&);
    VSyncScheduler& vsync_scheduler_for_display(Optional<u64> display_id);
    void present_pending_frames_on_vsync(Optional<u64> display_id, MonotonicTime frame_time);
    void publish_backing_stores(Web::CompositorContextId, PresentedContext&, BackingStoreManager::Publication&&);
    BackingStoreManager::GpuSharing gpu_sharing_for_context(Web::CompositorContextId) const;
    void unbind_presentation(Web::CompositorContextId);
    void update_scroll_capabilities();
    void did_finish_async_present(PendingAsyncPresent&);
    void deliver_rendered_frame(PendingAsyncPresent&);
    void cancel_pending_async_presents_for_context(Web::CompositorContextId);
    void schedule_gpu_completion_check();
    void check_gpu_completions();

    HashMap<Web::CompositorContextId, NonnullOwnPtr<PresentedContext>> m_presented_contexts;
    u64 m_next_web_content_client_id { 1 };
    HashMap<WebContentClientId, CompositorStateWebContentClient*> m_web_content_clients;
    HashMap<Web::CompositorContextId, HashMap<ContextTimer, NonnullRefPtr<Core::Timer>>> m_context_timers;

    DoublyLinkedList<PendingAsyncPresent> m_pending_async_presents;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CanvasSurfaceRegistry m_canvas_surface_registry;
    OwnPtr<DisplayListPlayerSkia> m_display_list_player;
    HashMap<Optional<u64>, OwnPtr<VSyncScheduler>> m_vsync_schedulers_by_display;
    RefPtr<Core::Timer> m_gpu_completion_timer;

    HashMap<CompositorStateClient*, PresentationCapabilities> m_presentations;
    u64 m_next_presentation_id { 1 };
    HashMap<PresentationId, CompositorStateClient*> m_presentations_by_id;
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

    AnimatedImage* animated_image_mutable(CompositorStateWebContentClient&, Compositing::ImageFrameResourceId);
    AnimatedImage* animated_image_with_session(CompositorStateWebContentClient&, i64 session_id);
    bool animated_image_is_drawn(CompositorStateWebContentClient*, Compositing::ImageFrameResourceId);
    void show_animated_image_frame(CompositorStateWebContentClient*, Compositing::ImageFrameResourceId, Gfx::DecodedImageFrame const&);
    // A frame that names an animated image brings frame 0 with it, whichever frame the image is at.
    void show_animated_image_frames_in(PresentedContext&);
    void update_animated_images();
    void schedule_animated_image_update(AK::Duration delay);
    HashMap<CompositorStateWebContentClient*, HashMap<u64, NonnullOwnPtr<AnimatedImage>>> m_animated_images;
    RefPtr<Core::Timer> m_animated_image_update_timer;
    RefPtr<Core::Timer> m_unpainted_video_update_timer;

    struct PlaceholderCanvas {
        CompositorStateWebContentClient* owner { nullptr };
        u64 secret { 0 };
        RefPtr<Gfx::PaintingSurface> surface;
        Optional<Gfx::IntSize> size;
        bool origin_clean { true };
    };
    HashMap<Compositing::CanvasId, PlaceholderCanvas> m_placeholder_canvases;

    CompositorCore m_core;
};

}
