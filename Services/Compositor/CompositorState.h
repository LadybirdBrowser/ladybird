/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <Compositor/AnimatedImage.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/CompositorCore.h>
#include <Compositor/ContextRasterTargets.h>
#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/RemoteLayer.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/VSyncScheduler.h>
#include <Compositor/WindowServer.h>
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
    virtual void did_hold_synchronized_resize_frame(u64 remote_layer_id, u64 token) = 0;
    virtual void did_commit_synchronized_resize(u64 remote_layer_id, u64 token) = 0;
};

class CompositorStateWebContentClient {
public:
    virtual ~CompositorStateWebContentClient() = default;

    virtual WebContentClientId web_content_client_id() const = 0;
    virtual void post_message_to_web_content(IPC::MessageBuffer&) = 0;
    virtual void post_message_to_render_clock(IPC::MessageBuffer&) = 0;
    virtual void create_video_edge(Media::VideoSinkHandle) = 0;
    virtual void release_video_edge(Media::VideoSinkHandle) = 0;
    virtual void video_sink_presented_frame(Media::VideoSinkHandle, RefPtr<Media::VideoFrame>, Media::DisplayingVideoSinkFrameCounts) { }
    virtual void video_sink_counted_frames(Media::VideoSinkHandle, Media::DisplayingVideoSinkFrameCounts) { }
    virtual void request_animated_image_frames(AnimatedImage::FrameRequest const&) { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) = 0;
    // The owner of this client's canvases, which are the only ones its contexts may draw.
    virtual Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const = 0;
    virtual void ask_to_paint_contexts_for_screenshot(u64 preparation_id, Vector<Web::CompositorContextId> const&) = 0;
    virtual void tell_remote_descendants_prepared_for_screenshot(u64 request_id) = 0;
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
    void update_display_list_resources(Web::CompositorContextId, Compositing::DisplayListResourceTransaction&&);
    // The fonts, image frames and video sinks that the display list of the next rasterization under the token draws, and
    // the bitmap it is rasterized into. Answers false for a token in use, for more rasterizations than a client can have
    // in flight, or for resources that a rasterization does not stage.
    bool stage_rasterization(CompositorStateWebContentClient&, u64 token, Compositing::DisplayListResourceTransaction&&, Gfx::ShareableBitmap target_bitmap);
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
    void set_window_server_may_show_high_dynamic_range(bool);
    void present_frame(Web::CompositorContextId, Gfx::IntRect viewport_rect);
    // Delivers the rendering opportunity a context requested now rather than at the next display tick: a
    // viewport change that arrived while an animation's opportunity was outstanding starts its update at once.
    void hurry_rendering_opportunity(Web::CompositorContextId);
    // Asks for the next display tick the context's maximum rate lets through, for its render clock.
    void request_clock_tick(Web::CompositorContextId, double maximum_frames_per_second);
    bool request_screenshot(Web::CompositorContextId, Gfx::ShareableBitmap&);
    // Has each process that owns a descendant of the context in another process paint it, along with whatever lies below
    // it, and tells the requester once every one of them has handed over its frames, or gone away.
    void prepare_remote_descendants_for_screenshot(CompositorStateWebContentClient& requester, Web::CompositorContextId, u64 request_id);
    void did_paint_contexts_for_screenshot(CompositorStateWebContentClient& painter, u64 preparation_id);
    Optional<Gfx::ShareableBitmap> take_frame_snapshot(Web::CompositorContextId, Optional<Gfx::IntSize> maximum_size);
    void presented_bitmap_ready_to_paint(Web::CompositorContextId, i32 bitmap_id);
    void set_client_gpu_presentation_capability(CompositorStateClient&, bool supported, u64 adapter_luid);
    void set_scrollbar_track_press_jumps(CompositorStateClient&, bool jumps);

    // A remote layer shows the frames of one root context at a time, in a view of the presentation it was created
    // for. Answers how that view finds the layer, whose kind is None where the presentation has no remote layers.
    Compositing::RemoteLayerHostHandle create_remote_layer(CompositorStateClient&, u64 remote_layer_id);
    void destroy_remote_layer(u64 remote_layer_id);
    // The layer keeps the frame it shows until the context it is given next presents one.
    void show_context_in_remote_layer(u64 remote_layer_id, Optional<Web::CompositorContextId>);
    // A view that scales the frames to fit it gives its size, in device pixels.
    void set_remote_layer_fit(u64 remote_layer_id, Optional<Gfx::IntSize> fit_within);

    // A window resize is shown together with the page's frame of the new size: the remote layers hold their changes
    // off screen from when the resize begins until the window commits it, and the presentation hears once the layer
    // holds a frame that the page laid out at the size it awaits.
    void begin_synchronized_resize(u64 remote_layer_id, u64 token);
    void await_synchronized_resize_frame(u64 remote_layer_id, u64 token, Gfx::IntSize);
    void commit_synchronized_resize(u64 remote_layer_id, u64 token);

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
    virtual void set_timer(Web::CompositorContextId, ContextTimer, AK::Duration delay) override;
    virtual void cancel_timer(Web::CompositorContextId, ContextTimer) override;
    virtual Optional<OverlayLabel> shape_overlay_label(Web::CompositorContextId, Optional<StringView> font_family, double device_pixel_ratio, StringView text) override;
    void context_timer_fired(Web::CompositorContextId, ContextTimer);

    // What the compositor keeps of a context to paint and present its frames.
    struct PresentedContext {
        AK_MAKE_NONCOPYABLE(PresentedContext);
        AK_MAKE_NONMOVABLE(PresentedContext);

    public:
        AK_ALLOC_WITH_KMALLOC;

        PresentedContext(CompositorStateWebContentClient&, OwnedCanvasSurfaces);

        CompositorStateWebContentClient& web_content_client;
        ContextRasterTargets raster_targets;
        Optional<u64> remote_layer_id;
    };

    PresentedContext* presented_context_if_present(Web::CompositorContextId);
    PresentedContext const* presented_context_if_present(Web::CompositorContextId) const;
    PresentedContext& presented_context(Web::CompositorContextId);
    CompositorStateClient* presentation_if_present(PresentationId) const;

    // The entry points of the raster host, which the logic in Rust paints and presents through. None of them calls back
    // into that logic.
    static RustFFI::FfiRasterHost raster_host(CompositorState&);
    static RustFFI::GpuBackend gpu_backend(RefPtr<Gfx::SkiaBackendContext> const&);
    Compositing::RustFFI::FfiRasterPainter begin_painting(Web::CompositorContextId, Gfx::PaintingSurface&, RustFFI::PaintingResources);
    void end_painting();
    RustFFI::StoreResizeOutcome resize_stores(Web::CompositorContextId, RustFFI::FfiStoreResize const&);
    bool add_store_if_window_server_reads_every_released_store(Web::CompositorContextId, Optional<PresentationId>);
    bool retire_idle_surplus_stores(Web::CompositorContextId, Optional<PresentationId>);
    void apply_resource_changes(Web::CompositorContextId, RustFFI::FfiResourceChanges const&);
    struct StagedRasterization;
    StagedRasterization* staged_rasterization(WebContentClientId, u64 token);
    bool staged_rasterization_has_resources(WebContentClientId, u64 token, ReadonlySpan<u64> font_ids, ReadonlySpan<u64> image_frame_ids);
    bool begin_staged_rasterization(WebContentClientId, u64 token, Compositing::RustFFI::FfiRasterPainter&);
    void finish_staged_rasterization(WebContentClientId, u64 token);
    void report_live_content(Web::CompositorContextId, ReadonlySpan<u64> canvas_ids, void* receiver, void (*receive)(void*, RustFFI::FfiLiveContent const*));
    Optional<RemoteLayer::Content> remote_layer_content(PresentedContext&, RustFFI::MediaKind, u64 media_id);
    bool remote_layer_can_show(Web::CompositorContextId, RustFFI::MediaKind, u64 media_id, RustFFI::FfiSublayerPlacement const&, double device_pixel_ratio);
    Vector<RemoteSublayer>* build_sublayers(Web::CompositorContextId, ReadonlySpan<RustFFI::FfiSublayer>);
    WindowServer::FrameHandOff frame_hand_off(Web::CompositorContextId);
    u64 submit_frame(Gfx::PaintingSurface*);
    void hand_off_once_gpu_work_is_scheduled(u64 token);
    Optional<PresentationId> show_frame_in_remote_layer(Web::CompositorContextId, RustFFI::FfiRemoteLayerFrame const&, NonnullOwnPtr<Vector<RemoteSublayer>>);
    void stop_showing_in_remote_layer(Web::CompositorContextId);
    void release_store_later(Web::CompositorContextId, i32 bitmap_id);
    void gpu_work_finished(u64 token);
    void schedule_gpu_completion_check();
    void check_gpu_completions();

    VSyncScheduler& vsync_scheduler_for_display(Optional<u64> display_id);
    void set_maximum_display_refresh_rate(double);
    void present_contexts_drawing_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    enum class VideoSinkUpdateResult : u8 {
        NoUnpaintedSinkRequiresUpdates,
        UnpaintedSinkRequiresUpdates,
    };
    VideoSinkUpdateResult update_all_video_sinks();
    bool context_video_sinks_require_updates(Web::CompositorContextId);
    void resolve_video_sinks(PresentedContext&);
    void update_unpainted_video_sinks();
    void schedule_unpainted_video_updates();
    int unpainted_video_update_interval_ms() const;

    HashMap<Web::CompositorContextId, NonnullOwnPtr<PresentedContext>> m_presented_contexts;

    struct StagedRasterization {
        AK_MAKE_NONCOPYABLE(StagedRasterization);
        AK_MAKE_NONMOVABLE(StagedRasterization);

    public:
        AK_ALLOC_WITH_KMALLOC;

        StagedRasterization() = default;

        Compositing::DisplayListResourceStorage resource_storage;
        DisplayListRasterCache raster_cache;
        Gfx::ShareableBitmap target_bitmap;
        RefPtr<Gfx::PaintingSurface> target_surface;
        // Rasters on the CPU, so the pixels match a raster of the same list in any other process.
        OwnPtr<DisplayListPlayerSkia> player;
    };
    HashMap<WebContentClientId, HashMap<u64, NonnullOwnPtr<StagedRasterization>>> m_staged_rasterizations;

    // A screenshot whose context has descendants in other processes, which waits for those processes to paint them.
    struct ScreenshotPreparation {
        WebContentClientId requester;
        u64 request_id { 0 };
        HashTable<WebContentClientId> awaited_painters;
        RefPtr<Core::Timer> deadline;
    };
    void finish_screenshot_preparation_if_done(u64 preparation_id);
    void finish_screenshot_preparation(u64 preparation_id);
    HashMap<u64, ScreenshotPreparation> m_screenshot_preparations;
    u64 m_next_screenshot_preparation_id { 1 };
    u64 m_next_web_content_client_id { 1 };
    HashMap<WebContentClientId, CompositorStateWebContentClient*> m_web_content_clients;
    HashMap<Web::CompositorContextId, HashMap<ContextTimer, NonnullRefPtr<Core::Timer>>> m_context_timers;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CanvasSurfaceRegistry m_canvas_surface_registry;
    // A context nested in another is painted while the frame of the other is, by a player of its own.
    Vector<NonnullOwnPtr<DisplayListPlayerSkia>> m_display_list_players;
    size_t m_painting_depth { 0 };
    HashMap<Optional<u64>, OwnPtr<VSyncScheduler>> m_vsync_schedulers_by_display;
    double m_maximum_display_refresh_rate { 60.0 };
    HashTable<u64> m_gpu_work_in_flight;
    u64 m_next_gpu_work_token { 1 };
    RefPtr<Core::Timer> m_gpu_completion_timer;
    // The contexts that draw a video sink with a new frame, while the video sinks are updated for a display tick.
    Vector<u64>* m_contexts_with_new_video_frames { nullptr };

    u64 m_next_presentation_id { 1 };
    HashMap<PresentationId, CompositorStateClient*> m_presentations_by_id;

    HashMap<CompositorStateClient*, NonnullRefPtr<WindowServer>> m_window_servers;

    struct RemoteLayerState {
        CompositorStateClient* presentation { nullptr };
        NonnullRefPtr<WindowServer> window_server;
        OwnPtr<RemoteLayer> layer;
        Optional<Web::CompositorContextId> shown_context_id;
        Optional<Gfx::IntSize> fit_within;
        // The size the page laid out the frame on display at, if it is the size of the frame.
        Optional<Gfx::IntSize> size_of_shown_frame_laid_out_by_page;
    };

    struct SynchronizedResize {
        u64 remote_layer_id { 0 };
        u64 token { 0 };
        Optional<Gfx::IntSize> awaited_size;
        bool frame_is_held { false };
    };
    RemoteLayerState* remote_layer_showing(Web::CompositorContextId);
    void report_held_synchronized_resize_frames(u64 remote_layer_id);
    void release_synchronized_resize_hold();
    Vector<SynchronizedResize> m_synchronized_resizes;
    Vector<NonnullRefPtr<WindowServer>> m_window_servers_holding_changes;
    RefPtr<Core::Timer> m_synchronized_resize_hold_timeout;
    HashMap<u64, RemoteLayerState> m_remote_layers;

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
        PresentableSurfacePool presented_surfaces;
        RefPtr<Gfx::PaintingSurface> surface;
        Optional<Gfx::IntSize> size;
        bool origin_clean { true };
    };
    HashMap<Compositing::CanvasId, PlaceholderCanvas> m_placeholder_canvases;

    CompositorCore m_core;
};

}
