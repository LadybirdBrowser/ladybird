/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <LibGfx/ImageFormats/ExifOrientedBitmap.h>
#include <LibGfx/ImageFormats/ExifReader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
#include <LibImageDecoders/RustFFI.h>

namespace Gfx {

struct PNGLoadingContext {
    AK_ALLOC_WITH_KMALLOC;

    IntSize size;
    u32 loop_count { 0 };
    Vector<ImageFrameDescriptor> frame_descriptors;
    Optional<Media::CodingIndependentCodePoints> cicp;
    Optional<ByteBuffer> icc_profile;
    OwnPtr<ExifMetadata> exif_metadata;

    RefPtr<Bitmap> pending_bitmap;
    int pending_frame_duration { 0 };

    ErrorOr<void> read_image_info(ImageDecoders::FFI::PNGImageInfo const&);
    ErrorOr<void> apply_exif_orientation();

    static bool image_info(void* context, ImageDecoders::FFI::PNGImageInfo const* info)
    {
        return !static_cast<PNGLoadingContext*>(context)->read_image_info(*info).is_error();
    }

    static bool get_frame_buffer(void* context, u32 duration_ms, u8** buffer, size_t* stride)
    {
        auto& self = *static_cast<PNGLoadingContext*>(context);
        if (self.pending_bitmap)
            return false;

        auto bitmap_or_error = Bitmap::create_shareable_with_gpu_access(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, self.size);
        if (bitmap_or_error.is_error())
            return false;

        self.pending_bitmap = bitmap_or_error.release_value();
        self.pending_frame_duration = AK::clamp_to<int>(duration_ms);
        *buffer = self.pending_bitmap->scanline_u8(0);
        *stride = self.pending_bitmap->pitch();
        return true;
    }

    static bool frame_decoded(void* context)
    {
        auto& self = *static_cast<PNGLoadingContext*>(context);
        if (!self.pending_bitmap)
            return false;
        return !self.frame_descriptors.try_empend(self.pending_bitmap.release_nonnull(), self.pending_frame_duration).is_error();
    }
};

ErrorOr<void> PNGLoadingContext::read_image_info(ImageDecoders::FFI::PNGImageInfo const& info)
{
    if (info.width > static_cast<u32>(NumericLimits<int>::max()) || info.height > static_cast<u32>(NumericLimits<int>::max()))
        return Error::from_string_literal("PNGImageDecoderPlugin: Image dimensions are too large");
    size = { static_cast<int>(info.width), static_cast<int>(info.height) };
    if (Bitmap::size_would_overflow(BitmapFormat::BGRA8888, size))
        return Error::from_string_literal("PNGImageDecoderPlugin: Image dimensions are too large");

    loop_count = info.loop_count;

    if (info.has_cicp) {
        cicp = Media::CodingIndependentCodePoints {
            Media::ColorPrimaries { info.cicp_color_primaries },
            Media::TransferCharacteristics { info.cicp_transfer_function },
            Media::MatrixCoefficients { info.cicp_matrix_coefficients },
            Media::VideoFullRangeFlag { info.cicp_video_full_range_flag },
        };
    } else if (info.icc_profile) {
        icc_profile = TRY(ByteBuffer::copy(info.icc_profile, info.icc_profile_len));
    }

    if (info.exif)
        exif_metadata = TRY(TIFF::read_exif_metadata({ info.exif, info.exif_len }));

    return {};
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> PNGImageDecoderPlugin::create(ReadonlyBytes bytes)
{
    auto decoder = adopt_own(*new PNGImageDecoderPlugin);
    auto& context = *decoder->m_context;

    auto result = ImageDecoders::FFI::png_rust_decode(bytes.data(), bytes.size(), &context, PNGLoadingContext::image_info, PNGLoadingContext::get_frame_buffer, PNGLoadingContext::frame_decoded);
    context.pending_bitmap = nullptr;

    if (result == ImageDecoders::FFI::PNGDecodeResult::InvalidHeader)
        return Error::from_string_literal("PNGImageDecoderPlugin: Unable to read the image header");

    if (result == ImageDecoders::FFI::PNGDecodeResult::DecodingFailed) {
        // NOTE: The header was read, so we know the size. We create a single-frame bitmap with that size and return it.
        //       This is weird, but kinda matches the behavior of other browsers.
        context.frame_descriptors.clear();
        auto bitmap = TRY(Bitmap::create_shareable_with_gpu_access(BitmapFormat::BGRA8888, AlphaType::Premultiplied, context.size));
        context.frame_descriptors.append({ move(bitmap), 0 });
        return decoder;
    }

    if (context.exif_metadata)
        TRY(context.apply_exif_orientation());
    return decoder;
}

PNGImageDecoderPlugin::PNGImageDecoderPlugin()
    : m_context(adopt_own(*new PNGLoadingContext))
{
}

size_t PNGImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

IntSize PNGImageDecoderPlugin::size()
{
    return m_context->size;
}

bool PNGImageDecoderPlugin::is_animated()
{
    return m_context->frame_descriptors.size() > 1;
}

size_t PNGImageDecoderPlugin::loop_count()
{
    return m_context->loop_count;
}

size_t PNGImageDecoderPlugin::frame_count()
{
    return m_context->frame_descriptors.size();
}

int PNGImageDecoderPlugin::frame_duration(size_t index)
{
    if (index >= m_context->frame_descriptors.size())
        return 0;
    return m_context->frame_descriptors[index].duration;
}

ErrorOr<ImageFrameDescriptor> PNGImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index >= m_context->frame_descriptors.size())
        return Error::from_errno(EINVAL);

    return m_context->frame_descriptors[index];
}

ErrorOr<Optional<Media::CodingIndependentCodePoints>> PNGImageDecoderPlugin::cicp()
{
    return m_context->cicp;
}

ErrorOr<Optional<ReadonlyBytes>> PNGImageDecoderPlugin::icc_data()
{
    if (m_context->icc_profile.has_value())
        return Optional<ReadonlyBytes>(*m_context->icc_profile);
    return OptionalNone {};
}

ErrorOr<void> PNGLoadingContext::apply_exif_orientation()
{
    auto orientation = exif_metadata->orientation().value_or(TIFF::Orientation::Default);
    if (orientation == TIFF::Orientation::Default)
        return {};

    for (auto& img_frame_descriptor : frame_descriptors) {
        auto& img = img_frame_descriptor.image;
        auto oriented_bmp = TRY(ExifOrientedBitmap::create(orientation, img->size(), img->format(), img->alpha_type()));

        for (int y = 0; y < img->size().height(); ++y) {
            for (int x = 0; x < img->size().width(); ++x) {
                auto pixel = img->get_pixel(x, y);
                oriented_bmp.set_pixel(x, y, pixel.value());
            }
        }

        img_frame_descriptor.image = oriented_bmp.bitmap();
    }

    size = ExifOrientedBitmap::oriented_size(size, orientation);

    return {};
}

PNGImageDecoderPlugin::~PNGImageDecoderPlugin() = default;

bool PNGImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return ImageDecoders::FFI::png_rust_sniff(data.data(), data.size());
}

Optional<Metadata const&> PNGImageDecoderPlugin::metadata()
{
    if (m_context->exif_metadata)
        return *m_context->exif_metadata;
    return OptionalNone {};
}

}
