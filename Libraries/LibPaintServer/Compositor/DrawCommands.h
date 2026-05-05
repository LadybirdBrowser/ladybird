/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/TypedTransfer.h>
#include <AK/Types.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Color.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibPaintServer/Types.h>

#define ASSERT_WIRE_SCALAR_LAYOUT(type, scalar_type, count)     \
    static_assert(__is_standard_layout(type));                  \
    static_assert(sizeof(type) == sizeof(scalar_type) * count); \
    static_assert(alignof(type) == alignof(scalar_type));

ASSERT_WIRE_SCALAR_LAYOUT(Gfx::AffineTransform, f32, 6);
ASSERT_WIRE_SCALAR_LAYOUT(Gfx::FloatMatrix4x4, f32, 16);
ASSERT_WIRE_SCALAR_LAYOUT(Gfx::Color, u32, 1);
ASSERT_WIRE_SCALAR_LAYOUT(Gfx::FloatPoint, f32, 2);
ASSERT_WIRE_SCALAR_LAYOUT(Gfx::FloatRect, f32, 4);
ASSERT_WIRE_SCALAR_LAYOUT(Gfx::IntSize, int, 2);
ASSERT_WIRE_SCALAR_LAYOUT(Gfx::FloatSize, float, 2);

namespace PaintServer {

#define ENUMERATE_DRAW_LIST_COMMAND_TYPES(X) \
    X(ClearRect)                             \
    X(FillRect)                              \
    X(FillRectWithRoundedCorners)            \
    X(DrawScaledImage)                       \
    X(DrawExternalContent)                   \
    X(DrawRect)                              \
    X(FillEllipse)                           \
    X(DrawGlyphRun)                          \
    X(Save)                                  \
    X(SaveLayer)                             \
    X(Restore)                               \
    X(Translate)                             \
    X(AddClipRect)                           \
    X(AddClipPath)                           \
    X(ApplyEffects)                          \
    X(ApplyBackdropFilter)                   \
    X(SetTransform)                          \
    X(ApplyTransform)                        \
    X(PaintLinearGradient)                   \
    X(PaintOuterBoxShadow)                   \
    X(PaintInnerBoxShadow)                   \
    X(PaintTextShadow)                       \
    X(FillPath)                              \
    X(StrokePath)                            \
    X(DrawEllipse)                           \
    X(DrawLine)                              \
    X(AddRoundedRectClip)                    \
    X(PaintRadialGradient)                   \
    X(PaintConicGradient)                    \
    X(PaintScrollBar)                        \
    X(DrawRepeatedImage)                     \
    X(ResetCanvasState)

// NOLINTNEXTLINE(performance-enum-size)
enum class CommandType : u32 {
    Invalid = 0,
#define __ENUMERATE_DRAW_LIST_COMMAND_TYPE(name) name,
    ENUMERATE_DRAW_LIST_COMMAND_TYPES(__ENUMERATE_DRAW_LIST_COMMAND_TYPE)
#undef __ENUMERATE_DRAW_LIST_COMMAND_TYPE
        Count,
};

bool is_draw_command(CommandType command_type);
bool is_state_command(CommandType command_type);
StringView command_type_name(CommandType command_type);

enum class WireLineStyle : u8 {
    Solid = 0,
    Dotted,
    Dashed,
};

enum class PaintStyleType : u8 {
    SolidColor = 0,
    SVGLinearGradient,
    SVGRadialGradient,
    SVGPattern,
    CanvasLinearGradient,
    CanvasRadialGradient,
    CanvasConicGradient,
    CanvasPattern,
};

enum class SVGSpreadMethod : u8 {
    Pad = 0,
    Repeat,
    Reflect,
};

enum class GradientColorSpace : u8 {
    OKLab = 1,
    sRGB,
    sRGBLinear,
    Lab,
    DisplayP3,
    A98RGB,
    ProPhotoRGB,
    Rec2020,
    HSL,
    HWB,
    LCH,
    OKLCH,
};

enum class GradientHueMethod : u8 {
    Shorter = 1,
    Longer,
    Increasing,
    Decreasing,
};

enum class WireCornerClip : u8 {
    Outside = 0,
    Inside,
};

struct DrawCommandView {
    CommandType type;
    ReadonlyBytes bytes;
};

struct ClearRectCommand {
    CommandType command_type { CommandType::ClearRect };
    u32 command_size { sizeof(ClearRectCommand) };
    Gfx::Color color;
    Gfx::FloatRect rect;
};

struct FillRectCommand {
    CommandType command_type { CommandType::FillRect };
    u32 command_size { sizeof(FillRectCommand) };
    Gfx::Color color;
    Gfx::FloatRect rect;
};

struct CornerRadiusCommand {
    f32 horizontal_radius { 0.0f };
    f32 vertical_radius { 0.0f };
};

struct FillRectWithRoundedCornersCommand {
    CommandType command_type { CommandType::FillRectWithRoundedCorners };
    u32 command_size { sizeof(FillRectWithRoundedCornersCommand) };
    Gfx::Color color;
    Gfx::FloatRect rect;
    CornerRadiusCommand top_left;
    CornerRadiusCommand top_right;
    CornerRadiusCommand bottom_right;
    CornerRadiusCommand bottom_left;
};

struct DrawScaledImageCommand {
    CommandType command_type { CommandType::DrawScaledImage };
    u32 command_size { sizeof(DrawScaledImageCommand) };
    ResourceID image_resource_id { 0 };
    u64 image_id { 0 };
    u32 scaling_mode { 0 };
    f32 opacity { 1.0f };
    u8 compositing_and_blending_operator { 0 };
    u8 padding[3] {};
    u32 filter_byte_count { 0 };
    Gfx::FloatRect src_rect;
    Gfx::FloatRect dst_rect;
    Gfx::FloatRect clip_rect;
    // Followed by filter_byte_count bytes of serialized SkImageFilter data.
};

struct DrawExternalContentCommand {
    CommandType command_type { CommandType::DrawExternalContent };
    u32 command_size { sizeof(DrawExternalContentCommand) };
    ResourceID image_resource_id { 0 };
    u64 image_id { 0 };
    u32 scaling_mode { 0 };
    u32 padding { 0 };
    Gfx::FloatRect dst_rect;
    Gfx::FloatRect clip_rect;
};

struct DrawRectCommand {
    CommandType command_type { CommandType::DrawRect };
    u32 command_size { sizeof(DrawRectCommand) };
    Gfx::Color color;
    Gfx::FloatRect rect;
    f32 thickness { 1.0f };
};

struct DrawLineCommand {
    CommandType command_type { CommandType::DrawLine };
    u32 command_size { sizeof(DrawLineCommand) };
    Gfx::Color color;
    Gfx::FloatPoint from;
    Gfx::FloatPoint to;
    f32 thickness { 1.0f };
    u8 style { to_underlying(WireLineStyle::Solid) };
    u8 padding[3] {};
    Gfx::Color alternate_color;
};

struct FillEllipseCommand {
    CommandType command_type { CommandType::FillEllipse };
    u32 command_size { sizeof(FillEllipseCommand) };
    Gfx::Color color;
    Gfx::FloatRect rect;
};

struct DrawEllipseCommand {
    CommandType command_type { CommandType::DrawEllipse };
    u32 command_size { sizeof(DrawEllipseCommand) };
    Gfx::Color color;
    Gfx::FloatRect rect;
    f32 thickness { 1.0f };
};

struct SaveCommand {
    CommandType command_type { CommandType::Save };
    u32 command_size { sizeof(SaveCommand) };
};

struct SaveLayerCommand {
    CommandType command_type { CommandType::SaveLayer };
    u32 command_size { sizeof(SaveLayerCommand) };
};

struct RestoreCommand {
    CommandType command_type { CommandType::Restore };
    u32 command_size { sizeof(RestoreCommand) };
};

struct ResetCanvasStateCommand {
    CommandType command_type { CommandType::ResetCanvasState };
    u32 command_size { sizeof(ResetCanvasStateCommand) };
};

struct TranslateCommand {
    CommandType command_type { CommandType::Translate };
    u32 command_size { sizeof(TranslateCommand) };
    Gfx::FloatPoint delta;
};

struct AddClipRectCommand {
    CommandType command_type { CommandType::AddClipRect };
    u32 command_size { sizeof(AddClipRectCommand) };
    Gfx::FloatRect rect;
};

struct SerializedFilterImageReference {
    ResourceID image_resource_id { 0 };
    ImageID image_id { 0 };
};

struct ApplyTransformCommand {
    CommandType command_type { CommandType::ApplyTransform };
    u32 command_size { sizeof(ApplyTransformCommand) };
    Gfx::FloatPoint origin;
    Gfx::FloatMatrix4x4 matrix { Gfx::FloatMatrix4x4::identity() };
};

struct SetTransformCommand {
    CommandType command_type { CommandType::SetTransform };
    u32 command_size { sizeof(SetTransformCommand) };
    Gfx::AffineTransform transform;
};

struct Glyph {
    u32 glyph_id { 0 };
    f32 x { 0.0f };
    f32 y { 0.0f };
};

struct WireColorStop {
    Gfx::Color color;
    f32 position { 0.0f };
};

struct WireCornerRadii {
    CornerRadiusCommand top_left;
    CornerRadiusCommand top_right;
    CornerRadiusCommand bottom_right;
    CornerRadiusCommand bottom_left;
};

struct AddRoundedRectClipCommand {
    CommandType command_type { CommandType::AddRoundedRectClip };
    u32 command_size { sizeof(AddRoundedRectClipCommand) };
    WireCornerRadii corner_radii;
    Gfx::FloatRect border_rect;
    u8 corner_clip { to_underlying(WireCornerClip::Outside) };
    u8 padding[3] {};
};

struct PaintOuterBoxShadowCommand {
    CommandType command_type { CommandType::PaintOuterBoxShadow };
    u32 command_size { sizeof(PaintOuterBoxShadowCommand) };
    Gfx::Color color;
    WireCornerRadii content_corner_radii;
    WireCornerRadii shadow_corner_radii;
    f32 blur_radius { 0.0f };
    Gfx::FloatRect device_content_rect;
    Gfx::FloatRect shadow_rect;
};

struct PaintInnerBoxShadowCommand {
    CommandType command_type { CommandType::PaintInnerBoxShadow };
    u32 command_size { sizeof(PaintInnerBoxShadowCommand) };
    Gfx::Color color;
    WireCornerRadii content_corner_radii;
    WireCornerRadii inner_shadow_corner_radii;
    f32 blur_radius { 0.0f };
    Gfx::FloatRect device_content_rect;
    Gfx::FloatRect outer_shadow_rect;
    Gfx::FloatRect inner_shadow_rect;
};

struct PaintScrollBarCommand {
    CommandType command_type { CommandType::PaintScrollBar };
    u32 command_size { sizeof(PaintScrollBarCommand) };
    u32 scroll_frame_id { 0 };
    f32 scroll_size { 0.0f };
    Gfx::FloatRect scrollbar_rect;
    Gfx::FloatRect gutter_rect;
    Gfx::FloatRect thumb_rect;
    Gfx::Color thumb_color;
    Gfx::Color track_color;
    u8 vertical { 0 };
    u8 padding[3] {};
};

struct DrawRepeatedImageCommand {
    CommandType command_type { CommandType::DrawRepeatedImage };
    u32 command_size { sizeof(DrawRepeatedImageCommand) };
    ResourceID image_resource_id { 0 };
    u64 image_id { 0 };
    u32 scaling_mode { 0 };
    u8 repeat_x { 0 };
    u8 repeat_y { 0 };
    u8 padding[2] {};
    Gfx::FloatRect dst_rect;
    Gfx::FloatRect clip_rect;
};

// Variable-length: fixed header followed by path_byte_count bytes of serialized SkPath data.
struct AddClipPathCommand {
    CommandType command_type { CommandType::AddClipPath };
    u32 command_size { 0 };
    Gfx::FloatRect bounding_rect;
    u8 fill_rule { 0 };
    u8 padding[3] {};
    u32 path_byte_count { 0 };
    // Followed by path_byte_count bytes of serialized SkPath data.
};

// Variable-length: fixed header followed by filter_byte_count bytes of serialized SkImageFilter data.
struct ApplyEffectsCommand {
    CommandType command_type { CommandType::ApplyEffects };
    u32 command_size { sizeof(ApplyEffectsCommand) };
    f32 opacity { 1.0f };
    u32 compositing_and_blending_operator { 0 };
    u8 has_mask_kind { 0 };
    u8 mask_kind { 0 };
    u8 padding[2] {};
    u32 filter_byte_count { 0 };
    // Followed by filter_byte_count bytes of serialized SkImageFilter data.
};

// Variable-length: fixed header followed by stop_count entries of WireColorStop.
struct PaintLinearGradientCommand {
    CommandType command_type { CommandType::PaintLinearGradient };
    u32 command_size { 0 };
    Gfx::FloatRect gradient_rect;
    f32 gradient_angle { 0.0f };
    f32 repeat_length { 1.0f };
    u8 has_repeat_length { 0 };
    u8 color_space { to_underlying(GradientColorSpace::OKLab) };
    u8 hue_method { to_underlying(GradientHueMethod::Shorter) };
    u8 padding { 0 };
    u32 stop_count { 0 };
    // Followed by stop_count entries of WireColorStop.
};

// Variable-length: fixed header followed by filter_byte_count bytes of serialized SkImageFilter data.
struct ApplyBackdropFilterCommand {
    CommandType command_type { CommandType::ApplyBackdropFilter };
    u32 command_size { sizeof(ApplyBackdropFilterCommand) };
    Gfx::FloatRect backdrop_region;
    WireCornerRadii corner_radii;
    u32 filter_byte_count { 0 };
    // Followed by filter_byte_count bytes of serialized SkImageFilter data.
    // FIXME: should be out of band?
};

// Variable-length: fixed header followed by glyph_count entries of (u32 glyph_id, f32 x, f32 y).
struct DrawGlyphRunCommand {
    CommandType command_type { CommandType::DrawGlyphRun };
    u32 command_size { 0 };
    ResourceID font_resource_id { 0 };
    f32 font_pixel_size { 0.0f };
    f32 font_ascent { 0.0f };
    f32 device_pixels_per_css_pixel { 1.0f };
    Gfx::Color color;
    Gfx::FloatPoint translation;
    Gfx::FloatRect text_rect;
    Gfx::FloatRect visual_bounds;
    u8 orientation { 0 };
    u8 padding[3] {};
    u32 glyph_count { 0 };
    // Followed by glyph_count * sizeof(WireGlyph) bytes.
};

// Variable-length: fixed header followed by glyph_count entries of WireGlyph.
struct PaintTextShadowCommand {
    CommandType command_type { CommandType::PaintTextShadow };
    u32 command_size { 0 };
    ResourceID font_resource_id { 0 };
    f32 font_pixel_size { 0.0f };
    f32 font_ascent { 0.0f };
    f32 device_pixels_per_css_pixel { 1.0f };
    Gfx::Color color;
    Gfx::FloatPoint translation;
    Gfx::FloatRect text_rect;
    Gfx::FloatRect visual_bounds;
    f32 blur_radius { 0.0f };
    u8 orientation { 0 };
    u8 padding_[3] {};
    u32 glyph_count { 0 };
    // Followed by glyph_count * sizeof(WireGlyph) bytes.
};

// Variable-length: fixed header followed by path_byte_count bytes of serialized SkPath,
// then paint_style_byte_count bytes of encoded style payload.
struct FillPathCommand {
    CommandType command_type { CommandType::FillPath };
    u32 command_size { 0 };
    Gfx::Color color;
    Gfx::FloatRect path_bounding_rect;
    f32 opacity { 1.0f };
    f32 blur_radius { 0.0f };
    u8 paint_style_type { to_underlying(PaintStyleType::SolidColor) };
    u8 winding_rule { 0 };
    u8 should_anti_alias { 1 };
    u8 compositing_and_blending_operator { 0 };
    u32 path_byte_count { 0 };
    u32 filter_byte_count { 0 };
    u32 paint_style_byte_count { 0 };
    // Followed by path_byte_count bytes of serialized SkPath,
    // then filter_byte_count bytes of serialized SkImageFilter data,
    // then paint_style_byte_count bytes of encoded style payload.
};

// Variable-length: fixed header followed by dash_count floats, then path_byte_count bytes
// of serialized SkPath, then paint_style_byte_count bytes of encoded style payload.
struct StrokePathCommand {
    CommandType command_type { CommandType::StrokePath };
    u32 command_size { 0 };
    Gfx::Color color;
    Gfx::FloatRect path_bounding_rect;
    f32 opacity { 1.0f };
    f32 thickness { 1.0f };
    f32 miter_limit { 4.0f };
    f32 dash_offset { 0.0f };
    f32 blur_radius { 0.0f };
    u32 dash_count { 0 };
    u8 paint_style_type { to_underlying(PaintStyleType::SolidColor) };
    u8 cap_style { 0 };
    u8 join_style { 0 };
    u8 should_anti_alias { 1 };
    u8 compositing_and_blending_operator { 0 };
    u32 path_byte_count { 0 };
    u32 filter_byte_count { 0 };
    u32 paint_style_byte_count { 0 };
    // Followed by dash_count floats, then path_byte_count bytes of serialized SkPath,
    // then filter_byte_count bytes of serialized SkImageFilter data,
    // then paint_style_byte_count bytes of encoded style payload.
};

// Variable-length: fixed header followed by stop_count WirePaintColorStop entries.
struct SVGLinearGradientPayload {
    Gfx::FloatPoint start_point;
    Gfx::FloatPoint end_point;
    Gfx::AffineTransform gradient_transform;
    u8 spread_method { to_underlying(SVGSpreadMethod::Pad) };
    u8 color_space { 0 };
    u8 has_gradient_transform { 0 };
    u8 padding { 0 };
    u32 stop_count { 0 };
    // Followed by stop_count WirePaintColorStop entries.
};

// Variable-length: fixed header followed by stop_count WirePaintColorStop entries.
struct SVGRadialGradientPayload {
    Gfx::FloatPoint start_center;
    f32 start_radius { 0.0f };
    Gfx::FloatPoint end_center;
    f32 end_radius { 0.0f };
    Gfx::AffineTransform gradient_transform;
    u8 spread_method { to_underlying(SVGSpreadMethod::Pad) };
    u8 color_space { 0 };
    u8 has_gradient_transform { 0 };
    u8 padding { 0 };
    u32 stop_count { 0 };
    // Followed by stop_count WirePaintColorStop entries.
};

// Variable-length: fixed header followed by draw_list_byte_count bytes of encoded draw-list commands.
struct SVGPatternPayload {
    Gfx::FloatRect tile_rect;
    Gfx::AffineTransform pattern_transform;
    u8 has_pattern_transform { 0 };
    u8 padding[3] {};
    u32 draw_list_byte_count { 0 };
    // Followed by draw_list_byte_count bytes of encoded draw-list commands.
};

// Variable-length: fixed header followed by stop_count WireColorStop entries.
struct CanvasLinearGradientPayload {
    Gfx::FloatPoint start_point;
    Gfx::FloatPoint end_point;
    u32 stop_count { 0 };
    // Followed by stop_count WireColorStop entries.
};

// Variable-length: fixed header followed by stop_count WireColorStop entries.
struct CanvasRadialGradientPayload {
    Gfx::FloatPoint start_center;
    f32 start_radius { 0.0f };
    Gfx::FloatPoint end_center;
    f32 end_radius { 0.0f };
    u32 stop_count { 0 };
    // Followed by stop_count WireColorStop entries.
};

// Variable-length: fixed header followed by stop_count WireColorStop entries.
struct CanvasConicGradientPayload {
    Gfx::FloatPoint center;
    f32 start_angle { 0.0f };
    u32 stop_count { 0 };
    // Followed by stop_count WireColorStop entries.
};

struct CanvasPatternPayload {
    ResourceID image_resource_id { 0 };
    ImageID image_id { 0 };
    Gfx::AffineTransform transform;
    u8 repeat_x { 0 };
    u8 repeat_y { 0 };
    u8 has_transform { 0 };
    u8 padding { 0 };
};

// Variable-length: fixed header followed by stop_count entries of WireColorStop.
struct PaintRadialGradientCommand {
    CommandType command_type { CommandType::PaintRadialGradient };
    u32 command_size { 0 };
    Gfx::FloatRect rect;
    Gfx::FloatPoint center;
    Gfx::FloatSize size;
    u8 repeating { 0 };
    u8 color_space { to_underlying(GradientColorSpace::OKLab) };
    u8 hue_method { to_underlying(GradientHueMethod::Shorter) };
    u8 padding { 0 };
    u32 stop_count { 0 };
    // Followed by stop_count WirePaintColorStop entries.
};

// Variable-length: fixed header followed by stop_count entries of WireColorStop.
struct PaintConicGradientCommand {
    CommandType command_type { CommandType::PaintConicGradient };
    u32 command_size { 0 };
    Gfx::FloatRect rect;
    f32 start_angle { 0.0f };
    Gfx::FloatPoint position;
    u8 color_space { to_underlying(GradientColorSpace::OKLab) };
    u8 hue_method { to_underlying(GradientHueMethod::Shorter) };
    u8 padding[2] {};
    u32 stop_count { 0 };
    // Followed by stop_count WirePaintColorStop entries.
};

ASSERT_WIRE_SCALAR_LAYOUT(CornerRadiusCommand, f32, 2);
ASSERT_WIRE_SCALAR_LAYOUT(Glyph, f32, 3);
ASSERT_WIRE_SCALAR_LAYOUT(WireColorStop, f32, 2);
ASSERT_WIRE_SCALAR_LAYOUT(WireCornerRadii, f32, 8);

#define ASSERT_DRAW_COMMAND_LAYOUT(name)                                 \
    static_assert(__is_standard_layout(name##Command));                  \
    static_assert(__builtin_offsetof(name##Command, command_type) == 0); \
    static_assert(__builtin_offsetof(name##Command, command_size) == sizeof(CommandType));

ENUMERATE_DRAW_LIST_COMMAND_TYPES(ASSERT_DRAW_COMMAND_LAYOUT)

#undef ASSERT_DRAW_COMMAND_LAYOUT
#undef ASSERT_WIRE_SCALAR_LAYOUT
#undef ENUMERATE_DRAW_LIST_COMMAND_TYPES

template<typename T>
ErrorOr<void> append_struct(Bytes destination, size_t& offset, T const& value)
{
    static_assert(__is_standard_layout(T));

    if (offset > destination.size() || destination.size() - offset < sizeof(T))
        return Error::from_string_literal("Wire destination is too small");

    AK::TypedTransfer<u8>::copy(destination.data() + offset, reinterpret_cast<u8 const*>(&value), sizeof(T));
    offset += sizeof(T);
    return {};
}

template<typename T>
ErrorOr<T> read_command_struct(ReadonlyBytes bytes, size_t offset = 0)
{
    static_assert(__is_standard_layout(T));

    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        return Error::from_string_literal("Draw list payload is truncated");

    T value {};
    AK::TypedTransfer<u8>::copy(reinterpret_cast<u8*>(&value), bytes.data() + offset, sizeof(T));
    return value;
}

template<typename Command>
ErrorOr<Command> decode_fixed_size_command(ReadonlyBytes command_bytes, CommandType command_type)
{
    if (command_bytes.size() != sizeof(Command))
        return Error::from_string_literal("Draw-list payload has invalid fixed command size");

    Command command = TRY(read_command_struct<Command>(command_bytes));
    if (command.command_type != command_type)
        return Error::from_string_literal("Draw-list payload has unsupported command type");
    if (command.command_size != sizeof(Command))
        return Error::from_string_literal("Draw-list payload has invalid fixed command header size");
    return command;
}

template<typename Command>
ErrorOr<Command> decode_fixed_size_command(DrawCommandView const& command_view)
{
    return decode_fixed_size_command<Command>(command_view.bytes, command_view.type);
}

template<typename Command>
ErrorOr<Command> decode_variable_size_command(ReadonlyBytes command_bytes, CommandType command_type)
{
    if (command_bytes.size() < sizeof(Command))
        return Error::from_string_literal("Draw-list payload is truncated");

    Command command = TRY(read_command_struct<Command>(command_bytes));
    if (command.command_type != command_type)
        return Error::from_string_literal("Draw-list payload has unsupported command type");
    if (command.command_size != command_bytes.size())
        return Error::from_string_literal("Draw-list payload has invalid variable command size");
    return command;
}

template<typename Command>
ErrorOr<Command> decode_variable_size_command(DrawCommandView const& command_view)
{
    return decode_variable_size_command<Command>(command_view.bytes, command_view.type);
}

template<typename Command>
ErrorOr<ReadonlyBytes> command_trailing_bytes(ReadonlyBytes command_bytes, size_t byte_count)
{
    if (command_bytes.size() < sizeof(Command) || command_bytes.size() - sizeof(Command) != byte_count)
        return Error::from_string_literal("Draw-list payload tail size mismatch");
    return command_bytes.slice(sizeof(Command), byte_count);
}

template<typename Command>
ErrorOr<ReadonlyBytes> command_trailing_bytes(DrawCommandView const& command_view, size_t byte_count)
{
    return command_trailing_bytes<Command>(command_view.bytes, byte_count);
}

template<typename Command, typename Item>
ErrorOr<ReadonlyBytes> command_trailing_items(ReadonlyBytes command_bytes, u32 item_count)
{
    if (command_bytes.size() < sizeof(Command))
        return Error::from_string_literal("Draw-list payload is truncated");

    size_t available_bytes = command_bytes.size() - sizeof(Command);
    if (static_cast<size_t>(item_count) > available_bytes / sizeof(Item))
        return Error::from_string_literal("Draw-list payload item count is truncated");

    size_t byte_count = static_cast<size_t>(item_count) * sizeof(Item);
    if (available_bytes != byte_count)
        return Error::from_string_literal("Draw-list payload item count does not match trailing bytes");

    return command_bytes.slice(sizeof(Command), byte_count);
}

template<typename Command, typename Item>
ErrorOr<ReadonlyBytes> command_trailing_items(DrawCommandView const& command_view, u32 item_count)
{
    return command_trailing_items<Command, Item>(command_view.bytes, item_count);
}

template<typename Command, typename Item>
ErrorOr<ReadonlySpan<Item>> command_trailing_span(ReadonlyBytes command_bytes, u32 item_count)
{
    static_assert(__is_standard_layout(Item));

    ReadonlyBytes bytes = TRY(command_trailing_items<Command, Item>(command_bytes, item_count));
    return ReadonlySpan<Item> { reinterpret_cast<Item const*>(bytes.data()), static_cast<size_t>(item_count) };
}

template<typename Command, typename Item>
ErrorOr<ReadonlySpan<Item>> command_trailing_span(DrawCommandView const& command_view, u32 item_count)
{
    return command_trailing_span<Command, Item>(command_view.bytes, item_count);
}

template<typename Command>
ErrorOr<ReadonlyBytes> command_trailing_bytes_slice(ReadonlyBytes command_bytes, size_t offset, size_t byte_count)
{
    if (command_bytes.size() < sizeof(Command))
        return Error::from_string_literal("Draw-list payload is truncated");

    size_t available_bytes = command_bytes.size() - sizeof(Command);
    if (offset > available_bytes || available_bytes - offset < byte_count)
        return Error::from_string_literal("Draw-list payload tail slice is truncated");

    return command_bytes.slice(sizeof(Command) + offset, byte_count);
}

template<typename Command>
ErrorOr<ReadonlyBytes> command_trailing_bytes_slice(DrawCommandView const& command_view, size_t offset, size_t byte_count)
{
    return command_trailing_bytes_slice<Command>(command_view.bytes, offset, byte_count);
}

template<typename Command, typename Item>
ErrorOr<ReadonlySpan<Item>> command_trailing_span_slice(ReadonlyBytes command_bytes, size_t offset, u32 item_count)
{
    static_assert(__is_standard_layout(Item));

    size_t byte_count = static_cast<size_t>(item_count) * sizeof(Item);
    ReadonlyBytes bytes = TRY(command_trailing_bytes_slice<Command>(command_bytes, offset, byte_count));
    return ReadonlySpan<Item> { reinterpret_cast<Item const*>(bytes.data()), static_cast<size_t>(item_count) };
}

template<typename Command, typename Item>
ErrorOr<ReadonlySpan<Item>> command_trailing_span_slice(DrawCommandView const& command_view, size_t offset, u32 item_count)
{
    return command_trailing_span_slice<Command, Item>(command_view.bytes, offset, item_count);
}

}
