/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/Forward.h>

class SkTextBlob;
template<typename T>
class sk_sp;

namespace Gfx {

class CanvasCommandPlayer {
    AK_MAKE_NONCOPYABLE(CanvasCommandPlayer);
    AK_MAKE_NONMOVABLE(CanvasCommandPlayer);

public:
    using TextBlobResolver = Function<sk_sp<SkTextBlob>(u64, ReadonlySpan<CanvasGlyph>)>;
    using CanvasSurfaceResolver = Function<PaintingSurface const*(u64)>;

    CanvasCommandPlayer(RefPtr<SkiaBackendContext>, IntSize, BitmapFormat, AlphaType, CanvasSurfaceResolver = {}, TextBlobResolver = {});
    ~CanvasCommandPlayer();

    NonnullRefPtr<PaintingSurface> surface() const;

    void clear(Color);

    void play(CanvasCommandList const&);

private:
    void play_command(CanvasCommands::DrawGlyphRun const&);
    void play_command(CanvasCommands::ClearRect const&);
    void play_command(CanvasCommands::FillRect const&);
    void play_command(CanvasCommands::DrawBitmap const&);
    void play_command(CanvasCommands::DrawCanvas const&);
    void play_command(CanvasCommands::FillPath const&);
    void play_command(CanvasCommands::StrokePath const&);
    void play_command(CanvasCommands::SetTransform const&);
    void play_command(CanvasCommands::Save const&);
    void play_command(CanvasCommands::Restore const&);
    void play_command(CanvasCommands::ClipPath const&);
    void play_command(CanvasCommands::Reset const&);

    NonnullRefPtr<PaintStyle> resolve_paint_style(CanvasPaintStyle const&) const;

    NonnullRefPtr<PaintingSurface> m_surface;
    NonnullOwnPtr<PainterSkia> m_painter;
    CanvasSurfaceResolver m_canvas_surface_resolver;
    TextBlobResolver m_text_blob_resolver;
};

}
