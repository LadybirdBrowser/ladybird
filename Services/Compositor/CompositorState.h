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
#include <LibCompositing/DisplayList/DisplayListResources.h>
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
    virtual void video_sink_frame_freshness_changed(Media::VideoSinkHandle, bool) { }
    virtual void tell_video_sink_is_up_to_date(u64 token) = 0;
    virtual void tell_screenshot_result(Compositing::ScreenshotRequestId, bool painted) = 0;
    virtual void request_animated_image_frames(AnimatedImage::FrameRequest const&) { }
    virtual void placeholder_canvas_committed(Compositing::CanvasId, Gfx::IntSize, bool origin_clean) = 0;
    // The owner of this client's canvases, which are the only ones its contexts may draw.
    virtual Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const = 0;
    virtual Compositing::RegisteredImageFrames const& registered_image_frames() const = 0;
    virtual CanvasHost& canvas_host() = 0;
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
    CompositorStateClient* presentation_for_context(Web::CompositorContextId) const;
    WebContentClientId register_web_content_client(CompositorStateWebContentClient&);
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, WebContentClientId);
    void destroy_contexts_for_web_content_client(CompositorStateWebContentClient&);

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context() const { return m_skia_backend_context; }
    CanvasSurfaceRegistry& canvas_surface_registry() { return m_canvas_surface_registry; }
    CanvasSurfaceRegistry const& canvas_surface_registry() const { return m_canvas_surface_registry; }

    void create_context(Web::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&);
    void destroy_context(Web::CompositorContextId);

    [[nodiscard]] bool describe_display_list_fonts(Web::CompositorContextId, ReadonlySpan<Compositing::DisplayListFontDescription>, Compositing::RegisteredTypefaces const&);
    // The fonts and image frames that the display list of the next rasterization under the token draws, and the bitmap
    // it is rasterized into. Answers false for a token in use, for more rasterizations than a client can have
    // in flight, or for resources that a rasterization does not stage.
    bool stage_rasterization(CompositorStateWebContentClient&, u64 token, ReadonlySpan<Compositing::DisplayListFontDescription>, ReadonlySpan<Compositing::ImageFrameResourceId>, Compositing::RegisteredTypefaces const&, Gfx::ShareableBitmap target_bitmap);
    void add_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    // For a presentation channel that arrives after the sinks did, or that replaces the one their edges were on.
    void create_video_edges(CompositorStateWebContentClient&);
    void remove_video_sink(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void bring_video_sink_up_to_date(CompositorStateWebContentClient&, Media::VideoSinkHandle, u64 token);
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
    IPC::RawMessage::OutcomeInRust handle_message_from_control(IPC::RawMessage&);
    IPC::RawMessage::OutcomeInRust handle_message_from_render_clock(WebContentClientId, IPC::RawMessage&);
    void set_context_visibility(Web::CompositorContextId, Compositing::ContextVisibility);
    // Paints once the video sinks that the screenshot draws have frames following their clocks, and tells the client.
    void request_screenshot(CompositorStateWebContentClient&, Web::CompositorContextId, Compositing::ScreenshotRequestId, Gfx::ShareableBitmap);
    // Has each process that owns a descendant of the context in another process paint it, along with whatever lies below
    // it, and tells the requester once every one of them has handed over its frames, or gone away.
    void prepare_remote_descendants_for_screenshot(CompositorStateWebContentClient& requester, Web::CompositorContextId, u64 request_id);
    void did_paint_contexts_for_screenshot(CompositorStateWebContentClient& painter, u64 preparation_id);
    Optional<Gfx::ShareableBitmap> take_frame_snapshot(Web::CompositorContextId);
    void presented_bitmap_ready_to_paint(Web::CompositorContextId, i32 bitmap_id);

    // A remote layer shows the frames of one root context at a time, in a view of the presentation it was created
    // for. Answers how that view finds the layer, whose kind is None where the presentation has no remote layers.
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
    virtual void set_timer(u64 timer_number, AK::Duration delay) override;
    virtual void cancel_timer(u64 timer_number) override;
    virtual void destroy_timer(u64 timer_number) override;
    virtual Optional<OverlayLabel> shape_overlay_label(Web::CompositorContextId, Optional<StringView> font_family, float font_size_in_points, double device_pixel_ratio, StringView text) override;

    // What the compositor keeps of a context to paint and present its frames.
    struct PresentedContext {
        AK_MAKE_NONCOPYABLE(PresentedContext);
        AK_MAKE_NONMOVABLE(PresentedContext);

    public:
        AK_ALLOC_WITH_KMALLOC;

        PresentedContext(CompositorStateWebContentClient&, OwnedCanvasSurfaces);

        CompositorStateWebContentClient& web_content_client;
        ContextRasterTargets raster_targets;
    };

    PresentedContext* presented_context_if_present(Web::CompositorContextId);
    PresentedContext const* presented_context_if_present(Web::CompositorContextId) const;
    PresentedContext& presented_context(Web::CompositorContextId);
    CompositorStateClient* presentation_if_present(PresentationId) const;

    // The entry points of the raster host, which the logic in Rust paints and presents through. None of them calls back
    // into that logic.
    static RustFFI::FfiRasterHost raster_host(CompositorState&);
    static RustFFI::GpuBackend gpu_backend(RefPtr<Gfx::SkiaBackendContext> const&);
    Compositing::RustFFI::FfiRasterPainter begin_painting(Web::CompositorContextId, Gfx::PaintingSurface&, RustFFI::PaintingResources, bool allows_lcd_text);
    void end_painting();
    bool create_store(Web::CompositorContextId, RustFFI::StoreKind, Gfx::IntSize, WindowServer* window_server_showing_stores, RustFFI::FfiNewStore&);
    void publish_stores(PresentationId, Web::CompositorContextId, bool added, ReadonlySpan<i32> bitmap_ids, ReadonlySpan<void*> images);
    void retire_published_stores(PresentationId, Web::CompositorContextId, ReadonlySpan<i32> bitmap_ids);
    bool take_registered_image_frames(Web::CompositorContextId, ReadonlySpan<u64> image_frame_ids);
    void set_drawn_image_frames(Web::CompositorContextId, ReadonlySpan<u64> image_frame_ids);
    void set_drawn_video_sinks(Web::CompositorContextId, ReadonlySpan<u64> handles);
    void set_drawn_vector_images(Web::CompositorContextId, ReadonlySpan<u64> vector_image_ids);
    void set_drawn_fonts(Web::CompositorContextId, ReadonlySpan<u64> font_ids);
    void set_drawn_clip_paths(Web::CompositorContextId, ReadonlySpan<u64> path_identities);
    void set_drawn_layer_filters(Web::CompositorContextId, ReadonlySpan<u64> layer_filter_cache_keys);
    bool context_holds_fonts(Web::CompositorContextId, ReadonlySpan<u64> font_ids);
    struct StagedRasterization;
    StagedRasterization* staged_rasterization(WebContentClientId, u64 token);
    bool staged_rasterization_has_resources(WebContentClientId, u64 token, ReadonlySpan<u64> font_ids, ReadonlySpan<u64> image_frame_ids);
    bool begin_staged_rasterization(WebContentClientId, u64 token, Compositing::RustFFI::FfiRasterPainter&);
    void finish_staged_rasterization(WebContentClientId, u64 token);
    CanvasHost& canvas_host_of(WebContentClientId);
    void begin_canvas_2d_painting(WebContentClientId, Compositing::CanvasId, RustFFI::FfiCanvas2DPainter&);
    bool end_canvas_2d_painting_reporting_whether_video_frames_were_sampled_on_gpu();
    void report_live_content(Web::CompositorContextId, ReadonlySpan<u64> canvas_ids, void* receiver, void (*receive)(void*, RustFFI::FfiLiveContent const*));
    Optional<RemoteLayer::Content> remote_layer_content(PresentedContext&, RustFFI::MediaKind, u64 media_id);
    bool remote_layer_can_show(RemoteLayer&, Web::CompositorContextId, RustFFI::MediaKind, u64 media_id, RustFFI::FfiSublayerPlacement const&, double device_pixel_ratio);
    Vector<RemoteSublayer>* build_sublayers(Web::CompositorContextId, ReadonlySpan<RustFFI::FfiSublayer>);
    u64 submit_frame(Gfx::PaintingSurface*);
    void hand_off_once_gpu_work_is_scheduled(u64 token);
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

        DrawnFonts fonts;
        DrawnImageFrames image_frames;
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
    HashMap<u64, NonnullRefPtr<Core::Timer>> m_timers;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CanvasSurfaceRegistry m_canvas_surface_registry;
    CanvasHost* m_canvas_host_painting_2d_context { nullptr };
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

    struct VideoSinkState {
        RefPtr<Media::DisplayingVideoSink> sink;
        bool requires_updates { false };
        bool was_ticking { false };
        bool published_frame_is_up_to_date { false };
        // A client waits for one request at a time, so the request it waits for is the last one it made.
        Optional<u64> up_to_date_request_token;
    };
    VideoSinkState* video_sink_state(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void video_sink_freshness_changed(CompositorStateWebContentClient&, Media::VideoSinkHandle);
    void publish_video_sink_freshness(CompositorStateWebContentClient&, Media::VideoSinkHandle, VideoSinkState&);

    struct ScreenshotSink {
        WebContentClientId client;
        Media::VideoSinkHandle handle;
        bool was_asked_to_catch_up { false };
        bool is_drawn { false };
    };
    // A screenshot that waits for the video sinks it draws to catch up to their clocks, ticking or not.
    struct PendingScreenshot {
        WebContentClientId requester;
        Web::CompositorContextId context_id;
        Compositing::ScreenshotRequestId request_id;
        Gfx::ShareableBitmap target_bitmap;
        Vector<ScreenshotSink> sinks;
        RefPtr<Core::Timer> deadline;
    };
    void catch_up_newly_drawn_screenshot_sinks(PendingScreenshot&);
    VideoSinkState* screenshot_sink_state(ScreenshotSink const&);
    bool screenshot_sinks_have_caught_up(PendingScreenshot const&);
    void paint_screenshots_whose_sinks_have_caught_up();
    void paint_pending_screenshot(u64 screenshot_id);
    HashMap<u64, PendingScreenshot> m_pending_screenshots;
    bool m_screenshot_check_is_scheduled { false };
    u64 m_next_pending_screenshot_id { 1 };
    // The frame that a video sink of the client shows now, which is null for a sink that the client does not have or
    // that shows none.
    RefPtr<Media::VideoFrame> current_video_frame(CompositorStateWebContentClient&, Media::VideoSinkHandle);
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
