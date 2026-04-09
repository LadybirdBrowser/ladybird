/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/YUVData.h>

#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkSurface.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Gfx {

StringView export_format_name(ExportFormat format)
{
    switch (format) {
#define ENUMERATE_EXPORT_FORMAT(format) \
    case Gfx::ExportFormat::format:     \
        return #format##sv;
        ENUMERATE_EXPORT_FORMATS(ENUMERATE_EXPORT_FORMAT)
#undef ENUMERATE_EXPORT_FORMAT
    }
    VERIFY_NOT_REACHED();
}

struct ImmutableBitmapImpl {
    RefPtr<SkiaBackendContext> context;
    sk_sp<SkImage> sk_image;
    SkBitmap sk_bitmap;
    RefPtr<Gfx::Bitmap> bitmap;
    ColorSpace color_space;
    OwnPtr<YUVData> yuv_data;
};

int ImmutableBitmap::width() const
{
    if (m_impl->yuv_data)
        return m_impl->yuv_data->size().width();
    return m_impl->sk_image->width();
}

int ImmutableBitmap::height() const
{
    if (m_impl->yuv_data)
        return m_impl->yuv_data->size().height();
    return m_impl->sk_image->height();
}

IntRect ImmutableBitmap::rect() const
{
    return { {}, size() };
}

IntSize ImmutableBitmap::size() const
{
    return { width(), height() };
}

AlphaType ImmutableBitmap::alpha_type() const
{
    // We assume premultiplied alpha type for opaque surfaces since that is Skia's preferred alpha type and the
    // effective pixel data is identical between premultiplied and unpremultiplied in that case.
    return m_impl->sk_image->alphaType() == kUnpremul_SkAlphaType ? AlphaType::Unpremultiplied : AlphaType::Premultiplied;
}

SkImage const* ImmutableBitmap::sk_image() const
{
    return m_impl->sk_image.get();
}

static int bytes_per_pixel_for_export_format(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
    case ExportFormat::Alpha8:
        return 1;
    case ExportFormat::RGB565:
    case ExportFormat::RGBA5551:
    case ExportFormat::RGBA4444:
        return 2;
    case ExportFormat::RGB888:
        return 3;
    case ExportFormat::RGBA8888:
        return 4;
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkColorType export_format_to_skia_color_type(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
        return SkColorType::kGray_8_SkColorType;
    case ExportFormat::Alpha8:
        return SkColorType::kAlpha_8_SkColorType;
    case ExportFormat::RGB565:
        return SkColorType::kRGB_565_SkColorType;
    case ExportFormat::RGBA5551:
        dbgln("FIXME: Support conversion to RGBA5551.");
        return SkColorType::kUnknown_SkColorType;
    case ExportFormat::RGBA4444:
        return SkColorType::kARGB_4444_SkColorType;
    case ExportFormat::RGB888:
        // This one needs to be converted manually because Skia has no valid 24-bit color type.
        VERIFY_NOT_REACHED();
    case ExportFormat::RGBA8888:
        return SkColorType::kRGBA_8888_SkColorType;
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<BitmapExportResult> ImmutableBitmap::export_to_byte_buffer(ExportFormat format, int flags, Optional<int> target_width, Optional<int> target_height) const
{
    int width = target_width.value_or(this->width());
    int height = target_height.value_or(this->height());

    if (format == ExportFormat::RGB888 && (width != this->width() || height != this->height())) {
        dbgln("FIXME: Ignoring target width and height because scaling is not implemented for this export format.");
        width = this->width();
        height = this->height();
    }

    Checked<size_t> buffer_pitch = width;
    int number_of_bytes = bytes_per_pixel_for_export_format(format);
    buffer_pitch *= number_of_bytes;
    if (buffer_pitch.has_overflow())
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    if (width > 0 && height > 0) {
        if (format == ExportFormat::RGB888) {
            // 24 bit RGB is not supported by Skia, so we need to handle this format ourselves.
            auto raw_buffer = buffer.data();
            for (auto y = 0; y < height; y++) {
                auto target_y = flags & ExportFlags::FlipY ? height - y - 1 : y;
                for (auto x = 0; x < width; x++) {
                    auto pixel = get_pixel(x, y);
                    auto buffer_offset = (target_y * buffer_pitch.value()) + (x * 3ull);
                    raw_buffer[buffer_offset + 0] = pixel.red();
                    raw_buffer[buffer_offset + 1] = pixel.green();
                    raw_buffer[buffer_offset + 2] = pixel.blue();
                }
            }
        } else {
            auto skia_format = export_format_to_skia_color_type(format);
            auto color_space = SkColorSpace::MakeSRGB();

            auto image_info = SkImageInfo::Make(width, height, skia_format, flags & ExportFlags::PremultiplyAlpha ? SkAlphaType::kPremul_SkAlphaType : SkAlphaType::kUnpremul_SkAlphaType, color_space);
            auto surface = SkSurfaces::WrapPixels(image_info, buffer.data(), buffer_pitch.value());
            VERIFY(surface);
            auto* surface_canvas = surface->getCanvas();
            auto dst_rect = Gfx::to_skia_rect(Gfx::Rect { 0, 0, width, height });

            if (flags & ExportFlags::FlipY) {
                surface_canvas->translate(0, dst_rect.height());
                surface_canvas->scale(1, -1);
            }

            surface_canvas->drawImageRect(sk_image(), dst_rect, Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor));
        }
    } else {
        VERIFY(buffer.is_empty());
    }

    return BitmapExportResult {
        .buffer = move(buffer),
        .width = width,
        .height = height,
    };
}

RefPtr<Gfx::Bitmap const> ImmutableBitmap::bitmap() const
{
    if (!m_impl->bitmap && m_impl->sk_image) {
        auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { m_impl->sk_image->width(), m_impl->sk_image->height() }));
        auto image_info = SkImageInfo::Make(bitmap->width(), bitmap->height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        SkPixmap pixmap(image_info, bitmap->begin(), bitmap->pitch());
        if (m_impl->context)
            m_impl->context->lock();
        m_impl->sk_image->readPixels(pixmap, 0, 0);
        if (m_impl->context)
            m_impl->context->unlock();
        m_impl->bitmap = move(bitmap);
    }
    return m_impl->bitmap;
}

bool ImmutableBitmap::is_yuv_backed() const
{
    return m_impl->yuv_data != nullptr;
}

ErrorOr<NonnullRefPtr<ImmutableBitmap>> ImmutableBitmap::create_from_yuv(NonnullOwnPtr<YUVData> yuv_data)
{
    // Hold onto the YUVData to lazily create the SkImage later.
    ImmutableBitmapImpl impl {
        .context = nullptr,
        .sk_image = nullptr,
        .sk_bitmap = {},
        .bitmap = nullptr,
        .color_space = {},
        .yuv_data = move(yuv_data),
    };
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(move(impl))));
}

static sk_sp<SkColorSpace> color_space_from_cicp(Media::CodingIndependentCodePoints const& cicp)
{
    auto gamut = [&] {
        if (cicp.color_primaries() == Media::ColorPrimaries::XYZ)
            return SkNamedGamut::kXYZ;

        auto primaries = [&] {
            switch (cicp.color_primaries()) {
            case Media::ColorPrimaries::Reserved:
            case Media::ColorPrimaries::Unspecified:
                return SkNamedPrimaries::kRec709;
            case Media::ColorPrimaries::XYZ:
                VERIFY_NOT_REACHED();
            case Media::ColorPrimaries::BT709:
                return SkNamedPrimaries::kRec709;
            case Media::ColorPrimaries::BT470M:
                return SkNamedPrimaries::kRec470SystemM;
            case Media::ColorPrimaries::BT470BG:
                return SkNamedPrimaries::kRec470SystemBG;
            case Media::ColorPrimaries::BT601:
                return SkNamedPrimaries::kRec601;
            case Media::ColorPrimaries::SMPTE240:
                return SkNamedPrimaries::kSMPTE_ST_240;
            case Media::ColorPrimaries::GenericFilm:
                return SkNamedPrimaries::kGenericFilm;
            case Media::ColorPrimaries::BT2020:
                return SkNamedPrimaries::kRec2020;
            case Media::ColorPrimaries::SMPTE431:
                return SkNamedPrimaries::kSMPTE_RP_431_2;
            case Media::ColorPrimaries::SMPTE432:
                return SkNamedPrimaries::kSMPTE_EG_432_1;
            case Media::ColorPrimaries::EBU3213:
                return SkNamedPrimaries::kITU_T_H273_Value22;
            }
            return SkNamedPrimaries::kRec709;
        }();
        skcms_Matrix3x3 result;
        VERIFY(primaries.toXYZD50(&result));
        return result;
    }();

    auto transfer_function = [&] {
        switch (cicp.transfer_characteristics()) {
        case Media::TransferCharacteristics::Unspecified:
        case Media::TransferCharacteristics::Reserved:
            return SkNamedTransferFn::kRec709;
        case Media::TransferCharacteristics::BT709:
            return SkNamedTransferFn::kRec709;
        case Media::TransferCharacteristics::BT470M:
            return SkNamedTransferFn::kRec470SystemM;
        case Media::TransferCharacteristics::BT470BG:
            return SkNamedTransferFn::kRec470SystemBG;
        case Media::TransferCharacteristics::BT601:
            return SkNamedTransferFn::kRec601;
        case Media::TransferCharacteristics::SMPTE240:
            return SkNamedTransferFn::kSMPTE_ST_240;
        case Media::TransferCharacteristics::Linear:
            return SkNamedTransferFn::kLinear;
        case Media::TransferCharacteristics::Log100:
        case Media::TransferCharacteristics::Log100Sqrt10:
            dbgln("Logarithmic transfer characteristics are not supported, using sRGB.");
            return SkNamedTransferFn::kSRGB;
        case Media::TransferCharacteristics::IEC61966:
            return SkNamedTransferFn::kIEC61966_2_4;
        case Media::TransferCharacteristics::BT1361:
            dbgln("BT.1361 transfer characteristics are not supported, using sRGB.");
            return SkNamedTransferFn::kSRGB;
        case Media::TransferCharacteristics::SRGB:
            return SkNamedTransferFn::kSRGB;
        case Media::TransferCharacteristics::BT2020BitDepth10:
            return SkNamedTransferFn::kRec2020_10bit;
        case Media::TransferCharacteristics::BT2020BitDepth12:
            return SkNamedTransferFn::kRec2020_12bit;
        case Media::TransferCharacteristics::SMPTE2084:
            return SkNamedTransferFn::kPQ;
        case Media::TransferCharacteristics::SMPTE428:
            return SkNamedTransferFn::kSMPTE_ST_428_1;
        case Media::TransferCharacteristics::HLG:
            return SkNamedTransferFn::kHLG;
        }
        return SkNamedTransferFn::kRec709;
    }();

    return SkColorSpace::MakeRGB(transfer_function, gamut);
}

bool ImmutableBitmap::ensure_sk_image(SkiaBackendContext& context) const
{
    if (m_impl->context) {
        VERIFY(m_impl->context.ptr() == &context);
        return true;
    }

    context.lock();
    ScopeGuard unlock_guard = [&context] {
        context.unlock();
    };

    auto* gr_context = context.sk_context();

    // Bitmap-backed: try to upload raster image to GPU texture
    if (m_impl->sk_image) {
        if (!gr_context)
            return true; // No GPU, but raster image is still usable
        auto gpu_image = SkImages::TextureFromImage(gr_context, m_impl->sk_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
        if (gpu_image) {
            m_impl->context = context;
            m_impl->sk_image = move(gpu_image);
        }
        return true;
    }

    // YUV-backed: GPU is required to decode YUV to RGB
    VERIFY(m_impl->yuv_data);

    if (!gr_context)
        return false; // No GPU, cannot create image from YUV data

    auto const& pixmaps = m_impl->yuv_data->skia_yuva_pixmaps();
    auto color_space = color_space_from_cicp(m_impl->yuv_data->cicp());

    auto sk_image = SkImages::TextureFromYUVAPixmaps(
        gr_context,
        pixmaps,
        skgpu::Mipmapped::kNo,
        false,
        color_space);

    if (!sk_image)
        return false;

    m_impl->context = context;
    m_impl->sk_image = move(sk_image);
    return true;
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    return m_impl->bitmap->get_pixel(x, y);
}

static SkAlphaType to_skia_alpha_type(Gfx::AlphaType alpha_type)
{
    switch (alpha_type) {
    case AlphaType::Premultiplied:
        return kPremul_SkAlphaType;
    case AlphaType::Unpremultiplied:
        return kUnpremul_SkAlphaType;
    default:
        VERIFY_NOT_REACHED();
    }
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space)
{
    SkBitmap sk_bitmap;
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->alpha_type()), color_space.color_space<sk_sp<SkColorSpace>>());
    sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(bitmap->scanline(0))), bitmap->pitch());
    sk_bitmap.setImmutable();
    auto sk_image = sk_bitmap.asImage();

    ImmutableBitmapImpl impl {
        .context = nullptr,
        .sk_image = move(sk_image),
        .sk_bitmap = move(sk_bitmap),
        .bitmap = move(bitmap),
        .color_space = move(color_space),
        .yuv_data = nullptr,
    };
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(move(impl))));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap> bitmap, AlphaType alpha_type, ColorSpace color_space)
{
    // Convert the source bitmap to the right alpha type on a mismatch. We want to do this when converting from a
    // Bitmap to an ImmutableBitmap, since at that point we usually know the right alpha type to use in context.
    auto source_bitmap = bitmap;
    if (source_bitmap->alpha_type() != alpha_type) {
        source_bitmap = MUST(bitmap->clone());
        source_bitmap->set_alpha_type_destructive(alpha_type);
    }

    return create(source_bitmap, move(color_space));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface> painting_surface)
{
    painting_surface->lock_context();
    auto sk_image = painting_surface->sk_image_snapshot<sk_sp<SkImage>>();
    painting_surface->unlock_context();

    ImmutableBitmapImpl impl {
        .context = painting_surface->skia_backend_context(),
        .sk_image = move(sk_image),
        .sk_bitmap = {},
        .bitmap = nullptr,
        .color_space = {},
        .yuv_data = nullptr,
    };
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(move(impl))));
}

ImmutableBitmap::ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> impl)
    : m_impl(move(impl))
{
}

ImmutableBitmap::~ImmutableBitmap()
{
    lock_context();
    m_impl->sk_image = nullptr;
    unlock_context();
}

void ImmutableBitmap::lock_context()
{
    auto& context = m_impl->context;
    if (context)
        context->lock();
}

void ImmutableBitmap::unlock_context()
{
    auto& context = m_impl->context;
    if (context)
        context->unlock();
}

}
