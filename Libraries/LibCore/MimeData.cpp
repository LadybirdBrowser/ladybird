/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCore/MimeData.h>
#include <LibURL/Parser.h>

namespace Core {

Vector<URL::URL> MimeData::urls() const
{
    auto it = m_data.find("text/uri-list"_sv);
    if (it == m_data.end())
        return {};
    Vector<URL::URL> urls;
    for (auto& line : StringView(it->value).split_view('\n')) {
        if (auto maybe_url = URL::Parser::basic_parse(line); maybe_url.has_value())
            urls.append(maybe_url.release_value());
    }
    return urls;
}

ErrorOr<void> MimeData::set_urls(Vector<URL::URL> const& urls)
{
    StringBuilder builder;
    for (auto& url : urls) {
        TRY(builder.try_append(url.to_byte_string()));
        TRY(builder.try_append('\n'));
    }
    set_data("text/uri-list"_string, TRY(builder.to_byte_buffer()));

    return {};
}

ByteString MimeData::text() const
{
    return ByteString::copy(m_data.get("text/plain"_sv).value_or({}));
}

void MimeData::set_text(ByteString const& text)
{
    set_data("text/plain"_string, text.to_byte_buffer());
}

// FIXME: Share this, TextEditor and HackStudio language detection somehow.
static Array constexpr s_plaintext_suffixes = {
    // Extensions
    ".c"_sv,
    ".cpp"_sv,
    ".gml"_sv,
    ".h"_sv,
    ".hpp"_sv,
    ".ini"_sv,
    ".ipc"_sv,
    ".txt"_sv,

    // Base names
    ".history"_sv,
    ".shellrc"_sv,
};

// See https://www.iana.org/assignments/media-types/<mime-type> for a list of registered MIME types.
// For example, https://www.iana.org/assignments/media-types/application/gzip
static Array const s_registered_mime_type = {
    MimeType { .name = "application/gzip"_sv, .common_extensions = { ".gz"_sv, ".gzip"_sv }, .description = "GZIP compressed data"_sv, .magic_bytes = Vector<u8> { 0x1F, 0x8B } },
    MimeType { .name = "application/javascript"_sv, .common_extensions = { ".js"_sv, ".mjs"_sv }, .description = "JavaScript source"_sv },
    MimeType { .name = "application/json"_sv, .common_extensions = { ".json"_sv }, .description = "JSON data"_sv },
    MimeType { .name = "application/pdf"_sv, .common_extensions = { ".pdf"_sv }, .description = "PDF document"_sv, .magic_bytes = Vector<u8> { 0x25, 'P', 'D', 'F', 0x2D } },
    MimeType { .name = "application/rtf"_sv, .common_extensions = { ".rtf"_sv }, .description = "Rich text file"_sv, .magic_bytes = Vector<u8> { 0x7B, 0x5C, 0x72, 0x74, 0x66, 0x31 } },
    MimeType { .name = "application/tar"_sv, .common_extensions = { ".tar"_sv }, .description = "Tape archive"_sv, .magic_bytes = Vector<u8> { 0x75, 0x73, 0x74, 0x61, 0x72 }, .offset = 0x101 },
    MimeType { .name = "application/vnd.iccprofile"_sv, .common_extensions = { ".icc"_sv }, .description = "ICC color profile"_sv, .magic_bytes = Vector<u8> { 'a', 'c', 's', 'p' }, .offset = 36 },
    MimeType { .name = "application/vnd.sqlite3"_sv, .common_extensions = { ".sqlite"_sv }, .description = "SQLite database"_sv, .magic_bytes = Vector<u8> { 'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', 0x00 } },
    MimeType { .name = "application/wasm"_sv, .common_extensions = { ".wasm"_sv }, .description = "WebAssembly bytecode"_sv, .magic_bytes = Vector<u8> { 0x00, 'a', 's', 'm' } },
    MimeType { .name = "application/x-7z-compressed"_sv, .common_extensions = { "7z"_sv }, .description = "7-Zip archive"_sv, .magic_bytes = Vector<u8> { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C } },
    MimeType { .name = "application/x-blender"_sv, .common_extensions = { ".blend"_sv, ".blended"_sv }, .description = "Blender project file"_sv, .magic_bytes = Vector<u8> { 'B', 'L', 'E', 'N', 'D', 'E', 'R' } },
    MimeType { .name = "application/x-bzip2"_sv, .common_extensions = { ".bz2"_sv }, .description = "BZIP2 compressed data"_sv, .magic_bytes = Vector<u8> { 'B', 'Z', 'h' } },
    MimeType { .name = "application/x-sheets+json"_sv, .common_extensions = { ".sheets"_sv }, .description = "Serenity Spreadsheet document"_sv },
    MimeType { .name = "application/xhtml+xml"_sv, .common_extensions = { ".xhtml"_sv, ".xht"_sv }, .description = "XHTML document"_sv },
    MimeType { .name = "application/zip"_sv, .common_extensions = { ".zip"_sv }, .description = "ZIP archive"_sv, .magic_bytes = Vector<u8> { 0x50, 0x4B } },

    MimeType { .name = "audio/flac"_sv, .common_extensions = { ".flac"_sv }, .description = "FLAC audio"_sv, .magic_bytes = Vector<u8> { 'f', 'L', 'a', 'C' } },
    MimeType { .name = "audio/midi"_sv, .common_extensions = { ".mid"_sv }, .description = "MIDI notes"_sv, .magic_bytes = Vector<u8> { 0x4D, 0x54, 0x68, 0x64 } },
    MimeType { .name = "audio/mpeg"_sv, .common_extensions = { ".mp3"_sv }, .description = "MP3 audio"_sv, .magic_bytes = Vector<u8> { 0xFF, 0xFB } },
    MimeType { .name = "audio/qoa"_sv, .common_extensions = { ".qoa"_sv }, .description = "Quite OK Audio"_sv, .magic_bytes = Vector<u8> { 'q', 'o', 'a', 'f' } },
    MimeType { .name = "audio/wav"_sv, .common_extensions = { ".wav"_sv }, .description = "WAVE audio"_sv, .magic_bytes = Vector<u8> { 'W', 'A', 'V', 'E' }, .offset = 8 },

    MimeType { .name = "extra/elf"_sv, .common_extensions = { ".elf"_sv }, .description = "ELF"_sv, .magic_bytes = Vector<u8> { 0x7F, 'E', 'L', 'F' } },
    MimeType { .name = "extra/ext"_sv, .description = "EXT filesystem"_sv, .magic_bytes = Vector<u8> { 0x53, 0xEF }, .offset = 0x438 },
    MimeType { .name = "extra/iso-9660"_sv, .common_extensions = { ".iso"_sv }, .description = "ISO 9660 CD/DVD image"_sv, .magic_bytes = Vector<u8> { 0x43, 0x44, 0x30, 0x30, 0x31 }, .offset = 0x8001 },
    MimeType { .name = "extra/iso-9660"_sv, .common_extensions = { ".iso"_sv }, .description = "ISO 9660 CD/DVD image"_sv, .magic_bytes = Vector<u8> { 0x43, 0x44, 0x30, 0x30, 0x31 }, .offset = 0x8801 },
    MimeType { .name = "extra/iso-9660"_sv, .common_extensions = { ".iso"_sv }, .description = "ISO 9660 CD/DVD image"_sv, .magic_bytes = Vector<u8> { 0x43, 0x44, 0x30, 0x30, 0x31 }, .offset = 0x9001 },
    MimeType { .name = "extra/isz"_sv, .common_extensions = { ".isz"_sv }, .description = "Compressed ISO image"_sv, .magic_bytes = Vector<u8> { 'I', 's', 'Z', '!' } },
    MimeType { .name = "extra/lua-bytecode"_sv, .description = "Lua bytecode"_sv, .magic_bytes = Vector<u8> { 0x1B, 'L', 'u', 'a' } },
    MimeType { .name = "extra/nes-rom"_sv, .common_extensions = { ".nes"_sv }, .description = "Nintendo Entertainment System ROM"_sv, .magic_bytes = Vector<u8> { 'N', 'E', 'S', 0x1A } },
    MimeType { .name = "extra/qcow"_sv, .common_extensions = { ".qcow"_sv, ".qcow2"_sv, ".qcow3"_sv }, .description = "QCOW file"_sv, .magic_bytes = Vector<u8> { 'Q', 'F', 'I' } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0x01 } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0x5E } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0x9C } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0xDA } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0x20 } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0x7D } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0xBB } },
    MimeType { .name = "extra/raw-zlib"_sv, .description = "Raw zlib stream"_sv, .magic_bytes = Vector<u8> { 0x78, 0xF9 } },
    MimeType { .name = "extra/win-31x-compressed"_sv, .description = "Windows 3.1X compressed file"_sv, .magic_bytes = Vector<u8> { 'K', 'W', 'A', 'J' } },
    MimeType { .name = "extra/win-95-compressed"_sv, .description = "Windows 95 compressed file"_sv, .magic_bytes = Vector<u8> { 'S', 'Z', 'D', 'D' } },

    MimeType { .name = "font/otf"_sv, .common_extensions = { "otf"_sv }, .description = "OpenType font"_sv, .magic_bytes = Vector<u8> { 'O', 'T', 'T', 'F' } },
    MimeType { .name = "font/ttf"_sv, .common_extensions = { "ttf"_sv }, .description = "TrueType font"_sv, .magic_bytes = Vector<u8> { 0x00, 0x01, 0x00, 0x00, 0x00 } },
    MimeType { .name = "font/woff"_sv, .common_extensions = { "woff"_sv }, .description = "WOFF font"_sv, .magic_bytes = Vector<u8> { 'W', 'O', 'F', 'F' } },
    MimeType { .name = "font/woff2"_sv, .common_extensions = { "woff2"_sv }, .description = "WOFF2 font"_sv, .magic_bytes = Vector<u8> { 'W', 'O', 'F', '2' } },

    MimeType { .name = "image/bmp"_sv, .common_extensions = { ".bmp"_sv }, .description = "BMP image data"_sv, .magic_bytes = Vector<u8> { 'B', 'M' } },
    MimeType { .name = "image/gif"_sv, .common_extensions = { ".gif"_sv }, .description = "GIF image data"_sv, .magic_bytes = Vector<u8> { 'G', 'I', 'F', '8', '7', 'a' } },
    MimeType { .name = "image/gif"_sv, .common_extensions = { ".gif"_sv }, .description = "GIF image data"_sv, .magic_bytes = Vector<u8> { 'G', 'I', 'F', '8', '9', 'a' } },
    MimeType { .name = "image/j2c"_sv, .common_extensions = { ".j2c"_sv, ".j2k"_sv }, .description = "JPEG2000 image data codestream"_sv, .magic_bytes = Vector<u8> { 0xFF, 0x4F, 0xFF, 0x51 } },
    MimeType { .name = "image/jp2"_sv, .common_extensions = { ".jp2"_sv, ".jpf"_sv, ".jpx"_sv }, .description = "JPEG2000 image data"_sv, .magic_bytes = Vector<u8> { 0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A } },
    MimeType { .name = "image/jpeg"_sv, .common_extensions = { ".jpg"_sv, ".jpeg"_sv }, .description = "JPEG image data"_sv, .magic_bytes = Vector<u8> { 0xFF, 0xD8, 0xFF } },
    MimeType { .name = "image/jxl"_sv, .common_extensions = { ".jxl"_sv }, .description = "JPEG XL image data"_sv, .magic_bytes = Vector<u8> { 0xFF, 0x0A } },
    MimeType { .name = "image/png"_sv, .common_extensions = { ".png"_sv }, .description = "PNG image data"_sv, .magic_bytes = Vector<u8> { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A } },
    MimeType { .name = "image/svg+xml"_sv, .common_extensions = { ".svg"_sv }, .description = "Scalable Vector Graphics image"_sv },
    MimeType { .name = "image/tiff"_sv, .common_extensions = { ".tiff"_sv }, .description = "TIFF image data"_sv, .magic_bytes = Vector<u8> { 'I', 'I', '*', 0x00 } },
    MimeType { .name = "image/tiff"_sv, .common_extensions = { ".tiff"_sv }, .description = "TIFF image data"_sv, .magic_bytes = Vector<u8> { 'M', 'M', 0x00, '*' } },
    MimeType { .name = "image/tinyvg"_sv, .common_extensions = { ".tvg"_sv }, .description = "TinyVG vector graphics"_sv, .magic_bytes = Vector<u8> { 0x72, 0x56 } },
    MimeType { .name = "image/webp"_sv, .common_extensions = { ".webp"_sv }, .description = "WebP image data"_sv, .magic_bytes = Vector<u8> { 'W', 'E', 'B', 'P' }, .offset = 8 },
    MimeType { .name = "image/x-icon"_sv, .common_extensions = { ".ico"_sv }, .description = "ICO image data"_sv },
    MimeType { .name = "image/x-ilbm"_sv, .common_extensions = { ".iff"_sv, ".lbm"_sv }, .description = "Interleaved bitmap image data"_sv, .magic_bytes = Vector<u8> { 0x46, 0x4F, 0x52, 0x4F } },
    MimeType { .name = "image/x-jbig2"_sv, .common_extensions = { ".jbig2"_sv, ".jb2"_sv }, .description = "JBIG2 image data"_sv, .magic_bytes = Vector<u8> { 0x97, 0x4A, 0x42, 0x32, 0x0D, 0x0A, 0x1A, 0x0A } },
    MimeType { .name = "image/x-portable-arbitrarymap"_sv, .common_extensions = { ".pam"_sv }, .description = "PAM image data"_sv, .magic_bytes = Vector<u8> { 0x50, 0x37, 0x0A } },
    MimeType { .name = "image/x-portable-bitmap"_sv, .common_extensions = { ".pbm"_sv }, .description = "PBM image data"_sv, .magic_bytes = Vector<u8> { 0x50, 0x31, 0x0A } },
    MimeType { .name = "image/x-portable-graymap"_sv, .common_extensions = { ".pgm"_sv }, .description = "PGM image data"_sv, .magic_bytes = Vector<u8> { 0x50, 0x32, 0x0A } },
    MimeType { .name = "image/x-portable-pixmap"_sv, .common_extensions = { ".ppm"_sv }, .description = "PPM image data"_sv, .magic_bytes = Vector<u8> { 0x50, 0x33, 0x0A } },
    MimeType { .name = "image/x-targa"_sv, .common_extensions = { ".tga"_sv }, .description = "Targa image data"_sv },

    MimeType { .name = "text/css"_sv, .common_extensions = { ".css"_sv }, .description = "Cascading Style Sheet"_sv },
    MimeType { .name = "text/csv"_sv, .common_extensions = { ".csv"_sv }, .description = "CSV text"_sv },
    MimeType { .name = "text/html"_sv, .common_extensions = { ".html"_sv, ".htm"_sv, ".xht"_sv, "/"_sv }, .description = "HTML document"_sv }, // FIXME: The "/" seems dubious
    MimeType { .name = "text/xml"_sv, .common_extensions = { ".xml"_sv }, .description = "XML document"_sv },
    MimeType { .name = "text/markdown"_sv, .common_extensions = { ".md"_sv }, .description = "Markdown document"_sv },
    MimeType { .name = "text/plain"_sv, .common_extensions = Vector(s_plaintext_suffixes.span()), .description = "plain text"_sv },
    MimeType { .name = "text/x-shellscript"_sv, .common_extensions = { ".sh"_sv }, .description = "POSIX shell script text executable"_sv, .magic_bytes = Vector<u8> { '#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n' } },

    MimeType { .name = "video/matroska"_sv, .common_extensions = { ".mkv"_sv }, .description = "Matroska container"_sv, .magic_bytes = Vector<u8> { 0x1A, 0x45, 0xDF, 0xA3 } },
    MimeType { .name = "video/webm"_sv, .common_extensions = { ".webm"_sv }, .description = "WebM video"_sv },
};

StringView guess_mime_type_based_on_filename(StringView path)
{
    for (auto const& mime_type : s_registered_mime_type) {
        for (auto const possible_extension : mime_type.common_extensions) {
            if (path.ends_with(possible_extension))
                return mime_type.name;
        }
    }

    return "application/octet-stream"_sv;
}

Optional<StringView> guess_mime_type_based_on_sniffed_bytes(ReadonlyBytes bytes)
{
    for (auto const& mime_type : s_registered_mime_type) {
        if (mime_type.magic_bytes.has_value()
            && bytes.size() >= mime_type.offset
            && bytes.slice(mime_type.offset).starts_with(*mime_type.magic_bytes)) {
            return mime_type.name;
        }
    }

    return {};
}

Optional<MimeType const&> get_mime_type_data(StringView mime_name)
{
    for (auto const& mime_type : s_registered_mime_type) {
        if (mime_name == mime_type.name)
            return mime_type;
    }

    return {};
}

Optional<StringView> guess_mime_type_based_on_sniffed_bytes(Core::File& file)
{
    // Read accounts for longest possible offset + signature we currently match against (extra/iso-9660)
    auto maybe_buffer = ByteBuffer::create_uninitialized(0x9006);
    if (maybe_buffer.is_error())
        return {};

    auto maybe_bytes = file.read_some(maybe_buffer.value());
    if (maybe_bytes.is_error())
        return {};

    return Core::guess_mime_type_based_on_sniffed_bytes(maybe_bytes.value());
}

}
