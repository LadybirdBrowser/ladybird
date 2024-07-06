/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibGfx/ImageFormats/AVIFLoader.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TinyVGLoader.h>
#include <LibGfx/ImageFormats/WebPLoader.h>

namespace Gfx {

static ErrorOr<OwnPtr<ImageDecoderPlugin>> probe_and_sniff_for_appropriate_plugin(ReadonlyBytes bytes)
{
    struct ImagePluginInitializer {
        bool (*sniff)(ReadonlyBytes) = nullptr;
        ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> (*create)(ReadonlyBytes) = nullptr;
    };

    static constexpr ImagePluginInitializer s_initializers[] = {
        { BMPImageDecoderPlugin::sniff, BMPImageDecoderPlugin::create },
        { GIFImageDecoderPlugin::sniff, GIFImageDecoderPlugin::create },
        { ICOImageDecoderPlugin::sniff, ICOImageDecoderPlugin::create },
        { JPEGImageDecoderPlugin::sniff, JPEGImageDecoderPlugin::create },
        { JPEGXLImageDecoderPlugin::sniff, JPEGXLImageDecoderPlugin::create },
        { PNGImageDecoderPlugin::sniff, PNGImageDecoderPlugin::create },
        { TIFFImageDecoderPlugin::sniff, TIFFImageDecoderPlugin::create },
        { TinyVGImageDecoderPlugin::sniff, TinyVGImageDecoderPlugin::create },
        { WebPImageDecoderPlugin::sniff, WebPImageDecoderPlugin::create },
        { AVIFImageDecoderPlugin::sniff, AVIFImageDecoderPlugin::create }
    };

    for (auto& plugin : s_initializers) {
        auto sniff_result = plugin.sniff(bytes);
        if (!sniff_result)
            continue;
        return TRY(plugin.create(bytes));
    }
    return OwnPtr<ImageDecoderPlugin> {};
}

ErrorOr<RefPtr<ImageDecoder>> ImageDecoder::try_create_for_raw_bytes(ReadonlyBytes bytes, [[maybe_unused]] Optional<ByteString> mime_type)
{
    if (auto plugin = TRY(probe_and_sniff_for_appropriate_plugin(bytes)); plugin)
        return adopt_ref_if_nonnull(new (nothrow) ImageDecoder(plugin.release_nonnull()));

    return RefPtr<ImageDecoder> {};
}

ImageDecoder::ImageDecoder(NonnullOwnPtr<ImageDecoderPlugin> plugin)
    : m_plugin(move(plugin))
{
}

}
