/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2024, doctortheemh <doctortheemh@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <LibGfx/ImageFormats/AVIFLoader.h>

#include <avif/avif.h>

namespace Gfx {

class AVIFLoadingContext {
    AK_MAKE_NONMOVABLE(AVIFLoadingContext);
    AK_MAKE_NONCOPYABLE(AVIFLoadingContext);

public:
    enum State {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
        BitmapDecoded,
    };

    State state { State::NotDecoded };
    ReadonlyBytes data;

    avifDecoder* decoder { nullptr };

    // image properties
    Optional<IntSize> size;
    bool has_alpha { false };
    size_t image_count { 0 };
    size_t repetition_count { 0 };
    ByteBuffer icc_data;

    Vector<ImageFrameDescriptor> frame_descriptors;

    AVIFLoadingContext() = default;
    ~AVIFLoadingContext()
    {
        avifDecoderDestroy(decoder);
        decoder = nullptr;
    }
};

AVIFImageDecoderPlugin::AVIFImageDecoderPlugin(ReadonlyBytes data, OwnPtr<AVIFLoadingContext> context)
    : m_context(move(context))
{
    m_context->data = data;
}

AVIFImageDecoderPlugin::~AVIFImageDecoderPlugin()
{
}

static ErrorOr<void> decode_avif_header(AVIFLoadingContext& context)
{
    if (context.state >= AVIFLoadingContext::HeaderDecoded)
        return {};

    if (context.decoder == nullptr) {
        context.decoder = avifDecoderCreate();

        if (context.decoder == nullptr) {
            return Error::from_string_literal("failed to allocate AVIF decoder");
        }
    }

    avifResult result = avifDecoderSetIOMemory(context.decoder, context.data.data(), context.data.size());
    if (result != AVIF_RESULT_OK)
        return Error::from_string_literal("Cannot set IO on avifDecoder");

    result = avifDecoderParse(context.decoder);
    if (result != AVIF_RESULT_OK)
        return Error::from_string_literal("Failed to decode AVIF");

    if (context.decoder->image->depth != 8)
        return Error::from_string_literal("Unsupported bitdepth");

    // Image header now decoded, save some results for fast access in other parts of the plugin.
    context.size = IntSize { context.decoder->image->width, context.decoder->image->height };
    context.has_alpha = context.decoder->alphaPresent == 1;
    context.image_count = context.decoder->imageCount;
    context.repetition_count = context.decoder->repetitionCount <= 0 ? 0 : context.decoder->repetitionCount;
    context.state = AVIFLoadingContext::State::HeaderDecoded;

    if (context.decoder->image->icc.size > 0) {
        context.icc_data.resize(context.decoder->image->icc.size);
        memcpy(context.icc_data.data(), context.decoder->image->icc.data, context.decoder->image->icc.size);
    }

    return {};
}

static ErrorOr<void> decode_avif_image(AVIFLoadingContext& context)
{
    VERIFY(context.state >= AVIFLoadingContext::State::HeaderDecoded);

    avifRGBImage rgb;
    while (avifDecoderNextImage(context.decoder) == AVIF_RESULT_OK) {
        auto bitmap_format = context.has_alpha ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
        auto bitmap = TRY(Bitmap::create(bitmap_format, context.size.value()));

        avifRGBImageSetDefaults(&rgb, context.decoder->image);
        rgb.pixels = bitmap->scanline_u8(0);
        rgb.rowBytes = bitmap->pitch();
        rgb.format = avifRGBFormat::AVIF_RGB_FORMAT_BGRA;

        avifResult result = avifImageYUVToRGB(context.decoder->image, &rgb);
        if (result != AVIF_RESULT_OK)
            return Error::from_string_literal("Conversion from YUV to RGB failed");

        auto duration = context.decoder->imageCount == 1 ? 0 : static_cast<int>(context.decoder->imageTiming.duration * 1000);
        context.frame_descriptors.append(ImageFrameDescriptor { bitmap, duration });

        context.state = AVIFLoadingContext::BitmapDecoded;
    }

    return {};
}

IntSize AVIFImageDecoderPlugin::size()
{
    return m_context->size.value();
}

bool AVIFImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    AVIFLoadingContext context;
    context.data = data;
    return !decode_avif_header(context).is_error();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> AVIFImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto context = TRY(try_make<AVIFLoadingContext>());
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) AVIFImageDecoderPlugin(data, move(context))));
    TRY(decode_avif_header(*plugin->m_context));
    return plugin;
}

bool AVIFImageDecoderPlugin::is_animated()
{
    return m_context->image_count > 1;
}

size_t AVIFImageDecoderPlugin::loop_count()
{
    return is_animated() ? m_context->repetition_count : 0;
}

size_t AVIFImageDecoderPlugin::frame_count()
{
    if (!is_animated())
        return 1;
    return m_context->image_count;
}

size_t AVIFImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

ErrorOr<ImageFrameDescriptor> AVIFImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index >= frame_count())
        return Error::from_string_literal("AVIFImageDecoderPlugin: Invalid frame index");

    if (m_context->state == AVIFLoadingContext::State::Error)
        return Error::from_string_literal("AVIFImageDecoderPlugin: Decoding failed");

    if (m_context->state < AVIFLoadingContext::State::BitmapDecoded) {
        TRY(decode_avif_image(*m_context));
        m_context->state = AVIFLoadingContext::State::BitmapDecoded;
    }

    if (index >= m_context->frame_descriptors.size())
        return Error::from_string_literal("AVIFImageDecoderPlugin: Invalid frame index");
    return m_context->frame_descriptors[index];
}

ErrorOr<Optional<ReadonlyBytes>> AVIFImageDecoderPlugin::icc_data()
{
    if (m_context->state < AVIFLoadingContext::State::HeaderDecoded)
        (void)frame(0);

    if (!m_context->icc_data.is_empty())
        return m_context->icc_data;
    return OptionalNone {};
}

}
