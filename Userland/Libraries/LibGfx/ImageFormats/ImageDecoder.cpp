/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/DDSLoader.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ILBMLoader.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/JBIG2Loader.h>
#include <LibGfx/ImageFormats/JPEG2000Loader.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <LibGfx/ImageFormats/PAMLoader.h>
#include <LibGfx/ImageFormats/PBMLoader.h>
#include <LibGfx/ImageFormats/PGMLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/PPMLoader.h>
#include <LibGfx/ImageFormats/TGALoader.h>
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
        { DDSImageDecoderPlugin::sniff, DDSImageDecoderPlugin::create },
        { GIFImageDecoderPlugin::sniff, GIFImageDecoderPlugin::create },
        { ICOImageDecoderPlugin::sniff, ICOImageDecoderPlugin::create },
        { ILBMImageDecoderPlugin::sniff, ILBMImageDecoderPlugin::create },
        { JBIG2ImageDecoderPlugin::sniff, JBIG2ImageDecoderPlugin::create },
        { JPEG2000ImageDecoderPlugin::sniff, JPEG2000ImageDecoderPlugin::create },
        { JPEGImageDecoderPlugin::sniff, JPEGImageDecoderPlugin::create },
        { JPEGXLImageDecoderPlugin::sniff, JPEGXLImageDecoderPlugin::create },
        { PAMImageDecoderPlugin::sniff, PAMImageDecoderPlugin::create },
        { PBMImageDecoderPlugin::sniff, PBMImageDecoderPlugin::create },
        { PGMImageDecoderPlugin::sniff, PGMImageDecoderPlugin::create },
        { PNGImageDecoderPlugin::sniff, PNGImageDecoderPlugin::create },
        { PPMImageDecoderPlugin::sniff, PPMImageDecoderPlugin::create },
        { TIFFImageDecoderPlugin::sniff, TIFFImageDecoderPlugin::create },
        { TinyVGImageDecoderPlugin::sniff, TinyVGImageDecoderPlugin::create },
        { WebPImageDecoderPlugin::sniff, WebPImageDecoderPlugin::create },
    };

    for (auto& plugin : s_initializers) {
        auto sniff_result = plugin.sniff(bytes);
        if (!sniff_result)
            continue;
        return TRY(plugin.create(bytes));
    }
    return OwnPtr<ImageDecoderPlugin> {};
}

static ErrorOr<OwnPtr<ImageDecoderPlugin>> probe_and_sniff_for_appropriate_plugin_with_known_mime_type(StringView mime_type, ReadonlyBytes bytes)
{
    struct ImagePluginWithMIMETypeInitializer {
        bool (*validate_before_create)(ReadonlyBytes) = nullptr;
        ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> (*create)(ReadonlyBytes) = nullptr;
        StringView mime_type;
    };

    static constexpr ImagePluginWithMIMETypeInitializer s_initializers_with_mime_type[] = {
        { TGAImageDecoderPlugin::validate_before_create, TGAImageDecoderPlugin::create, "image/x-targa"sv },
    };

    for (auto& plugin : s_initializers_with_mime_type) {
        if (plugin.mime_type != mime_type)
            continue;
        auto validation_result = plugin.validate_before_create(bytes);
        if (!validation_result)
            continue;
        return TRY(plugin.create(bytes));
    }
    return OwnPtr<ImageDecoderPlugin> {};
}

ErrorOr<RefPtr<ImageDecoder>> ImageDecoder::try_create_for_raw_bytes(ReadonlyBytes bytes, Optional<ByteString> mime_type)
{
    if (auto plugin = TRY(probe_and_sniff_for_appropriate_plugin(bytes)); plugin)
        return adopt_ref_if_nonnull(new (nothrow) ImageDecoder(plugin.release_nonnull()));

    if (mime_type.has_value()) {
        if (OwnPtr<ImageDecoderPlugin> plugin = TRY(probe_and_sniff_for_appropriate_plugin_with_known_mime_type(mime_type.value(), bytes)); plugin)
            return adopt_ref_if_nonnull(new (nothrow) ImageDecoder(plugin.release_nonnull()));
    }

    return RefPtr<ImageDecoder> {};
}

ImageDecoder::ImageDecoder(NonnullOwnPtr<ImageDecoderPlugin> plugin)
    : m_plugin(move(plugin))
{
}

}
