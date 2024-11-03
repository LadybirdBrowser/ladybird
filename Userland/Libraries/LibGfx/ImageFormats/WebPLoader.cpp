/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2024, doctortheemh <doctortheemh@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <LibGfx/ImageFormats/WebPLoader.h>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/mux.h>

namespace Gfx {

struct WebPLoadingContext {
    enum State {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
        BitmapDecoded,
    };

    State state { State::NotDecoded };
    ReadonlyBytes data;

    // Image properties
    IntSize size;
    bool has_alpha;
    bool has_animation;
    size_t frame_count;
    size_t loop_count;
    ByteBuffer icc_data;

    Vector<ImageFrameDescriptor> frame_descriptors;
};

WebPImageDecoderPlugin::WebPImageDecoderPlugin(ReadonlyBytes data, OwnPtr<WebPLoadingContext> context)
    : m_context(move(context))
{
    m_context->data = data;
}

WebPImageDecoderPlugin::~WebPImageDecoderPlugin() = default;

IntSize WebPImageDecoderPlugin::size()
{
    return m_context->size;
}

static ErrorOr<void> decode_webp_header(WebPLoadingContext& context)
{
    if (context.state >= WebPLoadingContext::HeaderDecoded)
        return {};

    int width = 0;
    int height = 0;
    int getinfo_result = WebPGetInfo(context.data.data(), context.data.size(), &width, &height);
    if (getinfo_result != 1)
        return Error::from_string_literal("Failed to decode webp image data");

    WebPBitstreamFeatures webp_bitstream_features {};
    VP8StatusCode vp8_result = WebPGetFeatures(context.data.data(), context.data.size(), &webp_bitstream_features);
    if (vp8_result != VP8_STATUS_OK)
        return Error::from_string_literal("Failed to get WebP bitstream features");

    // Image header now decoded, save some results for fast access in other parts of the plugin.
    context.size = IntSize { width, height };
    context.has_animation = webp_bitstream_features.has_animation;
    context.has_alpha = webp_bitstream_features.has_alpha;
    context.frame_count = 1;
    context.loop_count = 0;

    if (webp_bitstream_features.has_animation == 1) {
        WebPAnimDecoderOptions anim_decoder_options {};
        WebPAnimDecoderOptionsInit(&anim_decoder_options);
        anim_decoder_options.color_mode = MODE_BGRA;
        anim_decoder_options.use_threads = 1;

        WebPData webp_data { .bytes = context.data.data(), .size = context.data.size() };
        auto* anim_decoder = WebPAnimDecoderNew(&webp_data, &anim_decoder_options);
        if (anim_decoder == nullptr)
            return Error::from_string_literal("Failed to allocate WebPAnimDecoderNew failed");
        ScopeGuard guard { [=]() { WebPAnimDecoderDelete(anim_decoder); } };

        WebPAnimInfo anim_info {};
        int anim_getinfo_result = WebPAnimDecoderGetInfo(anim_decoder, &anim_info);
        if (anim_getinfo_result != 1)
            return Error::from_string_literal("Failed to get WebP animation info");

        context.frame_count = anim_info.frame_count;
        context.loop_count = anim_info.loop_count;
    }

    WebPData webp_data { .bytes = context.data.data(), .size = context.data.size() };
    WebPMux* mux = WebPMuxCreate(&webp_data, 0);
    ScopeGuard guard { [=]() { WebPMuxDelete(mux); } };

    uint32_t flag = 0;
    WebPMuxError err = WebPMuxGetFeatures(mux, &flag);
    if (err != WEBP_MUX_OK)
        return Error::from_string_literal("Failed to get webp features");

    if (flag & ICCP_FLAG) {
        WebPData icc_profile;
        err = WebPMuxGetChunk(mux, "ICCP", &icc_profile);
        if (err != WEBP_MUX_OK)
            return Error::from_string_literal("Failed to get ICCP chunk of webp");

        context.icc_data = TRY(context.icc_data.copy(icc_profile.bytes, icc_profile.size));
    }

    context.state = WebPLoadingContext::State::HeaderDecoded;
    return {};
}

static ErrorOr<void> decode_webp_image(WebPLoadingContext& context)
{
    VERIFY(context.state >= WebPLoadingContext::State::HeaderDecoded);

    if (context.has_animation) {
        WebPAnimDecoderOptions anim_decoder_options {};
        WebPAnimDecoderOptionsInit(&anim_decoder_options);
        anim_decoder_options.color_mode = MODE_BGRA;
        anim_decoder_options.use_threads = 1;

        WebPData webp_data { .bytes = context.data.data(), .size = context.data.size() };
        auto* anim_decoder = WebPAnimDecoderNew(&webp_data, &anim_decoder_options);
        if (anim_decoder == nullptr)
            return Error::from_string_literal("Failed to allocate WebPAnimDecoderNew failed");
        ScopeGuard guard { [=]() { WebPAnimDecoderDelete(anim_decoder); } };

        int old_timestamp = 0;
        while (WebPAnimDecoderHasMoreFrames(anim_decoder)) {
            uint8_t* frame_data = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderGetNext(anim_decoder, &frame_data, &timestamp))
                return Error::from_string_literal("Failed to decode animated frame");

            auto bitmap_format = context.has_alpha ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
            auto bitmap = TRY(Bitmap::create(bitmap_format, Gfx::AlphaType::Unpremultiplied, context.size));

            memcpy(bitmap->scanline_u8(0), frame_data, context.size.width() * context.size.height() * 4);

            auto duration = timestamp - old_timestamp;
            old_timestamp = timestamp;

            context.frame_descriptors.append(ImageFrameDescriptor { bitmap, duration });
        }
    } else {
        auto bitmap_format = context.has_alpha ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
        auto bitmap = TRY(Bitmap::create(bitmap_format, Gfx::AlphaType::Unpremultiplied, context.size));

        auto image_data = WebPDecodeBGRAInto(context.data.data(), context.data.size(), bitmap->scanline_u8(0), bitmap->data_size(), bitmap->pitch());
        if (image_data == nullptr)
            return Error::from_string_literal("Failed to decode webp image into bitmap");

        auto duration = 0;
        context.frame_descriptors.append(ImageFrameDescriptor { bitmap, duration });
    }

    return {};
}

bool WebPImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    WebPLoadingContext context;
    context.data = data;
    return !decode_webp_header(context).is_error();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> WebPImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto context = TRY(try_make<WebPLoadingContext>());
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) WebPImageDecoderPlugin(data, move(context))));
    TRY(decode_webp_header(*plugin->m_context));
    return plugin;
}

bool WebPImageDecoderPlugin::is_animated()
{
    return m_context->has_animation;
}

size_t WebPImageDecoderPlugin::loop_count()
{
    if (!is_animated())
        return 0;

    return m_context->loop_count;
}

size_t WebPImageDecoderPlugin::frame_count()
{
    if (!is_animated())
        return 1;

    return m_context->frame_count;
}

size_t WebPImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

ErrorOr<ImageFrameDescriptor> WebPImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index >= frame_count())
        return Error::from_string_literal("WebPImageDecoderPlugin: Invalid frame index");

    if (m_context->state == WebPLoadingContext::State::Error)
        return Error::from_string_literal("WebPImageDecoderPlugin: Decoding failed");

    if (m_context->state < WebPLoadingContext::State::BitmapDecoded) {
        TRY(decode_webp_image(*m_context));
        m_context->state = WebPLoadingContext::State::BitmapDecoded;
    }

    if (index >= m_context->frame_descriptors.size())
        return Error::from_string_literal("WebPImageDecoderPlugin: Invalid frame index");
    return m_context->frame_descriptors[index];
}

ErrorOr<Optional<ReadonlyBytes>> WebPImageDecoderPlugin::icc_data()
{
    if (m_context->state < WebPLoadingContext::State::HeaderDecoded)
        (void)frame(0);

    if (!m_context->icc_data.is_empty())
        return m_context->icc_data;
    return OptionalNone {};
}

}
