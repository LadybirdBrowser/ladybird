/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CanvasHost.h>
#include <Compositor/HostWebGLContext.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CanvasCommandPlayer.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/Painting/CanvasSurfaceRegistry.h>

namespace Compositor {

CanvasHost::CanvasHost(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, Web::Painting::CanvasSurfaceRegistry& canvas_surface_registry)
    : m_skia_backend_context(move(skia_backend_context))
    , m_canvas_surface_registry(canvas_surface_registry)
{
}

CanvasHost::~CanvasHost()
{
    for (auto canvas_id : m_contexts.keys())
        m_canvas_surface_registry.remove_canvas_surface(canvas_id);
}

OwnPtr<Gfx::CanvasCommandPlayer> CanvasHost::create_2d_command_player(Gfx::IntSize size, bool alpha)
{
    if (size.is_empty() || static_cast<i64>(size.width()) * static_cast<i64>(size.height()) > Gfx::max_canvas_area)
        return nullptr;

    auto format = alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    auto player = make<Gfx::CanvasCommandPlayer>(m_skia_backend_context, size, format, Gfx::AlphaType::Premultiplied, [this](u64 canvas_id) -> Gfx::PaintingSurface const* {
        return m_canvas_surface_registry.canvas_surface(Web::Painting::CanvasId { canvas_id });
    });

    // https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
    // "Thus, the bitmap of such a context starts off as opaque black instead of transparent black"
    // AD-HOC: Skia hands out a fully transparent surface by default; only clear when alpha is disabled.
    if (!alpha)
        player->clear(Gfx::Color::Black);

    return player;
}

static void copy_surface_contents(Gfx::PaintingSurface& source, Gfx::PaintingSurface& destination)
{
    destination.copy_from_surface(source);
}

static NonnullRefPtr<Gfx::PaintingSurface> create_presented_canvas_surface(Gfx::PaintingSurface& source)
{
    auto surface = Gfx::PaintingSurface::create_with_size(
        source.size(),
        Gfx::BitmapFormat::BGRA8888,
        Gfx::AlphaType::Premultiplied,
        source.skia_backend_context());
    copy_surface_contents(source, *surface);
    return surface;
}

CanvasHost::Canvas2DContext& CanvasHost::as_2d(Context& context)
{
    auto* canvas_context = context.get_pointer<Canvas2DContext>();
    VERIFY(canvas_context);
    return *canvas_context;
}

HostWebGLContext& CanvasHost::as_webgl(Context& context)
{
    auto* webgl_context = context.get_pointer<WebGLContext>();
    VERIFY(webgl_context);
    return **webgl_context;
}

Optional<Web::Painting::CanvasId> CanvasHost::create_2d_context(Gfx::IntSize size, bool alpha)
{
    auto command_player = create_2d_command_player(size, alpha);
    if (!command_player)
        return {};

    auto presented_surface = create_presented_canvas_surface(command_player->surface());
    auto canvas_id = m_canvas_surface_registry.create_canvas_surface(presented_surface);
    Canvas2DContext context {
        .command_player = command_player.release_nonnull(),
        .presented_surface = move(presented_surface),
    };
    m_contexts.set(canvas_id, move(context));
    return canvas_id;
}

CanvasHost::CreateWebGLContextResult CanvasHost::create_webgl_context(Web::WebGL::WebGLVersion version, Gfx::IntSize size, bool depth, bool stencil, bool antialias)
{
    if (!m_skia_backend_context)
        return {};

    auto context = HostWebGLContext::create(*m_skia_backend_context, version, { .depth = depth, .stencil = stencil, .antialias = antialias }, size);
    if (!context)
        return {};

    auto canvas_id = m_canvas_surface_registry.allocate_canvas_id();
    auto supported_extensions = context->gl_context().get_supported_opengl_extensions();
    m_contexts.set(canvas_id, context.release_nonnull());
    return { .success = true, .canvas_id = canvas_id, .supported_extensions = move(supported_extensions) };
}

void CanvasHost::destroy_context(Web::Painting::CanvasId canvas_id)
{
    m_contexts.remove(canvas_id);
    m_canvas_surface_registry.remove_canvas_surface(canvas_id);
}

bool CanvasHost::has_context(Web::Painting::CanvasId canvas_id) const
{
    return m_contexts.contains(canvas_id);
}

CanvasHost::Context* CanvasHost::context(Web::Painting::CanvasId canvas_id)
{
    auto it = m_contexts.find(canvas_id);
    if (it == m_contexts.end())
        return nullptr;
    return &it->value;
}

void CanvasHost::present_canvas_2d_context(Web::Painting::CanvasId canvas_id, Canvas2DContext& context)
{
    copy_surface_contents(context.command_player->surface(), context.presented_surface);
    m_canvas_surface_registry.set_canvas_surface(canvas_id, context.presented_surface);
    context.has_uncommitted_commands = false;
}

void CanvasHost::execute_canvas_2d_commands(Web::Painting::CanvasId canvas_id, Gfx::CanvasCommandList const& commands, bool commit)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);

    auto& canvas_context = as_2d(*context);
    if (!commands.is_empty()) {
        canvas_context.command_player->play(commands);
        canvas_context.has_uncommitted_commands = true;
    }

    if (commit && canvas_context.has_uncommitted_commands)
        present_canvas_2d_context(canvas_id, canvas_context);
}

void CanvasHost::execute_webgl_commands(Web::Painting::CanvasId canvas_id, ByteBuffer const& commands, Vector<Gfx::DecodedImageFrame> const& bitmaps)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    auto& webgl_context = as_webgl(*context);
    MUST(webgl_context.execute_commands(commands, bitmaps));
    if (auto surface = webgl_context.surface())
        m_canvas_surface_registry.set_canvas_surface(canvas_id, surface.release_nonnull());
}

ErrorOr<ByteBuffer> CanvasHost::execute_webgl_sync_call(Web::Painting::CanvasId canvas_id, ByteBuffer request)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    return as_webgl(*context).execute_sync_call(request);
}

Web::WebGL::ReadPixelsResult CanvasHost::webgl_read_pixels_robust_angle(Web::Painting::CanvasId canvas_id, Web::WebGL::GLint x, Web::WebGL::GLint y, Web::WebGL::GLsizei width, Web::WebGL::GLsizei height, Web::WebGL::GLenum format, Web::WebGL::GLenum type, Web::WebGL::GLsizei buf_size, Core::AnonymousBuffer pixels)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    return as_webgl(*context).read_pixels_robust_angle(x, y, width, height, format, type, buf_size, move(pixels));
}

void CanvasHost::webgl_read_buffer_sub_data(Web::Painting::CanvasId canvas_id, Web::WebGL::GLenum target, Web::WebGL::GLintptr offset, Web::WebGL::GLintptr size, Core::AnonymousBuffer data)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    as_webgl(*context).read_buffer_sub_data(target, offset, size, move(data));
}

void CanvasHost::present_webgl_canvas(Web::Painting::CanvasId canvas_id, bool preserve_drawing_buffer)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);

    auto surface = MUST(as_webgl(*context).prepare_for_compositing(preserve_drawing_buffer));
    m_canvas_surface_registry.set_canvas_surface(canvas_id, move(surface));
}

static Gfx::ShareableBitmap read_back_surface(Gfx::PaintingSurface& surface, Gfx::IntRect rect)
{
    auto clipped_rect = rect.intersected(surface.rect());
    if (clipped_rect.is_empty())
        return {};

    auto bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, clipped_rect.size());
    if (bitmap_or_error.is_error())
        return {};

    auto bitmap = bitmap_or_error.release_value();
    surface.flush();
    surface.read_into_bitmap(*bitmap, clipped_rect.location());
    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

Gfx::ShareableBitmap CanvasHost::read_back_pixels(Web::Painting::CanvasId canvas_id, Gfx::IntRect rect)
{
    auto* context = this->context(canvas_id);
    if (!context)
        return {};

    return context->visit(
        [rect](Canvas2DContext& canvas_context) {
            return read_back_surface(canvas_context.command_player->surface(), rect);
        },
        [rect](WebGLContext& webgl_context) {
            return webgl_context->read_back_drawing_buffer(rect);
        });
}

}
