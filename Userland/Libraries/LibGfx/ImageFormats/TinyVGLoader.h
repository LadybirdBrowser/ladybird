/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <LibGfx/VectorGraphic.h>

namespace Gfx {

// Current recommended SVG to TVG conversion (without installing tools)
// (FIXME: Implement our own converter!)
// 1. (Optional) Convert strokes to fills
//  * Only round joins/linecaps exist in TVG, so for other stroke kinds converting
//    them to fills (that still beziers etc, so are scalable) works better.
//  * This site can do that: https://iconly.io/tools/svg-convert-stroke-to-fill
// 2. Scale your SVG's width/height to large size (e.g. 1024x?)
//  * Current converters deal poorly with small values in paths.
//  * This site can do that: https://www.iloveimg.com/resize-image/resize-svg
//    (or just edit the viewbox if it has one).
// 3. Convert the SVG to a TVG
//  * This site can do that: https://svg-to-tvg-server.fly.dev/

// Decoder from the "Tiny Vector Graphics" format (v1.0).
// https://tinyvg.tech/download/specification.pdf

struct TinyVGHeader;

class TinyVGDecodedImageData final : public VectorGraphic {
public:
    using Style = Variant<Color, NonnullRefPtr<SVGGradientPaintStyle>>;

    struct DrawCommand {
        Path path;
        Optional<Style> fill {};
        Optional<Style> stroke {};
        float stroke_width { 0.0f };
    };

    virtual IntSize intrinsic_size() const override
    {
        return m_size;
    }

    virtual void draw_transformed(Painter&, AffineTransform) const override;

    ReadonlySpan<DrawCommand> draw_commands() const
    {
        return m_draw_commands;
    }

    static ErrorOr<NonnullRefPtr<TinyVGDecodedImageData>> decode(Stream& stream);
    static ErrorOr<NonnullRefPtr<TinyVGDecodedImageData>> decode(Stream& stream, TinyVGHeader const& header);

private:
    TinyVGDecodedImageData(IntSize size, Vector<DrawCommand> draw_commands)
        : m_size(size)
        , m_draw_commands(move(draw_commands))
    {
    }

    IntSize m_size;
    Vector<DrawCommand> m_draw_commands;
};

struct TinyVGLoadingContext;

class TinyVGImageDecoderPlugin final : public ImageDecoderPlugin {
public:
    static bool sniff(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);

    virtual IntSize size() override;
    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;

    virtual NaturalFrameFormat natural_frame_format() const override { return NaturalFrameFormat::Vector; }
    virtual ErrorOr<VectorImageFrameDescriptor> vector_frame(size_t index) override;

    virtual ~TinyVGImageDecoderPlugin() override;

private:
    TinyVGImageDecoderPlugin(ReadonlyBytes);

    NonnullOwnPtr<TinyVGLoadingContext> m_context;
};

}
