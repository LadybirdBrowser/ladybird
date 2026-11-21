/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2024, doctortheemh <doctortheemh@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Math.h>
#include <LibGfx/ImageFormats/AVIFLoader.h>

#include <avif/avif.h>

namespace Gfx {

class AVIFLoadingContext {
    AK_MAKE_NONMOVABLE(AVIFLoadingContext);
    AK_MAKE_NONCOPYABLE(AVIFLoadingContext);

public:
    AK_ALLOC_WITH_KMALLOC;

    enum State {
        NotDecoded = 0,
        HeaderDecoded,
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

        // This makes the decoder not error if an item in the file is missing the mandatory pixi property.
        // Reason for this is that older versions of ImageMagick do not set this property, which leads to
        // broken web content if the error is not ignored.
        context.decoder->strictFlags &= ~AVIF_STRICT_PIXI_REQUIRED;
    }

    avifResult result = avifDecoderSetIOMemory(context.decoder, context.data.data(), context.data.size());
    if (result != AVIF_RESULT_OK)
        return Error::from_string_literal("Cannot set IO on avifDecoder");

    result = avifDecoderParse(context.decoder);
    if (result != AVIF_RESULT_OK)
        return Error::from_string_literal("Failed to decode AVIF");

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

static int frame_duration_in_milliseconds(AVIFLoadingContext const& context, double duration_in_seconds)
{
    // NB: The duration comes from the file, so reject NaN and negative values and clamp huge ones.
    if (context.image_count == 1 || !(duration_in_seconds >= 0))
        return 0;
    return AK::clamp_to<int>(duration_in_seconds * 1000);
}

static ErrorOr<ImageFrameDescriptor> decode_avif_frame(AVIFLoadingContext& context, size_t index)
{
    VERIFY(context.state >= AVIFLoadingContext::State::HeaderDecoded);

    // NB: libavif decodes the next frame directly when frames are requested in order, does no decoding when the
    //     current frame is requested again, and otherwise decodes forward from the nearest keyframe.
    if (avifDecoderNthImage(context.decoder, index) != AVIF_RESULT_OK)
        return Error::from_string_literal("Failed to decode AVIF frame");

    auto bitmap_format = context.has_alpha ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
    auto bitmap = TRY(Bitmap::create_shareable_with_gpu_access(bitmap_format, AlphaType::Unpremultiplied, context.size.value()));

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, context.decoder->image);
    rgb.pixels = bitmap->scanline_u8(0);
    rgb.rowBytes = bitmap->pitch();
    rgb.format = avifRGBFormat::AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;

    avifResult result = avifImageYUVToRGB(context.decoder->image, &rgb);
    if (result != AVIF_RESULT_OK)
        return Error::from_string_literal("Conversion from YUV to RGB failed");

    return ImageFrameDescriptor { bitmap, frame_duration_in_milliseconds(context, context.decoder->imageTiming.duration) };
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

    return decode_avif_frame(*m_context, index);
}

int AVIFImageDecoderPlugin::frame_duration(size_t index)
{
    if (index >= frame_count())
        return 0;

    avifImageTiming timing;
    if (avifDecoderNthImageTiming(m_context->decoder, index, &timing) != AVIF_RESULT_OK)
        return 0;
    return frame_duration_in_milliseconds(*m_context, timing.duration);
}

ErrorOr<Optional<ReadonlyBytes>> AVIFImageDecoderPlugin::icc_data()
{
    if (!m_context->icc_data.is_empty())
        return m_context->icc_data;
    return OptionalNone {};
}

}
