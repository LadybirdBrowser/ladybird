/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/SkiaUtils.h>
#include <LibCompositing/DisplayList/DisplayListCommandsGenerated.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorFilterType.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/PathData.h>
#include <LibGfx/RustFFI.h>
#include <core/SkBitmap.h>
#include <core/SkBlender.h>
#include <core/SkColorFilter.h>
#include <core/SkColorSpace.h>
#include <core/SkData.h>
#include <core/SkImage.h>
#include <core/SkImageFilter.h>
#include <core/SkString.h>
#include <effects/SkColorMatrix.h>
#include <effects/SkImageFilters.h>
#include <effects/SkPerlinNoiseShader.h>
#include <effects/SkRuntimeEffect.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#    include <gpu/ganesh/GrBackendSurface.h>
#    include <gpu/ganesh/GrDirectContext.h>
#    include <gpu/ganesh/SkImageGanesh.h>
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlTypes.h>
#    include <ports/SkCFObject.h>
#endif

namespace Gfx {

static_assert(to_underlying(PathVerb::Move) == to_underlying(SkPathVerb::kMove));
static_assert(to_underlying(PathVerb::Line) == to_underlying(SkPathVerb::kLine));
static_assert(to_underlying(PathVerb::Quad) == to_underlying(SkPathVerb::kQuad));
static_assert(to_underlying(PathVerb::Conic) == to_underlying(SkPathVerb::kConic));
static_assert(to_underlying(PathVerb::Cubic) == to_underlying(SkPathVerb::kCubic));
static_assert(to_underlying(PathVerb::Close) == to_underlying(SkPathVerb::kClose));
static_assert(sizeof(PathVerb) == sizeof(SkPathVerb));
static_assert(sizeof(FloatPoint) == sizeof(SkPoint));

SkMatrix to_skia_matrix(AffineTransform const& affine_transform)
{
    SkScalar affine[6];
    affine[0] = affine_transform.a();
    affine[1] = affine_transform.b();
    affine[2] = affine_transform.c();
    affine[3] = affine_transform.d();
    affine[4] = affine_transform.e();
    affine[5] = affine_transform.f();

    SkMatrix matrix;
    matrix.setAffine(affine);
    return matrix;
}

SkPath to_skia_path(PathData const& data)
{
    return SkPath::Raw(
        { reinterpret_cast<SkPoint const*>(data.points.data()), data.points.size() },
        { reinterpret_cast<SkPathVerb const*>(data.verbs.data()), data.verbs.size() },
        { data.conic_weights.data(), data.conic_weights.size() },
        to_skia_path_fill_type(data.winding_rule));
}

static SkColorChannel to_skia_color_channel(FFI::ChannelSelector channel)
{
    switch (channel) {
    case FFI::ChannelSelector::Red:
        return SkColorChannel::kR;
    case FFI::ChannelSelector::Green:
        return SkColorChannel::kG;
    case FFI::ChannelSelector::Blue:
        return SkColorChannel::kB;
    case FFI::ChannelSelector::Alpha:
        return SkColorChannel::kA;
    }
    VERIFY_NOT_REACHED();
}

sk_sp<SkImageFilter> to_skia_image_filter(Compositing::FilterGraph const& graph, Function<DecodedImageFrame const&(u64)> const& image_frame, Optional<FloatRect> blur_edge)
{
    using Tag = Compositing::FilterNode::Tag;

    Vector<sk_sp<SkImageFilter>, 8> filters;
    filters.ensure_capacity(graph.nodes.size());
    auto input = [&](u32 index) -> sk_sp<SkImageFilter> {
        if (index == Compositing::FILTER_SOURCE_GRAPHIC)
            return nullptr;
        return filters[index];
    };
    auto filter_of = [&](Compositing::FilterNode const& node) -> sk_sp<SkImageFilter> {
        switch (node.tag) {
        case Tag::Arithmetic: {
            auto const& arithmetic = node.arithmetic;
            static constexpr bool enforce_premultiplied_color = true;
            return SkImageFilters::Arithmetic(
                SkFloatToScalar(arithmetic.k1),
                SkFloatToScalar(arithmetic.k2),
                SkFloatToScalar(arithmetic.k3),
                SkFloatToScalar(arithmetic.k4),
                enforce_premultiplied_color,
                input(arithmetic.background),
                input(arithmetic.foreground));
        }
        case Tag::Compose:
            return SkImageFilters::Compose(input(node.compose.outer), input(node.compose.inner));
        case Tag::Blend:
            return SkImageFilters::Blend(to_skia_blender(node.blend.mode), input(node.blend.background), input(node.blend.foreground));
        case Tag::Flood: {
            auto color = SkColorSetA(to_skia_color(node.flood.color), static_cast<u8>(node.flood.opacity * 255));
            return SkImageFilters::Shader(SkShaders::Color(color));
        }
        case Tag::DisplacementMap: {
            auto const& displacement_map = node.displacement_map;
            return SkImageFilters::DisplacementMap(
                to_skia_color_channel(displacement_map.x_channel),
                to_skia_color_channel(displacement_map.y_channel),
                displacement_map.scale,
                input(displacement_map.displacement),
                input(displacement_map.color));
        }
        case Tag::DropShadow: {
            auto const& drop_shadow = node.drop_shadow;
            return SkImageFilters::DropShadow(drop_shadow.offset_x, drop_shadow.offset_y, drop_shadow.radius, drop_shadow.radius, to_skia_color(drop_shadow.color), input(drop_shadow.input));
        }
        case Tag::Blur: {
            auto const& blur = node.blur;
            auto blurred = input(blur.input);
            if (blur.mirrors_at_backdrop_edge && blur_edge.has_value())
                blurred = SkImageFilters::Crop(to_skia_rect(*blur_edge), SkTileMode::kMirror, move(blurred));
            return SkImageFilters::Blur(blur.radius_x, blur.radius_y, blurred);
        }
        case Tag::ColorMatrix: {
            auto const& color_matrix = node.color_matrix;
            auto clamp = color_matrix.clamps_to_unit_range ? SkColorFilters::Clamp::kYes : SkColorFilters::Clamp::kNo;
            return SkImageFilters::ColorFilter(SkColorFilters::Matrix(color_matrix.matrix, clamp), input(color_matrix.input));
        }
        case Tag::ColorTable: {
            // NB: The color space in which the table is applied is determined by the color-interpolation-filters
            //     property and handled by the filter graph, so the table is applied directly here.
            auto const& color_table = node.color_table;
            return SkImageFilters::ColorFilter(SkColorFilters::TableARGB(color_table.alpha, color_table.red, color_table.green, color_table.blue), input(color_table.input));
        }
        case Tag::Saturate: {
            SkColorMatrix matrix;
            matrix.setSaturation(node.saturate.value);
            return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix), input(node.saturate.input));
        }
        case Tag::Image: {
            auto const& image = node.image;
            auto const& frame = image_frame(image.frame_id);
            return SkImageFilters::Image(sk_image_from_decoded_image_frame(frame), to_skia_rect(image.src_rect), to_skia_rect(image.dest_rect), to_skia_sampling_options(image.scaling_mode));
        }
        case Tag::Merge: {
            auto merge_inputs = graph.merge_inputs.slice(node.merge.first_input, node.merge.input_count);
            Vector<sk_sp<SkImageFilter>> merged;
            merged.ensure_capacity(merge_inputs.size());
            for (auto merge_input : merge_inputs)
                merged.unchecked_append(input(merge_input));
            return SkImageFilters::Merge(merged.data(), merged.size());
        }
        case Tag::Offset:
            return SkImageFilters::Offset(node.offset.dx, node.offset.dy, input(node.offset.input));
        case Tag::Erode:
            return SkImageFilters::Erode(node.erode.radius_x, node.erode.radius_y, input(node.erode.input));
        case Tag::Dilate:
            return SkImageFilters::Dilate(node.dilate.radius_x, node.dilate.radius_y, input(node.dilate.input));
        case Tag::Turbulence: {
            auto const& turbulence = node.turbulence;
            auto tile_stitch_size = SkISize::Make(turbulence.tile_stitch_size.width(), turbulence.tile_stitch_size.height());
            sk_sp<SkShader> turbulence_shader = [&] {
                switch (turbulence.kind) {
                case FFI::TurbulenceType::Turbulence:
                    return SkShaders::MakeTurbulence(turbulence.base_frequency_x, turbulence.base_frequency_y, turbulence.num_octaves, turbulence.seed, &tile_stitch_size);
                case FFI::TurbulenceType::FractalNoise:
                    return SkShaders::MakeFractalNoise(turbulence.base_frequency_x, turbulence.base_frequency_y, turbulence.num_octaves, turbulence.seed, &tile_stitch_size);
                }
                VERIFY_NOT_REACHED();
            }();
            return SkImageFilters::Shader(move(turbulence_shader));
        }
        case Tag::ColorSpaceConversion: {
            auto const& conversion = node.color_space_conversion;
            auto converted = input(conversion.input);
            if (conversion.source == conversion.destination)
                return converted;
            sk_sp<SkColorFilter> color_space_filter;
            switch (conversion.destination) {
            case InterpolationColorSpace::LinearRGB:
                color_space_filter = SkColorFilters::SRGBToLinearGamma();
                break;
            case InterpolationColorSpace::SRGB:
                color_space_filter = SkColorFilters::LinearToSRGBGamma();
                break;
            }
            return SkImageFilters::ColorFilter(move(color_space_filter), move(converted));
        }
        }
        VERIFY_NOT_REACHED();
    };
    for (auto const& node : graph.nodes)
        filters.unchecked_append(filter_of(node));
    if (filters.is_empty())
        return nullptr;
    return filters.last();
}

static sk_sp<SkImage> sk_image_borrowing_pixels(IntSize size, BitmapFormat format, AlphaType alpha_type, ColorSpace const& color_space, void const* pixels, size_t pitch)
{
    auto info = SkImageInfo::Make(size.width(), size.height(), to_skia_color_type(format), to_skia_alpha_type(format, alpha_type), to_skia_color_space(color_space));
    SkBitmap sk_bitmap;
    sk_bitmap.installPixels(info, const_cast<void*>(pixels), pitch);
    sk_bitmap.setImmutable();
    return sk_bitmap.asImage();
}

sk_sp<SkImage> sk_image_from_bitmap(Bitmap const& bitmap, ColorSpace const& color_space)
{
    return sk_image_borrowing_pixels(bitmap.size(), bitmap.format(), bitmap.alpha_type(), color_space, bitmap.scanline(0), bitmap.pitch());
}

sk_sp<SkImage> sk_image_from_decoded_image_frame(DecodedImageFrame const& frame)
{
    return sk_image_borrowing_pixels(frame.size(), frame.format(), frame.alpha_type(), frame.color_space(), frame.pixels().data(), frame.pitch());
}

sk_sp<SkImage> sk_texture_image_sharing_pixels_of_decoded_image_frame([[maybe_unused]] DecodedImageFrame const& frame, [[maybe_unused]] SkiaBackendContext& skia_backend_context)
{
#ifdef AK_OS_MACOS
    auto const* iosurface = frame.iosurface();
    auto* gr_context = skia_backend_context.sk_context();
    if (!iosurface || !gr_context)
        return nullptr;

    auto texture_format = first_is_one_of(frame.format(), BitmapFormat::RGBA8888, BitmapFormat::RGBx8888) ? MetalTextureFormat::RGBA8 : MetalTextureFormat::BGRA8;
    auto metal_texture = skia_backend_context.metal_context().create_texture_from_iosurface(*iosurface, texture_format, 0);
    if (!metal_texture)
        return nullptr;

    // NB: The backend texture retains the Metal texture, which retains the surface.
    GrMtlTextureInfo texture_info;
    texture_info.fTexture = sk_ret_cfp(metal_texture->texture());
    auto backend_texture = GrBackendTextures::MakeMtl(frame.width(), frame.height(), skgpu::Mipmapped::kNo, texture_info);
    return SkImages::BorrowTextureFrom(gr_context, backend_texture, kTopLeft_GrSurfaceOrigin,
        to_skia_color_type(frame.format()), to_skia_alpha_type(frame.format(), frame.alpha_type()), to_skia_color_space(frame.color_space()));
#else
    return nullptr;
#endif
}

sk_sp<SkImage> sk_image_adopting_bitmap(NonnullRefPtr<Bitmap> bitmap, ColorSpace const& color_space)
{
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->format(), bitmap->alpha_type()), to_skia_color_space(color_space));
    auto row_bytes = bitmap->pitch();
    auto byte_count = bitmap->size_in_bytes();
    auto* pixels = bitmap->scanline_u8(0);
    auto data = SkData::MakeWithProc(pixels, byte_count, [](void const*, void* context) { static_cast<Bitmap*>(context)->unref(); }, &bitmap.leak_ref());
    return SkImages::RasterFromData(info, move(data), row_bytes);
}

sk_sp<SkBlender> to_skia_blender(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    switch (compositing_and_blending_operator) {
    case CompositingAndBlendingOperator::Normal:
        return SkBlender::Mode(SkBlendMode::kSrcOver);
    case CompositingAndBlendingOperator::Multiply:
        return SkBlender::Mode(SkBlendMode::kMultiply);
    case CompositingAndBlendingOperator::Screen:
        return SkBlender::Mode(SkBlendMode::kScreen);
    case CompositingAndBlendingOperator::Overlay:
        return SkBlender::Mode(SkBlendMode::kOverlay);
    case CompositingAndBlendingOperator::Darken:
        return SkBlender::Mode(SkBlendMode::kDarken);
    case CompositingAndBlendingOperator::Lighten:
        return SkBlender::Mode(SkBlendMode::kLighten);
    case CompositingAndBlendingOperator::ColorDodge:
        return SkBlender::Mode(SkBlendMode::kColorDodge);
    case CompositingAndBlendingOperator::ColorBurn:
        return SkBlender::Mode(SkBlendMode::kColorBurn);
    case CompositingAndBlendingOperator::HardLight:
        return SkBlender::Mode(SkBlendMode::kHardLight);
    case CompositingAndBlendingOperator::SoftLight:
        return SkBlender::Mode(SkBlendMode::kSoftLight);
    case CompositingAndBlendingOperator::Difference:
        return SkBlender::Mode(SkBlendMode::kDifference);
    case CompositingAndBlendingOperator::Exclusion:
        return SkBlender::Mode(SkBlendMode::kExclusion);
    case CompositingAndBlendingOperator::Hue:
        return SkBlender::Mode(SkBlendMode::kHue);
    case CompositingAndBlendingOperator::Saturation:
        return SkBlender::Mode(SkBlendMode::kSaturation);
    case CompositingAndBlendingOperator::Color:
        return SkBlender::Mode(SkBlendMode::kColor);
    case CompositingAndBlendingOperator::Luminosity:
        return SkBlender::Mode(SkBlendMode::kLuminosity);
    case CompositingAndBlendingOperator::Clear:
        return SkBlender::Mode(SkBlendMode::kClear);
    case CompositingAndBlendingOperator::Copy:
        return SkBlender::Mode(SkBlendMode::kSrc);
    case CompositingAndBlendingOperator::SourceOver:
        return SkBlender::Mode(SkBlendMode::kSrcOver);
    case CompositingAndBlendingOperator::DestinationOver:
        return SkBlender::Mode(SkBlendMode::kDstOver);
    case CompositingAndBlendingOperator::SourceIn:
        return SkBlender::Mode(SkBlendMode::kSrcIn);
    case CompositingAndBlendingOperator::DestinationIn:
        return SkBlender::Mode(SkBlendMode::kDstIn);
    case CompositingAndBlendingOperator::SourceOut:
        return SkBlender::Mode(SkBlendMode::kSrcOut);
    case CompositingAndBlendingOperator::DestinationOut:
        return SkBlender::Mode(SkBlendMode::kDstOut);
    case CompositingAndBlendingOperator::SourceATop:
        return SkBlender::Mode(SkBlendMode::kSrcATop);
    case CompositingAndBlendingOperator::DestinationATop:
        return SkBlender::Mode(SkBlendMode::kDstATop);
    case CompositingAndBlendingOperator::Xor:
        return SkBlender::Mode(SkBlendMode::kXor);
    case CompositingAndBlendingOperator::Lighter:
        return SkBlender::Mode(SkBlendMode::kPlus);
    case CompositingAndBlendingOperator::PlusDarker:
        // https://drafts.fxtf.org/compositing/#porterduffcompositingoperators_plus_darker
        // FIXME: This does not match the spec, however it looks like Safari, the only popular browser supporting this operator.
        return SkRuntimeEffect::MakeForBlender(SkString(R"(
            vec4 main(vec4 source, vec4 destination) {
                return saturate(saturate(destination.a + source.a) - saturate(destination.a - destination) - saturate(source.a - source));
            }
        )"))
            .effect->makeBlender(nullptr);
    case CompositingAndBlendingOperator::PlusLighter:
        // https://drafts.fxtf.org/compositing/#porterduffcompositingoperators_plus_lighter
        return SkRuntimeEffect::MakeForBlender(SkString(R"(
            vec4 main(vec4 source, vec4 destination) {
                return saturate(source + destination);
            }
        )"))
            .effect->makeBlender(nullptr);
    default:
        VERIFY_NOT_REACHED();
    }
}

}
