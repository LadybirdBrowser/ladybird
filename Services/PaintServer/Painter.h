/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Resource/BitmapResource.h>
#include <LibGfx/Resource/Resource.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibGfx/Size.h>
#include <LibIPC/File.h>
#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Types.h>
#include <PaintServer/Compositor/DrawCommandPlayer.h>
#include <PaintServer/RenderClient/Types.h>
#include <core/SkImage.h>
#include <core/SkRefCnt.h>

class SkCanvas;

namespace Gfx {

class SkiaBackendContext;

}

namespace PaintServer {

class Painter {
public:
    enum class PaintingMode : u8 {
        GPU,
        Software,
    };

    // FIXME: Why does this exist? If you need to know the kind of something when logging, pass a string
    enum class MissingImageKind : u8 {
        Scaled,
        ExternalContent,
        Repeated,
    };

    enum class OperationResult : u8 {
        Completed,
        Failed,
    };

    struct RenderContext {
        DrawContext draw_context;
        Gfx::IntSize target_size;
        FrameOutputType output_type { FrameOutputType::Presentation };
        Optional<u64> presentation_buffer_id;
        Gfx::SharedImage* offscreen_content_image { nullptr };
    };

    struct SubmitResult {
        OperationResult operation_result { OperationResult::Failed };
        ImageID offscreen_image_id { 0 };
        Optional<Gfx::SharedImagePayload> offscreen_content_image;
        Optional<Gfx::ShareableBitmap> offscreen_bitmap;
    };

    struct SurfaceState {
        Gfx::IntSize logical_size;
        Gfx::IntSize buffer_size;
    };

    static NonnullOwnPtr<Painter> create(PaintingMode);

    virtual ~Painter();
    virtual void shutdown();

    virtual SubmitResult submit_commands(RenderContext const& render_context, FrameHeader const& header, ReadonlyBytes payload, ReleaseToken release_token);
    virtual bool create_surface(SurfaceID surface_id, Gfx::IntSize logical_size, Gfx::IntSize buffer_size);
    virtual void destroy_surface(SurfaceID surface_id);
    bool has_surface(SurfaceID surface_id) const;
    void clear_surface_cached_state(SurfaceID surface_id);

    ErrorOr<void> register_presentation_buffer(SurfaceID surface_id, u64 image_id, Gfx::SharedImagePayload image_payload);
    virtual void unregister_all_presentation_buffers_for_surface(SurfaceID surface_id);
    void unregister_presentation_buffer_by_id(u64 image_id);
    virtual bool set_surface_presentation_buffer(SurfaceID surface_id, u64 image_id);

    ErrorOr<Gfx::SharedImage> create_content_image(u64 image_id, Gfx::IntSize size, Gfx::BitmapFormat format);
    ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> create_content_image_painting_surface(Gfx::SharedImage&, OffscreenBackendPreference);
    ErrorOr<void> paint_draw_list_to_canvas(DrawContext const&, Gfx::PaintingSurface&, DrawListView);

    PaintingMode painting_mode() const { return m_painting_mode; }
    bool is_cpu_painting() const { return m_painting_mode == PaintingMode::Software; }
    void log_missing_image(ConnectionID connection_id, u64 image_id, MissingImageKind kind, Gfx::FloatRect const& dst_rect, Gfx::FloatRect const& clip_rect) const;

protected:
    explicit Painter(PaintingMode painting_mode)
        : m_painting_mode(painting_mode)
    {
    }

    ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> skia_context();
    ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> create_skia_context();
    virtual ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> create_gpu_backed_skia_context() = 0;
    ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> acquire_paint_surface(SurfaceID surface_id, Optional<u64> presentation_buffer_id);
    bool has_registered_presentation_buffer(u64 image_id) const;
    void unregister_presentation_buffer(u64 image_id);
    virtual char const* backend_name() const = 0;
    virtual void did_complete_submit(SurfaceID, FrameHeader const&, ReleaseToken) { }
    virtual ErrorOr<NonnullRefPtr<Gfx::Bitmap>> import_cpu_backed_presentation_buffer(Gfx::SharedImagePayload) = 0;
    virtual ErrorOr<Gfx::SharedImage> import_gpu_backed_presentation_buffer(Gfx::SharedImagePayload) = 0;
    virtual ErrorOr<Gfx::SharedImage> create_gpu_backed_content_image(u64, Gfx::IntSize, Gfx::BitmapFormat) = 0;
    ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> acquire_gpu_backed_paint_surface(SurfaceID, u64);
    sk_sp<SkImage> snapshot_shared_image(Gfx::SharedImage&);

    void note_registered_presentation_buffer(SurfaceID surface_id, u64 image_id);
    Optional<SurfaceState> surface_state(SurfaceID surface_id) const;
    Optional<u64> active_presentation_buffer_id(SurfaceID surface_id) const;
    void remove_paint_surface(SurfaceID surface_id);

    ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>> get_or_create_paint_surface(SurfaceID surface_id, Optional<u64> cache_key, Function<ErrorOr<NonnullRefPtr<Gfx::PaintingSurface>>()> create);
    // FIXME: Are these really needed INSIDE of Painter? Why jump through such hoops to source them
    // to DrawCommandPlayer instead of having them as free functions in the DrawCommandPlayer translation unit
    // or an adjacent DrawImageCommandPlayer one?
    ErrorOr<void> draw_scaled_image(DrawContext const& draw_context, DrawScaledImageCommand const& command, SkCanvas& canvas, sk_sp<SkImageFilter> const& image_filter);
    ErrorOr<void> draw_external_content(DrawContext const& draw_context, DrawExternalContentCommand const& command, SkCanvas& canvas);
    ErrorOr<void> draw_repeated_image(DrawContext const& draw_context, DrawRepeatedImageCommand const& command, SkCanvas& canvas);

private:
    friend class ResourceManager;

    struct PaintSurfaceCacheEntry {
        Optional<u64> key;
        RefPtr<Gfx::PaintingSurface> surface;
    };

    // FIXME: why is this here?
    static void draw_missing_image_placeholder(Gfx::FloatRect const& dst_rect, Gfx::FloatRect const& clip_rect, SkCanvas& canvas);

    HashMap<SurfaceID, SurfaceState> m_surfaces;
    HashMap<u64, SurfaceID> m_presentation_buffer_surface_ids;
    HashMap<SurfaceID, u64> m_surface_active_presentation_buffer;
    HashMap<u64, NonnullRefPtr<Gfx::Bitmap>> m_cpu_backed_presentation_buffers;
    HashMap<u64, Gfx::SharedImage> m_gpu_backed_presentation_buffers;
    HashMap<SurfaceID, PaintSurfaceCacheEntry> m_paint_surfaces;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    PaintingMode m_painting_mode { PaintingMode::GPU };
};

}
