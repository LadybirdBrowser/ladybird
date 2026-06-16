/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/WindingRule.h>
#include <LibIPC/Forward.h>

namespace Gfx {

struct CanvasLinearGradient {
    FloatPoint start_point;
    FloatPoint end_point;
    Vector<ColorStop> color_stops;
    Optional<float> repeat_length;
};

struct CanvasRadialGradient {
    FloatPoint start_center;
    float start_radius { 0 };
    FloatPoint end_center;
    float end_radius { 0 };
    Vector<ColorStop> color_stops;
    Optional<float> repeat_length;
};

struct CanvasConicGradient {
    FloatPoint center;
    float start_angle { 0 };
    Vector<ColorStop> color_stops;
    Optional<float> repeat_length;
};

struct CanvasPatternStyle {
    Optional<DecodedImageFrame> image;
    CanvasPatternPaintStyle::Repetition repetition { CanvasPatternPaintStyle::Repetition::Repeat };
    Optional<AffineTransform> transform;
};

using CanvasPaintStyle = Variant<Color, CanvasLinearGradient, CanvasRadialGradient, CanvasConicGradient, CanvasPatternStyle>;

inline constexpr i64 max_canvas_area = 16384 * 16384;

namespace CanvasCommands {

struct ClearRect {
    FloatRect rect;
    Color color;
};

struct FillRect {
    FloatRect rect;
    Color color;
};

struct DrawBitmap {
    DecodedImageFrame frame;
    FloatRect dst_rect;
    IntRect src_rect;
    ScalingMode scaling_mode { ScalingMode::NearestNeighbor };
    Optional<Filter> filter;
    float global_alpha { 1 };
    CompositingAndBlendingOperator compositing_and_blending_operator { CompositingAndBlendingOperator::SourceOver };
};

struct DrawCanvas {
    u64 source_canvas_id { 0 };
    FloatRect dst_rect;
    IntRect src_rect;
    ScalingMode scaling_mode { ScalingMode::NearestNeighbor };
    Optional<Filter> filter;
    float global_alpha { 1 };
    CompositingAndBlendingOperator compositing_and_blending_operator { CompositingAndBlendingOperator::SourceOver };
};

struct FillPath {
    Path path;
    CanvasPaintStyle style;
    WindingRule winding_rule { WindingRule::Nonzero };
    float blur_radius { 0 };
    Optional<Filter> filter;
    float global_alpha { 1 };
    CompositingAndBlendingOperator compositing_and_blending_operator { CompositingAndBlendingOperator::SourceOver };
};

struct StrokePath {
    Path path;
    CanvasPaintStyle style;
    float thickness { 1 };
    Path::CapStyle cap_style { Path::CapStyle::Butt };
    Path::JoinStyle join_style { Path::JoinStyle::Miter };
    float miter_limit { 10 };
    Vector<float> dash_array;
    float dash_offset { 0 };
    float blur_radius { 0 };
    Optional<Filter> filter;
    float global_alpha { 1 };
    CompositingAndBlendingOperator compositing_and_blending_operator { CompositingAndBlendingOperator::SourceOver };
};

struct SetTransform {
    AffineTransform transform;
};

struct Save { };

struct Restore { };

struct ClipPath {
    Path path;
    WindingRule winding_rule { WindingRule::Nonzero };
};

struct Reset { };

}

using CanvasCommand = Variant<
    CanvasCommands::ClearRect,
    CanvasCommands::FillRect,
    CanvasCommands::DrawBitmap,
    CanvasCommands::DrawCanvas,
    CanvasCommands::FillPath,
    CanvasCommands::StrokePath,
    CanvasCommands::SetTransform,
    CanvasCommands::Save,
    CanvasCommands::Restore,
    CanvasCommands::ClipPath,
    CanvasCommands::Reset>;

class CanvasCommandList {
public:
    CanvasCommandList() = default;
    explicit CanvasCommandList(Vector<CanvasCommand> commands)
        : m_commands(move(commands))
    {
    }

    void append(CanvasCommand&& command) { m_commands.append(move(command)); }

    bool is_empty() const { return m_commands.is_empty(); }
    size_t size() const { return m_commands.size(); }

    Vector<CanvasCommand> const& commands() const { return m_commands; }

private:
    Vector<CanvasCommand> m_commands;
};

CanvasPaintStyle to_canvas_paint_style(PaintStyle const&);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasLinearGradient const&);
template<>
ErrorOr<Gfx::CanvasLinearGradient> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasRadialGradient const&);
template<>
ErrorOr<Gfx::CanvasRadialGradient> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasConicGradient const&);
template<>
ErrorOr<Gfx::CanvasConicGradient> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasPatternStyle const&);
template<>
ErrorOr<Gfx::CanvasPatternStyle> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::ClearRect const&);
template<>
ErrorOr<Gfx::CanvasCommands::ClearRect> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::FillRect const&);
template<>
ErrorOr<Gfx::CanvasCommands::FillRect> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::DrawBitmap const&);
template<>
ErrorOr<Gfx::CanvasCommands::DrawBitmap> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::DrawCanvas const&);
template<>
ErrorOr<Gfx::CanvasCommands::DrawCanvas> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::FillPath const&);
template<>
ErrorOr<Gfx::CanvasCommands::FillPath> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::StrokePath const&);
template<>
ErrorOr<Gfx::CanvasCommands::StrokePath> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::SetTransform const&);
template<>
ErrorOr<Gfx::CanvasCommands::SetTransform> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::Save const&);
template<>
ErrorOr<Gfx::CanvasCommands::Save> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::Restore const&);
template<>
ErrorOr<Gfx::CanvasCommands::Restore> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::ClipPath const&);
template<>
ErrorOr<Gfx::CanvasCommands::ClipPath> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::Reset const&);
template<>
ErrorOr<Gfx::CanvasCommands::Reset> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommandList const&);
template<>
ErrorOr<Gfx::CanvasCommandList> decode(Decoder&);

}
