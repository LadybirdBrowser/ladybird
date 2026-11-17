/*
 * Copyright (c) 2020, Paul Roukema <roukemap@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibImageDecoders/RustFFI.h>

namespace Gfx {

struct ICOLoadingContext {
    AK_ALLOC_WITH_KMALLOC;

    enum class State : u8 {
        DirectoryDecoded,
        BitmapDecoded,
        Error,
    };

    ReadonlyBytes data;
    ImageDecoders::FFI::ICOImageInfo info {};
    State state { State::DirectoryDecoded };
    RefPtr<Bitmap> bitmap;
};

ICOImageDecoderPlugin::ICOImageDecoderPlugin(NonnullOwnPtr<ICOLoadingContext> context)
    : m_context(move(context))
{
}

ICOImageDecoderPlugin::~ICOImageDecoderPlugin() = default;

bool ICOImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return ImageDecoders::FFI::ico_rust_sniff(data.data(), data.size());
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> ICOImageDecoderPlugin::create(ReadonlyBytes data)
{
    ImageDecoders::FFI::ICOImageInfo info {};
    if (!ImageDecoders::FFI::ico_rust_decode_directory(data.data(), data.size(), &info))
        return Error::from_string_literal("ICOImageDecoderPlugin: Invalid ICO/CUR directory");

    auto context = TRY(adopt_nonnull_own_or_enomem(new (nothrow) ICOLoadingContext));
    context->data = data;
    context->info = info;
    return TRY(adopt_nonnull_own_or_enomem(new (nothrow) ICOImageDecoderPlugin(move(context))));
}

IntSize ICOImageDecoderPlugin::size()
{
    return { static_cast<int>(m_context->info.width), static_cast<int>(m_context->info.height) };
}

ErrorOr<NonnullRefPtr<Bitmap>> ICOImageDecoderPlugin::decode_image()
{
    auto image = m_context->data.slice(m_context->info.image_offset, m_context->info.image_size);
    if (PNGImageDecoderPlugin::sniff(image)) {
        auto png_decoder = TRY(PNGImageDecoderPlugin::create(image));
        return TRY(png_decoder->frame(0)).image;
    }
    auto bmp_decoder = TRY(BMPImageDecoderPlugin::create_as_included_in_ico({}, image));
    return TRY(bmp_decoder->frame(0)).image;
}

ErrorOr<ImageFrameDescriptor> ICOImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("ICOImageDecoderPlugin: Invalid frame index");

    if (m_context->state == ICOLoadingContext::State::Error)
        return Error::from_string_literal("ICOImageDecoderPlugin: Decoding failed");

    if (m_context->state == ICOLoadingContext::State::DirectoryDecoded) {
        auto bitmap_or_error = decode_image();
        if (bitmap_or_error.is_error()) {
            m_context->state = ICOLoadingContext::State::Error;
            return Error::from_string_literal("ICOImageDecoderPlugin: Decoding failed");
        }
        m_context->bitmap = bitmap_or_error.release_value();
        m_context->state = ICOLoadingContext::State::BitmapDecoded;
    }

    return ImageFrameDescriptor { *m_context->bitmap, 0 };
}

}
