/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <core/SkBitmap.h>
#include <core/SkBlurTypes.h>
#include <core/SkCanvas.h>
#include <core/SkColorFilter.h>
#include <core/SkMaskFilter.h>
#include <core/SkPath.h>
#include <core/SkPathBuilder.h>
#include <core/SkPathEffect.h>
#include <core/SkRRect.h>
#include <core/SkSurface.h>
#include <effects/SkDashPathEffect.h>
#include <effects/SkGradientShader.h>
#include <effects/SkImageFilters.h>
#include <gpu/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <pathops/SkPathOps.h>

#include <LibGfx/Filters/StackBlurFilter.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/ShadowPainting.h>

#ifdef USE_VULKAN
#    include <gpu/ganesh/vk/GrVkDirectContext.h>
#    include <gpu/vk/GrVkBackendContext.h>
#    include <gpu/vk/VulkanBackendContext.h>
#    include <gpu/vk/VulkanExtensions.h>
#endif

#ifdef AK_OS_MACOS
#    define FixedPoint FixedPointMacOS
#    define Duration DurationMacOS
#    include <gpu/GrBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlBackendContext.h>
#    include <gpu/ganesh/mtl/GrMtlDirectContext.h>
#    undef FixedPoint
#    undef Duration
#endif

namespace Web::Painting {

class DisplayListPlayerSkia::SkiaSurface {
public:
    SkCanvas& canvas() const { return *m_surface->getCanvas(); }

    SkiaSurface(sk_sp<SkSurface> surface)
        : m_surface(move(surface))
    {
    }

    void read_into_bitmap(Gfx::Bitmap& bitmap)
    {
        auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        SkPixmap pixmap(image_info, bitmap.begin(), bitmap.pitch());
        m_surface->readPixels(pixmap, 0, 0);
    }

private:
    sk_sp<SkSurface> m_surface;
};

#ifdef USE_VULKAN
class SkiaVulkanBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaVulkanBackendContext);
    AK_MAKE_NONMOVABLE(SkiaVulkanBackendContext);

public:
    SkiaVulkanBackendContext(sk_sp<GrDirectContext> context, NonnullOwnPtr<skgpu::VulkanExtensions> extensions)
        : m_context(move(context))
        , m_extensions(move(extensions))
    {
    }

    ~SkiaVulkanBackendContext() override {};

    void flush_and_submit() override
    {
        m_context->flush();
        m_context->submit(GrSyncCpu::kYes);
    }

    sk_sp<SkSurface> create_surface(int width, int height)
    {
        auto image_info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        return SkSurfaces::RenderTarget(m_context.get(), skgpu::Budgeted::kYes, image_info);
    }

    skgpu::VulkanExtensions const* extensions() const { return m_extensions.ptr(); }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullOwnPtr<skgpu::VulkanExtensions> m_extensions;
};

OwnPtr<SkiaBackendContext> DisplayListPlayerSkia::create_vulkan_context(Core::VulkanContext& vulkan_context)
{
    GrVkBackendContext backend_context;

    backend_context.fInstance = vulkan_context.instance;
    backend_context.fDevice = vulkan_context.logical_device;
    backend_context.fQueue = vulkan_context.graphics_queue;
    backend_context.fPhysicalDevice = vulkan_context.physical_device;
    backend_context.fMaxAPIVersion = vulkan_context.api_version;
    backend_context.fGetProc = [](char const* proc_name, VkInstance instance, VkDevice device) {
        if (device != VK_NULL_HANDLE) {
            return vkGetDeviceProcAddr(device, proc_name);
        }
        return vkGetInstanceProcAddr(instance, proc_name);
    };

    auto extensions = make<skgpu::VulkanExtensions>();
    backend_context.fVkExtensions = extensions.ptr();

    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeVulkan(backend_context);
    VERIFY(ctx);
    return make<SkiaVulkanBackendContext>(ctx, move(extensions));
}

DisplayListPlayerSkia::DisplayListPlayerSkia(SkiaBackendContext& context, Gfx::Bitmap& bitmap)
{
    VERIFY(bitmap.format() == Gfx::BitmapFormat::BGRA8888);
    auto surface = static_cast<SkiaVulkanBackendContext&>(context).create_surface(bitmap.width(), bitmap.height());
    m_surface = make<SkiaSurface>(surface);
    m_flush_context = [&bitmap, &surface = m_surface, &context] {
        context.flush_and_submit();
        surface->read_into_bitmap(bitmap);
    };
}
#endif

#ifdef AK_OS_MACOS
class SkiaMetalBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaMetalBackendContext);
    AK_MAKE_NONMOVABLE(SkiaMetalBackendContext);

public:
    SkiaMetalBackendContext(sk_sp<GrDirectContext> context)
        : m_context(move(context))
    {
    }

    ~SkiaMetalBackendContext() override {};

    sk_sp<SkSurface> wrap_metal_texture(Core::MetalTexture& metal_texture)
    {
        GrMtlTextureInfo mtl_info;
        mtl_info.fTexture = sk_ret_cfp(metal_texture.texture());
        auto backend_render_target = GrBackendRenderTarget(metal_texture.width(), metal_texture.height(), mtl_info);
        return SkSurfaces::WrapBackendRenderTarget(m_context.get(), backend_render_target, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, nullptr, nullptr);
    }

    void flush_and_submit() override
    {
        m_context->flush();
        m_context->submit(GrSyncCpu::kYes);
    }

private:
    sk_sp<GrDirectContext> m_context;
};

OwnPtr<SkiaBackendContext> DisplayListPlayerSkia::create_metal_context(Core::MetalContext const& metal_context)
{
    GrMtlBackendContext backend_context;
    backend_context.fDevice.retain((GrMTLHandle)metal_context.device());
    backend_context.fQueue.retain((GrMTLHandle)metal_context.queue());
    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeMetal(backend_context);
    return make<SkiaMetalBackendContext>(ctx);
}

DisplayListPlayerSkia::DisplayListPlayerSkia(SkiaBackendContext& context, Core::MetalTexture& metal_texture)
{
    auto image_info = SkImageInfo::Make(metal_texture.width(), metal_texture.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    VERIFY(is<SkiaMetalBackendContext>(context));
    auto surface = static_cast<SkiaMetalBackendContext&>(context).wrap_metal_texture(metal_texture);
    if (!surface) {
        dbgln("Failed to create Skia surface from Metal texture");
        VERIFY_NOT_REACHED();
    }
    m_surface = make<SkiaSurface>(surface);
    m_flush_context = [&context] mutable {
        context.flush_and_submit();
    };
}
#endif

DisplayListPlayerSkia::DisplayListPlayerSkia(Gfx::Bitmap& bitmap)
{
    VERIFY(bitmap.format() == Gfx::BitmapFormat::BGRA8888);
    auto image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    auto surface = SkSurfaces::WrapPixels(image_info, bitmap.begin(), bitmap.pitch());
    VERIFY(surface);
    m_surface = make<SkiaSurface>(surface);
}

DisplayListPlayerSkia::~DisplayListPlayerSkia()
{
    if (m_flush_context)
        m_flush_context();
}

static SkPoint to_skia_point(auto const& point)
{
    return SkPoint::Make(point.x(), point.y());
}

static SkRect to_skia_rect(auto const& rect)
{
    return SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
}

static SkColor to_skia_color(Gfx::Color const& color)
{
    return SkColorSetARGB(color.alpha(), color.red(), color.green(), color.blue());
}

static SkPath to_skia_path(Gfx::Path const& path)
{
    Optional<Gfx::FloatPoint> subpath_start_point;
    Optional<Gfx::FloatPoint> subpath_last_point;
    SkPathBuilder path_builder;
    auto close_subpath_if_needed = [&](auto last_point) {
        if (subpath_start_point == last_point)
            path_builder.close();
    };
    for (auto const& segment : path) {
        auto point = segment.point();
        switch (segment.command()) {
        case Gfx::PathSegment::Command::MoveTo: {
            if (subpath_start_point.has_value() && subpath_last_point.has_value())
                close_subpath_if_needed(subpath_last_point.value());
            subpath_start_point = point;
            path_builder.moveTo({ point.x(), point.y() });
            break;
        }
        case Gfx::PathSegment::Command::LineTo: {
            if (!subpath_start_point.has_value())
                subpath_start_point = Gfx::FloatPoint { 0.0f, 0.0f };
            path_builder.lineTo({ point.x(), point.y() });
            break;
        }
        case Gfx::PathSegment::Command::QuadraticBezierCurveTo: {
            if (!subpath_start_point.has_value())
                subpath_start_point = Gfx::FloatPoint { 0.0f, 0.0f };
            SkPoint pt1 = { segment.through().x(), segment.through().y() };
            SkPoint pt2 = { segment.point().x(), segment.point().y() };
            path_builder.quadTo(pt1, pt2);
            break;
        }
        case Gfx::PathSegment::Command::CubicBezierCurveTo: {
            if (!subpath_start_point.has_value())
                subpath_start_point = Gfx::FloatPoint { 0.0f, 0.0f };
            SkPoint pt1 = { segment.through_0().x(), segment.through_0().y() };
            SkPoint pt2 = { segment.through_1().x(), segment.through_1().y() };
            SkPoint pt3 = { segment.point().x(), segment.point().y() };
            path_builder.cubicTo(pt1, pt2, pt3);
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
        subpath_last_point = point;
    }

    close_subpath_if_needed(subpath_last_point);

    return path_builder.snapshot();
}

static SkPathFillType to_skia_path_fill_type(Gfx::WindingRule winding_rule)
{
    switch (winding_rule) {
    case Gfx::WindingRule::Nonzero:
        return SkPathFillType::kWinding;
    case Gfx::WindingRule::EvenOdd:
        return SkPathFillType::kEvenOdd;
    }
    VERIFY_NOT_REACHED();
}

static SkRRect to_skia_rrect(auto const& rect, CornerRadii const& corner_radii)
{
    SkRRect rrect;
    SkVector radii[4];
    radii[0].set(corner_radii.top_left.horizontal_radius, corner_radii.top_left.vertical_radius);
    radii[1].set(corner_radii.top_right.horizontal_radius, corner_radii.top_right.vertical_radius);
    radii[2].set(corner_radii.bottom_right.horizontal_radius, corner_radii.bottom_right.vertical_radius);
    radii[3].set(corner_radii.bottom_left.horizontal_radius, corner_radii.bottom_left.vertical_radius);
    rrect.setRectRadii(to_skia_rect(rect), radii);
    return rrect;
}

static SkColorType to_skia_color_type(Gfx::BitmapFormat format)
{
    switch (format) {
    case Gfx::BitmapFormat::Invalid:
        return kUnknown_SkColorType;
    case Gfx::BitmapFormat::BGRA8888:
    case Gfx::BitmapFormat::BGRx8888:
        return kBGRA_8888_SkColorType;
    case Gfx::BitmapFormat::RGBA8888:
        return kRGBA_8888_SkColorType;
    default:
        return kUnknown_SkColorType;
    }
}

static SkBitmap to_skia_bitmap(Gfx::Bitmap const& bitmap)
{
    SkColorType color_type = to_skia_color_type(bitmap.format());
    SkImageInfo image_info = SkImageInfo::Make(bitmap.width(), bitmap.height(), color_type, kUnpremul_SkAlphaType);
    SkBitmap sk_bitmap;
    sk_bitmap.setInfo(image_info);

    if (!sk_bitmap.installPixels(image_info, const_cast<Gfx::ARGB32*>(bitmap.begin()), bitmap.width() * 4)) {
        VERIFY_NOT_REACHED();
    }

    sk_bitmap.setImmutable();

    return sk_bitmap;
}

static SkMatrix to_skia_matrix(Gfx::AffineTransform const& affine_transform)
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

static SkSamplingOptions to_skia_sampling_options(Gfx::ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case Gfx::ScalingMode::NearestNeighbor:
        return SkSamplingOptions(SkFilterMode::kNearest);
    case Gfx::ScalingMode::BilinearBlend:
    case Gfx::ScalingMode::SmoothPixels:
        return SkSamplingOptions(SkFilterMode::kLinear);
    case Gfx::ScalingMode::BoxSampling:
        return SkSamplingOptions(SkCubicResampler::Mitchell());
    default:
        VERIFY_NOT_REACHED();
    }
}

#define APPLY_PATH_CLIP_IF_NEEDED                     \
    ScopeGuard restore_path_clip { [&] {              \
        if (command.clip_paths.size() > 0)            \
            surface().canvas().restore();             \
    } };                                              \
    if (command.clip_paths.size() > 0) {              \
        surface().canvas().save();                    \
        SkPath clip_path;                             \
        for (auto const& path : command.clip_paths)   \
            clip_path.addPath(to_skia_path(path));    \
        surface().canvas().clipPath(clip_path, true); \
    }

DisplayListPlayerSkia::SkiaSurface& DisplayListPlayerSkia::surface() const
{
    return static_cast<SkiaSurface&>(*m_surface);
}

CommandResult DisplayListPlayerSkia::draw_glyph_run(DrawGlyphRun const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColorFilter(SkColorFilters::Blend(to_skia_color(command.color), SkBlendMode::kSrcIn));
    auto const& glyphs = command.glyph_run->glyphs();
    auto const& font = command.glyph_run->font();
    auto scaled_font = font.with_size(font.point_size() * static_cast<float>(command.scale));
    for (auto const& glyph_or_emoji : glyphs) {
        auto transformed_glyph = glyph_or_emoji;
        transformed_glyph.visit([&](auto& glyph) {
            glyph.position = glyph.position.scaled(command.scale).translated(command.translation);
        });
        if (transformed_glyph.has<Gfx::DrawGlyph>()) {
            auto& glyph = transformed_glyph.get<Gfx::DrawGlyph>();
            auto const& point = glyph.position;
            auto const& code_point = glyph.code_point;
            auto top_left = point + Gfx::FloatPoint(scaled_font->glyph_left_bearing(code_point), 0);
            auto glyph_position = Gfx::GlyphRasterPosition::get_nearest_fit_for(top_left);
            auto maybe_font_glyph = scaled_font->glyph(code_point, glyph_position.subpixel_offset);
            if (!maybe_font_glyph.has_value())
                continue;
            if (maybe_font_glyph->is_color_bitmap()) {
                TODO();
            } else {
                auto sk_bitmap = to_skia_bitmap(*maybe_font_glyph->bitmap());
                auto sk_image = SkImages::RasterFromBitmap(sk_bitmap);
                auto const& blit_position = glyph_position.blit_position;
                canvas.drawImage(sk_image, blit_position.x(), blit_position.y(), SkSamplingOptions(), &paint);
            }
        }
    }
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::fill_rect(FillRect const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::draw_scaled_bitmap(DrawScaledBitmap const& command)
{
    auto src_rect = to_skia_rect(command.src_rect);
    auto dst_rect = to_skia_rect(command.dst_rect);
    auto bitmap = to_skia_bitmap(command.bitmap);
    auto image = SkImages::RasterFromBitmap(bitmap);
    auto& canvas = surface().canvas();
    SkPaint paint;
    canvas.drawImageRect(image, src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::draw_scaled_immutable_bitmap(DrawScaledImmutableBitmap const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto src_rect = to_skia_rect(command.src_rect);
    auto dst_rect = to_skia_rect(command.dst_rect);
    auto bitmap = to_skia_bitmap(command.bitmap->bitmap());
    auto image = SkImages::RasterFromBitmap(bitmap);
    auto& canvas = surface().canvas();
    SkPaint paint;
    canvas.drawImageRect(image, src_rect, dst_rect, to_skia_sampling_options(command.scaling_mode), &paint, SkCanvas::kStrict_SrcRectConstraint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::add_clip_rect(AddClipRect const& command)
{
    auto& canvas = surface().canvas();
    auto const& rect = command.rect;
    canvas.clipRect(to_skia_rect(rect));
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::save(Save const&)
{
    auto& canvas = surface().canvas();
    canvas.save();
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::restore(Restore const&)
{
    auto& canvas = surface().canvas();
    canvas.restore();
    return CommandResult::Continue;
}

static SkBitmap alpha_mask_from_bitmap(Gfx::Bitmap const& bitmap, Gfx::Bitmap::MaskKind kind)
{
    SkBitmap alpha_mask;
    alpha_mask.allocPixels(SkImageInfo::MakeA8(bitmap.width(), bitmap.height()));
    for (int y = 0; y < bitmap.height(); y++) {
        for (int x = 0; x < bitmap.width(); x++) {
            if (kind == Gfx::Bitmap::MaskKind::Luminance) {
                auto color = bitmap.get_pixel(x, y);
                *alpha_mask.getAddr8(x, y) = color.alpha() * color.luminosity() / 255;
            } else {
                VERIFY(kind == Gfx::Bitmap::MaskKind::Alpha);
                auto color = bitmap.get_pixel(x, y);
                *alpha_mask.getAddr8(x, y) = color.alpha();
            }
        }
    }
    return alpha_mask;
}

CommandResult DisplayListPlayerSkia::push_stacking_context(PushStackingContext const& command)
{
    auto& canvas = surface().canvas();

    auto affine_transform = Gfx::extract_2d_affine_transform(command.transform.matrix);
    auto new_transform = Gfx::AffineTransform {}
                             .set_translation(command.post_transform_translation.to_type<float>())
                             .translate(command.transform.origin)
                             .multiply(affine_transform)
                             .translate(-command.transform.origin);
    auto matrix = to_skia_matrix(new_transform);

    if (command.opacity < 1) {
        auto source_paintable_rect = to_skia_rect(command.source_paintable_rect);
        SkRect dest;
        matrix.mapRect(&dest, source_paintable_rect);
        canvas.saveLayerAlphaf(&dest, command.opacity);
    } else {
        canvas.save();
    }

    if (command.mask.has_value()) {
        auto alpha_mask = alpha_mask_from_bitmap(*command.mask.value().mask_bitmap, command.mask.value().mask_kind);
        SkMatrix mask_matrix;
        auto mask_position = command.source_paintable_rect.location();
        mask_matrix.setTranslate(mask_position.x(), mask_position.y());
        auto shader = alpha_mask.makeShader(SkSamplingOptions(), mask_matrix);
        canvas.clipShader(shader);
    }

    if (command.is_fixed_position) {
        // FIXME: Resetting matrix is not correct when element is nested in a transformed stacking context
        canvas.resetMatrix();
    }
    canvas.concat(matrix);

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::pop_stacking_context(PopStackingContext const&)
{
    surface().canvas().restore();
    return CommandResult::Continue;
}

static ColorStopList replace_transition_hints_with_normal_color_stops(ColorStopList const& color_stop_list)
{
    ColorStopList stops_with_replaced_transition_hints;

    auto const& first_color_stop = color_stop_list.first();
    // First color stop in the list should never have transition hint value
    VERIFY(!first_color_stop.transition_hint.has_value());
    stops_with_replaced_transition_hints.empend(first_color_stop.color, first_color_stop.position);

    // This loop replaces transition hints with five regular points, calculated using the
    // formula defined in the spec. After rendering using linear interpolation, this will
    // produce a result close enough to that obtained if the color of each point were calculated
    // using the non-linear formula from the spec.
    for (size_t i = 1; i < color_stop_list.size(); i++) {
        auto const& color_stop = color_stop_list[i];
        if (!color_stop.transition_hint.has_value()) {
            stops_with_replaced_transition_hints.empend(color_stop.color, color_stop.position);
            continue;
        }

        auto const& previous_color_stop = color_stop_list[i - 1];
        auto const& next_color_stop = color_stop_list[i];

        auto distance_between_stops = next_color_stop.position - previous_color_stop.position;
        auto transition_hint = color_stop.transition_hint.value();

        Array<float, 5> const transition_hint_relative_sampling_positions = {
            transition_hint * 0.33f,
            transition_hint * 0.66f,
            transition_hint,
            transition_hint + (1 - transition_hint) * 0.33f,
            transition_hint + (1 - transition_hint) * 0.66f,
        };

        for (auto const& transition_hint_relative_sampling_position : transition_hint_relative_sampling_positions) {
            auto position = previous_color_stop.position + transition_hint_relative_sampling_position * distance_between_stops;
            auto value = color_stop_step(previous_color_stop, next_color_stop, position);
            auto color = previous_color_stop.color.interpolate(next_color_stop.color, value);
            stops_with_replaced_transition_hints.empend(color, position);
        }

        stops_with_replaced_transition_hints.empend(color_stop.color, color_stop.position);
    }

    return stops_with_replaced_transition_hints;
}

static ColorStopList expand_repeat_length(ColorStopList const& color_stop_list, float repeat_length)
{
    // https://drafts.csswg.org/css-images/#repeating-gradients
    // When rendered, however, the color-stops are repeated infinitely in both directions, with their
    // positions shifted by multiples of the difference between the last specified color-stop’s position
    // and the first specified color-stop’s position. For example, repeating-linear-gradient(red 10px, blue 50px)
    // is equivalent to linear-gradient(..., red -30px, blue 10px, red 10px, blue 50px, red 50px, blue 90px, ...).

    auto first_stop_position = color_stop_list.first().position;
    int const negative_repeat_count = AK::ceil(first_stop_position / repeat_length);
    int const positive_repeat_count = AK::ceil((1.0f - first_stop_position) / repeat_length);

    ColorStopList color_stop_list_with_expanded_repeat = color_stop_list;

    auto get_color_between_stops = [](float position, auto const& current_stop, auto const& previous_stop) {
        auto distance_between_stops = current_stop.position - previous_stop.position;
        auto percentage = (position - previous_stop.position) / distance_between_stops;
        return previous_stop.color.interpolate(current_stop.color, percentage);
    };

    for (auto repeat_count = 1; repeat_count <= negative_repeat_count; repeat_count++) {
        for (auto stop : color_stop_list.in_reverse()) {
            stop.position += repeat_length * static_cast<float>(-repeat_count);
            if (stop.position < 0) {
                stop.color = get_color_between_stops(0.0f, stop, color_stop_list_with_expanded_repeat.first());
                color_stop_list_with_expanded_repeat.prepend(stop);
                break;
            }
            color_stop_list_with_expanded_repeat.prepend(stop);
        }
    }

    for (auto repeat_count = 0; repeat_count < positive_repeat_count; repeat_count++) {
        for (auto stop : color_stop_list) {
            stop.position += repeat_length * static_cast<float>(repeat_count);
            if (stop.position > 1) {
                stop.color = get_color_between_stops(1.0f, stop, color_stop_list_with_expanded_repeat.last());
                color_stop_list_with_expanded_repeat.append(stop);
                break;
            }
            color_stop_list_with_expanded_repeat.append(stop);
        }
    }

    return color_stop_list_with_expanded_repeat;
}

CommandResult DisplayListPlayerSkia::paint_linear_gradient(PaintLinearGradient const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& linear_gradient_data = command.linear_gradient_data;
    auto color_stop_list = linear_gradient_data.color_stops.list;
    auto const& repeat_length = linear_gradient_data.color_stops.repeat_length;
    VERIFY(!color_stop_list.is_empty());
    if (repeat_length.has_value())
        color_stop_list = expand_repeat_length(color_stop_list, *repeat_length);

    auto stops_with_replaced_transition_hints = replace_transition_hints_with_normal_color_stops(color_stop_list);

    Vector<SkColor> colors;
    Vector<SkScalar> positions;

    for (size_t stop_index = 0; stop_index < stops_with_replaced_transition_hints.size(); stop_index++) {
        auto const& stop = stops_with_replaced_transition_hints[stop_index];
        if (stop_index > 0 && stop == stops_with_replaced_transition_hints[stop_index - 1])
            continue;
        colors.append(to_skia_color(stop.color));
        positions.append(stop.position);
    }

    auto const& rect = command.gradient_rect;
    auto length = calculate_gradient_length<int>(rect.size(), linear_gradient_data.gradient_angle);
    auto bottom = rect.center().translated(0, -length / 2);
    auto top = rect.center().translated(0, length / 2);

    Array<SkPoint, 2> points;
    points[0] = to_skia_point(top);
    points[1] = to_skia_point(bottom);

    auto center = to_skia_rect(rect).center();
    SkMatrix matrix;
    matrix.setRotate(linear_gradient_data.gradient_angle, center.x(), center.y());

    auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), positions.size(), SkTileMode::kClamp, 0, &matrix);

    SkPaint paint;
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);

    return CommandResult::Continue;
}

static void add_spread_distance_to_border_radius(int& border_radius, int spread_distance)
{
    if (border_radius == 0 || spread_distance == 0)
        return;

    // https://drafts.csswg.org/css-backgrounds/#shadow-shape
    // To preserve the box’s shape when spread is applied, the corner radii of the shadow are also increased (decreased,
    // for inner shadows) from the border-box (padding-box) radii by adding (subtracting) the spread distance (and flooring
    // at zero). However, in order to create a sharper corner when the border radius is small (and thus ensure continuity
    // between round and sharp corners), when the border radius is less than the spread distance (or in the case of an inner
    // shadow, less than the absolute value of a negative spread distance), the spread distance is first multiplied by the
    // proportion 1 + (r-1)^3, where r is the ratio of the border radius to the spread distance, in calculating the corner
    // radii of the spread shadow shape.
    if (border_radius > AK::abs(spread_distance)) {
        border_radius += spread_distance;
    } else {
        auto r = (float)border_radius / AK::abs(spread_distance);
        border_radius += spread_distance * (1 + AK::pow(r - 1, 3.0f));
    }
}

CommandResult DisplayListPlayerSkia::paint_outer_box_shadow(PaintOuterBoxShadow const& command)
{
    auto const& outer_box_shadow_params = command.box_shadow_params;
    auto const& color = outer_box_shadow_params.color;
    auto const& spread_distance = outer_box_shadow_params.spread_distance;
    auto const& blur_radius = outer_box_shadow_params.blur_radius;

    auto content_rrect = to_skia_rrect(outer_box_shadow_params.device_content_rect, outer_box_shadow_params.corner_radii);

    auto shadow_rect = outer_box_shadow_params.device_content_rect;
    shadow_rect.inflate(spread_distance, spread_distance, spread_distance, spread_distance);
    auto offset_x = outer_box_shadow_params.offset_x;
    auto offset_y = outer_box_shadow_params.offset_y;
    shadow_rect.translate_by(offset_x, offset_y);

    auto add_spread_distance_to_corner_radius = [&](auto& corner_radius) {
        add_spread_distance_to_border_radius(corner_radius.horizontal_radius, spread_distance);
        add_spread_distance_to_border_radius(corner_radius.vertical_radius, spread_distance);
    };

    auto corner_radii = outer_box_shadow_params.corner_radii;
    add_spread_distance_to_corner_radius(corner_radii.top_left);
    add_spread_distance_to_corner_radius(corner_radii.top_right);
    add_spread_distance_to_corner_radius(corner_radii.bottom_right);
    add_spread_distance_to_corner_radius(corner_radii.bottom_left);

    auto& canvas = surface().canvas();
    canvas.save();
    canvas.clipRRect(content_rrect, SkClipOp::kDifference, true);
    SkPaint paint;
    paint.setColor(to_skia_color(color));
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    auto shadow_rounded_rect = to_skia_rrect(shadow_rect, corner_radii);
    canvas.drawRRect(shadow_rounded_rect, paint);
    canvas.restore();

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::paint_inner_box_shadow(PaintInnerBoxShadow const& command)
{
    auto const& outer_box_shadow_params = command.box_shadow_params;
    auto color = outer_box_shadow_params.color;
    auto device_content_rect = outer_box_shadow_params.device_content_rect;
    auto offset_x = outer_box_shadow_params.offset_x;
    auto offset_y = outer_box_shadow_params.offset_y;
    auto blur_radius = outer_box_shadow_params.blur_radius;
    auto spread_distance = outer_box_shadow_params.spread_distance;
    auto const& corner_radii = outer_box_shadow_params.corner_radii;

    auto outer_shadow_rect = device_content_rect.translated({ offset_x, offset_y });
    auto inner_shadow_rect = outer_shadow_rect.inflated(-spread_distance, -spread_distance, -spread_distance, -spread_distance);
    outer_shadow_rect.inflate(
        blur_radius + offset_y,
        blur_radius + abs(offset_x),
        blur_radius + abs(offset_y),
        blur_radius + offset_x);

    auto inner_rect_corner_radii = corner_radii;

    auto add_spread_distance_to_corner_radius = [&](auto& corner_radius) {
        add_spread_distance_to_border_radius(corner_radius.horizontal_radius, -spread_distance);
        add_spread_distance_to_border_radius(corner_radius.vertical_radius, -spread_distance);
    };

    add_spread_distance_to_corner_radius(inner_rect_corner_radii.top_left);
    add_spread_distance_to_corner_radius(inner_rect_corner_radii.top_right);
    add_spread_distance_to_corner_radius(inner_rect_corner_radii.bottom_right);
    add_spread_distance_to_corner_radius(inner_rect_corner_radii.bottom_left);

    auto outer_rect = to_skia_rrect(outer_shadow_rect, corner_radii);
    auto inner_rect = to_skia_rrect(inner_shadow_rect, inner_rect_corner_radii);

    SkPath outer_path;
    outer_path.addRRect(outer_rect);
    SkPath inner_path;
    inner_path.addRRect(inner_rect);

    SkPath result_path;
    if (!Op(outer_path, inner_path, SkPathOp::kDifference_SkPathOp, &result_path)) {
        VERIFY_NOT_REACHED();
    }

    auto& canvas = surface().canvas();
    SkPaint path_paint;
    path_paint.setColor(to_skia_color(color));
    path_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur_radius / 2));
    canvas.save();
    canvas.clipRRect(to_skia_rrect(device_content_rect, corner_radii), true);
    canvas.drawPath(result_path, path_paint);
    canvas.restore();

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::paint_text_shadow(PaintTextShadow const& command)
{
    auto& canvas = surface().canvas();
    auto blur_image_filter = SkImageFilters::Blur(command.blur_radius / 2, command.blur_radius / 2, nullptr);
    SkPaint blur_paint;
    blur_paint.setImageFilter(blur_image_filter);
    canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, &blur_paint, nullptr, 0));
    draw_glyph_run({
        .glyph_run = command.glyph_run,
        .color = command.color,
        .rect = command.text_rect,
        .translation = command.draw_location.to_type<float>() + command.text_rect.location().to_type<float>(),
        .scale = command.glyph_run_scale,
    });
    canvas.restore();
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::fill_rect_with_rounded_corners(FillRectWithRoundedCorners const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& rect = command.rect;

    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setColor(to_skia_color(command.color));

    auto rounded_rect = to_skia_rrect(rect, command.corner_radii);
    canvas.drawRRect(rounded_rect, paint);

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::fill_path_using_color(FillPathUsingColor const& command)
{
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    auto path = to_skia_path(command.path);
    path.setFillType(to_skia_path_fill_type(command.winding_rule));
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    canvas.drawPath(path, paint);
    return CommandResult::Continue;
}

static SkTileMode to_skia_tile_mode(SVGLinearGradientPaintStyle::SpreadMethod spread_method)
{
    switch (spread_method) {
    case SVGLinearGradientPaintStyle::SpreadMethod::Pad:
        return SkTileMode::kClamp;
    case SVGLinearGradientPaintStyle::SpreadMethod::Reflect:
        return SkTileMode::kMirror;
    case SVGLinearGradientPaintStyle::SpreadMethod::Repeat:
        return SkTileMode::kRepeat;
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkPaint paint_style_to_skia_paint(Painting::SVGGradientPaintStyle const& paint_style, Gfx::FloatRect bounding_rect)
{
    SkPaint paint;

    auto const& color_stops = paint_style.color_stops();

    Vector<SkColor> colors;
    colors.ensure_capacity(color_stops.size());
    Vector<SkScalar> positions;
    positions.ensure_capacity(color_stops.size());

    for (auto const& color_stop : color_stops) {
        colors.append(to_skia_color(color_stop.color));
        positions.append(color_stop.position);
    }

    if (is<SVGLinearGradientPaintStyle>(paint_style)) {
        auto const& linear_gradient_paint_style = static_cast<SVGLinearGradientPaintStyle const&>(paint_style);

        SkMatrix matrix;
        auto scale = linear_gradient_paint_style.scale();
        auto start_point = linear_gradient_paint_style.start_point().scaled(scale);
        auto end_point = linear_gradient_paint_style.end_point().scaled(scale);

        start_point.translate_by(bounding_rect.location());
        end_point.translate_by(bounding_rect.location());

        Array<SkPoint, 2> points;
        points[0] = to_skia_point(start_point);
        points[1] = to_skia_point(end_point);

        auto shader = SkGradientShader::MakeLinear(points.data(), colors.data(), positions.data(), color_stops.size(), to_skia_tile_mode(paint_style.spread_method()), 0, &matrix);
        paint.setShader(shader);
    } else if (is<SVGRadialGradientPaintStyle>(paint_style)) {
        auto const& radial_gradient_paint_style = static_cast<SVGRadialGradientPaintStyle const&>(paint_style);

        SkMatrix matrix;
        auto scale = radial_gradient_paint_style.scale();

        auto start_center = radial_gradient_paint_style.start_center().scaled(scale);
        auto end_center = radial_gradient_paint_style.end_center().scaled(scale);
        auto start_radius = radial_gradient_paint_style.start_radius() * scale;
        auto end_radius = radial_gradient_paint_style.end_radius() * scale;

        start_center.translate_by(bounding_rect.location());
        end_center.translate_by(bounding_rect.location());

        auto start_sk_point = to_skia_point(start_center);
        auto end_sk_point = to_skia_point(end_center);

        auto shader = SkGradientShader::MakeTwoPointConical(start_sk_point, start_radius, end_sk_point, end_radius, colors.data(), positions.data(), color_stops.size(), to_skia_tile_mode(paint_style.spread_method()), 0, &matrix);
        paint.setShader(shader);
    }

    return paint;
}

CommandResult DisplayListPlayerSkia::fill_path_using_paint_style(FillPathUsingPaintStyle const& command)
{
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    path.setFillType(to_skia_path_fill_type(command.winding_rule));
    auto paint = paint_style_to_skia_paint(*command.paint_style, command.bounding_rect().to_type<float>());
    paint.setAntiAlias(true);
    paint.setAlphaf(command.opacity);
    surface().canvas().drawPath(path, paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::stroke_path_using_color(StrokePathUsingColor const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return CommandResult::Continue;

    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));
    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    canvas.drawPath(path, paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::stroke_path_using_paint_style(StrokePathUsingPaintStyle const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return CommandResult::Continue;

    auto path = to_skia_path(command.path);
    path.offset(command.aa_translation.x(), command.aa_translation.y());
    auto paint = paint_style_to_skia_paint(*command.paint_style, command.bounding_rect().to_type<float>());
    paint.setAntiAlias(true);
    paint.setAlphaf(command.opacity);
    paint.setStyle(SkPaint::Style::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    surface().canvas().drawPath(path, paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::draw_ellipse(DrawEllipse const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return CommandResult::Continue;

    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));
    canvas.drawOval(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::fill_ellipse(FillEllipse const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(to_skia_color(command.color));
    canvas.drawOval(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::draw_line(DrawLine const& command)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!command.thickness)
        return CommandResult::Continue;

    auto from = to_skia_point(command.from);
    auto to = to_skia_point(command.to);
    auto& canvas = surface().canvas();

    SkPaint paint;
    paint.setStrokeWidth(command.thickness);
    paint.setColor(to_skia_color(command.color));

    switch (command.style) {
    case Gfx::LineStyle::Solid:
        break;
    case Gfx::LineStyle::Dotted: {
        auto length = command.to.distance_from(command.from);
        auto dot_count = floor(length / (static_cast<float>(command.thickness) * 2));
        auto interval = length / dot_count;
        SkScalar intervals[] = { 0, interval };
        paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, 0));
        paint.setStrokeCap(SkPaint::Cap::kRound_Cap);

        // NOTE: As Skia doesn't render a dot exactly at the end of a line, we need
        //       to extend it by less then an interval.
        auto direction = to - from;
        direction.normalize();
        to += direction * (interval / 2.0f);
        break;
    }
    case Gfx::LineStyle::Dashed: {
        auto length = command.to.distance_from(command.from) + command.thickness;
        auto dash_count = floor(length / static_cast<float>(command.thickness) / 4) * 2 + 1;
        auto interval = length / dash_count;
        SkScalar intervals[] = { interval, interval };
        paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, 0));

        auto direction = to - from;
        direction.normalize();
        from -= direction * (command.thickness / 2.0f);
        to += direction * (command.thickness / 2.0f);
        break;
    }
    }

    canvas.drawLine(from, to, paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::apply_backdrop_filter(ApplyBackdropFilter const& command)
{
    auto& canvas = surface().canvas();

    auto rect = to_skia_rect(command.backdrop_region);
    canvas.save();
    canvas.clipRect(rect);
    ScopeGuard guard = [&] { canvas.restore(); };

    for (auto const& filter_function : command.backdrop_filter.filters) {
        // See: https://drafts.fxtf.org/filter-effects-1/#supported-filter-functions
        filter_function.visit(
            [&](CSS::ResolvedBackdropFilter::Blur const& blur_filter) {
                auto blur_image_filter = SkImageFilters::Blur(blur_filter.radius, blur_filter.radius, nullptr);
                canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, blur_image_filter.get(), 0));
                canvas.restore();
            },
            [&](CSS::ResolvedBackdropFilter::ColorOperation const& color) {
                auto amount = clamp(color.amount, 0.0f, 1.0f);

                // Matrices are taken from https://drafts.fxtf.org/filter-effects-1/#FilterPrimitiveRepresentation
                sk_sp<SkColorFilter> color_filter;
                switch (color.operation) {
                case CSS::Filter::Color::Operation::Grayscale: {
                    float matrix[20] = {
                        0.2126f + 0.7874f * (1 - amount), 0.7152f - 0.7152f * (1 - amount), 0.0722f - 0.0722f * (1 - amount), 0, 0,
                        0.2126f - 0.2126f * (1 - amount), 0.7152f + 0.2848f * (1 - amount), 0.0722f - 0.0722f * (1 - amount), 0, 0,
                        0.2126f - 0.2126f * (1 - amount), 0.7152f - 0.7152f * (1 - amount), 0.0722f + 0.9278f * (1 - amount), 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Brightness: {
                    float matrix[20] = {
                        amount, 0, 0, 0, 0,
                        0, amount, 0, 0, 0,
                        0, 0, amount, 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Contrast: {
                    float intercept = -(0.5f * amount) + 0.5f;
                    float matrix[20] = {
                        amount, 0, 0, 0, intercept,
                        0, amount, 0, 0, intercept,
                        0, 0, amount, 0, intercept,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Invert: {
                    float matrix[20] = {
                        1 - 2 * amount, 0, 0, 0, amount,
                        0, 1 - 2 * amount, 0, 0, amount,
                        0, 0, 1 - 2 * amount, 0, amount,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Opacity: {
                    float matrix[20] = {
                        1, 0, 0, 0, 0,
                        0, 1, 0, 0, 0,
                        0, 0, 1, 0, 0,
                        0, 0, 0, amount, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Sepia: {
                    float matrix[20] = {
                        0.393f + 0.607f * (1 - amount), 0.769f - 0.769f * (1 - amount), 0.189f - 0.189f * (1 - amount), 0, 0,
                        0.349f - 0.349f * (1 - amount), 0.686f + 0.314f * (1 - amount), 0.168f - 0.168f * (1 - amount), 0, 0,
                        0.272f - 0.272f * (1 - amount), 0.534f - 0.534f * (1 - amount), 0.131f + 0.869f * (1 - amount), 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                case CSS::Filter::Color::Operation::Saturate: {
                    float matrix[20] = {
                        0.213f + 0.787f * amount, 0.715f - 0.715f * amount, 0.072f - 0.072f * amount, 0, 0,
                        0.213f - 0.213f * amount, 0.715f + 0.285f * amount, 0.072f - 0.072f * amount, 0, 0,
                        0.213f - 0.213f * amount, 0.715f - 0.715f * amount, 0.072f + 0.928f * amount, 0, 0,
                        0, 0, 0, 1, 0
                    };
                    color_filter = SkColorFilters::Matrix(matrix);
                    break;
                }
                default:
                    VERIFY_NOT_REACHED();
                }

                auto image_filter = SkImageFilters::ColorFilter(color_filter, nullptr);
                canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, image_filter.get(), 0));
                canvas.restore();
            },
            [&](CSS::ResolvedBackdropFilter::HueRotate const& hue_rotate) {
                float radians = AK::to_radians(hue_rotate.angle_degrees);

                auto cosA = cos(radians);
                auto sinA = sin(radians);

                auto a00 = 0.213f + cosA * 0.787f - sinA * 0.213f;
                auto a01 = 0.715f - cosA * 0.715f - sinA * 0.715f;
                auto a02 = 0.072f - cosA * 0.072f + sinA * 0.928f;
                auto a10 = 0.213f - cosA * 0.213f + sinA * 0.143f;
                auto a11 = 0.715f + cosA * 0.285f + sinA * 0.140f;
                auto a12 = 0.072f - cosA * 0.072f - sinA * 0.283f;
                auto a20 = 0.213f - cosA * 0.213f - sinA * 0.787f;
                auto a21 = 0.715f - cosA * 0.715f + sinA * 0.715f;
                auto a22 = 0.072f + cosA * 0.928f + sinA * 0.072f;

                float matrix[20] = {
                    a00, a01, a02, 0, 0,
                    a10, a11, a12, 0, 0,
                    a20, a21, a22, 0, 0,
                    0, 0, 0, 1, 0
                };

                auto color_filter = SkColorFilters::Matrix(matrix);
                auto image_filter = SkImageFilters::ColorFilter(color_filter, nullptr);
                canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, nullptr, image_filter.get(), 0));
                canvas.restore();
            },
            [&](CSS::ResolvedBackdropFilter::DropShadow const&) {
                dbgln("TODO: Implement drop-shadow() filter function!");
            });
    }

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::draw_rect(DrawRect const& command)
{
    auto const& rect = command.rect;
    auto& canvas = surface().canvas();
    SkPaint paint;
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1);
    paint.setColor(to_skia_color(command.color));
    canvas.drawRect(to_skia_rect(rect), paint);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::paint_radial_gradient(PaintRadialGradient const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& radial_gradient_data = command.radial_gradient_data;

    auto color_stop_list = radial_gradient_data.color_stops.list;
    VERIFY(!color_stop_list.is_empty());
    auto const& repeat_length = radial_gradient_data.color_stops.repeat_length;
    if (repeat_length.has_value())
        color_stop_list = expand_repeat_length(color_stop_list, repeat_length.value());

    auto stops_with_replaced_transition_hints = replace_transition_hints_with_normal_color_stops(color_stop_list);

    Vector<SkColor> colors;
    Vector<SkScalar> positions;

    for (size_t stop_index = 0; stop_index < stops_with_replaced_transition_hints.size(); stop_index++) {
        auto const& stop = stops_with_replaced_transition_hints[stop_index];
        if (stop_index > 0 && stop == stops_with_replaced_transition_hints[stop_index - 1])
            continue;
        colors.append(to_skia_color(stop.color));
        positions.append(stop.position);
    }

    auto const& rect = command.rect;
    auto center = to_skia_point(command.center.translated(command.rect.location()));

    auto const size = command.size.to_type<float>();
    SkMatrix matrix;
    // Skia does not support specifying of horizontal and vertical radius's separately,
    // so instead we apply scale matrix
    matrix.setScale(size.width() / size.height(), 1.0f, center.x(), center.y());

    SkTileMode tile_mode = SkTileMode::kClamp;
    if (repeat_length.has_value())
        tile_mode = SkTileMode::kRepeat;

    auto shader = SkGradientShader::MakeRadial(center, size.height(), colors.data(), positions.data(), positions.size(), tile_mode, 0, &matrix);

    SkPaint paint;
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::paint_conic_gradient(PaintConicGradient const& command)
{
    APPLY_PATH_CLIP_IF_NEEDED

    auto const& conic_gradient_data = command.conic_gradient_data;

    auto color_stop_list = conic_gradient_data.color_stops.list;
    auto const& repeat_length = conic_gradient_data.color_stops.repeat_length;
    if (repeat_length.has_value())
        color_stop_list = expand_repeat_length(color_stop_list, repeat_length.value());

    VERIFY(!color_stop_list.is_empty());
    auto stops_with_replaced_transition_hints = replace_transition_hints_with_normal_color_stops(color_stop_list);

    Vector<SkColor> colors;
    Vector<SkScalar> positions;
    for (size_t stop_index = 0; stop_index < stops_with_replaced_transition_hints.size(); stop_index++) {
        auto const& stop = stops_with_replaced_transition_hints[stop_index];
        if (stop_index > 0 && stop == stops_with_replaced_transition_hints[stop_index - 1])
            continue;
        colors.append(to_skia_color(stop.color));
        positions.append(stop.position);
    }

    auto const& rect = command.rect;
    auto center = command.position.translated(rect.location()).to_type<float>();

    SkMatrix matrix;
    matrix.setRotate(-90 + conic_gradient_data.start_angle, center.x(), center.y());
    auto shader = SkGradientShader::MakeSweep(center.x(), center.y(), colors.data(), positions.data(), positions.size(), SkTileMode::kRepeat, 0, 360, 0, &matrix);

    SkPaint paint;
    paint.setShader(shader);
    surface().canvas().drawRect(to_skia_rect(rect), paint);

    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::draw_triangle_wave(DrawTriangleWave const&)
{
    return CommandResult::Continue;
}

void DisplayListPlayerSkia::prepare_to_execute(size_t)
{
}

CommandResult DisplayListPlayerSkia::sample_under_corners(SampleUnderCorners const& command)
{
    auto rounded_rect = to_skia_rrect(command.border_rect, command.corner_radii);
    auto& canvas = surface().canvas();
    canvas.save();
    auto clip_op = command.corner_clip == CornerClip::Inside ? SkClipOp::kDifference : SkClipOp::kIntersect;
    canvas.clipRRect(rounded_rect, clip_op, true);
    return CommandResult::Continue;
}

CommandResult DisplayListPlayerSkia::blit_corner_clipping(BlitCornerClipping const&)
{
    auto& canvas = surface().canvas();
    canvas.restore();
    return CommandResult::Continue;
}

bool DisplayListPlayerSkia::would_be_fully_clipped_by_painter(Gfx::IntRect rect) const
{
    return surface().canvas().quickReject(to_skia_rect(rect));
}

}
