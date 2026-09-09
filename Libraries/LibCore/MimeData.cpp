/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <LibCore/MimeData.h>

namespace Core {

struct MimeType {
    StringView name;
    Vector<StringView> common_extensions;
};

// See https://www.iana.org/assignments/media-types/<mime-type> for a list of registered MIME types.
// For example, https://www.iana.org/assignments/media-types/application/gzip
static auto make_registered_mime_types()
{
    return to_array<MimeType>({
        { .name = "application/gzip"sv, .common_extensions = { ".gz"sv, ".gzip"sv } },
        { .name = "application/javascript"sv, .common_extensions = { ".js"sv, ".mjs"sv } },
        { .name = "application/json"sv, .common_extensions = { ".json"sv } },
        { .name = "application/pdf"sv, .common_extensions = { ".pdf"sv } },
        { .name = "application/rtf"sv, .common_extensions = { ".rtf"sv } },
        { .name = "application/tar"sv, .common_extensions = { ".tar"sv } },
        { .name = "application/vnd.iccprofile"sv, .common_extensions = { ".icc"sv } },
        { .name = "application/vnd.sqlite3"sv, .common_extensions = { ".sqlite"sv } },
        { .name = "application/wasm"sv, .common_extensions = { ".wasm"sv } },
        { .name = "application/x-7z-compressed"sv, .common_extensions = { ".7z"sv } },
        { .name = "application/x-blender"sv, .common_extensions = { ".blend"sv, ".blended"sv } },
        { .name = "application/x-bzip2"sv, .common_extensions = { ".bz2"sv } },
        { .name = "application/x-sheets+json"sv, .common_extensions = { ".sheets"sv } },
        { .name = "application/xhtml+xml"sv, .common_extensions = { ".xhtml"sv, ".xht"sv } },
        { .name = "application/zip"sv, .common_extensions = { ".zip"sv } },

        { .name = "audio/flac"sv, .common_extensions = { ".flac"sv } },
        { .name = "audio/matroska"sv, .common_extensions = { ".mka"sv } },
        { .name = "audio/midi"sv, .common_extensions = { ".mid"sv } },
        { .name = "audio/mp4"sv, .common_extensions = { ".m4a"sv } },
        { .name = "audio/mpeg"sv, .common_extensions = { ".mp3"sv } },
        { .name = "audio/ogg"sv, .common_extensions = { ".ogg"sv, ".oga"sv, ".opus"sv } },
        { .name = "audio/qoa"sv, .common_extensions = { ".qoa"sv } },
        { .name = "audio/wav"sv, .common_extensions = { ".wav"sv } },

        { .name = "extra/elf"sv, .common_extensions = { ".elf"sv } },
        { .name = "extra/iso-9660"sv, .common_extensions = { ".iso"sv } },
        { .name = "extra/isz"sv, .common_extensions = { ".isz"sv } },
        { .name = "extra/nes-rom"sv, .common_extensions = { ".nes"sv } },
        { .name = "extra/qcow"sv, .common_extensions = { ".qcow"sv, ".qcow2"sv, ".qcow3"sv } },

        { .name = "font/otf"sv, .common_extensions = { ".otf"sv } },
        { .name = "font/ttf"sv, .common_extensions = { ".ttf"sv } },
        { .name = "font/woff"sv, .common_extensions = { ".woff"sv } },
        { .name = "font/woff2"sv, .common_extensions = { ".woff2"sv } },

        { .name = "image/avif"sv, .common_extensions = { ".avif"sv } },
        { .name = "image/bmp"sv, .common_extensions = { ".bmp"sv } },
        { .name = "image/gif"sv, .common_extensions = { ".gif"sv } },
        { .name = "image/j2c"sv, .common_extensions = { ".j2c"sv, ".j2k"sv } },
        { .name = "image/jp2"sv, .common_extensions = { ".jp2"sv, ".jpf"sv, ".jpx"sv } },
        { .name = "image/jpeg"sv, .common_extensions = { ".jpg"sv, ".jpeg"sv } },
        { .name = "image/jxl"sv, .common_extensions = { ".jxl"sv } },
        { .name = "image/png"sv, .common_extensions = { ".png"sv } },
        { .name = "image/svg+xml"sv, .common_extensions = { ".svg"sv } },
        { .name = "image/tiff"sv, .common_extensions = { ".tiff"sv } },
        { .name = "image/webp"sv, .common_extensions = { ".webp"sv } },
        { .name = "image/x-icon"sv, .common_extensions = { ".ico"sv } },
        { .name = "image/x-ilbm"sv, .common_extensions = { ".iff"sv, ".lbm"sv } },
        { .name = "image/x-jbig2"sv, .common_extensions = { ".jbig2"sv, ".jb2"sv } },
        { .name = "image/x-portable-arbitrarymap"sv, .common_extensions = { ".pam"sv } },
        { .name = "image/x-portable-bitmap"sv, .common_extensions = { ".pbm"sv } },
        { .name = "image/x-portable-graymap"sv, .common_extensions = { ".pgm"sv } },
        { .name = "image/x-portable-pixmap"sv, .common_extensions = { ".ppm"sv } },
        { .name = "image/x-targa"sv, .common_extensions = { ".tga"sv } },

        { .name = "text/css"sv, .common_extensions = { ".css"sv } },
        { .name = "text/csv"sv, .common_extensions = { ".csv"sv } },
        { .name = "text/html"sv, .common_extensions = { ".html"sv, ".htm"sv, "/"sv } },
        { .name = "text/markdown"sv, .common_extensions = { ".md"sv } },
        { .name = "text/plain"sv, .common_extensions = { ".c"sv, ".cpp"sv, ".gml"sv, ".h"sv, ".hpp"sv, ".ini"sv, ".ipc"sv, ".txt"sv, ".history"sv, ".shellrc"sv } },
        { .name = "text/x-shellscript"sv, .common_extensions = { ".sh"sv } },
        { .name = "text/xml"sv, .common_extensions = { ".xml"sv } },

        { .name = "video/matroska"sv, .common_extensions = { ".mkv"sv } },
        { .name = "video/mp4"sv, .common_extensions = { ".mp4"sv, ".m4v"sv, ".mpg4"sv } },
        { .name = "video/ogg"sv, .common_extensions = { ".ogv"sv } },
        { .name = "video/webm"sv, .common_extensions = { ".webm"sv } },
    });
}

static auto const& registered_mime_types()
{
    static NeverDestroyed<decltype(make_registered_mime_types())> mime_types { make_registered_mime_types() };
    return *mime_types;
}

StringView guess_mime_type_based_on_filename(StringView path)
{
    for (auto const& mime_type : registered_mime_types()) {
        for (auto const possible_extension : mime_type.common_extensions) {
            if (path.ends_with(possible_extension, CaseSensitivity::CaseInsensitive))
                return mime_type.name;
        }
    }

    return "application/octet-stream"sv;
}

}
