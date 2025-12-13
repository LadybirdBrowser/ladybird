/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/AVIFLoader.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TinyVGLoader.h>
#include <LibGfx/ImageFormats/WebPLoader.h>

namespace Gfx {

static ErrorOr<OwnPtr<ImageDecoderPlugin>> probe_and_sniff_for_appropriate_plugin(NonnullRefPtr<ImageDecoderStream> stream)
{
    struct ImagePluginStreamingInitializer {
        bool (*sniff)(NonnullRefPtr<ImageDecoderStream>) = nullptr;
        ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> (*create)(NonnullRefPtr<ImageDecoderStream>) = nullptr;
    };

    static constexpr ImagePluginStreamingInitializer s_streaming_initializers[] = {
        { JPEGImageDecoderPlugin::sniff, JPEGImageDecoderPlugin::create },
        { JPEGXLImageDecoderPlugin::sniff, JPEGXLImageDecoderPlugin::create },
        { WebPImageDecoderPlugin::sniff, WebPImageDecoderPlugin::create },
        { AVIFImageDecoderPlugin::sniff, AVIFImageDecoderPlugin::create },
    };

    for (auto& plugin : s_streaming_initializers) {
        auto sniff_result = plugin.sniff(stream);
        TRY(stream->seek(0, SeekMode::SetPosition));
        if (!sniff_result)
            continue;

        return TRY(plugin.create(move(stream)));
    }

    struct ImagePluginFullDataInitializer {
        bool (*sniff)(ReadonlyBytes) = nullptr;
        ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> (*create)(ReadonlyBytes) = nullptr;
    };

    static constexpr ImagePluginFullDataInitializer s_full_data_initializers[] = {
        { BMPImageDecoderPlugin::sniff, BMPImageDecoderPlugin::create },
        { GIFImageDecoderPlugin::sniff, GIFImageDecoderPlugin::create },
        { ICOImageDecoderPlugin::sniff, ICOImageDecoderPlugin::create },
        { PNGImageDecoderPlugin::sniff, PNGImageDecoderPlugin::create },
        { TIFFImageDecoderPlugin::sniff, TIFFImageDecoderPlugin::create },
        { TinyVGImageDecoderPlugin::sniff, TinyVGImageDecoderPlugin::create },
    };

    TRY(stream->seek(0, SeekMode::SetPosition));
    auto full_data = TRY(stream->read_until_eof());

    for (auto& plugin : s_full_data_initializers) {
        auto sniff_result = plugin.sniff(full_data);
        if (!sniff_result)
            continue;

        return TRY(plugin.create(full_data));
    }

    return OwnPtr<ImageDecoderPlugin> {};
}

ErrorOr<ColorSpace> ImageDecoder::color_space()
{
    auto maybe_cicp = TRY(m_plugin->cicp());
    if (maybe_cicp.has_value())
        return ColorSpace::from_cicp(*maybe_cicp);

    auto maybe_icc_data = TRY(icc_data());
    if (!maybe_icc_data.has_value())
        return ColorSpace {};
    return ColorSpace::load_from_icc_bytes(maybe_icc_data.value());
}

ErrorOr<RefPtr<ImageDecoder>> ImageDecoder::try_create_for_stream(NonnullRefPtr<ImageDecoderStream> stream, [[maybe_unused]] Optional<ByteString> mime_type)
{
    if (auto plugin = TRY(probe_and_sniff_for_appropriate_plugin(stream)); plugin)
        return adopt_ref_if_nonnull(new (nothrow) ImageDecoder(plugin.release_nonnull()));

    return RefPtr<ImageDecoder> {};
}

ImageDecoder::ImageDecoder(NonnullOwnPtr<ImageDecoderPlugin> plugin)
    : m_plugin(move(plugin))
{
}

}
