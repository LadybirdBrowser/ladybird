/*
 * Copyright (c) 2023-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/NumericLimits.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/FilterImpl.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/TextLayout.h>
#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Compositor/DrawList.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <core/SkData.h>
#include <core/SkImage.h>
#include <core/SkImageFilter.h>
#include <core/SkPath.h>
#include <core/SkSerialProcs.h>

namespace Web::Painting {

using namespace PaintServer;

static void append_unique_external_content_source(Vector<NonnullRefPtr<ExternalContentSource>>& sources, NonnullRefPtr<ExternalContentSource> source)
{
    for (auto const& existing_source : sources) {
        if (existing_source.ptr() == source.ptr())
            return;
    }
    sources.append(move(source));
}

AccumulatedVisualContextNode const* context_node(VisualContextRef const& context)
{
    if (!context)
        return nullptr;
    return &context.tree->node_at(context.index);
}

ErrorOr<Vector<VisualContextRef>> collect_context_nodes_root_to_leaf(VisualContextRef context)
{
    Vector<VisualContextRef> nodes;
    if (!context)
        return nodes;
    for (VisualContextIndex index = context.index; index.value() != 0; index = context.tree->node_at(index).parent_index)
        TRY(nodes.try_insert(0, VisualContextRef { context.tree, index }));
    return nodes;
}

size_t shared_context_prefix_length(ReadonlySpan<VisualContextRef const> left, ReadonlySpan<VisualContextRef const> right)
{
    size_t prefix_length = 0;
    size_t const max_prefix_length = min(left.size(), right.size());
    while (prefix_length < max_prefix_length && left[prefix_length] == right[prefix_length])
        ++prefix_length;
    return prefix_length;
}

AccumulatedVisualContextTree const* first_context_tree(ReadonlySpan<VisualContextRef const> context_nodes)
{
    for (auto const& context : context_nodes) {
        if (context)
            return context.tree;
    }
    return nullptr;
}

namespace {

static ReadonlySpan<VisualContextRef const> trailing_context_nodes_in_tree(ReadonlySpan<VisualContextRef const> context_nodes, AccumulatedVisualContextTree const& tree)
{
    size_t start_index = context_nodes.size();
    while (start_index > 0) {
        auto const& context = context_nodes[start_index - 1];
        if (!context || context.tree != &tree)
            break;
        --start_index;
    }
    return context_nodes.slice(start_index, context_nodes.size() - start_index);
}

static bool display_list_has_contextual_commands(DisplayList const& display_list)
{
    for (auto const& item : display_list.commands()) {
        if (item.context_index.value())
            return true;
    }
    return false;
}

template<typename WireCommand>
ErrorOr<void> append_wire_payload(CommandByteWriter& append_bytes, WireCommand const& command)
{
    return append_bytes(ReadonlyBytes { &command, sizeof(WireCommand) });
}

struct FilterImageSerializationContext {
    PaintCommandEncodingContext const& encoding_context;
    HashMap<u32, Gfx::DecodedImageFrame const*> frames_by_skia_image_unique_id;
};

struct PaintStylePayload {
    u8 paint_style_type { to_underlying(PaintStyleType::SolidColor) };
    ReadonlyBytes bytes;
};

static Optional<ResourceID> ensure_resource_for_decoded_image_frame(PaintCommandEncodingContext const& encoding_context, Gfx::DecodedImageFrame const& frame, Optional<u64> stable_bitmap_id = {})
{
    if (!encoding_context.ensure_bitmap_resource)
        return {};
    return encoding_context.ensure_bitmap_resource(frame, stable_bitmap_id);
}

static sk_sp<SkData const> serialize_filter_image(SkImage* image, void* context)
{
    if (!image || !context)
        return nullptr;

    auto& serialization_context = *static_cast<FilterImageSerializationContext*>(context);
    auto it = serialization_context.frames_by_skia_image_unique_id.find(image->uniqueID());
    if (it == serialization_context.frames_by_skia_image_unique_id.end())
        return nullptr;

    auto resource_id = ensure_resource_for_decoded_image_frame(serialization_context.encoding_context, *it->value);
    if (!resource_id.has_value())
        return nullptr;

    SerializedFilterImageReference serialized_reference {
        .image_resource_id = resource_id.value(),
        .image_id = resource_id->raw(),
    };
    return SkData::MakeWithCopy(&serialized_reference, sizeof(serialized_reference));
}

static ErrorOr<ReadonlyBytes> serialize_filter_bytes(ByteBuffer& scratch_buffer, PaintCommandEncodingContext const& encoding_context, Optional<Gfx::Filter> const& filter)
{
    if (!filter.has_value())
        return ReadonlyBytes {};
    sk_sp<SkImageFilter> sk_filter = Gfx::to_skia_image_filter(filter.value());
    if (!sk_filter)
        return ReadonlyBytes {};

    FilterImageSerializationContext serialization_context {
        .encoding_context = encoding_context,
        .frames_by_skia_image_unique_id = {},
    };
    auto const& image_references = filter->impl().image_references;
    TRY(serialization_context.frames_by_skia_image_unique_id.try_ensure_capacity(image_references.size()));
    for (auto const& image_reference : image_references)
        serialization_context.frames_by_skia_image_unique_id.set(image_reference.skia_image_unique_id, image_reference.frame.ptr());

    SkSerialProcs serial_procs;
    serial_procs.fImageCtx = &serialization_context;
    serial_procs.fImageProc = serialize_filter_image;
    sk_sp<SkData> data = sk_filter->serialize(&serial_procs);
    if (!data)
        return ReadonlyBytes {};
    constexpr size_t max_filter_bytes = static_cast<size_t>(1024) * static_cast<size_t>(1024);
    if (data->size() > max_filter_bytes)
        return ReadonlyBytes {};
    TRY(scratch_buffer.try_resize(data->size()));
    if (data->size() != 0)
        __builtin_memcpy(scratch_buffer.data(), data->data(), data->size());
    return scratch_buffer.bytes();
}

static ErrorOr<void> append_clip_path_payload(CommandByteWriter& append_bytes, Gfx::Path const& path)
{
    SkPath sk_path = Gfx::to_skia_path(path);
    sk_sp<SkData> path_data = sk_path.serialize();
    if (!path_data)
        return Error::from_string_literal("Failed to serialize clip path");
    if (path_data->size() > NumericLimits<u32>::max())
        return Error::from_string_literal("Clip path payload too large");

    AddClipPathCommand add_clip_path;
    add_clip_path.command_size = static_cast<u32>(sizeof(AddClipPathCommand) + path_data->size());
    add_clip_path.bounding_rect = path.bounding_box();
    add_clip_path.path_byte_count = static_cast<u32>(path_data->size());
    add_clip_path.fill_rule = static_cast<u8>(sk_path.getFillType() == SkPathFillType::kEvenOdd ? to_underlying(Gfx::WindingRule::EvenOdd) : to_underlying(Gfx::WindingRule::Nonzero));

    ReadonlyBytes const fragments[] {
        ReadonlyBytes { reinterpret_cast<u8 const*>(&add_clip_path), sizeof(add_clip_path) },
        ReadonlyBytes { path_data->data(), path_data->size() },
    };
    for (auto fragment : fragments)
        TRY(append_bytes(fragment));
    return {};
}

}

void verify_nested_display_list_tree_ownership(DisplayList const& display_list, ReadonlySpan<VisualContextRef const> inherited_context_nodes, VisualContextRef parent_context, bool requires_shared_tree)
{
    if (!requires_shared_tree)
        return;
    if (!display_list_has_contextual_commands(display_list))
        return;
    if (parent_context) {
        VERIFY(&display_list.visual_context_tree() == parent_context.tree);
        return;
    }
    if (auto const* inherited_tree = first_context_tree(inherited_context_nodes))
        VERIFY(&display_list.visual_context_tree() == inherited_tree);
}

ErrorOr<Vector<VisualContextRef>> extend_effective_context_nodes(ReadonlySpan<VisualContextRef const> inherited_context_nodes, VisualContextRef local_context)
{
    Vector<VisualContextRef> effective_nodes;
    TRY(effective_nodes.try_ensure_capacity(inherited_context_nodes.size()));
    for (auto const& inherited_node : inherited_context_nodes)
        TRY(effective_nodes.try_append(inherited_node));

    if (!local_context)
        return effective_nodes;

    Vector<VisualContextRef> local_nodes = TRY(collect_context_nodes_root_to_leaf(local_context));
    if (inherited_context_nodes.is_empty())
        return local_nodes;

    size_t shared_prefix_length = 0;
    ReadonlySpan<VisualContextRef const> inherited_local_tree_nodes = trailing_context_nodes_in_tree(inherited_context_nodes, *local_context.tree);
    if (!inherited_local_tree_nodes.is_empty()) {
        shared_prefix_length = shared_context_prefix_length(inherited_local_tree_nodes, local_nodes.span());
        VERIFY(shared_prefix_length == inherited_local_tree_nodes.size());
    }

    TRY(effective_nodes.try_ensure_capacity(effective_nodes.size() + local_nodes.size() - shared_prefix_length));
    for (size_t index = shared_prefix_length; index < local_nodes.size(); ++index)
        TRY(effective_nodes.try_append(local_nodes[index]));
    return effective_nodes;
}

PaintServer::WireCornerRadii to_wire_corner_radii(CornerRadii const& corner_radii)
{
    return {
        .top_left = { static_cast<f32>(corner_radii.top_left.horizontal_radius), static_cast<f32>(corner_radii.top_left.vertical_radius) },
        .top_right = { static_cast<f32>(corner_radii.top_right.horizontal_radius), static_cast<f32>(corner_radii.top_right.vertical_radius) },
        .bottom_right = { static_cast<f32>(corner_radii.bottom_right.horizontal_radius), static_cast<f32>(corner_radii.bottom_right.vertical_radius) },
        .bottom_left = { static_cast<f32>(corner_radii.bottom_left.horizontal_radius), static_cast<f32>(corner_radii.bottom_left.vertical_radius) },
    };
}

static ErrorOr<void> append_context_setup_command(CommandByteWriter& append_bytes, AccumulatedVisualContextNode const& node)
{
    return node.data.visit(
        [&](EffectsData const&) -> ErrorOr<void> { return {}; },
        [&](PerspectiveData const& perspective) -> ErrorOr<void> {
            TRY(append_wire_payload(append_bytes, SaveCommand {}));
            return append_wire_payload(append_bytes, ApplyTransformCommand { .origin = { 0, 0 }, .matrix = perspective.matrix });
        },
        [&](ScrollData const&) -> ErrorOr<void> {
            return append_wire_payload(append_bytes, SaveCommand {});
        },
        [&](TransformData const& transform) -> ErrorOr<void> {
            TRY(append_wire_payload(append_bytes, SaveCommand {}));
            return append_wire_payload(append_bytes, ApplyTransformCommand { .origin = transform.origin, .matrix = transform.matrix });
        },
        [&](ClipData const& clip_data) -> ErrorOr<void> {
            TRY(append_wire_payload(append_bytes, SaveCommand {}));
            if (clip_data.corner_radii.has_any_radius()) {
                AddRoundedRectClipCommand add_rounded_rect_clip;
                add_rounded_rect_clip.corner_radii = to_wire_corner_radii(clip_data.corner_radii);
                add_rounded_rect_clip.border_rect = clip_data.rect.to_type<int>().to_type<f32>();
                add_rounded_rect_clip.corner_clip = to_underlying(WireCornerClip::Outside);
                return append_wire_payload(append_bytes, add_rounded_rect_clip);
            }
            return append_wire_payload(append_bytes, AddClipRectCommand { .rect = clip_data.rect.to_type<int>().to_type<f32>() });
        },
        [&](ClipPathData const& clip_path) -> ErrorOr<void> {
            TRY(append_wire_payload(append_bytes, SaveCommand {}));
            return append_clip_path_payload(append_bytes, clip_path.path);
        });
}

ErrorOr<void> append_context_setup_commands(CommandByteWriter& append_bytes, AccumulatedVisualContextTree const& tree, VisualContextIndex index)
{
    return append_context_setup_command(append_bytes, tree.node_at(index));
}

ErrorOr<void> append_apply_effects_payload(CommandByteWriter& append_bytes, PaintCommandEncodingContext const& encoding_context, Optional<Gfx::Filter> const& filter, float opacity, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Optional<Gfx::MaskKind> mask_kind)
{
    ByteBuffer scratch_buffer;
    scratch_buffer.ensure_capacity(65536);
    ReadonlyBytes serialized_filter = TRY(serialize_filter_bytes(scratch_buffer, encoding_context, filter));
    if (serialized_filter.size() > NumericLimits<u32>::max())
        return Error::from_string_literal("Effect filter payload too large");
    ApplyEffectsCommand apply_effects;
    apply_effects.command_size = static_cast<u32>(sizeof(ApplyEffectsCommand) + serialized_filter.size());
    apply_effects.opacity = opacity;
    apply_effects.compositing_and_blending_operator = to_underlying(compositing_and_blending_operator);
    apply_effects.filter_byte_count = static_cast<u32>(serialized_filter.size());
    apply_effects.has_mask_kind = mask_kind.has_value() ? 1 : 0;
    apply_effects.mask_kind = mask_kind.has_value() ? to_underlying(mask_kind.value()) : 0;
    ReadonlyBytes const fragments[] {
        ReadonlyBytes { reinterpret_cast<u8 const*>(&apply_effects), sizeof(apply_effects) },
        serialized_filter,
    };
    for (auto fragment : fragments)
        TRY(append_bytes(fragment));
    return {};
}

namespace {

static GradientColorSpace to_wire_gradient_color_space(CSS::RectangularColorSpace color_space)
{
    switch (color_space) {
    case CSS::RectangularColorSpace::Oklab:
        return GradientColorSpace::OKLab;
    case CSS::RectangularColorSpace::Srgb:
        return GradientColorSpace::sRGB;
    case CSS::RectangularColorSpace::SrgbLinear:
        return GradientColorSpace::sRGBLinear;
    case CSS::RectangularColorSpace::Lab:
        return GradientColorSpace::Lab;
    case CSS::RectangularColorSpace::DisplayP3:
        return GradientColorSpace::DisplayP3;
    case CSS::RectangularColorSpace::A98Rgb:
        return GradientColorSpace::A98RGB;
    case CSS::RectangularColorSpace::ProphotoRgb:
        return GradientColorSpace::ProPhotoRGB;
    case CSS::RectangularColorSpace::Rec2020:
        return GradientColorSpace::Rec2020;
    case CSS::RectangularColorSpace::DisplayP3Linear:
    case CSS::RectangularColorSpace::XyzD50:
    case CSS::RectangularColorSpace::XyzD65:
        return GradientColorSpace::OKLab;
    case CSS::RectangularColorSpace::Xyz:
        VERIFY_NOT_REACHED();
    }
    return GradientColorSpace::OKLab;
}

static GradientColorSpace to_wire_gradient_color_space(CSS::PolarColorSpace color_space)
{
    switch (color_space) {
    case CSS::PolarColorSpace::Hsl:
        return GradientColorSpace::HSL;
    case CSS::PolarColorSpace::Hwb:
        return GradientColorSpace::HWB;
    case CSS::PolarColorSpace::Lch:
        return GradientColorSpace::LCH;
    case CSS::PolarColorSpace::Oklch:
        return GradientColorSpace::OKLCH;
    }
    return GradientColorSpace::OKLab;
}

static GradientHueMethod to_wire_gradient_hue_method(CSS::HueInterpolationMethod hue_method)
{
    switch (hue_method) {
    case CSS::HueInterpolationMethod::Shorter:
        return GradientHueMethod::Shorter;
    case CSS::HueInterpolationMethod::Longer:
        return GradientHueMethod::Longer;
    case CSS::HueInterpolationMethod::Increasing:
        return GradientHueMethod::Increasing;
    case CSS::HueInterpolationMethod::Decreasing:
        return GradientHueMethod::Decreasing;
    }
    return GradientHueMethod::Shorter;
}

static SVGSpreadMethod to_wire_svg_spread_method(SVGGradientPaintStyle::SpreadMethod spread_method)
{
    switch (spread_method) {
    case SVGGradientPaintStyle::SpreadMethod::Pad:
        return SVGSpreadMethod::Pad;
    case SVGGradientPaintStyle::SpreadMethod::Repeat:
        return SVGSpreadMethod::Repeat;
    case SVGGradientPaintStyle::SpreadMethod::Reflect:
        return SVGSpreadMethod::Reflect;
    }
    return SVGSpreadMethod::Pad;
}

static void encode_wire_gradient_interpolation(CSS::ColorInterpolationMethodStyleValue::ColorInterpolationMethod const& interpolation_method, u8& color_space, u8& hue_method)
{
    interpolation_method.visit(
        [&](CSS::RectangularColorSpace rectangular_color_space) {
            color_space = to_underlying(to_wire_gradient_color_space(rectangular_color_space));
            hue_method = to_underlying(GradientHueMethod::Shorter);
        },
        [&](CSS::ColorInterpolationMethodStyleValue::PolarColorInterpolationMethod const& polar_color_space) {
            color_space = to_underlying(to_wire_gradient_color_space(polar_color_space.color_space));
            hue_method = to_underlying(to_wire_gradient_hue_method(polar_color_space.hue_interpolation_method));
        });
}

}

class DisplayListRecorder::CommandEncoder {
public:
    CommandEncoder(CommandByteWriter& append_bytes, PaintCommandEncodingContext const& encoding_context)
        : m_append_bytes(append_bytes)
        , m_encoding_context(encoding_context)
    {
        m_scratch_buffer.ensure_capacity(65536);
    }

    ErrorOr<void> append_apply_effects_payload(Optional<Gfx::Filter> const& filter, float opacity, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Optional<Gfx::MaskKind> mask_kind);
    ErrorOr<void> append_apply_backdrop_filter_payload(Gfx::IntRect const&, CornerRadii const&, Gfx::Filter const&);

    void append_glyph_run(NonnullRefPtr<Gfx::GlyphRun const> const&, Gfx::FloatPoint translation, Gfx::IntRect bounding_rect, Gfx::Color, Gfx::Orientation);
    void append_fill_rect(Gfx::IntRect const&, Gfx::Color);
    void append_save();
    void append_save_layer();
    void append_restore();
    void append_translate(Gfx::IntPoint delta);
    void append_add_clip_rect(Gfx::IntRect const&);
    void append_fill_rect_with_rounded_corners(Gfx::IntRect const&, Gfx::Color, CornerRadii const&);
    void append_add_rounded_rect_clip(CornerRadii const&, Gfx::IntRect, CornerClip);
    void append_draw_scaled_decoded_image_frame(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::DecodedImageFrame const&, Gfx::ScalingMode, Optional<u64> stable_bitmap_id = {});
    void append_draw_repeated_decoded_image_frame(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::DecodedImageFrame const&, Gfx::ScalingMode, bool repeat_x, bool repeat_y, Optional<u64> stable_bitmap_id = {});
    void append_draw_external_content(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, NonnullRefPtr<ExternalContentSource> const&, Gfx::ScalingMode, bool repeat_x, bool repeat_y);
    void append_draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Gfx::Color, int thickness, Gfx::LineStyle, Gfx::Color alternate_color);
    void append_draw_rect(Gfx::IntRect const&, Gfx::Color, bool rough);
    void append_draw_ellipse(Gfx::IntRect const&, Gfx::Color, int thickness);
    void append_fill_ellipse(Gfx::IntRect const&, Gfx::Color);
    void append_fill_path(Gfx::IntRect path_bounding_rect, Gfx::Path const&, float opacity, PaintStyleOrColor const&, Gfx::WindingRule, ShouldAntiAlias);
    void append_stroke_path(Gfx::Path::CapStyle, Gfx::Path::JoinStyle, float miter_limit, ReadonlySpan<float const> dash_array, float dash_offset, Gfx::IntRect path_bounding_rect, Gfx::Path const&, float opacity, PaintStyleOrColor const&, float thickness, ShouldAntiAlias);
    void append_paint_text_shadow(NonnullRefPtr<Gfx::GlyphRun const> const&, Gfx::IntRect shadow_bounding_rect, Gfx::IntRect text_rect, Gfx::FloatPoint draw_location, int blur_radius, Gfx::Color);
    void append_paint_linear_gradient(Gfx::IntRect const&, LinearGradientData const&);
    void append_paint_radial_gradient(Gfx::IntRect const&, RadialGradientData const&, Gfx::IntPoint center, Gfx::IntSize size);
    void append_paint_conic_gradient(Gfx::IntRect const&, ConicGradientData const&, Gfx::IntPoint position);
    void append_paint_outer_box_shadow(Gfx::Color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const&, Gfx::IntRect shadow_rect, CornerRadii const&);
    void append_paint_inner_box_shadow(Gfx::Color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const&, Gfx::IntRect outer_shadow_rect, Gfx::IntRect inner_shadow_rect, CornerRadii const&);
    void append_paint_scrollbar(u32 scroll_frame_id, Gfx::IntRect scrollbar_rect, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, double scroll_size, Gfx::Color thumb_color, Gfx::Color track_color, bool vertical);

private:
    ErrorOr<ReadonlyBytes> serialize_filter_bytes(Optional<Gfx::Filter> const&);

    CommandByteWriter& m_append_bytes;
    ByteBuffer m_scratch_buffer;
    PaintCommandEncodingContext const& m_encoding_context;
};

class FlatDisplayListEncoder {
public:
    FlatDisplayListEncoder(CommandByteWriter& append_bytes, PaintCommandEncodingContext const& encoding_context)
        : m_append_bytes(append_bytes)
        , m_encoder(append_bytes, encoding_context)
    {
    }

    ErrorOr<void> encode(DisplayList const& display_list)
    {
        auto const& visual_context_tree = display_list.visual_context_tree();
        VisualContextIndex applied_context_index;
        size_t applied_depth = 0;

        auto apply_context_node = [&](AccumulatedVisualContextNode const& node) -> ErrorOr<void> {
            return node.data.visit(
                [&](EffectsData const& effects) -> ErrorOr<void> {
                    return m_encoder.append_apply_effects_payload(effects.gfx_filter, effects.opacity, effects.blend_mode, {});
                },
                [&](PerspectiveData const& perspective) -> ErrorOr<void> {
                    TRY(append_wire_payload(m_append_bytes, SaveCommand {}));
                    return append_wire_payload(m_append_bytes, ApplyTransformCommand { .origin = { 0, 0 }, .matrix = perspective.matrix });
                },
                [&](ScrollData const&) -> ErrorOr<void> {
                    return append_wire_payload(m_append_bytes, SaveCommand {});
                },
                [&](TransformData const& transform) -> ErrorOr<void> {
                    TRY(append_wire_payload(m_append_bytes, SaveCommand {}));
                    return append_wire_payload(m_append_bytes, ApplyTransformCommand { .origin = transform.origin, .matrix = transform.matrix });
                },
                [&](ClipData const& clip) -> ErrorOr<void> {
                    TRY(append_wire_payload(m_append_bytes, SaveCommand {}));
                    if (clip.corner_radii.has_any_radius())
                        m_encoder.append_add_rounded_rect_clip(clip.corner_radii, clip.rect.to_type<int>(), CornerClip::Outside);
                    else
                        m_encoder.append_add_clip_rect(clip.rect.to_type<int>());
                    return {};
                },
                [&](ClipPathData const& clip_path) -> ErrorOr<void> {
                    TRY(append_wire_payload(m_append_bytes, SaveCommand {}));
                    return append_clip_path_payload(m_append_bytes, clip_path.path);
                });
        };

        auto switch_to_context = [&](VisualContextIndex target_index) -> ErrorOr<void> {
            if (applied_context_index == target_index)
                return {};

            Vector<VisualContextRef> applied_nodes;
            if (applied_context_index.value())
                applied_nodes = TRY(collect_context_nodes_root_to_leaf({ &visual_context_tree, applied_context_index }));
            Vector<VisualContextRef> target_nodes;
            if (target_index.value())
                target_nodes = TRY(collect_context_nodes_root_to_leaf({ &visual_context_tree, target_index }));

            size_t const shared_prefix_length = shared_context_prefix_length(applied_nodes.span(), target_nodes.span());
            while (applied_depth > shared_prefix_length) {
                TRY(append_wire_payload(m_append_bytes, RestoreCommand {}));
                --applied_depth;
            }

            for (size_t index = shared_prefix_length; index < target_nodes.size(); ++index) {
                TRY(apply_context_node(visual_context_tree.node_at(target_nodes[index].index)));
                ++applied_depth;
            }

            applied_context_index = target_index;
            return {};
        };

        for (auto const& item : display_list.commands()) {
            TRY(switch_to_context(item.context_index));
            if (item.kind == DisplayList::ItemKind::DrawCommand) {
                TRY(m_append_bytes(display_list.bytes_for(item)));
                continue;
            }
            auto const& nested = display_list.nested_display_list_for(item);
            if (!nested.display_list)
                continue;
            TRY(append_wire_payload(m_append_bytes, SaveCommand {}));
            TRY(append_wire_payload(m_append_bytes, TranslateCommand { .delta = nested.rect.location().to_type<f32>() }));
            TRY(encode(*nested.display_list));
            TRY(append_wire_payload(m_append_bytes, RestoreCommand {}));
        }

        while (applied_depth > 0) {
            TRY(append_wire_payload(m_append_bytes, RestoreCommand {}));
            --applied_depth;
        }
        return {};
    }

private:
    CommandByteWriter& m_append_bytes;
    DisplayListRecorder::CommandEncoder m_encoder;
};

static ErrorOr<ByteBuffer> encode_flat_display_list_commands(DisplayList const& display_list, PaintCommandEncodingContext const& encoding_context)
{
    ByteBuffer payload;
    CommandByteWriter writer = [&](ReadonlyBytes bytes) -> ErrorOr<void> { return payload.try_append(bytes); };
    FlatDisplayListEncoder encoder(writer, encoding_context);
    TRY(encoder.encode(display_list));
    return payload;
}

static ErrorOr<PaintStylePayload> encode_svg_paint_style_payload(PaintCommandEncodingContext const& encoding_context, ByteBuffer& scratch_buffer, PaintStyle const& paint_style)
{
    if (!paint_style)
        return Error::from_string_literal("Missing paint style");

    if (auto const* linear_gradient = as_if<SVGLinearGradientPaintStyle>(*paint_style)) {
        auto const color_stops = linear_gradient->color_stops();
        SVGLinearGradientPayload header;
        header.start_point = linear_gradient->start_point();
        header.end_point = linear_gradient->end_point();
        header.gradient_transform = linear_gradient->gradient_transform().value_or(Gfx::AffineTransform {});
        header.stop_count = static_cast<u32>(color_stops.size());
        header.spread_method = to_underlying(to_wire_svg_spread_method(linear_gradient->spread_method()));
        header.color_space = static_cast<u8>(to_underlying(linear_gradient->color_space()));
        header.has_gradient_transform = linear_gradient->gradient_transform().has_value() ? 1 : 0;
        TRY(scratch_buffer.try_resize(sizeof(SVGLinearGradientPayload) + (color_stops.size() * sizeof(WireColorStop))));
        size_t offset = 0;
        TRY(append_struct(scratch_buffer.bytes(), offset, header));
        for (auto const& stop : color_stops)
            TRY(append_struct(scratch_buffer.bytes(), offset, WireColorStop { .color = stop.color, .position = stop.position }));
        return PaintStylePayload { .paint_style_type = to_underlying(PaintStyleType::SVGLinearGradient), .bytes = scratch_buffer.bytes() };
    }

    if (auto const* radial_gradient = as_if<SVGRadialGradientPaintStyle>(*paint_style)) {
        auto const color_stops = radial_gradient->color_stops();
        SVGRadialGradientPayload header;
        header.start_center = radial_gradient->start_center();
        header.start_radius = radial_gradient->start_radius();
        header.end_center = radial_gradient->end_center();
        header.end_radius = radial_gradient->end_radius();
        header.gradient_transform = radial_gradient->gradient_transform().value_or(Gfx::AffineTransform {});
        header.stop_count = static_cast<u32>(color_stops.size());
        header.spread_method = to_underlying(to_wire_svg_spread_method(radial_gradient->spread_method()));
        header.color_space = static_cast<u8>(to_underlying(radial_gradient->color_space()));
        header.has_gradient_transform = radial_gradient->gradient_transform().has_value() ? 1 : 0;
        TRY(scratch_buffer.try_resize(sizeof(SVGRadialGradientPayload) + (color_stops.size() * sizeof(WireColorStop))));
        size_t offset = 0;
        TRY(append_struct(scratch_buffer.bytes(), offset, header));
        for (auto const& stop : color_stops)
            TRY(append_struct(scratch_buffer.bytes(), offset, WireColorStop { .color = stop.color, .position = stop.position }));
        return PaintStylePayload { .paint_style_type = to_underlying(PaintStyleType::SVGRadialGradient), .bytes = scratch_buffer.bytes() };
    }

    if (auto const* pattern = as_if<SVGPatternPaintStyle>(*paint_style)) {
        ByteBuffer pattern_draw_list = TRY(encode_flat_display_list_commands(*pattern->tile_display_list(), encoding_context));
        if (pattern_draw_list.size() > NumericLimits<u32>::max() - sizeof(SVGPatternPayload))
            return Error::from_string_literal("Pattern draw-list payload too large");
        SVGPatternPayload header;
        header.tile_rect = pattern->tile_rect();
        header.pattern_transform = pattern->pattern_transform().value_or(Gfx::AffineTransform {});
        header.draw_list_byte_count = static_cast<u32>(pattern_draw_list.size());
        header.has_pattern_transform = pattern->pattern_transform().has_value() ? 1 : 0;
        TRY(scratch_buffer.try_resize(sizeof(SVGPatternPayload) + pattern_draw_list.size()));
        __builtin_memcpy(scratch_buffer.data(), &header, sizeof(header));
        if (!pattern_draw_list.is_empty())
            __builtin_memcpy(scratch_buffer.data() + sizeof(header), pattern_draw_list.data(), pattern_draw_list.size());
        return PaintStylePayload { .paint_style_type = to_underlying(PaintStyleType::SVGPattern), .bytes = scratch_buffer.bytes() };
    }

    return Error::from_string_literal("Unsupported SVG paint style");
}

ErrorOr<ReadonlyBytes> DisplayListRecorder::CommandEncoder::serialize_filter_bytes(Optional<Gfx::Filter> const& filter)
{
    return ::Web::Painting::serialize_filter_bytes(m_scratch_buffer, m_encoding_context, filter);
}

ErrorOr<void> DisplayListRecorder::CommandEncoder::append_apply_effects_payload(Optional<Gfx::Filter> const& filter, float opacity, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Optional<Gfx::MaskKind> mask_kind)
{
    ReadonlyBytes serialized_filter = TRY(serialize_filter_bytes(filter));
    if (serialized_filter.size() > NumericLimits<u32>::max())
        return Error::from_string_literal("Effect filter payload too large");
    ApplyEffectsCommand apply_effects;
    apply_effects.command_size = static_cast<u32>(sizeof(ApplyEffectsCommand) + serialized_filter.size());
    apply_effects.opacity = opacity;
    apply_effects.compositing_and_blending_operator = to_underlying(compositing_and_blending_operator);
    apply_effects.filter_byte_count = static_cast<u32>(serialized_filter.size());
    apply_effects.has_mask_kind = mask_kind.has_value() ? 1 : 0;
    apply_effects.mask_kind = mask_kind.has_value() ? to_underlying(mask_kind.value()) : 0;
    ReadonlyBytes const fragments[] {
        ReadonlyBytes { reinterpret_cast<u8 const*>(&apply_effects), sizeof(apply_effects) },
        serialized_filter,
    };
    for (auto fragment : fragments)
        TRY(m_append_bytes(fragment));
    return {};
}

ErrorOr<void> DisplayListRecorder::CommandEncoder::append_apply_backdrop_filter_payload(Gfx::IntRect const& backdrop_region, CornerRadii const& corner_radii, Gfx::Filter const& backdrop_filter)
{
    ReadonlyBytes serialized_filter = TRY(serialize_filter_bytes(backdrop_filter));
    if (serialized_filter.size() > NumericLimits<u32>::max())
        return Error::from_string_literal("Backdrop filter payload too large");
    ApplyBackdropFilterCommand command {
        .command_size = static_cast<u32>(sizeof(ApplyBackdropFilterCommand) + serialized_filter.size()),
        .backdrop_region = backdrop_region.to_type<f32>(),
        .corner_radii = to_wire_corner_radii(corner_radii),
        .filter_byte_count = static_cast<u32>(serialized_filter.size()),
    };
    ReadonlyBytes const fragments[] {
        ReadonlyBytes { reinterpret_cast<u8 const*>(&command), sizeof(command) },
        serialized_filter,
    };
    for (auto fragment : fragments)
        TRY(m_append_bytes(fragment));
    return {};
}

void DisplayListRecorder::CommandEncoder::append_glyph_run(NonnullRefPtr<Gfx::GlyphRun const> const& glyph_run, Gfx::FloatPoint translation, Gfx::IntRect bounding_rect, Gfx::Color color, Gfx::Orientation orientation)
{
    auto const& glyphs = glyph_run->glyphs();
    if (glyphs.is_empty())
        return;
    auto const& font = glyph_run->font();
    size_t command_size = sizeof(DrawGlyphRunCommand) + (glyphs.size() * sizeof(Glyph));
    VERIFY(command_size <= NumericLimits<u32>::max());
    DrawGlyphRunCommand command;
    command.command_size = static_cast<u32>(command_size);
    command.font_resource_id = m_encoding_context.ensure_font_resource(font);
    command.font_pixel_size = font.pixel_size();
    command.font_ascent = font.pixel_metrics().ascent;
    command.device_pixels_per_css_pixel = static_cast<f32>(m_encoding_context.device_pixels_per_css_pixel);
    command.color = color;
    command.translation = translation;
    command.text_rect = bounding_rect.to_type<f32>();
    command.visual_bounds = glyph_run->cached_blob_bounds().translated(translation);
    command.orientation = static_cast<u8>(to_underlying(orientation));
    command.glyph_count = static_cast<u32>(glyphs.size());
    MUST(m_scratch_buffer.try_resize(command_size));
    __builtin_memcpy(m_scratch_buffer.data(), &command, sizeof(command));
    for (size_t index = 0; index < glyphs.size(); ++index) {
        Glyph wire_glyph { .glyph_id = glyphs[index].glyph_id, .x = glyphs[index].position.x(), .y = glyphs[index].position.y() };
        __builtin_memcpy(m_scratch_buffer.data() + sizeof(command) + (index * sizeof(Glyph)), &wire_glyph, sizeof(wire_glyph));
    }
    MUST(m_append_bytes(m_scratch_buffer.bytes()));
}

void DisplayListRecorder::CommandEncoder::append_fill_rect(Gfx::IntRect const& rect, Gfx::Color color) { MUST(append_wire_payload(m_append_bytes, FillRectCommand { .color = color, .rect = rect.to_type<f32>() })); }
void DisplayListRecorder::CommandEncoder::append_save() { MUST(append_wire_payload(m_append_bytes, SaveCommand {})); }
void DisplayListRecorder::CommandEncoder::append_save_layer() { MUST(append_wire_payload(m_append_bytes, SaveLayerCommand {})); }
void DisplayListRecorder::CommandEncoder::append_restore() { MUST(append_wire_payload(m_append_bytes, RestoreCommand {})); }
void DisplayListRecorder::CommandEncoder::append_translate(Gfx::IntPoint delta) { MUST(append_wire_payload(m_append_bytes, TranslateCommand { .delta = delta.to_type<f32>() })); }
void DisplayListRecorder::CommandEncoder::append_add_clip_rect(Gfx::IntRect const& rect) { MUST(append_wire_payload(m_append_bytes, AddClipRectCommand { .rect = rect.to_type<f32>() })); }

void DisplayListRecorder::CommandEncoder::append_fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Gfx::Color color, CornerRadii const& corner_radii)
{
    MUST(append_wire_payload(m_append_bytes, FillRectWithRoundedCornersCommand {
                                                 .color = color,
                                                 .rect = rect.to_type<f32>(),
                                                 .top_left = { static_cast<f32>(corner_radii.top_left.horizontal_radius), static_cast<f32>(corner_radii.top_left.vertical_radius) },
                                                 .top_right = { static_cast<f32>(corner_radii.top_right.horizontal_radius), static_cast<f32>(corner_radii.top_right.vertical_radius) },
                                                 .bottom_right = { static_cast<f32>(corner_radii.bottom_right.horizontal_radius), static_cast<f32>(corner_radii.bottom_right.vertical_radius) },
                                                 .bottom_left = { static_cast<f32>(corner_radii.bottom_left.horizontal_radius), static_cast<f32>(corner_radii.bottom_left.vertical_radius) },
                                             }));
}

void DisplayListRecorder::CommandEncoder::append_add_rounded_rect_clip(CornerRadii const& corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip)
{
    AddRoundedRectClipCommand command;
    command.corner_radii = to_wire_corner_radii(corner_radii);
    command.border_rect = border_rect.to_type<f32>();
    command.corner_clip = static_cast<u8>(to_underlying(corner_clip));
    MUST(append_wire_payload(m_append_bytes, command));
}

void DisplayListRecorder::CommandEncoder::append_draw_scaled_decoded_image_frame(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::DecodedImageFrame const& frame, Gfx::ScalingMode scaling_mode, Optional<u64> stable_bitmap_id)
{
    auto resource_id = ensure_resource_for_decoded_image_frame(m_encoding_context, frame, stable_bitmap_id);
    if (!resource_id.has_value())
        return;
    MUST(append_wire_payload(m_append_bytes, DrawScaledImageCommand {
                                                 .image_resource_id = resource_id.value(),
                                                 .image_id = resource_id->raw(),
                                                 .scaling_mode = static_cast<u32>(to_underlying(scaling_mode)),
                                                 .opacity = 1.0f,
                                                 .compositing_and_blending_operator = static_cast<u8>(to_underlying(Gfx::CompositingAndBlendingOperator::SourceOver)),
                                                 .src_rect = frame.rect().to_type<f32>(),
                                                 .dst_rect = dst_rect.to_type<f32>(),
                                                 .clip_rect = clip_rect.to_type<f32>(),
                                             }));
}

void DisplayListRecorder::CommandEncoder::append_draw_repeated_decoded_image_frame(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::DecodedImageFrame const& frame, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y, Optional<u64> stable_bitmap_id)
{
    auto resource_id = ensure_resource_for_decoded_image_frame(m_encoding_context, frame, stable_bitmap_id);
    if (!resource_id.has_value())
        return;
    MUST(append_wire_payload(m_append_bytes, DrawRepeatedImageCommand {
                                                 .image_resource_id = resource_id.value(),
                                                 .image_id = resource_id->raw(),
                                                 .scaling_mode = static_cast<u32>(to_underlying(scaling_mode)),
                                                 .repeat_x = static_cast<u8>(repeat_x ? 1 : 0),
                                                 .repeat_y = static_cast<u8>(repeat_y ? 1 : 0),
                                                 .dst_rect = dst_rect.to_type<f32>(),
                                                 .clip_rect = clip_rect.to_type<f32>(),
                                             }));
}

void DisplayListRecorder::CommandEncoder::append_draw_external_content(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, NonnullRefPtr<ExternalContentSource> const& source, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y)
{
    auto image_id = source->current_image_id();
    if (!image_id.has_value())
        return;
    if (repeat_x || repeat_y) {
        MUST(append_wire_payload(m_append_bytes, DrawRepeatedImageCommand {
                                                     .image_resource_id = { 0 },
                                                     .image_id = image_id.value(),
                                                     .scaling_mode = static_cast<u32>(to_underlying(scaling_mode)),
                                                     .repeat_x = static_cast<u8>(repeat_x ? 1 : 0),
                                                     .repeat_y = static_cast<u8>(repeat_y ? 1 : 0),
                                                     .dst_rect = dst_rect.to_type<f32>(),
                                                     .clip_rect = clip_rect.to_type<f32>(),
                                                 }));
        return;
    }
    MUST(append_wire_payload(m_append_bytes, DrawExternalContentCommand {
                                                 .image_resource_id = { 0 },
                                                 .image_id = image_id.value(),
                                                 .scaling_mode = static_cast<u32>(to_underlying(scaling_mode)),
                                                 .dst_rect = dst_rect.to_type<f32>(),
                                                 .clip_rect = clip_rect.to_type<f32>(),
                                             }));
}

void DisplayListRecorder::CommandEncoder::append_draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Gfx::Color color, int thickness, Gfx::LineStyle style, Gfx::Color alternate_color)
{
    MUST(append_wire_payload(m_append_bytes, DrawLineCommand {
                                                 .color = color,
                                                 .from = from.to_type<f32>(),
                                                 .to = to.to_type<f32>(),
                                                 .thickness = max(1.0f, static_cast<f32>(thickness)),
                                                 .style = static_cast<u8>(to_underlying(style)),
                                                 .alternate_color = alternate_color,
                                             }));
}

void DisplayListRecorder::CommandEncoder::append_draw_rect(Gfx::IntRect const& rect, Gfx::Color color, bool) { MUST(append_wire_payload(m_append_bytes, DrawRectCommand { .color = color, .rect = rect.to_type<f32>(), .thickness = 1.0f })); }
void DisplayListRecorder::CommandEncoder::append_draw_ellipse(Gfx::IntRect const& rect, Gfx::Color color, int thickness) { MUST(append_wire_payload(m_append_bytes, DrawEllipseCommand { .color = color, .rect = rect.to_type<f32>(), .thickness = max(1.0f, static_cast<f32>(thickness)) })); }
void DisplayListRecorder::CommandEncoder::append_fill_ellipse(Gfx::IntRect const& rect, Gfx::Color color) { MUST(append_wire_payload(m_append_bytes, FillEllipseCommand { .color = color, .rect = rect.to_type<f32>() })); }

void DisplayListRecorder::CommandEncoder::append_fill_path(Gfx::IntRect path_bounding_rect, Gfx::Path const& path, float opacity, PaintStyleOrColor const& paint_style_or_color, Gfx::WindingRule winding_rule, ShouldAntiAlias should_anti_alias)
{
    SkPath sk_path = Gfx::to_skia_path(path);
    sk_sp<SkData> path_data = sk_path.serialize();
    if (!path_data)
        return;
    size_t paint_style_size = 0;
    u8 paint_style_type = to_underlying(PaintStyleType::SolidColor);
    Gfx::Color color {};
    if (paint_style_or_color.has<Gfx::Color>()) {
        color = paint_style_or_color.get<Gfx::Color>();
    } else {
        auto payload = encode_svg_paint_style_payload(m_encoding_context, m_scratch_buffer, paint_style_or_color.get<PaintStyle>());
        if (payload.is_error())
            return;
        paint_style_type = payload.value().paint_style_type;
        paint_style_size = payload.value().bytes.size();
    }
    FillPathCommand command;
    command.command_size = static_cast<u32>(sizeof(FillPathCommand) + path_data->size() + paint_style_size);
    command.color = color;
    command.path_bounding_rect = path_bounding_rect.to_type<f32>();
    command.opacity = opacity;
    command.path_byte_count = static_cast<u32>(path_data->size());
    command.paint_style_byte_count = static_cast<u32>(paint_style_size);
    command.paint_style_type = paint_style_type;
    command.winding_rule = static_cast<u8>(to_underlying(winding_rule));
    command.should_anti_alias = should_anti_alias == ShouldAntiAlias::Yes ? 1 : 0;
    ReadonlyBytes const fragments[] {
        ReadonlyBytes { &command, sizeof(command) },
        ReadonlyBytes { path_data->data(), path_data->size() },
        ReadonlyBytes { paint_style_size == 0 ? nullptr : m_scratch_buffer.data(), paint_style_size },
    };
    for (auto fragment : fragments)
        MUST(m_append_bytes(fragment));
}

void DisplayListRecorder::CommandEncoder::append_stroke_path(Gfx::Path::CapStyle cap_style, Gfx::Path::JoinStyle join_style, float miter_limit, ReadonlySpan<float const> dash_array, float dash_offset, Gfx::IntRect path_bounding_rect, Gfx::Path const& path, float opacity, PaintStyleOrColor const& paint_style_or_color, float thickness, ShouldAntiAlias should_anti_alias)
{
    SkPath sk_path = Gfx::to_skia_path(path);
    sk_sp<SkData> path_data = sk_path.serialize();
    if (!path_data)
        return;
    size_t paint_style_size = 0;
    u8 paint_style_type = to_underlying(PaintStyleType::SolidColor);
    Gfx::Color color {};
    if (paint_style_or_color.has<Gfx::Color>()) {
        color = paint_style_or_color.get<Gfx::Color>();
    } else {
        auto payload = encode_svg_paint_style_payload(m_encoding_context, m_scratch_buffer, paint_style_or_color.get<PaintStyle>());
        if (payload.is_error())
            return;
        paint_style_type = payload.value().paint_style_type;
        paint_style_size = payload.value().bytes.size();
    }
    size_t dash_bytes_size = dash_array.size() * sizeof(float);
    size_t command_size = sizeof(StrokePathCommand) + dash_bytes_size + path_data->size() + paint_style_size;
    if (command_size > NumericLimits<u32>::max())
        return;
    StrokePathCommand command;
    command.command_size = static_cast<u32>(command_size);
    command.color = color;
    command.path_bounding_rect = path_bounding_rect.to_type<f32>();
    command.opacity = opacity;
    command.thickness = thickness;
    command.miter_limit = miter_limit;
    command.dash_offset = dash_offset;
    command.dash_count = static_cast<u32>(dash_array.size());
    command.path_byte_count = static_cast<u32>(path_data->size());
    command.paint_style_byte_count = static_cast<u32>(paint_style_size);
    command.paint_style_type = paint_style_type;
    command.cap_style = static_cast<u8>(to_underlying(cap_style));
    command.join_style = static_cast<u8>(to_underlying(join_style));
    command.should_anti_alias = should_anti_alias == ShouldAntiAlias::Yes ? 1 : 0;
    ReadonlyBytes const fragments[] {
        ReadonlyBytes { &command, sizeof(command) },
        ReadonlyBytes { dash_array.data(), dash_bytes_size },
        ReadonlyBytes { path_data->data(), path_data->size() },
        ReadonlyBytes { paint_style_size == 0 ? nullptr : m_scratch_buffer.data(), paint_style_size },
    };
    for (auto fragment : fragments)
        MUST(m_append_bytes(fragment));
}

void DisplayListRecorder::CommandEncoder::append_paint_text_shadow(NonnullRefPtr<Gfx::GlyphRun const> const& glyph_run, Gfx::IntRect shadow_bounding_rect, Gfx::IntRect text_rect, Gfx::FloatPoint draw_location, int blur_radius, Gfx::Color color)
{
    auto const& glyphs = glyph_run->glyphs();
    if (glyphs.is_empty())
        return;
    auto const& font = glyph_run->font();
    size_t command_size = sizeof(PaintTextShadowCommand) + (glyphs.size() * sizeof(Glyph));
    if (command_size > NumericLimits<u32>::max())
        return;
    PaintTextShadowCommand command;
    command.command_size = static_cast<u32>(command_size);
    command.font_resource_id = m_encoding_context.ensure_font_resource(font);
    command.font_pixel_size = font.pixel_size();
    command.font_ascent = font.pixel_metrics().ascent;
    command.device_pixels_per_css_pixel = static_cast<f32>(m_encoding_context.device_pixels_per_css_pixel);
    command.color = color;
    command.translation = draw_location + text_rect.location().to_type<float>();
    command.text_rect = text_rect.to_type<f32>();
    command.visual_bounds = Gfx::FloatRect { draw_location, shadow_bounding_rect.size().to_type<float>() };
    command.blur_radius = static_cast<f32>(blur_radius);
    command.glyph_count = static_cast<u32>(glyphs.size());
    MUST(m_scratch_buffer.try_resize(command_size));
    __builtin_memcpy(m_scratch_buffer.data(), &command, sizeof(command));
    for (size_t index = 0; index < glyphs.size(); ++index) {
        Glyph wire_glyph { .glyph_id = glyphs[index].glyph_id, .x = glyphs[index].position.x(), .y = glyphs[index].position.y() };
        __builtin_memcpy(m_scratch_buffer.data() + sizeof(command) + (index * sizeof(Glyph)), &wire_glyph, sizeof(wire_glyph));
    }
    MUST(m_append_bytes(m_scratch_buffer.bytes()));
}

void DisplayListRecorder::CommandEncoder::append_paint_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data)
{
    auto const& stops = data.color_stops.list;
    if (stops.is_empty())
        return;
    PaintLinearGradientCommand command;
    command.gradient_rect = gradient_rect.to_type<f32>();
    command.gradient_angle = data.gradient_angle;
    command.has_repeat_length = data.color_stops.repeat_length.has_value() ? 1 : 0;
    command.repeat_length = data.color_stops.repeat_length.value_or(1.0f);
    encode_wire_gradient_interpolation(data.interpolation_method, command.color_space, command.hue_method);
    command.stop_count = static_cast<u32>(stops.size());
    command.command_size = static_cast<u32>(sizeof(PaintLinearGradientCommand) + (stops.size() * sizeof(WireColorStop)));
    MUST(m_scratch_buffer.try_resize(command.command_size));
    __builtin_memcpy(m_scratch_buffer.data(), &command, sizeof(command));
    for (size_t index = 0; index < stops.size(); ++index) {
        WireColorStop stop { .color = stops[index].color, .position = stops[index].position };
        __builtin_memcpy(m_scratch_buffer.data() + sizeof(command) + (index * sizeof(WireColorStop)), &stop, sizeof(stop));
    }
    MUST(m_append_bytes(m_scratch_buffer.bytes()));
}

void DisplayListRecorder::CommandEncoder::append_paint_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size)
{
    auto const& stops = data.color_stops.list;
    if (stops.is_empty())
        return;
    PaintRadialGradientCommand command;
    command.rect = rect.to_type<f32>();
    command.center = center.translated(rect.location()).to_type<f32>();
    command.size = size.to_type<f32>();
    command.repeating = data.color_stops.repeating ? 1 : 0;
    encode_wire_gradient_interpolation(data.interpolation_method, command.color_space, command.hue_method);
    command.stop_count = static_cast<u32>(stops.size());
    command.command_size = static_cast<u32>(sizeof(PaintRadialGradientCommand) + (stops.size() * sizeof(WireColorStop)));
    MUST(m_scratch_buffer.try_resize(command.command_size));
    __builtin_memcpy(m_scratch_buffer.data(), &command, sizeof(command));
    for (size_t index = 0; index < stops.size(); ++index) {
        WireColorStop stop { .color = stops[index].color, .position = stops[index].position };
        __builtin_memcpy(m_scratch_buffer.data() + sizeof(command) + (index * sizeof(WireColorStop)), &stop, sizeof(stop));
    }
    MUST(m_append_bytes(m_scratch_buffer.bytes()));
}

void DisplayListRecorder::CommandEncoder::append_paint_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint position)
{
    auto const& stops = data.color_stops.list;
    if (stops.is_empty())
        return;
    PaintConicGradientCommand command;
    command.rect = rect.to_type<f32>();
    command.start_angle = data.start_angle;
    command.position = position.translated(rect.location()).to_type<f32>();
    encode_wire_gradient_interpolation(data.interpolation_method, command.color_space, command.hue_method);
    command.stop_count = static_cast<u32>(stops.size());
    command.command_size = static_cast<u32>(sizeof(PaintConicGradientCommand) + (stops.size() * sizeof(WireColorStop)));
    MUST(m_scratch_buffer.try_resize(command.command_size));
    __builtin_memcpy(m_scratch_buffer.data(), &command, sizeof(command));
    for (size_t index = 0; index < stops.size(); ++index) {
        WireColorStop stop { .color = stops[index].color, .position = stops[index].position };
        __builtin_memcpy(m_scratch_buffer.data() + sizeof(command) + (index * sizeof(WireColorStop)), &stop, sizeof(stop));
    }
    MUST(m_append_bytes(m_scratch_buffer.bytes()));
}

void DisplayListRecorder::CommandEncoder::append_paint_outer_box_shadow(Gfx::Color color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const& content_corner_radii, Gfx::IntRect shadow_rect, CornerRadii const& shadow_corner_radii)
{
    MUST(append_wire_payload(m_append_bytes, PaintOuterBoxShadowCommand {
                                                 .color = color,
                                                 .content_corner_radii = to_wire_corner_radii(content_corner_radii),
                                                 .shadow_corner_radii = to_wire_corner_radii(shadow_corner_radii),
                                                 .blur_radius = static_cast<f32>(blur_radius),
                                                 .device_content_rect = device_content_rect.to_type<f32>(),
                                                 .shadow_rect = shadow_rect.to_type<f32>(),
                                             }));
}

void DisplayListRecorder::CommandEncoder::append_paint_inner_box_shadow(Gfx::Color color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const& content_corner_radii, Gfx::IntRect outer_shadow_rect, Gfx::IntRect inner_shadow_rect, CornerRadii const& inner_shadow_corner_radii)
{
    MUST(append_wire_payload(m_append_bytes, PaintInnerBoxShadowCommand {
                                                 .color = color,
                                                 .content_corner_radii = to_wire_corner_radii(content_corner_radii),
                                                 .inner_shadow_corner_radii = to_wire_corner_radii(inner_shadow_corner_radii),
                                                 .blur_radius = static_cast<f32>(blur_radius),
                                                 .device_content_rect = device_content_rect.to_type<f32>(),
                                                 .outer_shadow_rect = outer_shadow_rect.to_type<f32>(),
                                                 .inner_shadow_rect = inner_shadow_rect.to_type<f32>(),
                                             }));
}

void DisplayListRecorder::CommandEncoder::append_paint_scrollbar(u32 scroll_frame_id, Gfx::IntRect scrollbar_rect, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, double scroll_size, Gfx::Color thumb_color, Gfx::Color track_color, bool vertical)
{
    Gfx::Color adjusted_thumb_color = thumb_color;
    if (gutter_rect.is_empty() && thumb_color == CSS::InitialValues::scrollbar_color().thumb_color)
        adjusted_thumb_color = adjusted_thumb_color.with_alpha(128);
    MUST(append_wire_payload(m_append_bytes, PaintScrollBarCommand {
                                                 .scroll_frame_id = scroll_frame_id,
                                                 .scroll_size = static_cast<f32>(scroll_size),
                                                 .scrollbar_rect = scrollbar_rect.to_type<f32>(),
                                                 .gutter_rect = gutter_rect.to_type<f32>(),
                                                 .thumb_rect = thumb_rect.to_type<f32>(),
                                                 .thumb_color = adjusted_thumb_color,
                                                 .track_color = track_color,
                                                 .vertical = static_cast<u8>(vertical ? 1 : 0),
                                             }));
}

int command_nesting_level_change(DrawCommandView const& command)
{
    switch (command.type) {
    case CommandType::Save:
    case CommandType::SaveLayer:
    case CommandType::ApplyEffects:
        return 1;
    case CommandType::Restore:
        return -1;
    default:
        return 0;
    }
}

StringView dump_command_name(DrawCommandView const& command)
{
    return command_type_name(command.type);
}

void dump_command(StringBuilder& builder, DrawCommandView const& command)
{
    switch (command.type) {
    case CommandType::FillRect: {
        auto const fill_rect = MUST(decode_fixed_size_command<FillRectCommand>(command));
        builder.appendff(" rect={} color={}", fill_rect.rect.to_type<int>(), fill_rect.color);
        break;
    }
    case CommandType::DrawGlyphRun: {
        auto const draw_glyph_run = MUST(decode_variable_size_command<DrawGlyphRunCommand>(command));
        builder.appendff(" rect={} translation={} color={}", draw_glyph_run.text_rect.to_type<int>(), draw_glyph_run.translation, draw_glyph_run.color);
        break;
    }
    case CommandType::Translate: {
        auto const translate = MUST(decode_fixed_size_command<TranslateCommand>(command));
        builder.appendff(" delta={}", translate.delta.to_type<int>());
        break;
    }
    case CommandType::AddClipRect: {
        auto const add_clip_rect = MUST(decode_fixed_size_command<AddClipRectCommand>(command));
        builder.appendff(" rect={}", add_clip_rect.rect.to_type<int>());
        break;
    }
    case CommandType::FillRectWithRoundedCorners: {
        auto const fill = MUST(decode_fixed_size_command<FillRectWithRoundedCornersCommand>(command));
        builder.appendff(" rect={} color={}", fill.rect.to_type<int>(), fill.color);
        break;
    }
    case CommandType::AddRoundedRectClip: {
        auto const clip = MUST(decode_fixed_size_command<AddRoundedRectClipCommand>(command));
        builder.appendff(" rect={}", clip.border_rect.to_type<int>());
        break;
    }
    case CommandType::DrawScaledImage: {
        auto const draw = MUST(decode_fixed_size_command<DrawScaledImageCommand>(command));
        builder.appendff(" dst_rect={} clip_rect={}", draw.dst_rect.to_type<int>(), draw.clip_rect.to_type<int>());
        break;
    }
    case CommandType::DrawRepeatedImage: {
        auto const draw = MUST(decode_fixed_size_command<DrawRepeatedImageCommand>(command));
        builder.appendff(" dst_rect={} clip_rect={}", draw.dst_rect.to_type<int>(), draw.clip_rect.to_type<int>());
        break;
    }
    case CommandType::DrawExternalContent: {
        auto const draw = MUST(decode_fixed_size_command<DrawExternalContentCommand>(command));
        builder.appendff(" dst_rect={} clip_rect={}", draw.dst_rect.to_type<int>(), draw.clip_rect.to_type<int>());
        break;
    }
    case CommandType::DrawRect: {
        auto const draw_rect = MUST(decode_fixed_size_command<DrawRectCommand>(command));
        builder.appendff(" rect={} color={}", draw_rect.rect.to_type<int>(), draw_rect.color);
        break;
    }
    case CommandType::DrawLine: {
        auto const draw_line = MUST(decode_fixed_size_command<DrawLineCommand>(command));
        builder.appendff(" from={} to={} color={} thickness={}", draw_line.from.to_type<int>(), draw_line.to.to_type<int>(), draw_line.color, static_cast<int>(draw_line.thickness));
        break;
    }
    case CommandType::FillEllipse: {
        auto const fill = MUST(decode_fixed_size_command<FillEllipseCommand>(command));
        builder.appendff(" rect={} color={}", fill.rect.to_type<int>(), fill.color);
        break;
    }
    case CommandType::DrawEllipse: {
        auto const draw = MUST(decode_fixed_size_command<DrawEllipseCommand>(command));
        builder.appendff(" rect={} color={} thickness={}", draw.rect.to_type<int>(), draw.color, static_cast<int>(draw.thickness));
        break;
    }
    case CommandType::FillPath: {
        auto const fill = MUST(decode_variable_size_command<FillPathCommand>(command));
        builder.appendff(" path_bounding_rect={}", fill.path_bounding_rect.to_type<int>());
        break;
    }
    case CommandType::StrokePath:
        break;
    case CommandType::PaintTextShadow: {
        auto const shadow = MUST(decode_variable_size_command<PaintTextShadowCommand>(command));
        builder.appendff(" shadow_rect={} text_rect={} draw_location={} blur_radius={} color={}", shadow.visual_bounds.to_type<int>(), shadow.text_rect.to_type<int>(), shadow.translation, static_cast<int>(shadow.blur_radius), shadow.color);
        break;
    }
    case CommandType::PaintLinearGradient: {
        auto const gradient = MUST(decode_variable_size_command<PaintLinearGradientCommand>(command));
        builder.appendff(" rect={}", gradient.gradient_rect.to_type<int>());
        break;
    }
    case CommandType::PaintRadialGradient: {
        auto const gradient = MUST(decode_variable_size_command<PaintRadialGradientCommand>(command));
        builder.appendff(" rect={} center={} size={}", gradient.rect.to_type<int>(), gradient.center.to_type<int>(), gradient.size.to_type<int>());
        break;
    }
    case CommandType::PaintConicGradient: {
        auto const gradient = MUST(decode_variable_size_command<PaintConicGradientCommand>(command));
        builder.appendff(" rect={} position={} angle={}", gradient.rect.to_type<int>(), gradient.position.to_type<int>(), gradient.start_angle);
        break;
    }
    case CommandType::PaintOuterBoxShadow: {
        auto const shadow = MUST(decode_fixed_size_command<PaintOuterBoxShadowCommand>(command));
        builder.appendff(" content_rect={} shadow_rect={} blur_radius={} color={}", shadow.device_content_rect.to_type<int>(), shadow.shadow_rect.to_type<int>(), static_cast<int>(shadow.blur_radius), shadow.color);
        break;
    }
    case CommandType::PaintInnerBoxShadow: {
        auto const shadow = MUST(decode_fixed_size_command<PaintInnerBoxShadowCommand>(command));
        builder.appendff(" content_rect={} outer_shadow_rect={} inner_shadow_rect={} blur_radius={} color={}", shadow.device_content_rect.to_type<int>(), shadow.outer_shadow_rect.to_type<int>(), shadow.inner_shadow_rect.to_type<int>(), static_cast<int>(shadow.blur_radius), shadow.color);
        break;
    }
    case CommandType::PaintScrollBar:
        break;
    case CommandType::ApplyEffects: {
        auto const effects = MUST(decode_variable_size_command<ApplyEffectsCommand>(command));
        builder.appendff(" opacity={} has_filter={}", effects.opacity, effects.filter_byte_count != 0);
        break;
    }
    case CommandType::ApplyBackdropFilter: {
        auto const backdrop_filter = MUST(decode_variable_size_command<ApplyBackdropFilterCommand>(command));
        builder.appendff(" backdrop_region={}", backdrop_filter.backdrop_region.to_type<int>());
        break;
    }
    default:
        break;
    }
}

bool DisplayList::append_draw_command(ReadonlyBytes bytes, VisualContextIndex context_index)
{
    if (context_index.value() && m_visual_context_tree->has_empty_effective_clip(context_index))
        return false;

    u32 const command_offset = static_cast<u32>(m_command_stream.size());
    MUST(m_command_stream.try_append(bytes));
    m_commands.append({
        .context_index = context_index,
        .kind = ItemKind::DrawCommand,
        .command_offset = command_offset,
        .command_size = static_cast<u32>(bytes.size()),
    });
    return true;
}

bool DisplayList::append_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect, VisualContextIndex context_index)
{
    if (context_index.value() && m_visual_context_tree->has_empty_effective_clip(context_index))
        return false;

    u32 const nested_display_list_index = m_nested_display_lists.size();
    MUST(m_nested_display_lists.try_append({ move(display_list), rect }));
    m_commands.append({
        .context_index = context_index,
        .kind = ItemKind::NestedDisplayList,
        .nested_display_list_index = nested_display_list_index,
    });
    return true;
}

void DisplayList::append_external_content_source(NonnullRefPtr<ExternalContentSource> source)
{
    append_unique_external_content_source(m_external_content_sources, move(source));
}

ReadonlyBytes DisplayList::bytes_for(CommandListItem const& item) const
{
    VERIFY(item.kind == ItemKind::DrawCommand);
    return m_command_stream.bytes().slice(item.command_offset, item.command_size);
}

DisplayList::NestedDisplayListEntry const& DisplayList::nested_display_list_for(CommandListItem const& item) const
{
    VERIFY(item.kind == ItemKind::NestedDisplayList);
    return m_nested_display_lists[item.nested_display_list_index];
}

DisplayListRecorder::~DisplayListRecorder() = default;

static Optional<PaintServer::DrawCommandView> command_view_for(ReadonlyBytes bytes)
{
    PaintServer::Cursor cursor(bytes);
    auto command = MUST(cursor.next());
    if (!command.has_value())
        return {};
    return command.release_value();
}

DisplayListRecorder::DisplayListRecorder(DisplayList& display_list, PaintCommandEncodingContext& encoding_context)
    : m_display_list(&display_list)
    , m_encoding_context(&encoding_context)
    , m_command_writer([this](ReadonlyBytes bytes) -> ErrorOr<void> {
        return m_pending_command_bytes.try_append(bytes);
    })
    , m_command_encoder(make<CommandEncoder>(m_command_writer, encoding_context))
{
}

DisplayListRecorder::DisplayListRecorder(DisplayList& display_list, DisplayListRecorder& parent)
    : DisplayListRecorder(display_list, *parent.m_encoding_context)
{
}

DisplayListRecorder::CommandCapture::CommandCapture(DisplayListRecorder& recorder)
    : m_recorder(&recorder)
{
}

DisplayListRecorder::CommandCapture::~CommandCapture()
{
    if (m_recorder)
        m_recorder->end_capture();
}

CachedDisplayListCommands DisplayListRecorder::CommandCapture::take()
{
    VERIFY(m_recorder);
    auto commands = move(m_recorder->m_captured_commands);
    m_recorder->m_is_capturing = false;
    m_recorder = nullptr;
    return commands;
}

DisplayListRecorder::CommandCapture DisplayListRecorder::begin_command_capture()
{
    VERIFY(!m_is_capturing);
    m_is_capturing = true;
    m_captured_commands = {};
    return CommandCapture(*this);
}

void DisplayListRecorder::end_capture()
{
    m_is_capturing = false;
    m_captured_commands = {};
}

AccumulatedVisualContextTree const& DisplayListRecorder::visual_context_tree() const
{
    VERIFY(m_display_list);
    return m_display_list->visual_context_tree();
}

bool DisplayListRecorder::append_serialized_command(ReadonlyBytes command)
{
    VERIFY(m_display_list);
    if (visual_context_tree().has_empty_effective_clip(m_accumulated_visual_context_index))
        return false;

    auto command_view = command_view_for(command);
    if (command_view.has_value())
        m_save_nesting_level += command_nesting_level_change(*command_view);

    bool appended = m_display_list->append_draw_command(command, m_accumulated_visual_context_index);
    if (!appended)
        return false;
    if (m_is_capturing) {
        u32 const command_offset = static_cast<u32>(m_captured_commands.draw_commands.size());
        MUST(m_captured_commands.draw_commands.try_append_command(command));
        MUST(m_captured_commands.items.try_append({
            .kind = CachedDisplayListCommands::ItemKind::DrawCommand,
            .command_offset = command_offset,
            .command_size = static_cast<u32>(command.size()),
        }));
    }
    return true;
}

bool DisplayListRecorder::append_encoded_command(Function<ErrorOr<void>(CommandEncoder&)>&& callback)
{
    VERIFY(m_command_encoder);
    m_pending_command_bytes.clear();
    MUST(callback(*m_command_encoder));
    if (m_pending_command_bytes.is_empty())
        return false;
    return append_serialized_command(m_pending_command_bytes.bytes());
}

void DisplayListRecorder::append_external_content_source(NonnullRefPtr<ExternalContentSource> source)
{
    VERIFY(m_display_list);
    m_display_list->append_external_content_source(source);
    if (m_is_capturing) {
        for (auto const& existing_source : m_captured_commands.external_content_sources) {
            if (existing_source.ptr() == source.ptr())
                return;
        }
        m_captured_commands.external_content_sources.append(move(source));
    }
}

void DisplayListRecorder::append_nested_display_list_metadata(RefPtr<DisplayList> display_list, Gfx::IntRect rect)
{
    VERIFY(m_display_list);
    if (visual_context_tree().has_empty_effective_clip(m_accumulated_visual_context_index))
        return;
    if (display_list) {
        for (auto const& source : display_list->external_content_sources())
            append_external_content_source(source);
    }
    if (!m_display_list->append_nested_display_list(display_list, rect, m_accumulated_visual_context_index))
        return;
    if (m_is_capturing) {
        u32 const nested_display_list_index = m_captured_commands.nested_display_lists.size();
        MUST(m_captured_commands.nested_display_lists.try_append({ move(display_list), rect }));
        MUST(m_captured_commands.items.try_append({
            .kind = CachedDisplayListCommands::ItemKind::NestedDisplayList,
            .nested_display_list_index = nested_display_list_index,
        }));
    }
}

Optional<FlatPtr> DisplayListRecorder::current_source_context_namespace_id() const
{
    VERIFY(m_display_list);
    return m_display_list->source_context_namespace_id();
}

void DisplayListRecorder::replay_cached_commands(CachedDisplayListCommands const& commands)
{
    if (visual_context_tree().has_empty_effective_clip(m_accumulated_visual_context_index))
        return;
    for (auto const& source : commands.external_content_sources)
        append_external_content_source(source);
    for (auto const& item : commands.items) {
        if (item.kind == CachedDisplayListCommands::ItemKind::DrawCommand) {
            append_serialized_command(commands.draw_commands.bytes().slice(item.command_offset, item.command_size));
            continue;
        }
        auto const& nested = commands.nested_display_lists[item.nested_display_list_index];
        append_nested_display_list_metadata(nested.display_list, nested.rect);
    }
}

void DisplayListRecorder::paint_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect)
{
    if (display_list && !display_list->source_context_namespace_id().has_value()) {
        if (auto source_context_namespace_id = current_source_context_namespace_id(); source_context_namespace_id.has_value())
            display_list->set_source_context_namespace_id(source_context_namespace_id);
    }
    append_nested_display_list_metadata(move(display_list), rect);
}

void DisplayListRecorder::add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip)
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_add_rounded_rect_clip(corner_radii, border_rect, corner_clip);
        return {};
    });
}

void DisplayListRecorder::begin_masks(ReadonlySpan<MaskInfo> masks)
{
    for (auto const& mask : masks) {
        save();
        add_clip_rect(mask.rect);
        save_layer();
    }
}

void DisplayListRecorder::end_masks(ReadonlySpan<MaskInfo> masks)
{
    for (size_t index = masks.size(); index-- > 0;) {
        auto const& mask = masks[index];
        auto mask_kind = mask.kind == Gfx::MaskKind::Luminance ? Optional<Gfx::MaskKind>(Gfx::MaskKind::Luminance) : Optional<Gfx::MaskKind> {};
        apply_effects(1.0f, Gfx::CompositingAndBlendingOperator::DestinationIn, {}, mask_kind);
        paint_nested_display_list(mask.display_list, mask.rect);
        restore();
        restore();
        restore();
    }
}

void DisplayListRecorder::fill_rect(Gfx::IntRect const& rect, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_fill_rect(rect, color); return {}; });
}

void DisplayListRecorder::fill_rect_transparent(Gfx::IntRect const& rect)
{
    if (rect.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_fill_rect(rect, Color::Transparent); return {}; });
}

void DisplayListRecorder::fill_path(FillPathParams params)
{
    if (params.paint_style_or_color.has<Gfx::Color>() && params.paint_style_or_color.get<Gfx::Color>().alpha() == 0)
        return;
    auto path_bounding_int_rect = enclosing_int_rect(params.path.bounding_box());
    if (path_bounding_int_rect.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_fill_path(path_bounding_int_rect, params.path, params.opacity, params.paint_style_or_color, params.winding_rule, params.should_anti_alias);
        return {};
    });
}

void DisplayListRecorder::stroke_path(StrokePathParams params)
{
    if (params.thickness == 0.0f)
        return;
    if (params.paint_style_or_color.has<Gfx::Color>() && params.paint_style_or_color.get<Gfx::Color>().alpha() == 0)
        return;
    auto path_bounding_rect = params.path.bounding_box();
    path_bounding_rect.inflate(params.thickness, params.thickness);
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_stroke_path(params.cap_style, params.join_style, params.miter_limit, params.dash_array.span(), params.dash_offset, path_bounding_int_rect, params.path, params.opacity, params.paint_style_or_color, params.thickness, params.should_anti_alias);
        return {};
    });
}

void DisplayListRecorder::draw_ellipse(Gfx::IntRect const& rect, Color color, int thickness)
{
    if (rect.is_empty() || color.alpha() == 0 || !thickness)
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_draw_ellipse(rect, color, thickness); return {}; });
}

void DisplayListRecorder::fill_ellipse(Gfx::IntRect const& rect, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_fill_ellipse(rect, color); return {}; });
}

void DisplayListRecorder::fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data)
{
    if (gradient_rect.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_paint_linear_gradient(gradient_rect, data); return {}; });
}

void DisplayListRecorder::fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position)
{
    if (rect.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_paint_conic_gradient(rect, data, position); return {}; });
}

void DisplayListRecorder::fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size)
{
    if (rect.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_paint_radial_gradient(rect, data, center, size); return {}; });
}

void DisplayListRecorder::draw_rect(Gfx::IntRect const& rect, Color color, bool rough)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_draw_rect(rect, color, rough); return {}; });
}

void DisplayListRecorder::draw_external_content(Gfx::IntRect const& dst_rect, NonnullRefPtr<ExternalContentSource> source, Gfx::ScalingMode scaling_mode)
{
    draw_external_content(dst_rect, dst_rect, move(source), scaling_mode, false, false);
}

void DisplayListRecorder::draw_external_content(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, NonnullRefPtr<ExternalContentSource> source, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y)
{
    if (dst_rect.is_empty() || clip_rect.is_empty())
        return;
    if (append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
            serializer.append_draw_external_content(dst_rect, clip_rect, source, scaling_mode, repeat_x, repeat_y);
            return {};
        })) {
        append_external_content_source(source);
    }
}

void DisplayListRecorder::draw_scaled_decoded_image_frame(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::DecodedImageFrame const& frame, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_draw_scaled_decoded_image_frame(dst_rect, clip_rect, frame, scaling_mode);
        return {};
    });
}

void DisplayListRecorder::draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness, Gfx::LineStyle style, Color alternate_color)
{
    if (color.alpha() == 0 || !thickness)
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_draw_line(from, to, color, thickness, style, alternate_color); return {}; });
}

void DisplayListRecorder::draw_text(Gfx::IntRect const& rect, Utf16String const& raw_text, Gfx::Font const& font, Gfx::TextAlignment alignment, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    auto glyph_run = Gfx::shape_text({}, 0, raw_text.utf16_view(), font, Gfx::GlyphRun::TextType::Ltr);
    float baseline_x = 0;
    if (alignment == Gfx::TextAlignment::CenterLeft)
        baseline_x = static_cast<float>(rect.x());
    else if (alignment == Gfx::TextAlignment::Center)
        baseline_x = static_cast<float>(rect.x()) + ((static_cast<float>(rect.width()) - glyph_run->width()) / 2.0f);
    else if (alignment == Gfx::TextAlignment::CenterRight)
        baseline_x = static_cast<float>(rect.right()) - glyph_run->width();
    else
        TODO();
    auto const metrics = font.pixel_metrics();
    float baseline_y = static_cast<float>(rect.y()) + (metrics.ascent + ((static_cast<float>(rect.height()) - (metrics.ascent + metrics.descent)) / 2.0f));
    draw_glyph_run({ baseline_x, baseline_y }, *glyph_run, color, rect, 1.0, Orientation::Horizontal);
}

void DisplayListRecorder::draw_glyph_run(Gfx::FloatPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale, Orientation orientation)
{
    if (color.alpha() == 0)
        return;
    glyph_run.ensure_text_blob(static_cast<float>(scale));
    NonnullRefPtr<Gfx::GlyphRun const> glyph_run_ref { glyph_run };
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_glyph_run(glyph_run_ref, baseline_start, rect, color, orientation);
        return {};
    });
}

void DisplayListRecorder::add_clip_rect(Gfx::IntRect const& rect)
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_add_clip_rect(rect); return {}; });
}

void DisplayListRecorder::translate(Gfx::IntPoint delta)
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_translate(delta); return {}; });
}

void DisplayListRecorder::save()
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_save(); return {}; });
}

void DisplayListRecorder::save_layer()
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_save_layer(); return {}; });
}

void DisplayListRecorder::restore()
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> { serializer.append_restore(); return {}; });
}

void DisplayListRecorder::apply_backdrop_filter(Gfx::IntRect const& backdrop_region, CornerRadii const& corner_radii, Gfx::Filter const& backdrop_filter)
{
    if (backdrop_region.is_empty())
        return;
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        return serializer.append_apply_backdrop_filter_payload(backdrop_region, corner_radii, backdrop_filter);
    });
}

void DisplayListRecorder::paint_outer_box_shadow(Gfx::Color color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const& content_corner_radii, Gfx::IntRect shadow_rect, CornerRadii const& shadow_corner_radii)
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_paint_outer_box_shadow(color, blur_radius, device_content_rect, content_corner_radii, shadow_rect, shadow_corner_radii);
        return {};
    });
}

void DisplayListRecorder::paint_inner_box_shadow(Gfx::Color color, int blur_radius, Gfx::IntRect device_content_rect, CornerRadii const& content_corner_radii, Gfx::IntRect outer_shadow_rect, Gfx::IntRect inner_shadow_rect, CornerRadii const& inner_shadow_corner_radii)
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_paint_inner_box_shadow(color, blur_radius, device_content_rect, content_corner_radii, outer_shadow_rect, inner_shadow_rect, inner_shadow_corner_radii);
        return {};
    });
}

void DisplayListRecorder::paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const& glyph_run, double glyph_run_scale, Color color, Gfx::FloatPoint draw_location)
{
    glyph_run.ensure_text_blob(static_cast<float>(glyph_run_scale));
    NonnullRefPtr<Gfx::GlyphRun const> glyph_run_ref { glyph_run };
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_paint_text_shadow(glyph_run_ref, bounding_rect, text_rect, draw_location, blur_radius, color);
        return {};
    });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, CornerRadii const& corner_radii)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    if (!corner_radii.has_any_radius()) {
        fill_rect(rect, color);
        return;
    }
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_fill_rect_with_rounded_corners(rect, color, corner_radii);
        return {};
    });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, int radius)
{
    fill_rect_with_rounded_corners(rect, color, radius, radius, radius, radius);
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius)
{
    fill_rect_with_rounded_corners(rect, color, { { top_left_radius, top_left_radius }, { top_right_radius, top_right_radius }, { bottom_right_radius, bottom_right_radius }, { bottom_left_radius, bottom_left_radius } });
}

void DisplayListRecorder::paint_scrollbar(ScrollFrameIndex scroll_frame_index, Gfx::IntRect scrollbar_rect, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, double scroll_size, Color thumb_color, Color track_color, bool vertical)
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        serializer.append_paint_scrollbar(scroll_frame_index.value(), scrollbar_rect, gutter_rect, thumb_rect, scroll_size, thumb_color, track_color, vertical);
        return {};
    });
}

void DisplayListRecorder::apply_effects(float opacity, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Optional<Gfx::Filter> filter, Optional<Gfx::MaskKind> mask_kind)
{
    append_encoded_command([&](CommandEncoder& serializer) -> ErrorOr<void> {
        return serializer.append_apply_effects_payload(filter, opacity, compositing_and_blending_operator, mask_kind);
    });
}

}
