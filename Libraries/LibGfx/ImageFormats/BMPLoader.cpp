/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022, Bruno Conde <brunompconde@gmail.com>
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibImageDecoders/RustFFI.h>

namespace Gfx {

struct BMPLoadingContext {
    AK_MAKE_NONCOPYABLE(BMPLoadingContext);
    AK_MAKE_NONMOVABLE(BMPLoadingContext);

public:
    AK_ALLOC_WITH_KMALLOC;

    BMPLoadingContext(ImageDecoders::FFI::BMPDecoder* decoder, ImageDecoders::FFI::BMPImageInfo const& info, ReadonlyBytes data)
        : decoder(decoder)
        , info(info)
        , data(data)
    {
    }

    ~BMPLoadingContext()
    {
        ImageDecoders::FFI::bmp_rust_decoder_free(decoder);
    }

    enum class State : u8 {
        HeaderDecoded,
        PixelDataDecoded,
        Error,
    };

    ImageDecoders::FFI::BMPDecoder* decoder { nullptr };
    ImageDecoders::FFI::BMPImageInfo info {};
    ReadonlyBytes data;
    State state { State::HeaderDecoded };
    RefPtr<Bitmap> bitmap;
};

static BitmapFormat to_bitmap_format(ImageDecoders::FFI::BMPBitmapFormat format)
{
    switch (format) {
    case ImageDecoders::FFI::BMPBitmapFormat::BGRx8888:
        return BitmapFormat::BGRx8888;
    case ImageDecoders::FFI::BMPBitmapFormat::BGRA8888:
        return BitmapFormat::BGRA8888;
    case ImageDecoders::FFI::BMPBitmapFormat::RGBx8888:
        return BitmapFormat::RGBx8888;
    }
    VERIFY_NOT_REACHED();
}

BMPImageDecoderPlugin::BMPImageDecoderPlugin(NonnullOwnPtr<BMPLoadingContext> context)
    : m_context(move(context))
{
}

BMPImageDecoderPlugin::~BMPImageDecoderPlugin() = default;

IntSize BMPImageDecoderPlugin::size()
{
    return { static_cast<int>(m_context->info.width), static_cast<int>(m_context->info.height) };
}

bool BMPImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return ImageDecoders::FFI::bmp_rust_sniff(data.data(), data.size());
}

ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> BMPImageDecoderPlugin::create_impl(ReadonlyBytes data, IncludedInICO included_in_ico)
{
    ImageDecoders::FFI::BMPImageInfo info {};
    auto* decoder = ImageDecoders::FFI::bmp_rust_decoder_new(data.data(), data.size(), included_in_ico == IncludedInICO::Yes, &info);
    if (!decoder)
        return Error::from_string_literal("BMPImageDecoderPlugin: BMP has an invalid header");
    ArmedScopeGuard free_decoder = [decoder] { ImageDecoders::FFI::bmp_rust_decoder_free(decoder); };

    if (info.width > static_cast<u32>(NumericLimits<int>::max()) || info.height > static_cast<u32>(NumericLimits<int>::max()))
        return Error::from_string_literal("BMPImageDecoderPlugin: Image dimensions are too large");

    auto context = TRY(adopt_nonnull_own_or_enomem(new (nothrow) BMPLoadingContext(decoder, info, data)));
    free_decoder.disarm();

    return TRY(adopt_nonnull_own_or_enomem(new (nothrow) BMPImageDecoderPlugin(move(context))));
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> BMPImageDecoderPlugin::create(ReadonlyBytes data)
{
    return create_impl(data, IncludedInICO::No);
}

ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> BMPImageDecoderPlugin::create_as_included_in_ico(Badge<ICOImageDecoderPlugin>, ReadonlyBytes data)
{
    return create_impl(data, IncludedInICO::Yes);
}

ErrorOr<ImageFrameDescriptor> BMPImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("BMPImageDecoderPlugin: Invalid frame index");

    if (m_context->state == BMPLoadingContext::State::Error)
        return Error::from_string_literal("BMPImageDecoderPlugin: Decoding failed");

    if (m_context->state == BMPLoadingContext::State::HeaderDecoded) {
        auto bitmap = TRY(Bitmap::create_shareable_with_gpu_access(to_bitmap_format(m_context->info.format), AlphaType::Unpremultiplied, size()));
        if (!ImageDecoders::FFI::bmp_rust_decode(m_context->decoder, m_context->data.data(), m_context->data.size(), bitmap->unchecked_scanline_u8(0), bitmap->pitch())) {
            m_context->state = BMPLoadingContext::State::Error;
            return Error::from_string_literal("BMPImageDecoderPlugin: Decoding failed");
        }
        m_context->bitmap = move(bitmap);
        m_context->state = BMPLoadingContext::State::PixelDataDecoded;
    }

    return ImageFrameDescriptor { *m_context->bitmap, 0 };
}

ErrorOr<Optional<ReadonlyBytes>> BMPImageDecoderPlugin::icc_data()
{
    auto const& info = m_context->info;
    switch (info.icc_profile) {
    case ImageDecoders::FFI::BMPICCProfile::Absent:
        return OptionalNone {};
    case ImageDecoders::FFI::BMPICCProfile::OutOfBounds:
        return Error::from_string_literal("BMPImageDecoderPlugin: ICC profile data out of bounds");
    case ImageDecoders::FFI::BMPICCProfile::Embedded:
        return m_context->data.slice(info.icc_profile_offset, info.icc_profile_size);
    }
    VERIFY_NOT_REACHED();
}

}
